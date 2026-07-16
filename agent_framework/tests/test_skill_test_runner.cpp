#include <agent/skills/skill_test_runner.hpp>

#include <cassert>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <thread>

namespace {

std::set<std::filesystem::path> test_jails() {
    std::set<std::filesystem::path> result;
    std::error_code ec;
    for(const auto& entry : std::filesystem::directory_iterator(
            std::filesystem::temp_directory_path(), ec)) {
        if(entry.path().filename().string().starts_with("agent-skill-test-jail-"))
            result.insert(entry.path());
    }
    return result;
}

} // namespace

int main() {
    using namespace agent_framework;
    namespace fs = std::filesystem;
    const fs::path root = AGENT_STAGE6_FIXTURE_ROOT;
    const auto valid = parse_skill_test_file(root / "stage6-valid/tests/workflow.json");
    assert(valid.descriptor);
    assert(valid.descriptor->name == "workflow returns normalized value");
    assert(valid.descriptor->target.kind == "workflow");
    assert(valid.descriptor->expect.at("ok") == true);

    const std::vector<std::string> invalid = {
        "invalid-version.json", "invalid-kind.json", "invalid-path.json",
        "invalid-target.json", "invalid-mock.json", "invalid-expect.json"};
    for(const auto& file : invalid) {
        const auto result = parse_skill_test_file(root / "stage6-invalid/tests" / file);
        assert(!result.descriptor);
        assert(!result.diagnostics.empty());
        assert(result.diagnostics.front().code == "skill_test_descriptor_invalid");
        assert(!result.diagnostics.front().location.empty());
    }

    auto oversized = nlohmann::json::parse(std::ifstream(root / "stage6-valid/tests/workflow.json"));
    oversized["input"] = std::string(1024 * 1024 + 1, 'x');
    const auto too_large = parse_skill_test_descriptor(oversized, "oversized.json");
    assert(!too_large.descriptor);
    assert(too_large.diagnostics.front().location == "/input");

    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    assert(registry->get("stage6-valid"));
    SkillTestRunner runner(registry);
    const auto jails_before = test_jails();
    auto run_one = [&](const std::string& filter,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        SkillTestRunOptions options;
        options.filter = filter;
        options.timeout = timeout;
        const auto suite = runner.run("stage6-valid", options);
        if(!suite.ok) std::cerr << suite.to_json().dump(2) << '\n';
        assert(suite.ok);
        assert(suite.passed == 1U && suite.failed == 0U && suite.cases.size() == 1U);
        return suite.cases.front();
    };
    assert(run_one("resource returns JSON").output.at("value") == 7);
    assert(run_one("tool uses declared mock").events ==
           std::vector<std::string>({"invocation_started", "invocation_completed"}));
    assert(run_one("workflow returns normalized value").output.at("value") == 7);
    assert(run_one("script runs in jail").exit_code == 0);
    assert(run_one("CLI has empty secret environment").stdout_text == "cli:ok:unset\n");
    assert(run_one("secrets are not inherited").to_json().dump().find("PRIVATE_TOKEN") ==
           std::string::npos);
    assert(run_one("undeclared mock is denied").passed);
    assert(run_one("timeout terminates process", std::chrono::milliseconds(100)).passed);

    std::atomic<int> active_cases{0};
    std::atomic<int> maximum_active{0};
    SkillTestRunOptions parallel_options;
    parallel_options.filter = "parallel pass";
    parallel_options.jobs = 2;
    parallel_options.case_state_observer = [&](const std::string&, bool started) {
        if(!started) {
            active_cases.fetch_sub(1);
            return;
        }
        const auto active = active_cases.fetch_add(1) + 1;
        auto observed = maximum_active.load();
        while(observed < active &&
              !maximum_active.compare_exchange_weak(observed, active)) {}
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while(maximum_active.load() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
    };
    const auto parallel = runner.run("stage6-valid", parallel_options);
    assert(parallel.ok && parallel.passed == 2U && parallel.cases.size() == 2U);
    assert(maximum_active.load() == 2);
    assert(parallel.cases[0].name == "parallel pass alpha");
    assert(parallel.cases[1].name == "parallel pass beta");

    SkillTestRunOptions jobs_eight_options;
    jobs_eight_options.filter = "parallel ";
    jobs_eight_options.jobs = 8;
    const auto jobs_eight = runner.run("stage6-valid", jobs_eight_options);
    assert(!jobs_eight.ok && jobs_eight.passed == 2U && jobs_eight.failed == 1U);
    assert(jobs_eight.cases.size() == 3U);
    assert(jobs_eight.cases[0].name == "parallel mixed failure");
    assert(jobs_eight.cases[1].name == "parallel pass alpha");
    assert(jobs_eight.cases[2].name == "parallel pass beta");

    SkillTestRunOptions active_cancel_options;
    active_cancel_options.filter = "cancel terminates process";
    active_cancel_options.control = std::make_shared<TaskControl>();
    std::atomic<bool> cancel_case_started{false};
    active_cancel_options.case_state_observer = [&](const std::string&, bool started) {
        if(started) cancel_case_started.store(true);
    };
    auto active_cancel = std::async(std::launch::async, [&] {
        return runner.run("stage6-valid", active_cancel_options);
    });
    const auto cancel_start_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(!cancel_case_started.load() && std::chrono::steady_clock::now() < cancel_start_deadline)
        std::this_thread::yield();
    assert(cancel_case_started.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    active_cancel_options.control->request_cancel();
    const auto actively_cancelled = active_cancel.get();
    assert(actively_cancelled.ok && actively_cancelled.passed == 1U);

    SkillTestRunOptions mismatch_options;
    mismatch_options.filter = "mismatch is detected";
    const auto mismatches = runner.run("stage6-valid", mismatch_options);
    assert(!mismatches.ok && mismatches.failed == 2U);
    for(const auto& result : mismatches.cases)
        assert(result.error.value("code", "") == "skill_test_expectation_failed");

    SkillTestRunOptions cancelled_options;
    cancelled_options.filter = "resource returns JSON";
    cancelled_options.control = std::make_shared<TaskControl>();
    cancelled_options.control->request_cancel();
    const auto cancelled = runner.run("stage6-valid", cancelled_options);
    assert(!cancelled.ok && cancelled.error.value("code", "") == "skill_cancelled");
    assert(test_jails() == jails_before);

    std::cout << "test_skill_test_runner: contracts and isolated execution ok\n";
    return 0;
}

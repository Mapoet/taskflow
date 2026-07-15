#include <agent/skill_test_runner.hpp>

#include <agent/skill_capability_runtime.hpp>
#include <agent/skill_lifecycle.hpp>
#include <agent/skill_runtime.hpp>
#include <agent/skill_script_tool.hpp>
#include <agent/skill_services.hpp>
#include <agent/skill_workflow.hpp>
#include <agent/toolbus.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <set>
#include <thread>

namespace agent_framework {

namespace {

SkillTestParseResult invalid(const std::filesystem::path& source, std::string location,
                             std::string message) {
    SkillDiagnostic diagnostic;
    diagnostic.severity = SkillDiagnosticSeverity::Error;
    diagnostic.code = "skill_test_descriptor_invalid";
    diagnostic.path = source;
    diagnostic.location = std::move(location);
    diagnostic.message = std::move(message);
    diagnostic.suggestion = "conform to agent.taskflow/skill-test/v1";
    return {std::nullopt, {std::move(diagnostic)}};
}

bool exact_keys(const nlohmann::json& object, const std::set<std::string>& allowed) {
    if(!object.is_object()) return false;
    for(auto it = object.begin(); it != object.end(); ++it)
        if(!allowed.contains(it.key())) return false;
    return true;
}

bool safe_resource_id(const std::string& value) {
    return !value.empty() && value.front() != '/' && value.find("..") == std::string::npos &&
           value.find('\\') == std::string::npos;
}

struct JailGuard {
    std::filesystem::path root;
    ~JailGuard() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

nlohmann::json failure(std::string code, std::string message,
                       nlohmann::json details = nlohmann::json::object()) {
    return {{"code", std::move(code)}, {"message", std::move(message)},
            {"details", std::move(details)}};
}

bool copy_package_safely(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(destination, ec);
    if(ec) { error = "jail directory creation failed"; return false; }
    for(fs::recursive_directory_iterator it(source, fs::directory_options::none, ec), end;
        !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if(ec) break;
        if(fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status))) {
            error = "package contains a link or special file";
            return false;
        }
        const auto relative = fs::relative(it->path(), source, ec);
        if(ec || relative.empty() || *relative.begin() == "..") {
            error = "package traversal detected";
            return false;
        }
        const auto target = destination / relative;
        if(fs::is_directory(status)) fs::create_directories(target, ec);
        else {
            fs::create_directories(target.parent_path(), ec);
            if(!ec) fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
            if(!ec) fs::permissions(target, status.permissions(), ec);
        }
        if(ec) break;
    }
    if(ec) { error = "package copy into jail failed"; return false; }
    return true;
}

std::optional<SkillResourceDescriptor> find_resource(const SkillManifest& manifest,
                                                     const std::string& id,
                                                     SkillResourceType kind) {
    const auto found = std::find_if(manifest.resources.begin(), manifest.resources.end(),
        [&](const auto& resource) { return resource.id == id && resource.kind == kind; });
    return found == manifest.resources.end() ? std::nullopt : std::optional(*found);
}

SkillResourceKind loader_kind(SkillResourceType kind) {
    switch(kind) {
        case SkillResourceType::Reference: return SkillResourceKind::Reference;
        case SkillResourceType::Asset: return SkillResourceKind::Asset;
        case SkillResourceType::Config: return SkillResourceKind::Config;
        case SkillResourceType::Schema: return SkillResourceKind::Schema;
        case SkillResourceType::Test: return SkillResourceKind::Test;
        default: return SkillResourceKind::AnyDeclared;
    }
}

std::map<std::string, std::string> resource_digests(
    const SkillIndexEntry& entry, const SkillManifest& manifest) {
    std::map<std::string, std::string> result;
    const auto package = entry.script_jail.value_or(entry.file_path.parent_path());
    for(const auto& resource : manifest.resources) {
        std::string error;
        const auto digest = skill_sha256_file(package / resource.path, &error);
        if(digest) result[resource.id] = *digest;
    }
    return result;
}

std::vector<std::string> event_names(const std::vector<SkillEvent>& events) {
    std::vector<std::string> result;
    result.reserve(events.size());
    for(const auto& event : events) result.emplace_back(skill_event_type_cstr(event.type));
    return result;
}

bool ordered_events_match(const nlohmann::json& expected,
                          const std::vector<std::string>& actual) {
    if(!expected.is_array()) return false;
    std::size_t position = 0;
    for(const auto& event : expected) {
        const auto type = event.value("type", "");
        while(position < actual.size() && actual[position] != type) ++position;
        if(position == actual.size()) return false;
        ++position;
    }
    return true;
}

SkillPermissionGrant isolated_grants(const SkillManifest& manifest,
                                     const std::filesystem::path& package,
                                     const nlohmann::json& mocks) {
    SkillPermissionGrant grant;
    const auto mock_tools = mocks.value("tools", nlohmann::json::object());
    for(const auto& requested : manifest.permissions.tools) {
        if(requested == "run_skill_script" || requested == "run_skill_cli" ||
           requested.starts_with("skill::") || mock_tools.contains(requested))
            grant.tools.push_back(requested);
    }
    for(const auto& scope : manifest.permissions.filesystem_read)
        grant.filesystem_read.push_back((package / scope).lexically_normal().string());
    for(const auto& scope : manifest.permissions.filesystem_write)
        grant.filesystem_write.push_back((package / scope).lexically_normal().string());
    return grant;
}

struct ActualResult {
    bool ok = false;
    nlohmann::json output = nullptr;
    nlohmann::json error = nlohmann::json::object();
    std::vector<std::string> events;
    std::string stdout_text;
    std::string stderr_text;
    std::optional<int> exit_code;
};

nlohmann::json normalized_error(const nlohmann::json& value,
                                std::string fallback = "skill_test_execution_failed") {
    if(value.is_object() && value.contains("code")) return value;
    if(value.is_object() && value.contains("error") && value["error"].is_object())
        return value["error"];
    return failure(std::move(fallback), "test target execution failed");
}

bool expectation_matches(const SkillTestDescriptor& descriptor,
                         const ActualResult& actual,
                         const std::map<std::string, std::string>& digests,
                         nlohmann::json& mismatch) {
    const auto& expect = descriptor.expect;
    if(expect.at("ok").get<bool>() != actual.ok) {
        mismatch = {{"field", "ok"}, {"expected", expect.at("ok")}, {"actual", actual.ok},
                    {"actualError", actual.error}};
        return false;
    }
    if(expect.contains("output") && expect["output"] != actual.output) {
        mismatch = {{"field", "output"}, {"expected", expect["output"]}, {"actual", actual.output}};
        return false;
    }
    if(expect.contains("error")) {
        const auto expected_code = expect["error"].value("code", "");
        if(actual.error.value("code", "") != expected_code) {
            mismatch = {{"field", "error.code"}, {"expected", expected_code},
                        {"actual", actual.error.value("code", "")}};
            return false;
        }
    }
    if(expect.contains("events") && !ordered_events_match(expect["events"], actual.events)) {
        mismatch = {{"field", "events"}, {"expected", expect["events"]}, {"actual", actual.events}};
        return false;
    }
    if(expect.contains("stdout") && expect["stdout"] != actual.stdout_text) {
        mismatch = {{"field", "stdout"}, {"expected", expect["stdout"]},
                    {"actual", actual.stdout_text}};
        return false;
    }
    if(expect.contains("stderr") && expect["stderr"] != actual.stderr_text) {
        mismatch = {{"field", "stderr"}, {"expected", expect["stderr"]},
                    {"actual", actual.stderr_text}};
        return false;
    }
    if(expect.contains("exitCode") &&
       (!actual.exit_code || *actual.exit_code != expect["exitCode"].get<int>())) {
        const nlohmann::json actual_exit = actual.exit_code
            ? nlohmann::json(*actual.exit_code) : nlohmann::json(nullptr);
        mismatch = {{"field", "exitCode"}, {"expected", expect["exitCode"]},
                    {"actual", actual_exit}};
        return false;
    }
    if(expect.contains("resourceDigests")) {
        for(auto it = expect["resourceDigests"].begin(); it != expect["resourceDigests"].end(); ++it) {
            const auto found = digests.find(it.key());
            if(found == digests.end() || found->second != it.value()) {
                mismatch = {{"field", "resourceDigests/" + it.key()}, {"expected", it.value()},
                            {"actual", found == digests.end() ? nlohmann::json(nullptr)
                                                              : nlohmann::json(found->second)}};
                return false;
            }
        }
    }
    return true;
}

} // namespace

SkillTestParseResult parse_skill_test_descriptor(const nlohmann::json& value,
                                                  const std::filesystem::path& source) {
    if(!value.is_object()) return invalid(source, "", "descriptor must be an object");
    if(value.dump().size() > 1024 * 1024)
        return invalid(source, "/input", "descriptor exceeds the one MiB limit");
    const std::set<std::string> top = {
        "apiVersion", "kind", "name", "target", "input", "mocks", "expect"};
    if(!exact_keys(value, top)) return invalid(source, "", "descriptor contains unknown fields");
    if(value.value("apiVersion", "") != "agent.taskflow/skill-test/v1")
        return invalid(source, "/apiVersion", "unsupported test descriptor version");
    if(value.value("kind", "") != "SkillTest")
        return invalid(source, "/kind", "kind must be SkillTest");
    if(!value.contains("name") || !value["name"].is_string() || value["name"].get<std::string>().empty())
        return invalid(source, "/name", "name must be a non-empty string");
    if(!value.contains("target") ||
       !exact_keys(value["target"], {"kind", "resource"}))
        return invalid(source, "/target", "target must contain only kind and resource");
    const auto target_kind = value["target"].value("kind", "");
    const std::set<std::string> target_kinds = {"resource", "tool", "workflow", "script", "cli"};
    if(!target_kinds.contains(target_kind))
        return invalid(source, "/target/kind", "unsupported target kind");
    const auto resource = value["target"].value("resource", "");
    if(!safe_resource_id(resource))
        return invalid(source, "/target/resource", "target resource contains an unsafe path");
    if(value.contains("mocks")) {
        if(!exact_keys(value["mocks"], {"tools"}))
            return invalid(source, "/mocks", "only deterministic tool mocks are allowed");
        if(value["mocks"].contains("tools") && !value["mocks"]["tools"].is_object())
            return invalid(source, "/mocks/tools", "tool mocks must be an object");
        if(value["mocks"].contains("tools")) {
            for(auto it = value["mocks"]["tools"].begin(); it != value["mocks"]["tools"].end(); ++it) {
                if(!exact_keys(it.value(), {"output", "error"}))
                    return invalid(source, "/mocks/tools/" + it.key(), "mock contains unknown fields");
            }
        }
    }
    if(!value.contains("expect") ||
       !exact_keys(value["expect"], {"ok", "output", "error", "events", "stdout",
                                     "stderr", "exitCode", "resourceDigests"}) ||
       !value["expect"].contains("ok") || !value["expect"]["ok"].is_boolean())
        return invalid(source, "/expect", "expect.ok is required and unknown fields are forbidden");
    if(value["expect"].contains("events")) {
        if(!value["expect"]["events"].is_array())
            return invalid(source, "/expect/events", "events must be an array");
        for(std::size_t index = 0; index < value["expect"]["events"].size(); ++index) {
            const auto& event = value["expect"]["events"][index];
            if(!exact_keys(event, {"type"}) || !event.contains("type") || !event["type"].is_string())
                return invalid(source, "/expect/events/" + std::to_string(index),
                               "event expectations require only a string type");
        }
    }
    SkillTestDescriptor descriptor;
    descriptor.name = value["name"].get<std::string>();
    descriptor.target = {target_kind, resource};
    descriptor.input = value.value("input", nlohmann::json::object());
    descriptor.mocks = value.value("mocks", nlohmann::json::object());
    descriptor.expect = value["expect"];
    descriptor.source = source;
    return {std::move(descriptor), {}};
}

SkillTestParseResult parse_skill_test_file(const std::filesystem::path& path) {
    try {
        std::ifstream input(path);
        if(!input) return invalid(path, "", "test descriptor could not be opened");
        nlohmann::json value;
        input >> value;
        return parse_skill_test_descriptor(value, path);
    } catch(const std::exception& error) {
        return invalid(path, "", std::string("invalid JSON: ") + error.what());
    }
}

nlohmann::json SkillTestCaseResult::to_json() const {
    return {{"name", name}, {"passed", passed}, {"output", output}, {"error", error},
            {"events", events}, {"stdout", stdout_text}, {"stderr", stderr_text},
            {"exitCode", exit_code ? nlohmann::json(*exit_code) : nlohmann::json(nullptr)},
            {"resourceDigests", resource_digests}, {"durationMs", duration_ms}};
}

nlohmann::json SkillTestSuiteResult::to_json() const {
    auto serialized = nlohmann::json::array();
    for(const auto& result : cases) serialized.push_back(result.to_json());
    return {{"ok", ok}, {"passed", passed}, {"failed", failed},
            {"cases", std::move(serialized)}, {"error", error}};
}

SkillTestSuiteResult SkillTestRunner::run(const std::string& skill_id,
                                          const SkillTestRunOptions& options) const {
    namespace fs = std::filesystem;
    SkillTestSuiteResult suite;
    if(options.jobs == 0 || options.jobs > 64) {
        suite.error = failure("skill_test_options_invalid", "jobs must be in the range 1..64");
        return suite;
    }
    if(options.timeout <= std::chrono::milliseconds::zero()) {
        suite.error = failure("skill_test_options_invalid", "timeout must be positive");
        return suite;
    }
    if(options.control && options.control->is_cancel_requested()) {
        suite.error = failure(kSkillCancelled, "test run was cancelled before execution");
        return suite;
    }

    const auto pinned = registry_->snapshot();
    const auto source_entry = pinned.get(skill_id);
    const auto source_manifest = pinned.get_manifest(skill_id);
    if(!source_entry || !source_manifest) {
        suite.error = failure("skill_test_skill_not_found", "skill is not available",
                              {{"skill", skill_id}});
        return suite;
    }

    static std::atomic<std::uint64_t> sequence{0};
    const auto jail_root = fs::temp_directory_path() /
        ("agent-skill-test-jail-" + std::to_string(sequence.fetch_add(1)) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    JailGuard jail{jail_root};
    const auto source_package = source_entry->script_jail.value_or(source_entry->file_path.parent_path());
    const auto jailed_package = jail_root / skill_id;
    std::string copy_error;
    if(!copy_package_safely(source_package, jailed_package, copy_error)) {
        suite.error = failure("skill_test_jail_invalid", copy_error);
        return suite;
    }

    auto registry = std::make_shared<SkillRegistry>(jail_root);
    registry->scan_or_reload();
    const auto entry = registry->get(skill_id);
    const auto manifest = registry->get_manifest(skill_id);
    if(!entry || !manifest || !registry->valid()) {
        suite.error = failure("skill_test_jail_invalid", "copied package failed Registry validation");
        return suite;
    }
    auto loader = std::make_shared<SkillLoader>(*registry);
    const auto digests = resource_digests(*entry, *manifest);

    std::vector<SkillTestDescriptor> descriptors;
    for(const auto& resource : manifest->resources) {
        if(resource.kind != SkillResourceType::Test) continue;
        const auto parsed = parse_skill_test_file(jailed_package / resource.path);
        if(!parsed.descriptor) {
            suite.error = failure("skill_test_descriptor_invalid", "package contains an invalid test",
                                  {{"path", resource.path},
                                   {"location", parsed.diagnostics.empty()
                                       ? "" : parsed.diagnostics.front().location}});
            return suite;
        }
        if(options.filter.empty() || parsed.descriptor->name.find(options.filter) != std::string::npos)
            descriptors.push_back(*parsed.descriptor);
    }
    std::sort(descriptors.begin(), descriptors.end(), [](const auto& left, const auto& right) {
        return std::tie(left.name, left.source) < std::tie(right.name, right.source);
    });
    if(descriptors.empty()) {
        suite.error = failure("skill_test_filter_unmatched", "no tests matched the requested filter");
        return suite;
    }

    for(const auto& descriptor : descriptors) {
        if(options.control && options.control->is_cancel_requested()) {
            suite.error = failure(kSkillCancelled, "test run was cancelled");
            break;
        }
        const auto started = std::chrono::steady_clock::now();
        SkillTestCaseResult result;
        result.name = descriptor.name;
        result.resource_digests = digests;
        ActualResult actual;
        std::vector<SkillEvent> runtime_events;
        auto control = options.control ? options.control : std::make_shared<TaskControl>();

        auto bus = std::make_shared<ToolBus>();
        const auto mocks = descriptor.mocks.value("tools", nlohmann::json::object());
        for(auto it = mocks.begin(); it != mocks.end(); ++it) {
            if(std::find(manifest->permissions.tools.begin(), manifest->permissions.tools.end(),
                         it.key()) == manifest->permissions.tools.end()) continue;
            const auto specification = it.value();
            ToolMeta mock_meta;
            mock_meta.name = it.key();
            mock_meta.side_effect = ToolSideEffect::ReadOnly;
            mock_meta.schema = {{"type", "object"}, {"additionalProperties", true}};
            bus->register_local_tool(it.key(), [specification](const nlohmann::json&) {
                if(specification.contains("error")) return specification["error"];
                return specification.value("output", nlohmann::json::object());
            }, mock_meta);
        }

        SkillInvocationContext context;
        context.control = control;
        context.grants = isolated_grants(*manifest, jailed_package, descriptor.mocks);
        context.environment.clear();
        context.secret_provider = {};
        context.event_sink = [&](const SkillEvent& event) { runtime_events.push_back(event); };
        context.task_id = "skill-test";
        context.run_id = descriptor.name;

        auto runtime = std::make_shared<SkillRuntime>(registry, loader);
        auto capabilities = std::make_shared<SkillCapabilityRuntime>(
            registry, loader, runtime, bus,
            [](const SkillMcpDescriptor&, const SkillInvocationContext&) {
                return std::shared_ptr<MCPClient>{};
            });

        const auto kind = descriptor.target.kind;
        if(kind == "resource") {
            const auto resource = std::find_if(manifest->resources.begin(), manifest->resources.end(),
                [&](const auto& candidate) {
                    return candidate.id == descriptor.target.resource &&
                           candidate.kind != SkillResourceType::Test;
                });
            if(resource == manifest->resources.end()) {
                actual.error = failure(kSkillDependencyUnavailable, "resource is unavailable");
            } else {
                std::string load_error;
                const auto content = loader->load_resource(
                    skill_id, resource->path, loader_kind(resource->kind),
                    resource->size_limit.value_or(1024U * 1024U), &load_error);
                if(!content) actual.error = failure("skill_test_resource_failed", load_error);
                else {
                    actual.ok = true;
                    if(resource->media_type == "application/json") {
                        try { actual.output = nlohmann::json::parse(*content); }
                        catch(const std::exception&) { actual.output = *content; }
                    } else actual.output = *content;
                }
            }
        } else if(kind == "tool") {
            const auto bound = capabilities->bind(skill_id, context, {false});
            if(!bound.ok()) {
                actual.error = failure(kSkillDependencyUnavailable,
                                       "declared tool dependency is unavailable");
            } else {
                const auto value = bound.binding->invoke_capability(
                    descriptor.target.resource, descriptor.input);
                if(value.is_object() && value.contains("code")) actual.error = normalized_error(value);
                else { actual.ok = true; actual.output = value; }
                bound.binding->close();
            }
        } else if(kind == "workflow") {
            SkillWorkflowRuntime workflows(registry, loader, runtime, capabilities);
            SkillWorkflowRunOptions run_options;
            run_options.context = context;
            const auto workflow = workflows.run(
                skill_id, descriptor.target.resource, descriptor.input, std::move(run_options));
            actual.ok = workflow.ok;
            actual.output = workflow.output;
            if(!workflow.ok) actual.error = normalized_error(workflow.error);
        } else if(kind == "script" || kind == "cli") {
            const auto resource_type = kind == "script" ? SkillResourceType::Script
                                                         : SkillResourceType::Cli;
            const auto resource = find_resource(*manifest, descriptor.target.resource, resource_type);
            if(!resource) actual.error = failure(kSkillDependencyUnavailable, "process resource unavailable");
            else {
                auto services = std::make_shared<SkillServices>();
                services->registry = registry;
                services->loader = loader;
                services->runtime = runtime;
                register_skill_script_tool(*bus, services);
                ToolCallControl call_control;
                call_control.cancellation_requested = [control] { return control->is_cancel_requested(); };
                call_control.skill_context = std::make_shared<SkillInvocationContext>(context);
                call_control.active_skill_id = skill_id;
                nlohmann::json request{{"skill_id", skill_id}, {"relative_path", resource->path}};
                if(kind == "script") request["input"] = descriptor.input;
                else request["args"] = descriptor.input.value("args", nlohmann::json::array());
                auto future = bus->call_tool(kind == "script" ? "run_skill_script" : "run_skill_cli",
                                             request, call_control);
                bool runner_timeout = future.wait_for(options.timeout) != std::future_status::ready;
                if(runner_timeout) {
                    control->mark_deadline_exceeded();
                    control->request_cancel();
                }
                const auto value = future.get();
                actual.stdout_text = value.value("stdout", "");
                actual.stderr_text = value.value("stderr", "");
                if(value.contains("exit_code")) actual.exit_code = value["exit_code"].get<int>();
                if(runner_timeout) actual.error = failure("skill_test_timeout", "test deadline exceeded");
                else if(value.contains("code") || value.contains("error"))
                    actual.error = normalized_error(value);
                else {
                    actual.ok = actual.exit_code.value_or(-1) == 0;
                    if(kind == "script" && actual.ok) {
                        if(value.contains("output")) actual.output = value["output"];
                        else {
                            try { actual.output = nlohmann::json::parse(actual.stdout_text); }
                            catch(const std::exception&) { actual.output = actual.stdout_text; }
                        }
                    }
                    if(!actual.ok) actual.error = failure("skill_test_process_failed",
                                                          "process returned a non-zero exit code");
                }
            }
        }
        actual.events = event_names(runtime_events);
        result.output = actual.output;
        result.events = actual.events;
        result.stdout_text = actual.stdout_text;
        result.stderr_text = actual.stderr_text;
        result.exit_code = actual.exit_code;
        nlohmann::json mismatch;
        result.passed = expectation_matches(descriptor, actual, digests, mismatch);
        result.error = result.passed ? actual.error
            : failure("skill_test_expectation_failed", "test expectation did not match", mismatch);
        result.duration_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        suite.cases.push_back(std::move(result));
    }
    for(const auto& result : suite.cases) {
        if(result.passed) ++suite.passed;
        else ++suite.failed;
    }
    suite.ok = suite.error.is_null() && suite.failed == 0;
    if(suite.error.is_null() && suite.failed != 0)
        suite.error = failure("skill_tests_failed", "one or more skill tests failed",
                              {{"failed", suite.failed}});
    return suite;
}

} // namespace agent_framework

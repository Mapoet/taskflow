#include <agent/skill_test_runner.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

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

    std::cout << "test_skill_test_runner: contracts ok\n";
    return 0;
}

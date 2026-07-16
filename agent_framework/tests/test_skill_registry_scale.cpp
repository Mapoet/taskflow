#include <agent/skills/skill_registry.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;
using json = nlohmann::json;

std::string skill_id(std::size_t index) {
    std::ostringstream value;
    value << "scale-" << std::setw(5) << std::setfill('0') << index;
    return value.str();
}

void add_skill(const fs::path& root, std::size_t index) {
    const auto id = skill_id(index);
    fs::create_directories(root / id);
    std::ofstream output(root / id / "SKILL.md");
    assert(output.good());
    output << "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: " << id
           << "\nversion: 1.0.0\ndescription: scale fixture " << id
           << "\n---\nscale\n";
}

std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
}
}

int main() {
    const auto root = fs::temp_directory_path() / "agent-skill-registry-scale";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    const std::vector<std::size_t> sizes = {1, 100, 1000, 10000};
    json metrics = json::array();
    std::size_t created = 0;
    for(const auto size : sizes) {
        while(created < size) add_skill(root, created++);
        SkillRegistry registry(root);
        auto started = std::chrono::steady_clock::now();
        registry.scan_or_reload();
        const auto scan_ms = elapsed_ms(started);
        assert(registry.valid());
        const auto entries = registry.entries();
        assert(entries.size() == size);
        assert(entries.front().id == skill_id(0));
        assert(entries.back().id == skill_id(size - 1));

        started = std::chrono::steady_clock::now();
        const auto routed = registry.match("please use " + skill_id(size - 1));
        const auto route_ms = elapsed_ms(started);
        assert(routed && *routed == skill_id(size - 1));

        const auto generation = registry.snapshot().generation();
        started = std::chrono::steady_clock::now();
        registry.publish(entries);
        const auto publish_ms = elapsed_ms(started);
        assert(registry.snapshot().generation() == generation + 1);
        assert(registry.entries().size() == size);
        metrics.push_back({{"skills", size}, {"scanMs", scan_ms},
                           {"routeMs", route_ms}, {"publishMs", publish_ms}});
    }
    fs::remove_all(root, ec);
    assert(!ec);
    std::cout << json{{"registryScale", metrics}}.dump() << '\n';
}

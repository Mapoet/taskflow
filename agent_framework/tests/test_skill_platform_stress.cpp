#include <agent/skill_capability_runtime.hpp>
#include <agent/skill_lifecycle.hpp>
#include <agent/skill_resource_cache.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;
using json = nlohmann::json;

void write_file(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << value;
}

std::size_t descriptor_count(const fs::path& path) {
    std::error_code ec;
    std::size_t count = 0;
    for(const auto& entry : fs::directory_iterator(path, ec)) {
        (void)entry;
        ++count;
    }
    return ec ? 0 : count;
}

std::size_t jail_count() {
    std::error_code ec;
    std::size_t count = 0;
    for(const auto& entry : fs::directory_iterator(fs::temp_directory_path(), ec))
        if(entry.path().filename().string().starts_with("agent-skill-test-jail-")) ++count;
    return count;
}

std::string children() {
    std::ifstream input("/proc/self/task/" + std::to_string(::getpid()) + "/children");
    std::string value;
    std::getline(input, value);
    return value;
}

struct MockState {
    std::mutex mutex;
    std::size_t created = 0;
    std::size_t disconnected = 0;
    std::size_t calls = 0;
};

class MockTransport final : public MCPTransportInterface {
public:
    explicit MockTransport(std::shared_ptr<MockState> state) : state_(std::move(state)) {}
    bool connect(const std::string&) override { connected_ = true; return true; }
    void disconnect() override {
        if(!connected_) return;
        connected_ = false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->disconnected;
    }
    bool is_connected() const override { return connected_; }
    MCPTransport get_transport_type() const override { return MCPTransport::HTTP; }
    void send_notification(const json&) override {}
    json transceive(const json& request) override {
        const auto id = request.at("id");
        const auto method = request.at("method").get<std::string>();
        if(method == "initialize")
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"protocolVersion", "2024-11-05"},
                                {"capabilities", json::object()}}}};
        if(method == "tools/list")
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"tools", json::array({{{"name", "echo"},
                        {"description", "echo"}, {"inputSchema", {{"type", "object"}}}}})}}}};
        if(method == "tools/call") {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->calls;
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"content", json::array({{{"type", "text"},
                        {"text", "echo"}}})}, {"isError", false}}}};
        }
        throw std::runtime_error("unexpected mock request");
    }
private:
    std::shared_ptr<MockState> state_;
    bool connected_ = false;
};
}

int main() {
    const auto root = fs::temp_directory_path() / "agent-skill-platform-stress";
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto package = root / "stress";
    write_file(package / "data.txt", "payload");
    write_file(package / "mcp.json", R"({"server":"mock","transport":"mock","tools":["echo"]})");
    write_file(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: stress
version: 1.0.0
description: platform stress fixture
permissions:
  tools: [skill::stress::mock.echo]
  filesystem:
    read: [.]
resources:
  references:
    - id: data
      path: data.txt
  mcp:
    - id: mock
      path: mcp.json
---
stress
)");

    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    assert(registry->valid());
    auto loader = std::make_shared<SkillLoader>(*registry);
    auto runtime = std::make_shared<SkillRuntime>(registry, loader);
    auto bus = std::make_shared<ToolBus>();
    auto mock = std::make_shared<MockState>();
    SkillCapabilityRuntime capabilities(registry, loader, runtime, bus,
        [mock](const SkillMcpDescriptor&, const SkillInvocationContext&) {
            std::lock_guard<std::mutex> lock(mock->mutex);
            ++mock->created;
            return MCPClient::create_with_transport(std::make_unique<MockTransport>(mock), true);
        });

    std::string digest_error;
    const auto digest = skill_sha256_file(package / "data.txt", &digest_error);
    assert(digest);
    SkillResourceHandle cache_handle;
    cache_handle.path = package / "data.txt";
    cache_handle.descriptor.id = "data";
    cache_handle.descriptor.cache_policy = SkillCachePolicy::OnDemand;
    cache_handle.resource_digest = *digest;
    cache_handle.package_digest = registry->get("stress")->package_digest;
    cache_handle.size = 7;
    cache_handle.view_size = 7;
    SkillResourceCache cache(root / "cache");

    const auto fd_before = descriptor_count("/proc/self/fd");
    const auto children_before = children();
    const auto jails_before = jail_count();
    std::set<std::string> committed_side_effects;
    for(std::size_t cycle = 0; cycle < 1000; ++cycle) {
        if(cycle != 0 && cycle % 100 == 0) {
            registry->scan_or_reload();
            loader = std::make_shared<SkillLoader>(*registry);
            runtime = std::make_shared<SkillRuntime>(registry, loader);
        }
        SkillInvocationContext context;
        context.control = std::make_shared<TaskControl>();
        context.grants.filesystem_read = {package.string()};
        context.grants.tools = {"skill::stress::mock.echo"};
        context.run_id = "cycle-" + std::to_string(cycle);
        if(cycle % 4 == 0) {
            context.control->request_cancel();
            const auto cancelled = runtime->begin(
                "stress", "data", SkillResourceType::Reference, json::object(), context);
            assert(!cancelled.ok && cancelled.error.value("code", "") == kSkillCancelled);
            context.control = std::make_shared<TaskControl>();
        }
        const auto started = runtime->begin(
            "stress", "data", SkillResourceType::Reference, json::object(), context);
        assert(started.ok && started.ticket);
        const auto finished = runtime->finish(*started.ticket, {{"cycle", cycle}});
        assert(finished.ok);
        assert(committed_side_effects.insert(context.run_id).second);

        auto bound = capabilities.bind("stress", context, {.publish_to_toolbus = false});
        assert(bound.ok());
        const auto response = bound.binding->invoke_capability("mock.echo", json::object());
        assert(response.at("content").at(0).at("text") == "echo");
        bound.binding->close();

        {
            const auto acquired = cache.acquire_policy(cache_handle);
            assert(acquired.ok && acquired.lease);
        }
    }

    assert(committed_side_effects.size() == 1000);
    assert(mock->created == 1000 && mock->calls == 1000 && mock->disconnected == 1000);
    assert(cache.inspect().leased_objects == 0);
    assert(descriptor_count("/proc/self/fd") <= fd_before + 2);
    assert(children() == children_before);
    assert(jail_count() == jails_before);
    fs::remove_all(root, ec);
    std::cout << json{{"cycles", 1000}, {"sideEffects", committed_side_effects.size()},
                     {"mcpSessions", mock->created}, {"leasedObjects", 0}}.dump() << '\n';
}

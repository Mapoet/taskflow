#include <agent/skill_capability_runtime.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace agent_framework;
using json = nlohmann::json;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << content;
}

struct MockState {
    std::mutex mutex;
    int created = 0;
    int disconnected = 0;
    int calls = 0;
    bool secret_checked = false;
};

class MockTransport final : public MCPTransportInterface {
public:
    explicit MockTransport(std::shared_ptr<MockState> state) : state_(std::move(state)) {}

    bool connect(const std::string&) override { connected_ = true; return true; }
    void disconnect() override {
        if (!connected_) return;
        connected_ = false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->disconnected;
    }
    bool is_connected() const override { return connected_; }
    MCPTransport get_transport_type() const override { return MCPTransport::HTTP; }
    void send_notification(const json&) override {}

    json transceive(const json& request) override {
        return transceive_cancellable(request, {});
    }

    json transceive_cancellable(
        const json& request, const std::function<bool()>& cancellation_requested) override {
        const auto id = request.at("id");
        const std::string method = request.at("method");
        if (method == "initialize") {
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"protocolVersion", "2024-11-05"},
                                {"capabilities", json::object()}}}};
        }
        if (method == "tools/list") {
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"tools", json::array({
                        {{"name", "echo"}, {"description", "echo"},
                         {"inputSchema", {{"type", "object"}}}},
                        {{"name", "hidden"}, {"description", "hidden"},
                         {"inputSchema", {{"type", "object"}}}},
                        {{"name", "slow"}, {"description", "slow"},
                         {"inputSchema", {{"type", "object"}}}}
                    })}}}};
        }
        if (method == "tools/call") {
            const std::string tool = request.at("params").at("name");
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                ++state_->calls;
            }
            if (tool == "slow") {
                for (int i = 0; i < 100; ++i) {
                    if (cancellation_requested && cancellation_requested())
                        throw std::runtime_error("mock MCP call cancelled");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"content", json::array({
                        {{"type", "text"}, {"text", tool}}
                    })}, {"isError", false}}}};
        }
        throw std::runtime_error("unexpected MCP method");
    }

private:
    std::shared_ptr<MockState> state_;
    bool connected_ = false;
};

SkillInvocationContext make_context(const fs::path& package,
                                    std::vector<SkillEvent>& events) {
    SkillInvocationContext context;
    context.control = std::make_shared<TaskControl>();
    context.grants.tools = {
        "base_echo", "base_private",
        "skill::stage3-skill::public",
        "skill::stage3-skill::private",
        "skill::stage3-skill::data.echo",
        "skill::stage3-skill::lazy.echo",
        "skill::stage3-skill::lazy.slow"
    };
    context.grants.filesystem_read = {package.string()};
    context.grants.secrets = {"mcp-token"};
    context.secret_provider = [](std::string_view reference) -> std::optional<std::string> {
        return reference == "mcp-token" ? std::optional<std::string>("secret-value")
                                         : std::nullopt;
    };
    context.event_sink = [&](const SkillEvent& event) { events.push_back(event); };
    context.task_id = "stage3-task";
    context.run_id = "stage3-run";
    return context;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_stage3_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path package = base / "stage3-skill";

    write_file(package / "tools/public.json", R"({"source":"base_echo","export":true})");
    write_file(package / "tools/private.json", R"({"source":"base_private","export":false})");
    write_file(package / "mcp/data.json", R"({
      "server":"data-server","transport":"mock","startup":"eager",
      "tool-filters":["*","!hidden"],
      "secret-references":{"AUTH":"mcp-token"},
      "tools":[{"name":"echo","export":true},{"name":"hidden","export":true}]
    })");
    write_file(package / "mcp/lazy.json", R"({
      "server":"lazy-server","transport":"mock","startup":"lazy",
      "tools":[{"name":"echo","export":false},{"name":"slow","export":false}]
    })");
    write_file(package / "prompts/report.json", R"({
      "template":"Hello {{name}}: {{topic}} [{{task_id}}]",
      "max-bytes":128,
      "variables":{
        "name":{"source":"input","path":"/name","schema":{"type":"string"}},
        "topic":{"source":"context","path":"/topic"},
        "task_id":{"source":"task","path":"/id"}
      }
    })");
    write_file(package / "prompts/denied.json", R"({
      "template":"{{token}}",
      "variables":{"token":{"source":"secret","path":"/token"}}
    })");
    write_file(package / "templates/tiny.json", R"({
      "template":"123456{{value}}","max-bytes":5,
      "variables":{"value":{"source":"input","path":"/value"}}
    })");
    write_file(package / "schemas/tool-input.json", R"({
      "type":"object","properties":{"x":{"type":"integer"}},"additionalProperties":false
    })");
    write_file(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: stage3-skill
version: 1.0.0
description: Stage 3 capability fixture
permissions:
  tools: [base_echo, base_private, skill::stage3-skill::public, skill::stage3-skill::private, skill::stage3-skill::data.echo, skill::stage3-skill::lazy.echo, skill::stage3-skill::lazy.slow]
  filesystem:
    read: [.]
  secrets: [mcp-token]
resources:
  tools:
    - id: public
      path: tools/public.json
      input-schema: tool-input
    - id: private
      path: tools/private.json
  mcp:
    - id: data
      path: mcp/data.json
    - id: lazy
      path: mcp/lazy.json
  prompts:
    - id: report
      path: prompts/report.json
    - id: denied
      path: prompts/denied.json
  templates:
    - id: tiny
      path: templates/tiny.json
  schemas:
    - id: tool-input
      path: schemas/tool-input.json
---
body
)");
    const fs::path network_package = base / "network-denied";
    write_file(network_package / "mcp/server.json", R"({
      "server":"remote","transport":"http","url":"https://mcp.example.org/rpc",
      "startup":"eager"
    })");
    write_file(network_package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: network-denied
version: 1.0.0
description: Network permission fixture
permissions:
  network: [https://mcp.example.org]
resources:
  mcp:
    - id: remote
      path: mcp/server.json
---
body
)");
    const fs::path malformed_package = base / "malformed-prompt";
    write_file(malformed_package / "prompts/bad.json",
               R"({"template":"x","max-bytes":"invalid"})");
    write_file(malformed_package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: malformed-prompt
version: 1.0.0
description: Malformed prompt fixture
permissions:
  filesystem:
    read: [.]
resources:
  prompts:
    - id: bad
      path: prompts/bad.json
---
body
)");

    auto registry = std::make_shared<SkillRegistry>(base);
    registry->scan_or_reload();
    assert(registry->valid());
    auto loader = std::make_shared<SkillLoader>(*registry);
    auto skill_runtime = std::make_shared<SkillRuntime>(registry, loader);
    auto bus = std::make_shared<ToolBus>();
    ToolMeta base_meta;
    base_meta.schema = {
        {"type", "object"},
        {"properties", {{"x", {{"type", "integer"}}}}}
    };
    base_meta.description = "base";
    bus->register_local_tool("base_echo", [](const json& input) {
        return json{{"source", "public"}, {"input", input}};
    }, base_meta);
    bus->register_local_tool("base_private", [](const json& input) {
        return json{{"source", "private"}, {"input", input}};
    }, base_meta);

    auto state = std::make_shared<MockState>();
    SkillMcpClientFactory factory = [state](const SkillMcpDescriptor& descriptor,
                                             const SkillInvocationContext& context) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            ++state->created;
            if (!descriptor.secret_references.empty()) {
                assert(descriptor.secret_references.at("AUTH") == "mcp-token");
                assert(context.secret_provider("mcp-token") == "secret-value");
                state->secret_checked = true;
            }
        }
        return MCPClient::create_with_transport(std::make_unique<MockTransport>(state), true);
    };
    SkillCapabilityRuntime capabilities(registry, loader, skill_runtime, bus, factory);
    std::vector<SkillEvent> denied_events;
    auto network_denied = capabilities.bind(
        "network-denied", make_context(network_package, denied_events));
    assert(!network_denied.ok());
    assert(network_denied.error.value("code", "") == kSkillPermissionDenied);
    assert(state->created == 0);
    auto malformed = capabilities.bind(
        "malformed-prompt", make_context(malformed_package, denied_events));
    assert(!malformed.ok());
    assert(malformed.error.value("code", "") == kSkillDescriptorInvalid);
    assert(state->created == 0);
    std::vector<SkillEvent> events;
    SkillInvocationContext context = make_context(package, events);
    auto bound = capabilities.bind("stage3-skill", context);
    assert(bound.ok());
    assert(state->created == 1);
    assert(state->secret_checked);
    assert(bound.binding->registered_tools().size() == 5U);
    assert(!contains(bound.binding->registered_tools(), "skill::stage3-skill::data.hidden"));

    const auto exported = bus->export_as_llm_tools();
    std::vector<std::string> exported_names;
    for (const auto& meta : exported) exported_names.push_back(meta.name);
    assert(contains(exported_names, "skill::stage3-skill::public"));
    assert(contains(exported_names, "skill::stage3-skill::data.echo"));
    assert(!contains(exported_names, "skill::stage3-skill::private"));
    assert(!contains(exported_names, "skill::stage3-skill::lazy.echo"));
    assert(!contains(exported_names, "skill::stage3-skill::lazy.slow"));

    json local = bus->call_tool("skill::stage3-skill::public", {{"x", 7}}).get();
    assert(local.value("source", "") == "public");
    json private_call = bus->call_tool("skill::stage3-skill::private", {{"x", 8}}).get();
    assert(private_call.value("source", "") == "private");
    json mcp = bus->call_tool("skill::stage3-skill::data.echo", json::object()).get();
    assert(mcp.at("content").at(0).at("text") == "echo");

    SkillPromptSources prompt_sources;
    prompt_sources.input = {{"name", "Mapoet"}};
    prompt_sources.context = {{"topic", "GNSS-R"}};
    prompt_sources.task = {{"id", "task-3"}};
    auto prompt = bound.binding->render_prompt("report", prompt_sources);
    assert(prompt.ok);
    assert(prompt.text == "Hello Mapoet: GNSS-R [task-3]");
    prompt_sources.input = {{"name", 7}};
    auto invalid = bound.binding->render_prompt("report", prompt_sources);
    assert(!invalid.ok && invalid.error.value("code", "") == kSkillInputInvalid);
    prompt_sources.input = json::object();
    auto missing = bound.binding->render_prompt("report", prompt_sources);
    assert(!missing.ok && missing.error.value("code", "") == kSkillPromptVariableMissing);
    auto denied = bound.binding->render_prompt("denied", {});
    assert(!denied.ok && denied.error.value("code", "") == kSkillPromptSourceDenied);
    SkillPromptSources tiny_sources;
    tiny_sources.input = {{"value", "x"}};
    auto tiny = bound.binding->render_prompt("tiny", tiny_sources);
    assert(!tiny.ok && tiny.error.value("code", "") == kSkillPromptSizeExceeded);

    // A conflicting bind is all-or-nothing and cannot remove the successful owner.
    std::vector<SkillEvent> conflict_events;
    auto conflict = capabilities.bind("stage3-skill", make_context(package, conflict_events));
    assert(!conflict.ok());
    assert(conflict.error.value("code", "") == kSkillCapabilityConflict);
    assert(bus->call_tool("skill::stage3-skill::public", json::object()).get().value("source", "") ==
           "public");

    // Disable/reload affects new bindings, while the task-pinned binding remains usable.
    fs::rename(package / "SKILL.md", package / "SKILL.disabled", ec);
    assert(!ec);
    registry->scan_or_reload();
    assert(!capabilities.bind("stage3-skill", make_context(package, conflict_events)).ok());
    assert(bus->call_tool("skill::stage3-skill::private", json::object()).get().value("source", "") ==
           "private");
    assert(bus->call_tool("skill::stage3-skill::public", {{"x", 9}}).get().value("source", "") ==
           "public");
    prompt_sources.input = {{"name", "Mapoet"}};
    assert(bound.binding->render_prompt("report", prompt_sources).ok);

    // Lazy MCP starts on first use; close cooperatively cancels the in-flight call.
    assert(state->created == 2); // failed eager conflict constructed and cleaned up one client
    json lazy_echo = bus->call_tool(
        "skill::stage3-skill::lazy.echo", json::object()).get();
    assert(lazy_echo.at("content").at(0).at("text") == "echo");
    assert(state->created == 3); // all tools in one MCP resource share the lazy session
    auto slow = bus->call_tool("skill::stage3-skill::lazy.slow", json::object());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bound.binding->close(std::chrono::milliseconds(1000));
    const json cancelled = slow.get();
    assert(cancelled.value("code", "") == kSkillCancelled);
    assert(state->created == 3);
    assert(state->disconnected == 3);
    assert(bus->call_tool("skill::stage3-skill::public", json::object()).get().value("code", "") ==
           "unknown_tool");
    assert(std::any_of(events.begin(), events.end(), [](const SkillEvent& event) {
        return event.type == SkillEventType::Cancelled;
    }));

    fs::remove_all(base, ec);
    std::cout << "test_skill_capability_runtime: ok\n";
    return 0;
}

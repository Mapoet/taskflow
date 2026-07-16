#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <taskflow/taskflow.hpp>
#include <workflow/nodeflow.hpp>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;
using json = nlohmann::json;

class PolicyProbeAdapter final : public ModelAdapter {
public:
    std::future<LLMOutput> invoke(
        const LLMInput& input,
        std::function<void(std::string_view)> /*stream_callback*/) override {
        (void)input;
        LLMOutput output;
        if (calls++ == 0) {
            output.is_final = false;
            CallSpec call;
            call.name = "denied_tool";
            call.arguments = json::object();
            CallSpec network_call;
            network_call.name = "allowed_tool";
            network_call.arguments = {{"url", "https://evil.example.org/path"}};
            output.tool_calls = {std::move(call), std::move(network_call)};
        } else {
            output.is_final = true;
            output.final_answer = "policy complete";
        }
        return std::async(std::launch::deferred,
                          [output = std::move(output)]() mutable { return std::move(output); });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt&,
        std::function<void(std::string_view)> callback) override {
        return invoke(LLMInput{}, std::move(callback));
    }
    std::vector<ToolMeta> get_available_tools() const override { return {}; }
    void configure(const ModelConfig&) override {}
    std::string get_model_name() const override { return "skill-policy-probe"; }
    bool supports_multimodal() const override { return false; }

    int calls = 0;
};

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    assert(output.good());
    output << content;
}

ToolMeta tool_meta(const std::string& name) {
    ToolMeta meta;
    meta.name = name;
    meta.description = name;
    meta.schema = {{"type", "object"}, {"properties", json::object()}};
    return meta;
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_loop_policy_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    write_file(base / "policy-skill/SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: policy-skill
version: 1.0.0
description: Agent loop policy fixture
trigger-keywords: [activate-policy]
permissions:
  tools: [allowed_tool]
  network: [https://api.example.org]
---
Use the allowed tool only.
)");
    auto services = std::make_shared<SkillServices>();
    services->registry = std::make_shared<SkillRegistry>(base);
    services->registry->scan_or_reload();
    assert(services->registry->valid());
    services->loader = std::make_shared<SkillLoader>(*services->registry);

    auto adapter = std::make_shared<PolicyProbeAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("probe", adapter);
    llm->set_default_adapter("probe");

    int allowed_calls = 0;
    int denied_calls = 0;
    auto bus = std::make_shared<ToolBus>();
    ToolMeta allowed_meta = tool_meta("allowed_tool");
    allowed_meta.schema = {{"type", "object"},
                           {"properties", {{"url", {{"type", "string"}}}}},
                           {"required", {"url"}}};
    allowed_meta.permission_targets.push_back(
        {ToolMeta::PermissionTargetKind::Network, "url", {}, {}});
    bus->register_local_tool("allowed_tool", [&](const json&) {
        ++allowed_calls;
        return json{{"ok", true}};
    }, allowed_meta);
    bus->register_local_tool("denied_tool", [&](const json&) {
        ++denied_calls;
        return json{{"bypassed", true}};
    }, tool_meta("denied_tool"));

    AgentWorkflowDeps deps{llm, bus, services};
    AgentConfig config;
    config.name = "skill_policy_loop";
    config.system_prompt = "test";
    config.max_iterations = 4;
    auto state = std::make_shared<internal::AgentThreadState>();
    state->initial_user_prompt = "activate-policy";
    state->execution_context.emplace();
    state->execution_context->skill_grants.tools = {"allowed_tool", "denied_tool"};
    state->execution_context->skill_grants.network = {"https://api.example.org"};

    std::vector<SkillEvent> events;
    CliAgentGraphOptions options;
    options.skill_event_sink = [&](const SkillEvent& event) { events.push_back(event); };
    json final;
    std::shared_ptr<internal::AgentThreadState> final_state;
    CliAgentTerminalSinkOptions sink;
    sink.on_final_json = [&](const json& value) { final = value; };
    sink.on_final_state = [&](const std::shared_ptr<internal::AgentThreadState>& value) {
        final_state = value;
    };

    workflow::GraphBuilder builder("skill_policy_loop");
    build_cli_agent_graph_with_terminal_sink(builder, config, deps, state, sink, "AgentLoop",
                                             options);
    tf::Executor executor;
    builder.run_async(executor).wait();

    assert(final.value("final_answer", "") == "policy complete");
    assert(final_state && final_state->active_skill_id == "policy-skill");
    assert(allowed_calls == 0);
    assert(denied_calls == 0);
    assert(std::any_of(events.begin(), events.end(), [](const SkillEvent& event) {
        return event.type == SkillEventType::PermissionDenied &&
               event.code == kSkillPermissionDenied &&
               event.details.value("target", "") == "denied_tool";
    }));
    assert(std::any_of(events.begin(), events.end(), [](const SkillEvent& event) {
        return event.type == SkillEventType::PermissionDenied &&
               event.details.value("permission", "") == "network" &&
               event.details.value("target", "") == "https://evil.example.org";
    }));

    fs::remove_all(base, ec);
    return 0;
}

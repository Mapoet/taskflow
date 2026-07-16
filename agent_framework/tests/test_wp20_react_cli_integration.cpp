/**
 * @file test_wp20_react_cli_integration.cpp
 * @brief WP2.0 GraphExecutor::run_react_cli_sync 双轮集成（Fake LLM，无网络）
 */

#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/core/types.hpp>

#include <cassert>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <taskflow/taskflow.hpp>

namespace {

using namespace agent_framework;

static const char* kFirstUser = "first_round_user";
static const char* kSecondUser = "second_round_user";

class TwoTurnHistoryAdapter final : public ModelAdapter {
public:
    static bool messages_contain_user(const RenderedPrompt& rp, const char* user_content) {
        for (const auto& m : rp.messages) {
            if (!m.is_object()) {
                continue;
            }
            if (m.value("role", "") != "user") {
                continue;
            }
            const std::string c = m.value("content", "");
            if (c == user_content) {
                return true;
            }
        }
        return false;
    }

    std::future<LLMOutput> invoke(
        const LLMInput& /*input*/,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return std::async(std::launch::async, []() {
            LLMOutput out;
            out.is_final = true;
            out.final_answer = "invoke_should_not_be_used_in_this_test";
            return out;
        });
    }

    std::future<LLMOutput> invoke_with_rendered(
        const RenderedPrompt& rendered,
        std::function<void(std::string_view)> /*stream_callback*/ = nullptr) override {
        return std::async(std::launch::async, [this, rendered]() {
            std::lock_guard<std::mutex> lock(mu_);
            ++call_count_;
            LLMOutput out;
            out.is_final = true;
            out.tool_calls.clear();
            if (call_count_ == 1) {
                last_message_counts_.push_back(rendered.messages.size());
                out.final_answer = "answer_one";
            } else {
                last_message_counts_.push_back(rendered.messages.size());
                if (!messages_contain_user(rendered, kFirstUser)) {
                    out.final_answer = "error_no_first_user_in_history";
                } else {
                    out.final_answer = "answer_two";
                }
            }
            return out;
        });
    }

    std::vector<ToolMeta> get_available_tools() const override {
        return {};
    }

    void configure(const ModelConfig& /*config*/) override {}

    std::string get_model_name() const override {
        return "fake-model-two-turn-hist";
    }

    bool supports_multimodal() const override {
        return false;
    }

    int call_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return call_count_;
    }

    std::vector<std::size_t> last_message_counts() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_message_counts_;
    }

private:
    mutable std::mutex mu_;
    int call_count_ = 0;
    std::vector<std::size_t> last_message_counts_;
};

void test_i1_two_runs_history_carries() {
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);

    auto adapter = std::make_shared<TwoTurnHistoryAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", adapter);
    llm->set_default_adapter("fake");

    auto bus = std::make_shared<ToolBus>();
    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.system_prompt = "sys";
    cfg.max_iterations = 4;

    auto session = std::make_shared<internal::AgentThreadState>();
    tf::Executor executor;
    GraphExecutor gx;

    session->initial_user_prompt = kFirstUser;
    ReactCliRunRequest req1;
    req1.config = cfg;
    req1.deps = deps;
    req1.session = session;
    req1.options.sink.on_final_json = [](const nlohmann::json&) {};

    WorkflowResult r1 = gx.run_react_cli_sync(executor, req1);
    assert(r1.success);
    assert(r1.outputs.contains("history_size"));
    assert(r1.outputs.at("history_size").get<std::size_t>() >= 1U);
    assert(session->history.size() >= 2U);

    session->initial_user_prompt = kSecondUser;
    ReactCliRunRequest req2;
    req2.config = cfg;
    req2.deps = deps;
    req2.session = session;
    req2.options.sink.on_final_json = [](const nlohmann::json&) {};

    WorkflowResult r2 = gx.run_react_cli_sync(executor, req2);
    assert(r2.success);
    assert(r2.outputs.at("final_answer").get<std::string>() == "answer_two");
    assert(adapter->call_count() == 2);
    assert(adapter->last_message_counts().size() == 2U);
    assert(adapter->last_message_counts()[1] > adapter->last_message_counts()[0]);
}

void test_i2_single_run_success() {
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);

    auto adapter = std::make_shared<TwoTurnHistoryAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", adapter);
    llm->set_default_adapter("fake");

    auto bus = std::make_shared<ToolBus>();
    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.system_prompt = "sys";
    cfg.max_iterations = 4;

    auto session = std::make_shared<internal::AgentThreadState>();
    session->initial_user_prompt = kFirstUser;

    tf::Executor executor;
    GraphExecutor gx;
    ReactCliRunRequest req;
    req.config = cfg;
    req.deps = deps;
    req.session = session;
    req.options.sink.on_final_json = [](const nlohmann::json&) {};

    WorkflowResult r = gx.run_react_cli_sync(executor, req);
    assert(r.success);
    assert(r.outputs.contains("history_size"));
    assert(r.outputs.at("history_size").get<std::size_t>() >= 1U);
}

void test_i3_unified_execute_persists_two_turns() {
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
    auto adapter = std::make_shared<TwoTurnHistoryAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", adapter);
    llm->set_default_adapter("fake");
    auto bus = std::make_shared<ToolBus>();
    auto store = std::make_shared<InMemorySessionStore>();
    tf::Executor executor;
    GraphExecutor gx;

    AgentConfig cfg;
    cfg.system_prompt = "sys";
    cfg.max_iterations = 4;
    AgentWorkflowDeps deps{llm, bus, nullptr};

    auto run = [&](const char* prompt) {
        ExecutionRequest req;
        req.config = cfg;
        req.deps = deps;
        req.session = std::make_shared<internal::AgentThreadState>();
        req.session->initial_user_prompt = prompt;
        req.context.session_id = "persistent-session";
        req.session_store = store;
        req.options.react.sink.on_final_json = [](const json&) {};
        return gx.execute_sync(executor, std::move(req));
    };

    ExecutionResult first = run(kFirstUser);
    assert(first.success && first.committed_revision == 1);
    ExecutionResult second = run(kSecondUser);
    assert(second.success && second.committed_revision == 2);
    assert(second.outputs.at("final_answer") == "answer_two");
    assert(store->load_or_create("persistent-session").state.history.size() >= 4U);
}

void test_i4_cancel_does_not_commit_or_mutate_session() {
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    auto adapter = std::make_shared<TwoTurnHistoryAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", adapter);
    llm->set_default_adapter("fake");
    auto store = std::make_shared<InMemorySessionStore>();
    auto session = std::make_shared<internal::AgentThreadState>();
    session->initial_user_prompt = "cancelled";
    auto control = std::make_shared<TaskControl>();
    control->request_cancel();

    ExecutionRequest req;
    req.config.system_prompt = "sys";
    req.deps = {llm, std::make_shared<ToolBus>(), nullptr};
    req.session = session;
    req.context.session_id = "cancel-session";
    req.control = control;
    req.session_store = store;
    req.options.react.sink.on_final_json = [](const json&) {};
    tf::Executor executor;
    GraphExecutor gx;
    ExecutionResult result = gx.execute_sync(executor, std::move(req));
    assert(!result.success);
    assert(result.status == ExecutionTerminalStatus::Cancelled);
    assert(store->load_or_create("cancel-session").revision == 0);
    assert(session->history.empty());
    assert(session->initial_user_prompt == "cancelled");
}

void test_i5_template_registry_drives_unified_execution() {
    (void)::setenv("AGENT_VERIFIER", "off", 1);
    auto adapter = std::make_shared<TwoTurnHistoryAdapter>();
    auto llm = std::make_shared<LLMClient>();
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());
    llm->register_adapter("fake", adapter);
    llm->set_default_adapter("fake");

    GraphExecutor gx;
    assert(gx.get_template(kWorkflowTemplateReactCli) != nullptr);
    gx.register_template("react_alias", std::make_shared<ReActTemplate>());

    ExecutionRequest req;
    req.template_id = "react_alias";
    req.config.system_prompt = "sys";
    req.config.max_iterations = 4;
    req.deps = {llm, std::make_shared<ToolBus>(), nullptr};
    req.session = std::make_shared<internal::AgentThreadState>();
    req.session->initial_user_prompt = kFirstUser;
    req.context.session_id = "template-alias-session";
    req.options.persist_session = false;
    req.options.react.sink.on_final_json = [](const json&) {};

    tf::Executor executor;
    ExecutionResult result = gx.execute_sync(executor, std::move(req));
    assert(result.success);
    assert(result.outputs.at("final_answer") == "answer_one");
}

void test_i6_unknown_template_fails_before_execution() {
    GraphExecutor gx;
    ExecutionRequest req;
    req.template_id = "missing_template";
    tf::Executor executor;
    ExecutionResult result = gx.execute_sync(executor, std::move(req));
    assert(!result.success);
    assert(result.error == "unknown workflow template: missing_template");
}

} // namespace

int main() {
    test_i1_two_runs_history_carries();
    test_i2_single_run_success();
    test_i3_unified_execute_persists_two_turns();
    test_i4_cancel_does_not_commit_or_mutate_session();
    test_i5_template_registry_drives_unified_execution();
    test_i6_unknown_template_fails_before_execution();
    return 0;
}

/**
 * @file test_wp20_react_cli_integration.cpp
 * @brief WP2.0 GraphExecutor::run_react_cli_sync 双轮集成（Fake LLM，无网络）
 */

#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>

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
        return "two-turn-hist-fake";
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

} // namespace

int main() {
    test_i1_two_runs_history_carries();
    test_i2_single_run_success();
    return 0;
}

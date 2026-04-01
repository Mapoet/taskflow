/**
 * @file agent_loop_node.cpp
 * @brief Agent 循环节点封装实现
 */

#include "node/agent_loop_node.hpp"
#include "node/llm_node.hpp"
#include "node/tool_call_node.hpp"
#include "node/knowledge_base_node.hpp"
#include "agent/internal/agent_thread_state.hpp"
#include "agent/internal/loop_io_keys.hpp"
#include <any>
#include <unordered_map>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>

namespace agent_framework {
namespace node {

std::pair<std::shared_ptr<workflow::LoopNode>, tf::Task>
AgentLoopNode::create(
    workflow::GraphBuilder& builder,
    const std::string& name,
    const AgentConfig& agent_config,
    std::shared_ptr<LLMClient> llm_client,
    std::shared_ptr<ToolBus> toolbus,
    std::shared_ptr<MemoryStore> memory_store,
    std::shared_ptr<VectorStore> vector_store,
    const std::vector<std::pair<std::string, std::string>>& input_specs,
    const std::vector<std::string>& output_keys
) {
    // NOTE: workflow::create_loop_decl currently does NOT pass body outputs into condition_func.
    // Therefore WP1.5 loop uses closure state:
    // - body_func updates shared state
    // - condition_func reads shared state (ignores its argument)
    // - exit_func emits final outputs once (avoid promise re-set)

    struct Shared {
        std::shared_ptr<internal::AgentThreadState> state;
        LLMOutput last_llm;
        std::string final_answer;
        bool is_final = false;
    };
    auto shared = std::make_shared<Shared>();

    auto body_func = [agent_config, llm_client, toolbus, shared](
                         const std::unordered_map<std::string, std::any>& inps)
        -> std::unordered_map<std::string, std::any> {
        const char* dbg_env = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
        const bool dbg = dbg_env && std::string(dbg_env) != "0";

        auto st = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
            inps.at(std::string(internal::kAgentState)));
        if (!shared->state) {
            shared->state = st ? std::make_shared<internal::AgentThreadState>(*st)
                               : std::make_shared<internal::AgentThreadState>();
        }
        const int it = shared->state ? shared->state->iteration : 0;
        if (dbg) {
            std::cout << "[AgentLoop] iter=" << it << " history_size="
                      << (shared->state ? shared->state->history.size() : 0) << "\n";
            std::cout.flush();
        }

        LLMInput llm_in;
        llm_in.system_prompt =
            std::any_cast<std::string>(inps.at(std::string(internal::kSystemPrompt)));
        llm_in.user_prompt =
            std::any_cast<std::string>(inps.at(std::string(internal::kUserQuery)));
        llm_in.history = shared->state->history;
        if (toolbus) {
            llm_in.tools = toolbus->export_as_llm_tools();
        }

        // invoke (LLMClient will render using its configured PromptRenderer)
        if (dbg) {
            std::cout << "[AgentLoop] calling LLM...\n";
            std::cout.flush();
        }
        const auto t0 = std::chrono::steady_clock::now();
        LLMOutput llm_out = llm_client->invoke(llm_in, "", nullptr).get();
        const auto t1 = std::chrono::steady_clock::now();
        if (dbg) {
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "[AgentLoop] LLM done in " << ms << "ms"
                      << " tool_calls=" << llm_out.tool_calls.size()
                      << " is_final=" << (llm_out.is_final ? "true" : "false")
                      << " final_answer_len=" << llm_out.final_answer.size() << "\n";
            std::cout.flush();
        }
        shared->last_llm = llm_out;

        // assistant message
        Message a;
        a.role = "assistant";
        a.timestamp = std::time(nullptr);
        if (!llm_out.tool_calls.empty()) {
            json j;
            j["tool_calls"] = json::array();
            for (const auto& c : llm_out.tool_calls) {
                json one;
                if (c.tool_call_id && !c.tool_call_id->empty()) {
                    one["id"] = *c.tool_call_id;
                }
                one["type"] = "function";
                one["function"] = json{{"name", c.name}, {"arguments", c.arguments.dump()}};
                j["tool_calls"].push_back(std::move(one));
            }
            a.content = j.dump();
        } else if (!llm_out.final_answer.empty()) {
            a.content = llm_out.final_answer;
        } else {
            a.content = llm_out.reasoning;
        }
        shared->state->history.push_back(std::move(a));

        // tools (sequential)
        std::vector<CallSpec> calls = llm_out.tool_calls;
        if (static_cast<int>(calls.size()) > agent_config.max_tool_calls_per_iteration) {
            calls.resize(static_cast<std::size_t>(agent_config.max_tool_calls_per_iteration));
        }
        for (const auto& c : calls) {
            if (dbg) {
                std::cout << "[AgentLoop] calling tool " << c.name << " args=" << c.arguments.dump()
                          << "\n";
                std::cout.flush();
            }
            json result = toolbus->call_tool(c.name, c.arguments).get();
            Message tm;
            tm.role = "tool";
            tm.tool_call_id = c.tool_call_id;
            tm.tool_name = c.name;
            tm.tool_result = result;
            tm.timestamp = std::time(nullptr);
            shared->state->history.push_back(std::move(tm));
            if (dbg) {
                std::cout << "[AgentLoop] tool " << c.name << " result=" << result.dump() << "\n";
                std::cout.flush();
            }
        }

        shared->state->iteration += 1;
        shared->is_final = llm_out.tool_calls.empty() && (llm_out.is_final || !llm_out.final_answer.empty());
        shared->final_answer = llm_out.final_answer;

        return {};
    };

    auto condition_func = [agent_config, shared](const std::unordered_map<std::string, std::any>&) -> int {
        const int it = shared->state ? shared->state->iteration : 0;
        if (it >= agent_config.max_iterations) {
            return 1;
        }
        if (shared->is_final) {
            return 1;
        }
        return 0;
    };

    auto exit_func = [shared](const std::unordered_map<std::string, std::any>&) -> std::unordered_map<std::string, std::any> {
        return {
            {std::string(internal::kFinalAnswer), std::any{shared->final_answer}},
            {std::string(internal::kNextAgentState), std::any{shared->state}},
            {std::string(internal::kLlmOutput), std::any{shared->last_llm}},
            {std::string(internal::kIsFinal), std::any{shared->is_final}}
        };
    };

    return builder.create_loop_decl(name, input_specs, body_func, condition_func, exit_func, output_keys);
}

void AgentLoopNode::build_loop_body(
    workflow::GraphBuilder& builder,
    const AgentConfig& agent_config,
    std::shared_ptr<LLMClient> llm_client,
    std::shared_ptr<ToolBus> toolbus,
    std::shared_ptr<MemoryStore> memory_store,
    std::shared_ptr<VectorStore> vector_store,
    const std::unordered_map<std::string, std::any>& inputs
) {
    // expose loop inputs as a source node
    auto [loop_in, loop_task] = builder.create_any_source("LoopInput", inputs);
    (void)loop_task;

    // LLM node: render + invoke_with_rendered_prompt
    const std::string model_name = agent_config.model_config.model_name;
    const std::string provider = "";  // default provider
    auto [llm_node, llm_task] = LLMNode::create(
        builder,
        "LLM",
        llm_client,
        std::make_shared<PromptRenderer>(),
        model_name,
        provider,
        {{"LoopInput", std::string(internal::kSystemPrompt)},
         {"LoopInput", std::string(internal::kUserQuery)},
         {"LoopInput", std::string(internal::kAgentState)}},
        nullptr);
    (void)llm_task;

    // ToolAggregator: sequential tool execution
    auto tool_agg = [toolbus, agent_config](
                        const std::unordered_map<std::string, std::any>& inps)
        -> std::unordered_map<std::string, std::any> {
        const LLMOutput llm_out = std::any_cast<LLMOutput>(inps.at(std::string(internal::kLlmOutput)));
        std::vector<Message> tool_msgs;
        bool had_error = false;

        std::vector<CallSpec> calls = llm_out.tool_calls;
        if (static_cast<int>(calls.size()) > agent_config.max_tool_calls_per_iteration) {
            calls.resize(static_cast<std::size_t>(agent_config.max_tool_calls_per_iteration));
        }

        for (const auto& c : calls) {
            json result = toolbus->call_tool(c.name, c.arguments).get();
            Message m;
            m.role = "tool";
            m.content = "";
            m.tool_name = c.name;
            m.tool_result = result;
            m.timestamp = std::time(nullptr);
            tool_msgs.push_back(std::move(m));

            if (result.is_object() && result.contains("code") && result["code"].is_string()) {
                had_error = true;
            }
        }

        return {
            {std::string(internal::kToolMessages), std::any{tool_msgs}},
            {std::string(internal::kToolHadError), std::any{had_error}}
        };
    };

    auto [tool_node, tool_task] = builder.create_any_node(
        "ToolAggregator",
        {{"LLM", std::string(internal::kLlmOutput)}},
        tool_agg,
        {std::string(internal::kToolMessages), std::string(internal::kToolHadError)});
    (void)tool_task;

    // StateMerge: update history + iteration
    auto state_merge = [](
                          const std::unordered_map<std::string, std::any>& inps)
        -> std::unordered_map<std::string, std::any> {
        auto st = std::any_cast<std::shared_ptr<internal::AgentThreadState>>(
            inps.at(std::string(internal::kAgentState)));
        const LLMOutput llm_out = std::any_cast<LLMOutput>(inps.at(std::string(internal::kLlmOutput)));
        const std::vector<Message> tool_msgs =
            std::any_cast<std::vector<Message>>(inps.at(std::string(internal::kToolMessages)));

        auto next = std::make_shared<internal::AgentThreadState>();
        if (st) {
            *next = *st;
        }
        // append assistant message
        Message a;
        a.role = "assistant";
        a.timestamp = std::time(nullptr);
        if (!llm_out.tool_calls.empty()) {
            json j;
            j["tool_calls"] = json::array();
            for (const auto& c : llm_out.tool_calls) {
                json one;
                one["type"] = "function";
                one["function"] = json{{"name", c.name}, {"arguments", c.arguments.dump()}};
                if (c.tool_call_id) {
                    one["id"] = *c.tool_call_id;
                }
                j["tool_calls"].push_back(std::move(one));
            }
            a.content = j.dump();
        } else if (!llm_out.final_answer.empty()) {
            a.content = llm_out.final_answer;
        } else {
            a.content = llm_out.reasoning;
        }
        next->history.push_back(std::move(a));
        for (const auto& tm : tool_msgs) {
            next->history.push_back(tm);
        }
        next->iteration += 1;

        const bool is_final = llm_out.tool_calls.empty() && (llm_out.is_final || !llm_out.final_answer.empty());
        const std::string final_answer = llm_out.final_answer;

        return {
            {std::string(internal::kNextAgentState), std::any{next}},
            {std::string(internal::kIsFinal), std::any{is_final}},
            {std::string(internal::kFinalAnswer), std::any{final_answer}},
            {std::string(internal::kLlmOutput), std::any{llm_out}}
        };
    };

    builder.create_any_node(
        "StateMerge",
        {{"LoopInput", std::string(internal::kAgentState)},
         {"LLM", std::string(internal::kLlmOutput)},
         {"ToolAggregator", std::string(internal::kToolMessages)}},
        state_merge,
        {std::string(internal::kNextAgentState),
         std::string(internal::kIsFinal),
         std::string(internal::kFinalAnswer),
         std::string(internal::kLlmOutput)});
}

int AgentLoopNode::check_loop_condition(
    const std::unordered_map<std::string, std::any>& outputs,
    int iteration_count,
    int max_iterations
) {
    // 检查是否超过最大迭代次数
    if (iteration_count >= max_iterations) {
        return 1;  // 退出循环
    }
    
    // 检查是否已完成任务
    if (outputs.find("is_final") != outputs.end()) {
        bool is_final = std::any_cast<bool>(outputs.at("is_final"));
        if (is_final) {
            return 1;  // 退出循环
        }
    }
    
    return 0;  // 继续循环
    // 注意：workflow 的 create_loop_decl 的 condition_func 不直接支持 iteration_count
    // 实际实现中需要使用共享状态来跟踪迭代次数
}

void AgentLoopNode::build_exit_handler(
    workflow::GraphBuilder& builder,
    const std::unordered_map<std::string, std::any>& inputs
) {
    (void)builder;
    (void)inputs;
}

} // namespace node
} // namespace agent_framework


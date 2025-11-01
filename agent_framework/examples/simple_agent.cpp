/**
 * @file simple_agent.cpp
 * @brief 简单 Agent 示例
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#include <agent/types.hpp>
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>
#include <unordered_map>
#include <any>

namespace wf = workflow;

int main() {
    // 创建执行器和图构建器
    tf::Executor executor(std::thread::hardware_concurrency());
    wf::GraphBuilder builder("simple_agent");

    // 创建系统提示词源节点
    auto [sys_node, _] = builder.create_typed_source(
        "SystemPrompt",
        std::make_tuple(std::string("你是一个有用的助手。")),
        {"prompt"}
    );

    // 创建用户输入源节点
    auto [user_node, _] = builder.create_any_source(
        "UserInput",
        std::unordered_map<std::string, std::any>{
            {"query", std::any{std::string("你好")}}
        }
    );

    // 创建 LLM 节点（简化示例，实际需要实现 LLM 客户端）
    auto [llm_node, _] = builder.create_any_node(
        "LLM",
        {{"SystemPrompt", "prompt"}, {"UserInput", "query"}},
        [](const std::unordered_map<std::string, std::any>& inputs) {
            std::string prompt = std::any_cast<std::string>(inputs.at("prompt"));
            std::string query = std::any_cast<std::string>(inputs.at("query"));
            
            // 简化的 LLM 输出（实际应调用 LLM API）
            std::string answer = prompt + " " + query;
            
            return std::unordered_map<std::string, std::any>{
                {"final_answer", std::any{answer}}
            };
        },
        {"final_answer"}
    );

    // 创建输出 Sink
    builder.create_any_sink(
        "Output",
        {{"LLM", "final_answer"}},
        [](const std::unordered_map<std::string, std::any>& outputs) {
            std::string answer = std::any_cast<std::string>(outputs.at("final_answer"));
            std::cout << "Answer: " << answer << std::endl;
        }
    );

    // 执行工作流
    std::cout << "Starting simple agent workflow..." << std::endl;
    builder.run(executor);
    
    return 0;
}


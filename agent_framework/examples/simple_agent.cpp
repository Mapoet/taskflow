/**
 * @file simple_agent.cpp
 * @brief 简单 Agent 示例
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#include <agent/core/types.hpp>
#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>
#include <unordered_map>
#include <any>

namespace wf = workflow;

int main() {
    // 创建执行器和图构建器
    // 使用默认线程数（Taskflow 会自动检测）
    tf::Executor executor;
    wf::GraphBuilder builder("simple_agent");

    // 创建系统提示词源节点（统一使用 any_source 以避免类型混合问题）
    auto [sys_node, sys_task] = builder.create_any_source(
        "SystemPrompt",
        std::unordered_map<std::string, std::any>{
            {"prompt", std::any{std::string("你是一个有用的助手。")}}
        }
    );
    (void)sys_task;  // 未使用的任务句柄

    // 创建用户输入源节点
    auto [user_node, user_task] = builder.create_any_source(
        "UserInput",
        std::unordered_map<std::string, std::any>{
            {"query", std::any{std::string("你好")}}
        }
    );
    (void)user_task;  // 未使用的任务句柄

    // 创建 LLM 节点（简化示例，实际需要实现 LLM 客户端）
    auto [llm_node, llm_task] = builder.create_any_node(
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
    (void)llm_task;  // 未使用的任务句柄

    // 创建输出 Sink
    auto [sink_node, sink_task] = builder.create_any_sink(
        "Output",
        {{"LLM", "final_answer"}},
        [](const std::unordered_map<std::string, std::any>& outputs) {
            try {
                std::string answer = std::any_cast<std::string>(outputs.at("final_answer"));
                std::cout << "Answer: " << answer << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error in sink callback: " << e.what() << std::endl;
            }
        }
    );
    (void)sink_task;  // 未使用的任务句柄

    // 执行工作流
    std::cout << "Starting simple agent workflow..." << std::endl;
    
    // 调试：输出图信息
    std::cout << "Dumping workflow graph:" << std::endl;
    builder.dump(std::cout);
    std::cout << std::endl;
    
    // 检查任务流是否为空（暂时注释掉，GraphBuilder 可能没有 name() 方法）
    std::cout << "Checking taskflow status..." << std::endl;
    
    try {
        std::cout << "About to run executor..." << std::endl;
        auto future = builder.run_async(executor);
        std::cout << "Executor started, waiting for completion..." << std::endl;
        future.wait();
        std::cout << "Workflow completed successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred!" << std::endl;
        return 1;
    }
    
    return 0;
}


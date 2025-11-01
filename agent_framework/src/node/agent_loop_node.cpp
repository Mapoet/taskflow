/**
 * @file agent_loop_node.cpp
 * @brief Agent 循环节点封装实现
 */

#include "node/agent_loop_node.hpp"
#include "node/llm_node.hpp"
#include "node/tool_call_node.hpp"
#include "node/knowledge_base_node.hpp"
#include <any>
#include <unordered_map>

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
    // 构建循环体函数
    auto body_builder_fn = [agent_config, llm_client, toolbus, memory_store, vector_store](
        workflow::GraphBuilder& body_builder,
        const std::unordered_map<std::string, std::any>& inputs
    ) {
        build_loop_body(
            body_builder,
            agent_config,
            llm_client,
            toolbus,
            memory_store,
            vector_store,
            inputs
        );
    };
    
    // 循环条件判断函数（注意：workflow 的 condition_func 只接收 inputs，不接收 iteration_count）
    // 我们需要通过其他方式跟踪迭代次数，或者从 outputs 中推断
    auto condition_func = [agent_config](
        const std::unordered_map<std::string, std::any>& outputs
    ) -> int {
        // 简化实现：从 outputs 中检查 is_final，或使用固定的最大迭代次数
        // 实际实现中可能需要使用共享状态来跟踪迭代次数
        if (outputs.find("is_final") != outputs.end()) {
            bool is_final = std::any_cast<bool>(outputs.at("is_final"));
            if (is_final) {
                return 1;  // 退出循环
            }
        }
        // TODO: 添加迭代次数跟踪
        return 0;  // 继续循环
    };
    
    // 退出处理函数
    auto exit_builder_fn = [](workflow::GraphBuilder& exit_builder,
                              const std::unordered_map<std::string, std::any>& inputs) {
        build_exit_handler(exit_builder, inputs);
    };
    
    // 创建循环节点
    return builder.create_loop_decl(
        name,
        input_specs,
        body_builder_fn,
        condition_func,
        exit_builder_fn,
        output_keys
    );
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
    // 1. 创建知识库查询节点（如果需要）
    std::string query = std::any_cast<std::string>(inputs.at("query"));
    
    std::shared_ptr<workflow::AnySource> kb_source;
    tf::Task kb_task;
    
    if (vector_store && agent_config.enable_knowledge_base) {
        // 这里需要 encoder_manager，但为了简化，假设已提供
        // 实际实现中应该从外部传入或使用全局管理器
        // auto [kb_node, kb_task] = KnowledgeBaseSourceNode::create(...);
    }
    
    // 2. 创建 LLM 节点
    std::vector<std::pair<std::string, std::string>> llm_input_specs = {
        {"SystemPrompt", "prompt"}
    };
    
    if (inputs.find("query") != inputs.end()) {
        llm_input_specs.push_back({"UserInput", "query"});
    }
    
    auto [llm_node, llm_task] = LLMNode::create(
        builder,
        "LLM",
        llm_client,
        llm_input_specs,
        nullptr  // 流式回调
    );
    
    // 3. 创建工具调用节点（如果 LLM 输出工具调用）
    auto [tool_node, tool_task] = ToolCallNode::create_parallel(
        builder,
        "ToolCall",
        toolbus,
        {{"LLM", "tool_calls"}}
    );
    
    // 4. 创建聚合节点（合并工具调用结果和 LLM 输出）
    auto aggregate_functor = [](
        const std::unordered_map<std::string, std::any>& inputs
    ) -> std::unordered_map<std::string, std::any> {
        // 聚合工具调用结果和 LLM 输出
        bool is_final = std::any_cast<bool>(inputs.at("is_final"));
        std::string final_answer = std::any_cast<std::string>(inputs.at("final_answer"));
        
        return {
            {"is_final", std::any{is_final}},
            {"final_answer", std::any{final_answer}},
            {"tool_results", inputs.at("results")}
        };
    };
    
    builder.create_any_node(
        "Aggregate",
        {
            {"LLM", "is_final"},
            {"LLM", "final_answer"},
            {"ToolCall", "results"}
        },
        aggregate_functor,
        {"is_final", "final_answer", "tool_results"}
    );
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
    // 退出处理：保存最终结果、清理资源等
    // 可以在这里添加记忆存储、日志记录等逻辑
    // TODO: 实现退出处理逻辑
}

} // namespace node
} // namespace agent_framework


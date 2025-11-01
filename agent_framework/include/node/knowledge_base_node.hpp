/**
 * @file knowledge_base_node.hpp
 * @brief 知识库节点封装：将向量检索封装为 workflow Source 节点
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_NODE_KNOWLEDGE_BASE_NODE_H__
#define __AGENT_NODE_KNOWLEDGE_BASE_NODE_H__

#include <workflow/nodeflow.hpp>
#include "../agent/types.hpp"
#include "../agent/vectorstore.hpp"
#include "../agent/encoder.hpp"
#include <string>
#include <memory>

namespace agent_framework {
namespace node {

/**
 * @brief 知识库 Source 节点封装类
 * 将向量检索封装为 workflow AnySource，提供查询接口
 */
class KnowledgeBaseSourceNode {
public:
    /**
     * @brief 创建知识库 Source 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param vector_store 向量存储
     * @param encoder_manager 编码器管理器
     * @return (节点指针, 任务句柄)
     * 
     * 节点提供以下输出键：
     *   - "context": 检索到的上下文摘要（字符串）
     *   - "results": 原始检索结果列表（std::vector<RetrievalResult>）
     *   - "citations": 引用信息列表（std::vector<Citation>）
     */
    static std::pair<std::shared_ptr<workflow::AnySource>, tf::Task>
    create(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<VectorStore> vector_store,
        std::shared_ptr<EncoderManager> encoder_manager
    );
    
    /**
     * @brief 设置查询参数（在节点创建后调用）
     * @param node 节点指针
     * @param query_text 查询文本
     * @param top_k 返回前 k 个结果
     * @param modality 模态类型（可选）
     */
    static void set_query(
        std::shared_ptr<workflow::AnySource> node,
        const std::string& query_text,
        int top_k = 5,
        const std::string& modality = ""
    );

private:
    /**
     * @brief 执行向量检索
     * @param query_text 查询文本
     * @param vector_store 向量存储
     * @param encoder_manager 编码器管理器
     * @param top_k 返回前 k 个结果
     * @param modality 模态类型
     * @return 检索结果
     */
    static std::unordered_map<std::string, std::any> perform_retrieval(
        const std::string& query_text,
        std::shared_ptr<VectorStore> vector_store,
        std::shared_ptr<EncoderManager> encoder_manager,
        int top_k,
        const std::string& modality
    );
    
    /**
     * @brief 生成上下文摘要
     * @param results 检索结果
     * @return 上下文摘要字符串
     */
    static std::string generate_context_summary(
        const std::vector<RetrievalResult>& results
    );
    
    /**
     * @brief 提取引用信息
     * @param results 检索结果
     * @return 引用列表
     */
    static std::vector<Citation> extract_citations(
        const std::vector<RetrievalResult>& results
    );
};

} // namespace node
} // namespace agent_framework

#endif // __AGENT_NODE_KNOWLEDGE_BASE_NODE_H__


/**
 * @file knowledge_base_node.cpp
 * @brief 知识库节点封装实现
 */

#include "node/knowledge_base_node.hpp"
#include <sstream>
#include <algorithm>

namespace agent_framework {
namespace node {

std::pair<std::shared_ptr<workflow::AnySource>, tf::Task>
KnowledgeBaseSourceNode::create(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<VectorStore> vector_store,
    std::shared_ptr<EncoderManager> encoder_manager
) {
    // 知识库 Source 节点的初始值（占位符，实际通过 set_query 设置）
    std::unordered_map<std::string, std::any> initial_values = {
        {"context", std::any{std::string("")}},
        {"results", std::any{std::vector<RetrievalResult>{}}},
        {"citations", std::any{std::vector<Citation>{}}}
    };
    
    return builder.create_any_source(name, initial_values);
}

void KnowledgeBaseSourceNode::set_query(
    std::shared_ptr<workflow::AnySource> node,
    const std::string& query_text,
    int top_k,
    const std::string& modality
) {
    // 注意：这里需要访问节点的内部状态来更新查询结果
    // 实际实现可能需要通过节点的 API 来更新
    // TODO: 实现查询更新逻辑
}

std::unordered_map<std::string, std::any> KnowledgeBaseSourceNode::perform_retrieval(
    const std::string& query_text,
    std::shared_ptr<VectorStore> vector_store,
    std::shared_ptr<EncoderManager> encoder_manager,
    int top_k,
    const std::string& modality
) {
    // 1. 编码查询文本
    auto encoder = encoder_manager->get_encoder("text");
    if (!encoder) {
        return {
            {"context", std::any{std::string("")}},
            {"results", std::any{std::vector<RetrievalResult>{}}},
            {"citations", std::any{std::vector<Citation>{}}}
        };
    }
    
    Embedding query_embedding = encoder->encode(query_text);
    
    // 2. 向量检索
    std::vector<RetrievalResult> results = vector_store->search(
        query_embedding,
        top_k,
        modality
    );
    
    // 3. 生成上下文摘要
    std::string context = generate_context_summary(results);
    
    // 4. 提取引用信息
    std::vector<Citation> citations = extract_citations(results);
    
    return {
        {"context", std::any{context}},
        {"results", std::any{results}},
        {"citations", std::any{citations}}
    };
}

std::string KnowledgeBaseSourceNode::generate_context_summary(
    const std::vector<RetrievalResult>& results
) {
    std::ostringstream oss;
    
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        oss << "[" << (i + 1) << "] " << result.content;
        if (i < results.size() - 1) {
            oss << "\n\n";
        }
    }
    
    return oss.str();
}

std::vector<Citation> KnowledgeBaseSourceNode::extract_citations(
    const std::vector<RetrievalResult>& results
) {
    std::vector<Citation> citations;
    
    for (const auto& result : results) {
        Citation citation;
        citation.doc_id = result.doc_id;
        citation.source = result.modality;  // 或从 metadata 中提取
        citation.excerpt = result.content.substr(0, 200);  // 截取前200字符
        citation.relevance_score = result.score;
        
        // 从 metadata 中提取页码（如果有）
        if (result.metadata.find("page_number") != result.metadata.end()) {
            citation.page_number = result.metadata.at("page_number").get<int>();
        }
        
        citations.push_back(citation);
    }
    
    return citations;
}

} // namespace node
} // namespace agent_framework


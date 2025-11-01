/**
 * @file vectorstore.hpp
 * @brief VectorStore 模块：多模态向量数据库接口
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_VECTORSTORE_H__
#define __AGENT_VECTORSTORE_H__

#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

// 前向声明
namespace agent_framework {
    class Encoder;
}

namespace agent_framework {

// ============================================================================
// 向量存储后端接口
// ============================================================================

/**
 * @brief 向量存储后端虚基类
 * 定义统一的向量数据库接口
 */
class VectorStoreBackend {
public:
    virtual ~VectorStoreBackend() = default;
    
    /**
     * @brief 插入文档向量
     * @param doc 文档
     * @param embedding 向量嵌入
     */
    virtual void insert(const Document& doc, const Embedding& embedding) = 0;
    
    /**
     * @brief 批量插入
     * @param docs 文档列表
     * @param embeddings 向量嵌入列表
     */
    virtual void insert_batch(const std::vector<Document>& docs, 
                             const std::vector<Embedding>& embeddings) = 0;
    
    /**
     * @brief 向量检索
     * @param query_vector 查询向量
     * @param top_k 返回前 k 个结果
     * @param modality 模态类型（可选，空字符串表示所有模态）
     * @return 检索结果列表
     */
    virtual std::vector<RetrievalResult> search(
        const Embedding& query_vector,
        int top_k = 5,
        const std::string& modality = ""
    ) = 0;
    
    /**
     * @brief 删除文档
     * @param doc_id 文档 ID
     * @return true 如果删除成功
     */
    virtual bool delete_document(const std::string& doc_id) = 0;
    
    /**
     * @brief 更新文档
     * @param doc 文档
     * @param embedding 新的向量嵌入
     * @return true 如果更新成功
     */
    virtual bool update_document(const Document& doc, const Embedding& embedding) = 0;
    
    /**
     * @brief 获取索引统计信息
     * @return 统计信息（JSON 格式）
     */
    virtual json get_statistics() const = 0;
    
    /**
     * @brief 保存索引
     * @param path 保存路径
     * @return true 如果保存成功
     */
    virtual bool save_index(const std::string& path) = 0;
    
    /**
     * @brief 加载索引
     * @param path 加载路径
     * @return true 如果加载成功
     */
    virtual bool load_index(const std::string& path) = 0;
};

/**
 * @brief Faiss 后端实现
 */
class FaissBackend : public VectorStoreBackend {
public:
    /**
     * @brief 构造函数
     * @param dimension 向量维度
     * @param index_type 索引类型（"IVF_PQ", "Flat", "HNSW" 等）
     */
    explicit FaissBackend(int dimension, const std::string& index_type = "IVF_PQ");
    
    void insert(const Document& doc, const Embedding& embedding) override;
    void insert_batch(const std::vector<Document>& docs, 
                     const std::vector<Embedding>& embeddings) override;
    std::vector<RetrievalResult> search(
        const Embedding& query_vector,
        int top_k = 5,
        const std::string& modality = ""
    ) override;
    bool delete_document(const std::string& doc_id) override;
    bool update_document(const Document& doc, const Embedding& embedding) override;
    json get_statistics() const override;
    bool save_index(const std::string& path) override;
    bool load_index(const std::string& path) override;
    
private:
    int dimension_;
    std::string index_type_;
    void* index_;  // faiss::Index* 指针（前向声明避免暴露 Faiss 头文件）
    std::map<std::string, Document> documents_;  // doc_id -> Document
    std::mutex index_mutex_;
    
    /**
     * @brief 创建 Faiss 索引
     */
    void create_index();
};

/**
 * @brief Milvus 后端实现
 */
class MilvusBackend : public VectorStoreBackend {
public:
    /**
     * @brief 构造函数
     * @param host Milvus 服务器地址
     * @param port Milvus 服务器端口
     * @param collection_name 集合名称
     */
    explicit MilvusBackend(const std::string& host = "localhost", 
                          int port = 19530,
                          const std::string& collection_name = "default");
    
    void insert(const Document& doc, const Embedding& embedding) override;
    void insert_batch(const std::vector<Document>& docs, 
                     const std::vector<Embedding>& embeddings) override;
    std::vector<RetrievalResult> search(
        const Embedding& query_vector,
        int top_k = 5,
        const std::string& modality = ""
    ) override;
    bool delete_document(const std::string& doc_id) override;
    bool update_document(const Document& doc, const Embedding& embedding) override;
    json get_statistics() const override;
    bool save_index(const std::string& path) override;
    bool load_index(const std::string& path) override;
    
private:
    std::string host_;
    int port_;
    std::string collection_name_;
    void* milvus_client_;  // Milvus 客户端指针（实际类型取决于 Milvus SDK）
    std::mutex client_mutex_;
    
    /**
     * @brief 连接 Milvus 服务器
     */
    void connect_milvus();
    
    /**
     * @brief 创建集合
     */
    void create_collection();
};

// ============================================================================
// VectorStore 管理器
// ============================================================================

/**
 * @brief VectorStore 管理器（使用后端）
 */
class VectorStore {
public:
    explicit VectorStore(std::unique_ptr<VectorStoreBackend> backend);
    
    /**
     * @brief 插入文档（多模态）
     * @param doc 文档
     * @param embedding 向量嵌入
     */
    void insert(const Document& doc, const Embedding& embedding);
    
    /**
     * @brief 向量检索
     * @param query_vector 查询向量
     * @param top_k 返回前 k 个结果
     * @param modality 模态类型（可选）
     * @return 检索结果列表
     */
    std::vector<RetrievalResult> search(
        const Embedding& query_vector,
        int top_k = 5,
        const std::string& modality = ""
    );
    
    /**
     * @brief 混合检索（语义 + 关键词）
     * @param query_text 查询文本
     * @param query_vector 查询向量
     * @param top_k 返回前 k 个结果
     * @return 检索结果列表
     */
    std::vector<RetrievalResult> hybrid_search(
        const std::string& query_text,
        const Embedding& query_vector,
        int top_k = 5
    );
    
    /**
     * @brief 注册编码器
     * @param modality 模态类型（"text", "image", "audio", "video"）
     * @param encoder 编码器
     */
    void register_encoder(const std::string& modality,
                         std::shared_ptr<Encoder> encoder);
    
    /**
     * @brief 获取编码器
     * @param modality 模态类型
     * @return 编码器（如果存在）
     */
    std::shared_ptr<Encoder> get_encoder(const std::string& modality) const;
    
    /**
     * @brief 切换后端
     * @param new_backend 新的后端实例
     */
    void switch_backend(std::unique_ptr<VectorStoreBackend> new_backend);
    
private:
    std::unique_ptr<VectorStoreBackend> backend_;
    std::map<std::string, std::shared_ptr<Encoder>> encoders_;
    std::mutex backend_mutex_;
    std::mutex encoders_mutex_;
};

} // namespace agent_framework

#endif // __AGENT_VECTORSTORE_H__

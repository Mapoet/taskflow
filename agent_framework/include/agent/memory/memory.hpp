/**
 * @file memory.hpp
 * @brief Memory 模块：事件溯源和记忆管理
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_MEMORY_H__
#define __AGENT_MEMORY_H__

#include <agent/core/types.hpp>
#include <string>
#include <vector>
#include <memory>
#include <ctime>
#include <mutex>

namespace agent_framework {

// ============================================================================
// 记忆存储后端接口
// ============================================================================

/**
 * @brief 记忆存储后端虚基类
 * 定义统一的存储接口，支持不同的存储后端
 */
class MemoryBackend {
public:
    virtual ~MemoryBackend() = default;

    /**
     * @brief 存储事件
     * @param event 事件
     */
    virtual void store_event(const Event& event) = 0;

    /**
     * @brief 查询事件
     * @param session_id 会话 ID
     * @param node_name 节点名称（可选，空字符串表示所有节点）
     * @param start_time 起始时间（0 表示不限制）
     * @param end_time 结束时间（0 表示不限制）
     * @return 事件列表
     */
    virtual std::vector<Event> query_events(
        const std::string& session_id,
        const std::string& node_name = "",
        std::time_t start_time = 0,
        std::time_t end_time = 0
    ) = 0;

    /**
     * @brief 存储消息
     * @param message 消息
     */
    virtual void store_message(const Message& message) = 0;

    /**
     * @brief 查询对话历史
     * @param session_id 会话 ID
     * @param max_messages 最大消息数
     * @return 消息列表（按时间顺序）
     */
    virtual std::vector<Message> get_conversation_history(
        const std::string& session_id,
        int max_messages = 10
    ) = 0;

    /**
     * @brief 存储记忆摘要
     * @param summary 记忆摘要
     */
    virtual void store_memory_summary(const MemorySummary& summary) = 0;

    /**
     * @brief 查询记忆摘要
     * @param query 查询文本
     * @param top_k 返回前 k 个结果
     * @return 记忆摘要列表
     */
    virtual std::vector<MemorySummary> query_memory_summaries(
        const std::string& query,
        int top_k = 5
    ) = 0;

    /**
     * @brief 清理过期数据
     * @param expiry_time 过期时间戳
     */
    virtual void cleanup_expired_data(std::time_t expiry_time) = 0;
};

/**
 * @brief 文件系统后端实现
 */
class FileMemoryBackend : public MemoryBackend {
public:
    explicit FileMemoryBackend(const std::string& data_dir);

    void store_event(const Event& event) override;
    std::vector<Event> query_events(
        const std::string& session_id,
        const std::string& node_name = "",
        std::time_t start_time = 0,
        std::time_t end_time = 0
    ) override;
    void store_message(const Message& message) override;
    std::vector<Message> get_conversation_history(
        const std::string& session_id,
        int max_messages = 10
    ) override;
    void store_memory_summary(const MemorySummary& summary) override;
    std::vector<MemorySummary> query_memory_summaries(
        const std::string& query,
        int top_k = 5
    ) override;
    void cleanup_expired_data(std::time_t expiry_time) override;

private:
    std::string data_dir_;
    std::mutex file_mutex_;

    /**
     * @brief 获取事件日志文件路径
     * @param session_id 会话 ID
     * @return 文件路径
     */
    std::string get_event_log_path(const std::string& session_id) const;

    /**
     * @brief 追加事件到文件
     * @param event 事件
     * @param path 文件路径
     */
    void append_event_to_file(const Event& event, const std::string& path);
};

/**
 * @brief SQLite 后端实现
 */
class SQLiteMemoryBackend : public MemoryBackend {
public:
    explicit SQLiteMemoryBackend(const std::string& db_path);
    ~SQLiteMemoryBackend();

    void store_event(const Event& event) override;
    std::vector<Event> query_events(
        const std::string& session_id,
        const std::string& node_name = "",
        std::time_t start_time = 0,
        std::time_t end_time = 0
    ) override;
    void store_message(const Message& message) override;
    std::vector<Message> get_conversation_history(
        const std::string& session_id,
        int max_messages = 10
    ) override;
    void store_memory_summary(const MemorySummary& summary) override;
    std::vector<MemorySummary> query_memory_summaries(
        const std::string& query,
        int top_k = 5
    ) override;
    void cleanup_expired_data(std::time_t expiry_time) override;

private:
    std::string db_path_;
    void* db_;  // sqlite3* 指针（前向声明避免暴露 SQLite 头文件）
    std::mutex db_mutex_;

    /**
     * @brief 初始化数据库表
     */
    void init_database();

    /**
     * @brief 执行 SQL 语句
     * @param sql SQL 语句
     * @param params 参数列表
     */
    void execute_sql(const std::string& sql, const std::vector<std::string>& params = {});
};

/**
 * @brief 内存后端实现（临时存储，不持久化）
 */
class InMemoryBackend : public MemoryBackend {
public:
    InMemoryBackend();

    void store_event(const Event& event) override;
    std::vector<Event> query_events(
        const std::string& session_id,
        const std::string& node_name = "",
        std::time_t start_time = 0,
        std::time_t end_time = 0
    ) override;
    void store_message(const Message& message) override;
    std::vector<Message> get_conversation_history(
        const std::string& session_id,
        int max_messages = 10
    ) override;
    void store_memory_summary(const MemorySummary& summary) override;
    std::vector<MemorySummary> query_memory_summaries(
        const std::string& query,
        int top_k = 5
    ) override;
    void cleanup_expired_data(std::time_t expiry_time) override;

private:
    std::map<std::string, std::vector<Event>> events_;
    std::map<std::string, std::vector<Message>> messages_;
    std::vector<MemorySummary> summaries_;
    std::mutex data_mutex_;
};

// ============================================================================
// Memory 管理器
// ============================================================================

/**
 * @brief Memory 管理器（使用后端）
 */
class MemoryStore {
public:
    explicit MemoryStore(std::unique_ptr<MemoryBackend> backend);

    /**
     * @brief 存储事件（事件溯源）
     * @param event 事件
     */
    void store_event(const Event& event);

    /**
     * @brief 查询对话历史
     * @param session_id 会话 ID
     * @param max_messages 最大消息数
     * @return 消息列表
     */
    std::vector<Message> get_conversation_history(
        const std::string& session_id,
        int max_messages = 10
    );

    /**
     * @brief 查询短期记忆（当前会话）
     * @param session_id 会话 ID
     * @return 事件列表
     */
    std::vector<Event> get_short_term_memory(const std::string& session_id);

    /**
     * @brief 存储长期记忆摘要
     * @param session_id 会话 ID
     * @param summary 记忆摘要
     */
    void store_long_term_memory(const std::string& session_id,
                               const MemorySummary& summary);

    /**
     * @brief 查询长期记忆
     * @param query 查询文本
     * @param top_k 返回前 k 个结果
     * @return 记忆摘要列表
     */
    std::vector<MemorySummary> query_long_term_memory(
        const std::string& query, int top_k = 5
    );

    /**
     * @brief 切换后端（运行时切换）
     * @param new_backend 新的后端实例
     */
    void switch_backend(std::unique_ptr<MemoryBackend> new_backend);

private:
    std::unique_ptr<MemoryBackend> backend_;
    std::mutex backend_mutex_;
};

} // namespace agent_framework

#endif // __AGENT_MEMORY_H__

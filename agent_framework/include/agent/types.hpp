/**
 * @file types.hpp
 * @brief 公共数据结构定义
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_TYPES_H__
#define __AGENT_TYPES_H__

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <map>
#include <chrono>
#include <memory>
#include <ctime>
#include <functional>

using json = nlohmann::json;

namespace agent_framework {

// ============================================================================
// 枚举类型
// ============================================================================

/**
 * @brief MCP 传输类型
 */
enum class MCPTransport {
    STDIO,      // 标准输入输出
    HTTP,       // HTTP 传输
    WEBSOCKET   // WebSocket 传输
};

/**
 * @brief 节点状态枚举（节点生命周期）
 */
enum class NodeState {
    Created,    // 已创建
    Waiting,    // 等待依赖就绪
    Ready,      // 所有依赖完成
    Executing,  // 正在执行
    Success,    // 执行成功
    Failed,     // 执行失败
    Emitting,   // 设置输出值
    Error       // 错误状态
};

/**
 * @brief 多模态类型
 */
enum class ModalityType {
    TEXT,       // 文本
    IMAGE,      // 图像
    AUDIO,      // 音频
    VIDEO,      // 视频
    MULTIMODAL  // 多模态混合
};

/**
 * @brief 融合策略类型
 */
enum class FusionStrategy {
    EARLY,          // 早期融合（输入层）
    INTERMEDIATE,   // 中期融合（交叉注意力）
    LATE,           // 晚期融合（分别检索后合并）
    HYBRID          // 混合融合（综合多种方式）
};

/**
 * @brief 工具副作用类别（WP2.1b 编排；Unknown 在编排上等价于 Write/串行）
 */
enum class ToolSideEffect {
    Unknown = 0,
    ReadOnly = 1,
    Write = 2
};

/**
 * @brief 从配置字符串解析副作用（大小写不敏感）；无法识别时返回 nullopt
 */
std::optional<ToolSideEffect> tool_side_effect_from_string(std::string_view s);

/**
 * @brief WP2.1d：工具调用前 hook 对单次调用的裁决
 */
enum class ToolHookVerdict {
    Allow,   ///< 继续链；参数为当前累积值
    Deny,    ///< 短路；返回 hook_denied future
    Replace  ///< 整对象替换参数后继续链
};

/**
 * @brief WP2.1c：`_af_truncation.kind`（与 context-budget.md schema 一致）
 */
enum class AfTruncationKind {
    tool_result,
    user_injection,
    llm_context,
    wire_payload
};

// ============================================================================
// 记忆管理相关类型（提前定义，供 LLMInput 使用）
// ============================================================================

/**
 * @brief 消息结构（对话历史）
 */
struct Message {
    std::string role;              // 角色（"user", "assistant", "system", "tool"）
    std::string content;           // 消息内容
    std::optional<std::string> tool_call_id;  // OpenAI tool_call_id（如果是 tool 消息）
    std::optional<std::string> tool_name;  // 工具名称（如果是 tool 消息）
    std::optional<json> tool_result;       // 工具执行结果
    std::time_t timestamp;         // 时间戳
};

// ============================================================================
// LLM 相关类型
// ============================================================================

/**
 * @brief 工具元数据
 */
struct ToolMeta {
    std::string name;              // 工具名称
    json schema;                  // JSON Schema 描述（OpenAI Function Calling 格式）
    std::string description;       // 工具说明
    /** WP2.1b：编排用；不进入 LLM tools JSON（tool_formatter 仅导出 name/schema/description） */
    ToolSideEffect side_effect = ToolSideEffect::Unknown;
};

/**
 * @brief LLM 输入结构
 */
struct LLMInput {
    std::string system_prompt;     // 系统提示词（角色定义、行为规范）
    /** WP1.8：活动技能正文（不含 frontmatter）；由 PromptRenderer 加 `## Active skill (id: …)` 标题 */
    std::optional<std::string> skill_block;
    /** 与 skill_block 配套的 id，供系统区标题展示；缺省则 PromptRenderer 使用 `unknown` */
    std::optional<std::string> active_skill_id;
    std::string user_prompt;       // 用户提示词（当前问题或指令）
    std::string context;           // 从知识库检索的内容摘要（多模态 RAG 结果）
    std::map<std::string, std::string> extra_variables;  // 用户自定义模板变量（{{var}}）
    std::vector<ToolMeta> tools;   // 可用工具列表（ToolBus 导出）
    std::vector<Message> history;   // 对话历史（可选，用于多轮对话）
    /** WP2.agents：注入系统区；由 PromptRenderer 追加 `## Outbound subtasks` */
    std::optional<std::string> orchestrator_subtask_digest;
    std::optional<std::string> image_data;  // 图像 base64 编码（可选）
    std::optional<std::string> audio_data;  // 音频 base64 编码（可选）
    /** Request-scoped cooperative cancellation/deadline check. */
    std::function<bool()> cancellation_requested;
};

/**
 * @brief 渲染后的提示词结构
 * 包含渲染后的文本内容和多模态数据，由 PromptRenderer 生成
 */
struct RenderedPrompt {
    std::string rendered_text;              // 渲染后的文本提示词
    std::vector<json> messages;             // 格式化后的消息列表（OpenAI messages 格式）
    json tools_json;                        // 格式化后的工具列表（JSON）
    std::optional<std::string> image_data;  // 图像 base64（已嵌入 messages）
    std::optional<std::string> audio_data;  // 音频 base64（已嵌入 messages）
    int total_tokens = 0;                   // 估算的总 token 数
    /** WP2.1c：`AGENT_CONTEXT_BUDGET_STRICT=1` 且合并/注入后仍超限时为 true，调用方应跳过 LLM */
    bool context_budget_blocked = false;
    std::function<bool()> cancellation_requested;
};

/**
 * @brief 工具调用规范
 */
struct CallSpec {
    std::string name;              // 工具名称
    json arguments;                // 调用参数（JSON 对象）
    /** OpenAI `tool_calls[].id` / Anthropic `tool_use.id`，供多轮 tool_result 对齐 */
    std::optional<std::string> tool_call_id;
};

/**
 * @brief LLM 输出结构
 */
struct LLMOutput {
    std::vector<CallSpec> tool_calls;  // 工具调用列表
    std::string reasoning;             // 中间思考和计划描述
    bool is_final;                     // 是否已完成任务（true 表示生成最终答案）
    std::string final_answer;          // 最终回答（仅当 is_final 为真时有效）
    std::optional<std::string> audio_out;  // 语音输出（可选，如 GPT-4o Realtime API）
    std::optional<std::string> image_out;  // 图像输出（可选，如 GPT-4o Realtime API）
};

/**
 * @brief LLM HTTP 层错误（供重试与排障）
 */
class llm_http_error : public std::runtime_error {
public:
    int status_code;                      ///< 0 表示连接/传输失败；否则 HTTP 状态码
    std::string body_excerpt;            ///< 响应体摘录
    std::string provider;                 ///< openai / anthropic 等
    std::optional<int> retry_after_sec;  ///< 若服务端返回 Retry-After

    llm_http_error(int status_code, std::string provider, std::string body_excerpt,
                   std::optional<int> retry_after_sec = std::nullopt);
};

inline llm_http_error::llm_http_error(int status_code, std::string provider,
                                      std::string body_excerpt,
                                      std::optional<int> retry_after_sec)
    : std::runtime_error("[" + provider + "] HTTP " + std::to_string(status_code) + ": " +
                         body_excerpt.substr(0, std::min<std::size_t>(body_excerpt.size(), 256u))),
      status_code(status_code),
      body_excerpt(std::move(body_excerpt)),
      provider(std::move(provider)),
      retry_after_sec(retry_after_sec) {}

/**
 * @brief 模型配置
 */
struct ModelConfig {
    std::string model_name;        // 模型名称（如 "gpt-4o", "claude-3-opus"）
    double temperature = 0.7;      // 温度参数
    double top_p = 1.0;            // Top P 参数
    int max_tokens = 4096;         // 最大 token 数
    bool stream = true;            // 是否启用流式输出
    int http_timeout_sec = 120;    // HTTP 连接/读超时（秒）
    int max_retries = 3;           // 失败重试次数（不含首次）
    std::map<std::string, json> extra_params;  // 额外参数
};

// ============================================================================
// 多模态向量检索相关类型
// ============================================================================

/**
 * @brief 向量嵌入类型别名
 */
using Embedding = std::vector<float>;

/**
 * @brief 查询向量（向量 + 模态类型）
 */
using QueryVector = std::pair<std::vector<float>, std::string>;

/**
 * @brief 文档元数据
 */
struct DocumentMetadata {
    std::string doc_id;            // 文档唯一标识
    std::string modality;          // 模态类型（"text", "image", "audio", "video"）
    std::string content;           // 原始内容（文本、图像路径、音频路径等）
    std::string description;       // 描述信息（图像描述、音频转录等）
    std::time_t timestamp;         // 创建时间戳
    std::map<std::string, json> extra_metadata;  // 额外元数据
};

/**
 * @brief 文档结构（用于向量存储）
 */
struct Document {
    std::string doc_id;            // 文档唯一标识
    Embedding embedding;           // 向量嵌入
    DocumentMetadata metadata;      // 元数据
};

/**
 * @brief 检索结果
 */
struct RetrievalResult {
    std::string doc_id;            // 文档 ID
    float score;                   // 相似度分数
    std::vector<float> embedding;  // 文档向量
    std::string content;           // 文档内容
    std::string modality;          // 模态类型
    std::string description;       // 描述信息
    std::map<std::string, json> metadata;  // 额外元数据
};

/**
 * @brief 对齐后的文档（跨模态对齐结果）
 */
struct AlignedDocument {
    std::string text;              // 文本内容
    std::string image;             // 图像内容（路径或 base64）
    std::string audio;             // 音频内容（路径或 base64）
    float score;                   // 对齐分数
    float alignment_score;         // 注意力对齐分数
};

/**
 * @brief 引用信息（用于知识库检索后的引用）
 */
struct Citation {
    std::string doc_id;            // 文档 ID
    std::string source;            // 来源（文件名、URL 等）
    std::string excerpt;           // 引用片段
    int page_number = -1;          // 页码（如果有）
    float relevance_score = 0.0f;  // 相关性分数
};

// ============================================================================
// 记忆管理相关类型（Message 已在上方定义）
// ============================================================================

/**
 * @brief 事件类型（事件溯源）
 */
struct Event {
    std::time_t timestamp;         // 时间戳
    std::string node_name;         // 节点名称
    std::string event_type;        // 事件类型（"input", "output", "error"）
    json data;                     // 事件数据（JSON 格式）
};

/**
 * @brief 记忆摘要（长期记忆）
 */
struct MemorySummary {
    std::string session_id;        // 会话 ID
    std::string summary;          // 摘要内容
    std::vector<std::string> keywords;  // 关键词
    Embedding summary_embedding;  // 摘要向量（用于检索）
    std::time_t created_at;       // 创建时间
    std::time_t updated_at;       // 更新时间
};

// ============================================================================
// 工具相关类型
// ============================================================================

/**
 * @brief 工具信息（工具特性）
 */
struct ToolInfo {
    std::string name;              // 工具名称
    bool requires_gpu = false;    // 是否需要 GPU
    bool is_heavy = false;         // 是否为重型工具（CPU 密集型）
    bool is_io_bound = false;      // 是否为 I/O 密集型
    int estimated_duration_ms = 0; // 预估执行时间（毫秒）
    std::map<std::string, json> capabilities;  // 工具能力描述
};

/**
 * @brief 调度任务（工具调用调度）
 */
struct ScheduledTask {
    CallSpec spec;                 // 工具调用规范
    int priority = 1;              // 优先级（1=高, 2=中, 3=低）
    std::optional<int> execution_order;  // 执行顺序（-1 表示可并行）
};

// ============================================================================
// 配置相关类型
// ============================================================================

/**
 * @brief Agent 配置
 */
struct AgentConfig {
    std::string name;              // Agent 名称
    std::string system_prompt;     // 系统提示词
    ModelConfig model_config;      // 模型配置
    int max_iterations = 10;       // 最大迭代次数
    int max_tool_calls_per_iteration = 5;  // 每次迭代最大工具调用数
    bool enable_memory = true;     // 是否启用记忆
    bool enable_knowledge_base = true;  // 是否启用知识库
    /** WP2.1b：是否允许连续 ReadOnly 工具并行（默认 false，与历史串行一致） */
    bool enable_parallel_read_tools = false;
    /** WP2.1b：ReadOnly 组内最大并发；<=0 在 resolve 时视为 1 */
    int max_parallel_read_tools = 4;
    /** WP2.agents：同轮连续 a2a_submit_task 并行（默认开启） */
    bool enable_parallel_a2a_submits = true;
    /** WP2.agents：submit 批内最大并发；<=0 在 resolve 时视为 1 */
    int max_parallel_a2a_submits = 4;
    std::map<std::string, json> extra_config;  // 额外配置
};

/**
 * @brief 工作流配置
 */
struct WorkflowConfig {
    std::string name;              // 工作流名称
    std::string description;       // 工作流描述
    std::vector<std::string> input_keys;  // 输入键列表
    std::vector<std::string> output_keys; // 输出键列表
    std::map<std::string, json> node_configs;  // 节点配置
    std::map<std::string, json> extra_config;  // 额外配置
};

// ============================================================================
// 性能监控相关类型
// ============================================================================

/**
 * @brief 节点统计信息
 */
struct NodeStats {
    int execution_count = 0;       // 执行次数
    std::chrono::milliseconds total_time{0};  // 总执行时间
    std::chrono::milliseconds avg_time{0};    // 平均执行时间
    std::chrono::milliseconds max_time{0};     // 最大执行时间
    std::chrono::milliseconds min_time{std::chrono::milliseconds::max()};  // 最小执行时间
};

/**
 * @brief 性能报告
 */
struct PerformanceReport {
    std::map<std::string, NodeStats> node_stats;  // 节点统计信息
    std::chrono::milliseconds total_execution_time{0};  // 总执行时间
    std::time_t report_time;       // 报告生成时间
};

// ============================================================================
// 工作流执行相关类型
// ============================================================================

/**
 * @brief 工作流执行结果
 */
struct WorkflowResult {
    bool success;                  // 是否成功
    json outputs;                  // 输出结果（键值对）
    std::vector<Event> events;     // 事件日志
    PerformanceReport performance; // 性能报告
    std::optional<std::string> error_message;  // 错误信息（如果失败）
    /** WP2.8：Verifier abort 时 CLI 进程退出码 4；否则为 0 */
    int exit_code = 0;
};

// ============================================================================
// UI 相关类型（可选，用于类型定义）
// ============================================================================

/**
 * @brief Web 连接信息（抽象概念，实际实现由具体类完成）
 */
struct WebConnectionInfo {
    std::string session_id;        // 会话 ID
    std::string connection_type;    // 连接类型（"SSE", "WebSocket"）
    std::time_t connected_at;      // 连接时间
    bool is_active = true;         // 是否活跃
};

/**
 * @brief 流式输出消息
 */
struct StreamMessage {
    std::string session_id;        // 会话 ID
    std::string message_type;      // 消息类型（"token", "final", "error"）
    std::string content;           // 消息内容
    std::time_t timestamp;         // 时间戳
};

// ============================================================================
// A2A (Agent2Agent) 协议相关类型
// ============================================================================

/**
 * @brief Agent Task 状态枚举（A2A 协议）
 */
enum class AgentTaskStatus {
    PENDING,              // 待处理
    WORKING,              // 执行中
    COMPLETED,            // 已完成
    FAILED,               // 失败
    INPUT_REQUIRED,       // 需要输入
    CANCELLED             // 已取消
};

/**
 * @brief Agent Skill（技能）结构（A2A 协议）
 */
struct AgentSkill {
    std::string name;                          // 技能名称
    std::string description;                   // 技能描述
    json input_schema;                         // 输入参数 JSON Schema
    json output_schema;                        // 输出参数 JSON Schema
    std::vector<std::string> required_capabilities;  // 所需能力
};

/**
 * @brief Agent Card（智能体名片）结构（A2A 协议）
 */
struct AgentCard {
    std::string name;                          // Agent 名称
    std::string description;                   // 描述
    std::string provider;                      // 提供商
    std::string api_endpoint;                 // API 端点 URL
    std::vector<std::string> capabilities;    // 支持的能力（如 "streaming", "push-notifications"）
    json authentication_scheme;               // 认证方案要求
    std::vector<AgentSkill> skills;           // 技能列表
    
    // 序列化/反序列化
    json to_json() const;
    static AgentCard from_json(const json& j);
};

/**
 * @brief Agent FileInfo（文件信息）结构（A2A 协议）
 */
struct AgentFileInfo {
    std::string mime_type;                     // MIME 类型
    std::optional<std::string> uri;           // 文件 URI（可选）
    std::optional<std::vector<uint8_t>> bytes; // 文件字节（可选）
    std::optional<std::string> name;          // 文件名（可选）
};

/**
 * @brief Agent Part（部件）结构（A2A 协议）
 * 构成 Agent Message 或 Agent Artifact 内容的基本单元
 */
struct AgentPart {
    enum class Type {
        TEXT,    // 文本
        FILE,    // 文件
        DATA     // JSON 数据
    };
    
    Type type;                                // 部件类型
    std::optional<std::string> text;          // 文本内容（type == TEXT）
    std::optional<AgentFileInfo> file;        // 文件信息（type == FILE）
    std::optional<json> data;                 // JSON 数据（type == DATA）
    
    json to_json() const;
    static AgentPart from_json(const json& j);
};

/**
 * @brief Agent Message（消息）结构（A2A 协议）
 * Agent 之间传递信息的载体（注意：与 agent_framework::Message 不同，后者用于对话历史）
 */
struct AgentMessage {
    enum class Role {
        USER,    // 用户角色
        AGENT    // Agent 角色
    };
    
    Role role;                                // 来源角色
    std::vector<AgentPart> parts;            // 消息部件列表
    std::optional<std::string> message_id;    // 消息 ID（可选）
    std::chrono::system_clock::time_point timestamp;
    
    json to_json() const;
    static AgentMessage from_json(const json& j);
};

/**
 * @brief Agent Artifact（工件）结构（A2A 协议）
 * 任务执行完成后产生的最终输出或成果物
 */
struct AgentArtifact {
    std::string artifact_id;                  // 工件 ID
    std::string task_id;                      // 关联任务 ID
    std::vector<AgentPart> parts;             // 工件内容部件
    json metadata;                            // 元数据
    std::chrono::system_clock::time_point created_at;
    bool is_immutable = true;                 // 是否不可变
    
    json to_json() const;
    static AgentArtifact from_json(const json& j);
};

/**
 * @brief Agent Task（任务）结构（A2A 协议）
 * 跟踪和管理一次协作交互的核心实体
 */
struct AgentTask {
    std::string task_id;                      // 唯一任务 ID
    std::optional<std::string> session_id;   // 会话 ID（可选）
    AgentTaskStatus status;                   // 当前状态
    std::vector<AgentMessage> messages;      // 交互历史
    std::vector<AgentArtifact> artifacts;    // 生成的工件
    json metadata;                           // 扩展元数据
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    
    json to_json() const;
    static AgentTask from_json(const json& j);
};

} // namespace agent_framework

#endif // __AGENT_TYPES_H__

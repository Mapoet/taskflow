/**
 * @file ui_sink_node.hpp
 * @brief UI 输出节点封装：将 UI 输出封装为 workflow Sink 节点
 * @author Mapoet
 * @version 0.1
 * @date 2025-01-XX
 */
#ifndef __AGENT_NODE_UI_SINK_NODE_H__
#define __AGENT_NODE_UI_SINK_NODE_H__

#include <workflow/nodeflow.hpp>
#include "../agent/types.hpp"
#include "../agent/ui_manager.hpp"
#include <string>
#include <memory>

namespace agent_framework {
namespace node {

/**
 * @brief UI Sink 节点封装类
 * 将 UI 输出封装为 workflow AnySink
 */
class UISinkNode {
public:
    /**
     * @brief 创建 CLI Sink 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param ui_manager UI 管理器
     * @param input_specs 输入规格：
     *   - {"Output", "content"} - 输出内容（字符串）
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_cli(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<UIManager> ui_manager,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建 ImGui Sink 节点
     * @param builder 图构建器
     * @param name 节点名称
     * @param ui_manager UI 管理器
     * @param input_specs 输入规格：
     *   - {"Output", "content"} - 输出内容（字符串）
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_imgui(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<UIManager> ui_manager,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );
    
    /**
     * @brief 创建 Web Sink 节点（SSE/WebSocket）
     * @param builder 图构建器
     * @param name 节点名称
     * @param ui_manager UI 管理器
     * @param session_id 会话 ID
     * @param input_specs 输入规格：
     *   - {"Output", "content"} - 输出内容（字符串）
     *   - {"StreamToken", "token"} - 流式 token（可选，字符串）
     * @return (节点指针, 任务句柄)
     */
    static std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
    create_web(
        workflow::GraphBuilder& builder,
        const std::string& name,
        std::shared_ptr<UIManager> ui_manager,
        const std::string& session_id,
        const std::vector<std::pair<std::string, std::string>>& input_specs
    );

private:
    /**
     * @brief 处理输出到 UI
     * @param ui_manager UI 管理器
     * @param session_id 会话 ID（可选）
     * @param outputs 输出数据映射
     */
    static void handle_ui_output(
        std::shared_ptr<UIManager> ui_manager,
        const std::string& session_id,
        const std::unordered_map<std::string, std::any>& outputs
    );
};

} // namespace node
} // namespace agent_framework

#endif // __AGENT_NODE_UI_SINK_NODE_H__


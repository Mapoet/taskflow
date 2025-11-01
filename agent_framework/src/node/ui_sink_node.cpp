/**
 * @file ui_sink_node.cpp
 * @brief UI 输出节点封装实现
 */

#include "node/ui_sink_node.hpp"
#include <any>

namespace agent_framework {
namespace node {

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
UISinkNode::create_cli(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<UIManager> ui_manager,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    auto sink_callback = [ui_manager](
        const std::unordered_map<std::string, std::any>& outputs
    ) {
        handle_ui_output(ui_manager, "", outputs);
    };
    
    return builder.create_any_sink(name, input_specs, sink_callback);
}

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
UISinkNode::create_imgui(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<UIManager> ui_manager,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    auto sink_callback = [ui_manager](
        const std::unordered_map<std::string, std::any>& outputs
    ) {
        handle_ui_output(ui_manager, "", outputs);
    };
    
    return builder.create_any_sink(name, input_specs, sink_callback);
}

std::pair<std::shared_ptr<workflow::AnySink>, tf::Task>
UISinkNode::create_web(
    workflow::GraphBuilder& builder,
    const std::string& name,
    std::shared_ptr<UIManager> ui_manager,
    const std::string& session_id,
    const std::vector<std::pair<std::string, std::string>>& input_specs
) {
    auto sink_callback = [ui_manager, session_id](
        const std::unordered_map<std::string, std::any>& outputs
    ) {
        handle_ui_output(ui_manager, session_id, outputs);
    };
    
    return builder.create_any_sink(name, input_specs, sink_callback);
}

void UISinkNode::handle_ui_output(
    std::shared_ptr<UIManager> ui_manager,
    const std::string& session_id,
    const std::unordered_map<std::string, std::any>& outputs
) {
    // 提取输出内容
    std::string content;
    if (outputs.find("content") != outputs.end()) {
        content = std::any_cast<std::string>(outputs.at("content"));
    }
    
    // 提取流式 token（如果有）
    std::string token;
    if (outputs.find("token") != outputs.end()) {
        token = std::any_cast<std::string>(outputs.at("token"));
    }
    
    // 构建 JSON 消息
    json message;
    if (!content.empty()) {
        message["type"] = "final";
        message["content"] = content;
    } else if (!token.empty()) {
        message["type"] = "token";
        message["content"] = token;
    } else {
        message["type"] = "output";
        message["content"] = content;
    }
    
    // 分发消息到 UI 管理器
    if (session_id.empty()) {
        ui_manager->dispatch_message("output", message);
    } else {
        ui_manager->stream_token(session_id, content);
    }
}

} // namespace node
} // namespace agent_framework


/**
 * @file news_sources_tool.hpp
 * @brief 注册 web_configured_source（AGENT_NEWS_SOURCES_JSON + 与 web_* 相同门闩）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-04
 */
#ifndef __AGENT_NEWS_SOURCES_TOOL_H__
#define __AGENT_NEWS_SOURCES_TOOL_H__

#include <agent/toolbus/toolbus.hpp>

namespace agent_framework {

/**
 * @brief AGENT_WEB_ENABLE、OpenSSL、且 AGENT_NEWS_SOURCES_JSON 指向有效 v1 文件时注册 web_configured_source。
 */
void register_web_configured_source_if_configured(ToolBus& bus);

} // namespace agent_framework

#endif // __AGENT_NEWS_SOURCES_TOOL_H__

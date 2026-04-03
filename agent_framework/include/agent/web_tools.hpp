/**
 * @file web_tools.hpp
 * @brief 内建 web_* 工具注册（AGENT_WEB_ENABLE；需 HTTPS/httplib+OpenSSL）
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-04
 */
#ifndef __AGENT_WEB_TOOLS_H__
#define __AGENT_WEB_TOOLS_H__

#include "toolbus.hpp"
#include "types.hpp"

namespace agent_framework {

json web_fetch_invoke(const json& j);
json web_rss_feed_invoke(const json& j);
json web_fetch_archive_invoke(const json& j);

/**
 * @brief AGENT_WEB_ENABLE 且 OpenSSL 可用时注册 web_search / web_fetch / web_rss_feed / web_fetch_archive。
 * 幂等：已存在 web_search 则跳过。
 */
void register_builtin_web_tools_if_configured(ToolBus& bus);

} // namespace agent_framework

#endif // __AGENT_WEB_TOOLS_H__

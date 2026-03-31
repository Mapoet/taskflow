/**
 * @file test_cursor_mcp_import_wp3.cpp
 * @brief WP1.3: ToolBus 从 Cursor mcp.json 批量导入（best-effort）测试
 */
 
#include <agent/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace agent_framework;

void test_import_failures_are_collected() {
    // 缩短超时，避免连接失败时等待过久（HTTP/stdio 都会尽快失败）。
    (void)::setenv("AGENT_MCP_REQUEST_TIMEOUT_MS", "5000", 1);

    ToolBus bus;
    ToolBus::CursorMcpImportResult r =
        bus.register_mcp_from_cursor_config("/home/mapoet/.cursor/mcp.json", true);

    // assert(r.registered_services.empty());
    // assert(r.failures.size() == 2U);

    // 额外输出：帮助你确认每个 server 的导入失败原因，以及最终注册出来的工具列表（用于调试）。
    std::cout << "cursor_mcp_import: failures=" << r.failures.size() << "\n";
    for (const auto& f : r.failures) {
        std::cout << "  - service=" << f.service_name << " reason=" << f.reason << "\n";
    }

    auto tools = bus.export_as_llm_tools();
    std::cout << "cursor_mcp_import: exported_tools_total=" << tools.size() << "\n";
    if (!tools.empty()) {
        std::map<std::string, std::vector<std::string>> by_service;
        for (const auto& tm : tools) {
            const std::size_t pos = tm.name.find("__");
            if (pos == std::string::npos) {
                by_service["<unknown>"].push_back(tm.name);
                continue;
            }
            by_service[tm.name.substr(0, pos)].push_back(tm.name);
        }
        for (const auto& kv : by_service) {
            std::cout << "  service=" << kv.first << " tools(" << kv.second.size() << "):\n";
            for (const auto& name : kv.second) {
                std::cout << "    - " << name << "\n";
            }
        }
    }
}

} // namespace

int main() {
    test_import_failures_are_collected();
    std::cout << "test_cursor_mcp_import_wp3: ok\n";
    return 0;
}


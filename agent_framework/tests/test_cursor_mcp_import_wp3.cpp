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
    (void)::setenv("AGENT_MCP_REQUEST_TIMEOUT_MS", "1500", 1);

    ToolBus bus;
    ToolBus::CursorMcpImportResult r =
        bus.register_mcp_from_cursor_config("tests/fixtures/mcp/cursor_mcp.json", true);

    assert(r.registered_services.empty());
    assert(r.failures.size() == 2U);
}

} // namespace

int main() {
    test_import_failures_are_collected();
    std::cout << "test_cursor_mcp_import_wp3: ok\n";
    return 0;
}


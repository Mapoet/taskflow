#include <agent/toolbus/process_tools.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/distributed/object_store.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
#if defined(_WIN32)
    return 0;
#else
    namespace fs = std::filesystem;
    using namespace agent_framework;
    const auto root=fs::temp_directory_path()/"agent-process-tools-test";
    std::error_code ec; fs::remove_all(root,ec); fs::create_directories(root);
    ::setenv("AGENT_FS_ROOT",root.c_str(),1);
    ToolBus bus; register_builtin_process_tools_if_configured(bus);
    auto object_store=std::make_shared<distributed::FilesystemObjectStore>(root/"objects");
    configure_network_tool_object_store(object_store,"test");
    for(const auto* name:{"Bash","Python","CMake","Make","Curl","Wget"}) assert(bus.get_tool_info(name));
    const auto exported=bus.export_as_llm_tools();
    for(const auto* alias:{"bash","python3","curl","wget"}) {
        assert(bus.get_tool_info(alias));
        for(const auto& meta:exported) assert(meta.name!=alias);
    }
    auto r=bus.call_tool("Bash",{{"command","printf typed > result.txt"},{"timeout_ms",5000}}).get();
    assert(r.value("exit_code",-1)==0 && fs::is_regular_file(root/"result.txt"));
    r=bus.call_tool("Python",{{"args",{"-c","print('ok')"}},{"timeout_ms",5000}}).get();
    assert(r.value("exit_code",-1)==0 && r.value("stdout","").find("ok")!=std::string::npos);
    r=bus.call_tool("Curl",{{"url","https://example.invalid"},
                             {"headers",{{"Authorization","raw-secret"}}}}).get();
    assert(r.contains("error") && r.at("error").is_object());
    assert(r.at("error").at("code")=="sensitive_header_forbidden");
    r=bus.call_tool("Curl",{{"url","https://example.invalid"},
                             {"credential_ref","vault://missing"}}).get();
    assert(r.contains("error") && r.at("error").is_object());
    assert(r.at("error").at("code")=="credential_unavailable");
    r=bus.call_tool("Wget",{{"url","https://example.invalid"},{"method","POST"},
                             {"output_path","x"}}).get();
    assert(r.contains("error") && r.at("error").is_object());
    assert(r.at("error").at("code")=="method_disallowed");
    r=bus.call_tool("Curl",{{"url","https://example.invalid"},{"method","POST"},
                             {"retry",1}}).get();
    assert(r.at("error").at("code")=="idempotency_key_required");
    ::unsetenv("AGENT_FS_ROOT"); fs::remove_all(root,ec);
    std::clog<<"test_process_tools: ok\n";
    return 0;
#endif
}

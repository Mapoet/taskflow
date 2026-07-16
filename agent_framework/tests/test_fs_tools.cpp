/**
 * @file test_fs_tools.cpp
 * @brief 内建 fs_* 工具：根监禁、读写删、grep、replace（无网络）
 */

#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p);
    assert(f.good());
    f << content;
}

} // namespace

int main() {
#if defined(_WIN32)
    std::clog << "test_fs_tools: skip (paths POSIX-oriented)\n";
    return 0;
#else
    using namespace agent_framework;
    using json = nlohmann::json;

    const fs::path root = fs::temp_directory_path() / "agent_fs_tools_test";
    const fs::path outside = fs::temp_directory_path() / "agent_fs_tools_outside";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    assert(fs::create_directories(root));
    assert(fs::create_directories(root / "sub"));
    assert(fs::create_directories(outside));
    write_file(root / "hello.txt", "hello world\nline2\n");
    write_file(root / "sub" / "a.cpp", "int x = 1;\n");
    write_file(outside / "secret.txt", "secret");

    const std::string root_s = root.string();
    (void)::setenv("AGENT_FS_ROOT", root_s.c_str(), 1);
    (void)::setenv("AGENT_FS_MAX_READ_BYTES", "65536", 1);
    (void)::setenv("AGENT_FS_MAX_WRITE_BYTES", "65536", 1);
    (void)::setenv("AGENT_FS_SEARCH_MAX_RESULTS", "100", 1);
    (void)::setenv("AGENT_FS_MAX_GREP_FILES", "50", 1);
    (void)::setenv("AGENT_FS_MAX_GREP_MATCHES", "100", 1);

    ToolBus bus;
    register_builtin_fs_tools_if_configured(bus);
    register_builtin_fs_tools_if_configured(bus); // idempotent

    assert(bus.get_tool_info("fs_read").has_value());
    assert(bus.get_tool_info("fs_replace").has_value());

    {
        json r = bus.call_tool("fs_read", json{{"path", "hello.txt"}}).get();
        assert(r.contains("content"));
        assert(r["content"].get<std::string>().find("hello") != std::string::npos);
    }

    {
        json r = bus.call_tool("fs_read",
                               json{{"path", "../" + outside.filename().string() + "/secret.txt"}})
                     .get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "path_outside_root");
    }

    {
        json r = bus
                     .call_tool("fs_write",
                                json{{"path", "new.txt"},
                                     {"content", "abc"},
                                     {"confirm_overwrite", false}})
                     .get();
        assert(r.contains("written"));
        json r2 = bus.call_tool("fs_read", json{{"path", "new.txt"}}).get();
        assert(r2["content"].get<std::string>() == "abc");
    }

    {
        json r = bus
                     .call_tool("fs_write",
                                json{{"path", "new.txt"},
                                     {"content", "xyz"},
                                     {"confirm_overwrite", false}})
                     .get();
        assert(r.contains("error"));
        json r2 = bus
                     .call_tool("fs_write",
                                json{{"path", "new.txt"},
                                     {"content", "xyz"},
                                     {"confirm_overwrite", true}})
                     .get();
        assert(r2.contains("written"));
    }

    {
        json r = bus.call_tool("fs_grep", json{{"regex", "hello"}, {"root_path", "."}}).get();
        assert(r.contains("matches"));
        assert(r["matches"].size() >= 1);
    }

    {
        json r = bus.call_tool("fs_replace",
                               json{{"path", "hello.txt"},
                                    {"old_string", "world"},
                                    {"new_string", "W"},
                                    {"dry_run", true}})
                     .get();
        assert(r.contains("dry_run") && r["dry_run"].get<bool>());
        assert(r["match_count"].get<std::size_t>() >= 1);
    }

    {
        json r = bus.call_tool("fs_replace",
                               json{{"path", "hello.txt"},
                                    {"old_string", "world"},
                                    {"new_string", "W"},
                                    {"dry_run", false},
                                    {"confirm_write", false}})
                     .get();
        assert(r.contains("error"));

        json r2 = bus.call_tool("fs_replace",
                                json{{"path", "hello.txt"},
                                     {"old_string", "world"},
                                     {"new_string", "W"},
                                     {"dry_run", false},
                                     {"confirm_write", true}})
                     .get();
        assert(r2.contains("replaced"));
    }

    {
        json r = bus.call_tool("fs_mkdir", json{{"path", "d1"}, {"parents", true}}).get();
        assert(r.contains("created"));
    }

    {
        write_file(root / "sub" / "keep.txt", "k");
        json r = bus.call_tool("fs_delete", json{{"path", "sub"}, {"confirm", true}}).get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "directory_not_empty");
    }

    {
        json r = bus.call_tool("fs_delete", json{{"path", "new.txt"}, {"confirm", false}}).get();
        assert(r.contains("error") && r["error"]["code"] == "confirm_required");
    }

    {
        json r = bus.call_tool("fs_delete", json{{"path", "new.txt"}, {"confirm", true}}).get();
        assert(r.contains("deleted"));
    }

    (void)::unsetenv("AGENT_FS_ROOT");
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);

    std::clog << "test_fs_tools: ok\n";
    return 0;
#endif
}

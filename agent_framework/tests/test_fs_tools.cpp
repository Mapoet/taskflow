/**
 * @file test_fs_tools.cpp
 * @brief 内建 fs_* 工具：根监禁、读写删、grep、replace（无网络）
 */

#include <agent/toolbus/fs_tools.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cassert>
#include <algorithm>
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
    assert(bus.get_tool_info("Read").has_value());
    assert(bus.get_tool_info("Edit").has_value());
    const auto exported = bus.export_as_llm_tools();
    const auto has_export = [&](std::string_view name) {
        return std::any_of(exported.begin(), exported.end(),
                           [&](const ToolMeta& meta) { return meta.name == name; });
    };
    assert(has_export("Read") && has_export("Write") && has_export("Edit"));
    assert(has_export("Glob") && has_export("Grep") && has_export("LS"));
    assert(has_export("Cat") && has_export("Sed"));
    assert(has_export("Mkdir") && has_export("Touch") && has_export("Remove"));
    assert(!has_export("fs_read") && !has_export("fs_replace"));
    assert(!has_export("fs_mkdir") && !has_export("fs_delete"));

    {
        json r = bus.call_tool("Read", json{{"path", "hello.txt"}}).get();
        assert(r.contains("content"));
        assert(r["content"].get<std::string>().find("hello") != std::string::npos);
    }

    {
        json r = bus.call_tool("Cat", json{{"path", "hello.txt"}}).get();
        assert(r.at("content").get<std::string>().find("hello") != std::string::npos);
        assert(r.contains("revision"));
        const auto revision = r.at("revision").get<std::string>();
        const auto slice = bus.call_tool("Read", json{{"path", "hello.txt"},
                                                       {"offset", 0}, {"limit", 5}}).get();
        assert(slice.at("content") == "hello" && slice.at("truncated") == true);
        r = bus.call_tool("LS", json{{"path", "."}, {"depth", 1}}).get();
        assert(r.contains("entries"));
        r = bus.call_tool("Sed", json{{"path", "hello.txt"}, {"pattern", "hello"},
                                      {"replacement", "HELLO"}}).get();
        assert(r.value("dry_run", false));
        r = bus.call_tool("Sed", json{{"path", "hello.txt"}, {"pattern", "hello"},
                                      {"replacement", "hello"}, {"write", true},
                                      {"confirm_write", true},
                                      {"expected_revision", "sha256:stale"}}).get();
        assert(r.at("error").at("code") == "revision_conflict");
        r = bus.call_tool("Sed", json{{"path", "hello.txt"}, {"pattern", "hello"},
                                      {"replacement", "hello"}, {"write", true},
                                      {"confirm_write", true},
                                      {"expected_revision", revision}}).get();
        assert(r.value("replaced", false) && r.contains("revision"));
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
        json r = bus.call_tool("Mkdir", json{{"path", "d1/nested"}, {"parents", true}}).get();
        assert(r.value("created", false));
        r = bus.call_tool("Touch", json{{"path", "d1/nested/touched.txt"}}).get();
        assert(r.value("touched", false) && r.value("created", false));
    }

    {
        write_file(root / "sub" / "keep.txt", "k");
        json r = bus.call_tool("Remove", json{{"path", "sub"}, {"confirm_remove", true}}).get();
        assert(r.contains("error"));
        assert(r["error"]["code"] == "directory_not_empty");
    }

    {
        json r = bus.call_tool("Remove", json{{"path", "new.txt"}, {"confirm_remove", false}}).get();
        assert(r.contains("error") && r["error"]["code"] == "confirm_required");
    }

    {
        json r = bus.call_tool("Remove", json{{"path", "new.txt"}, {"confirm_remove", true}}).get();
        assert(r.value("removed", false));
        r = bus.call_tool("Remove", json{{"path", "."}, {"recursive", true}, {"confirm_remove", true}}).get();
        assert(r.at("error").at("code") == "root_remove_refused");
        r = bus.call_tool("Remove", json{{"path", "d1"}, {"recursive", true}, {"confirm_remove", true}}).get();
        assert(r.value("removed", false));
    }

    (void)::unsetenv("AGENT_FS_ROOT");
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);

    std::clog << "test_fs_tools: ok\n";
    return 0;
#endif
}

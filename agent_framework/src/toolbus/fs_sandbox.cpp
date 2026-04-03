/**
 * @file fs_sandbox.cpp
 * @brief AGENT_FS_ROOT 路径监禁与配置解析
 */

#include <agent/fs_sandbox.hpp>

#include <cstdlib>
#include <cstring>

namespace agent_framework {

namespace fs = std::filesystem;

namespace {

std::size_t parse_size_env(const char* key, std::size_t default_v) {
    const char* e = std::getenv(key);
    if (!e || !*e) {
        return default_v;
    }
    char* end = nullptr;
    unsigned long v = std::strtoul(e, &end, 10);
    if (end == e || v == 0UL) {
        return default_v;
    }
    return static_cast<std::size_t>(v);
}

} // namespace

json fs_tool_error(const std::string& code, const std::string& message) {
    return json{{"error", json{{"code", code}, {"message", message}}}};
}

bool fs_is_path_inside_root(const fs::path& target, const fs::path& root_canon) {
    std::error_code ec;
    fs::path t = fs::weakly_canonical(target, ec);
    if (ec) {
        return false;
    }
    fs::path r = fs::weakly_canonical(root_canon, ec);
    if (ec) {
        return false;
    }
    std::string ts = t.string();
    std::string rs = r.string();
    if (ts == rs) {
        return true;
    }
    if (ts.size() <= rs.size()) {
        return false;
    }
    char sep = static_cast<char>(fs::path::preferred_separator);
    if (ts[rs.size()] != sep) {
        return false;
    }
    return ts.compare(0, rs.size(), rs) == 0;
}

std::optional<fs::path> fs_resolve_under_root(const std::string& rel_or_abs, const fs::path& root,
                                               json& err) {
    std::error_code ec;
    fs::path root_can = fs::weakly_canonical(root, ec);
    if (ec || root_can.empty()) {
        err = fs_tool_error("fs_root_invalid", "AGENT_FS_ROOT is not a valid directory");
        return std::nullopt;
    }

    fs::path combined;
    fs::path raw(rel_or_abs);
    if (raw.is_absolute()) {
        combined = raw;
    } else {
        combined = root_can / raw;
    }

    fs::path canon = fs::weakly_canonical(combined, ec);
    if (ec) {
        err = fs_tool_error("path_resolution_failed", ec.message());
        return std::nullopt;
    }

    if (!fs_is_path_inside_root(canon, root_can)) {
        err = fs_tool_error("path_outside_root", "path escapes AGENT_FS_ROOT");
        return std::nullopt;
    }
    return canon;
}

std::optional<FsSandboxConfig> load_fs_sandbox_config_from_env() {
    const char* root_env = std::getenv("AGENT_FS_ROOT");
    if (!root_env || !*root_env) {
        return std::nullopt;
    }
    std::error_code ec;
    fs::path root_abs = fs::absolute(fs::path(root_env), ec);
    if (ec) {
        return std::nullopt;
    }
    fs::path root_can = fs::weakly_canonical(root_abs, ec);
    if (ec || !fs::is_directory(root_can)) {
        return std::nullopt;
    }

    FsSandboxConfig cfg;
    cfg.root = std::move(root_can);
    cfg.max_read_bytes = parse_size_env("AGENT_FS_MAX_READ_BYTES", 1048576);
    cfg.max_write_bytes = parse_size_env("AGENT_FS_MAX_WRITE_BYTES", cfg.max_read_bytes);
    cfg.max_list_depth = parse_size_env("AGENT_FS_MAX_LIST_DEPTH", 8);
    cfg.max_list_entries = parse_size_env("AGENT_FS_MAX_LIST_ENTRIES", 5000);
    cfg.max_grep_files = parse_size_env("AGENT_FS_MAX_GREP_FILES", 200);
    cfg.max_grep_matches = parse_size_env("AGENT_FS_MAX_GREP_MATCHES", 500);
    cfg.max_line_length = parse_size_env("AGENT_FS_MAX_LINE_LENGTH", 8192);
    cfg.search_max_results = parse_size_env("AGENT_FS_SEARCH_MAX_RESULTS", 500);
    return cfg;
}

} // namespace agent_framework

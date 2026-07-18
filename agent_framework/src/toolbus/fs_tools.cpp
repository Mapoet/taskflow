/**
 * @file fs_tools.cpp
 * @brief 内建 fs_* 工具实现
 */

#include <agent/toolbus/fs_sandbox.hpp>
#include <agent/toolbus/fs_tools.hpp>
#include <agent/context_budget/context_budget.hpp>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <regex>
#include <vector>

namespace agent_framework {

namespace fs = std::filesystem;

namespace {

bool utf8_validate(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c <= 0x7FU) {
            ++i;
            continue;
        }
        const std::size_t rem = s.size() - i;
        if ((c & 0xE0U) == 0xC0U) {
            if (rem < 2) {
                return false;
            }
            if ((static_cast<unsigned char>(s[i + 1]) & 0xC0U) != 0x80U) {
                return false;
            }
            const uint32_t cp =
                (static_cast<uint32_t>(c & 0x1FU) << 6U) |
                static_cast<uint32_t>(s[i + 1] & 0x3FU);
            if (cp < 0x80U) {
                return false;
            }
            i += 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            if (rem < 3) {
                return false;
            }
            if ((static_cast<unsigned char>(s[i + 1]) & 0xC0U) != 0x80U ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0U) != 0x80U) {
                return false;
            }
            const uint32_t cp =
                (static_cast<uint32_t>(c & 0x0FU) << 12U) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6U) |
                static_cast<uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3FU);
            if (cp < 0x800U) {
                return false;
            }
            i += 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            if (rem < 4) {
                return false;
            }
            if ((static_cast<unsigned char>(s[i + 1]) & 0xC0U) != 0x80U ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0U) != 0x80U ||
                (static_cast<unsigned char>(s[i + 3]) & 0xC0U) != 0x80U) {
                return false;
            }
            const uint32_t cp =
                (static_cast<uint32_t>(c & 0x07U) << 18U) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12U) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6U) |
                static_cast<uint32_t>(static_cast<unsigned char>(s[i + 3]) & 0x3FU);
            if (cp < 0x10000U || cp > 0x10FFFFU) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::string to_hex_preview(std::string_view data, std::size_t max_bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    const std::size_t n = std::min(data.size(), max_bytes);
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = static_cast<unsigned char>(data[i]);
        out.push_back(hex[b >> 4U]);
        out.push_back(hex[b & 0xFU]);
    }
    return out;
}

void glob_segment_to_regex(std::string_view seg, std::string& re) {
    for (std::size_t i = 0; i < seg.size(); ++i) {
        const char c = seg[i];
        if (c == '*') {
            re += "[^/]*";
        } else if (c == '?') {
            re += "[^/]";
        } else if (c == '.' || c == '^' || c == '$' || c == '+' || c == '(' || c == ')' ||
                   c == '[' || c == ']' || c == '{' || c == '}' || c == '|' || c == '\\') {
            re.push_back('\\');
            re.push_back(c);
        } else {
            re.push_back(c);
        }
    }
}

std::vector<std::string> split_glob_path(const std::string& pattern_in) {
    std::string pattern = pattern_in;
    for (char& c : pattern) {
        if (c == '\\') {
            c = '/';
        }
    }
    std::vector<std::string> segs;
    std::string cur;
    for (char c : pattern) {
        if (c == '/') {
            if (!cur.empty()) {
                segs.push_back(cur);
            } else if (segs.empty()) {
            }
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        segs.push_back(cur);
    }
    return segs;
}

std::string glob_path_to_regex(const std::string& pattern_in) {
    const std::vector<std::string> segs = split_glob_path(pattern_in);
    std::string re = "^";
    bool prev_was_globstar = false;
    bool first_seg = true;
    for (std::size_t si = 0; si < segs.size(); ++si) {
        const std::string& seg = segs[si];
        if (seg == "**") {
            re += "(?:.*/)*";
            prev_was_globstar = true;
            continue;
        }
        if (!first_seg && !prev_was_globstar) {
            re += "/";
        }
        prev_was_globstar = false;
        first_seg = false;
        glob_segment_to_regex(seg, re);
    }
    re += "$";
    return re;
}

} // namespace

struct FsToolsState {
    FsSandboxConfig cfg;

    explicit FsToolsState(FsSandboxConfig c) : cfg(std::move(c)) {}

    json do_read(const json& j) const {
        json err = json::object();
        const std::string path_str = j.at("path").get<std::string>();
        std::optional<fs::path> path = fs_resolve_under_root(path_str, cfg.root, err);
        if (!path) {
            return err;
        }
        if (!fs::is_regular_file(*path)) {
            return fs_tool_error("not_a_file", path_str);
        }
        std::size_t max_b = cfg.max_read_bytes;
        if (j.contains("max_bytes") && j["max_bytes"].is_number_integer()) {
            const int mb = j["max_bytes"].get<int>();
            if (mb > 0) {
                max_b = std::min(static_cast<std::size_t>(mb), cfg.max_read_bytes);
            }
        } else if (j.contains("max_bytes") && j["max_bytes"].is_number_unsigned()) {
            max_b = std::min(j["max_bytes"].get<std::size_t>(), cfg.max_read_bytes);
        }
        std::error_code ec;
        const auto fsize = fs::file_size(*path, ec);
        if (ec) {
            return fs_tool_error("file_size_failed", ec.message());
        }
        if (fsize > max_b) {
            return fs_tool_error("file_too_large", "file exceeds max_bytes / AGENT_FS_MAX_READ_BYTES");
        }
        std::ifstream in(path->string(), std::ios::binary);
        if (!in) {
            return fs_tool_error("open_failed", "cannot open file for read");
        }
        std::string content(static_cast<std::size_t>(fsize), '\0');
        in.read(content.data(), static_cast<std::streamsize>(fsize));
        if (!in) {
            return fs_tool_error("read_failed", "short read");
        }
        std::string mode = "utf8";
        if (j.contains("mode") && j["mode"].is_string()) {
            mode = j["mode"].get<std::string>();
        }
        if (mode == "binary_preview") {
            const std::size_t cap = std::min(max_b, static_cast<std::size_t>(256));
            return json{{"hex_preview", to_hex_preview(content, cap)},
                        {"truncated", content.size() > cap}};
        }
        if (!utf8_validate(content)) {
            return fs_tool_error("invalid_utf8", "file is not valid UTF-8; use mode binary_preview");
        }
        return json{{"content", content}};
    }

    json do_write(const json& j) const {
        json err = json::object();
        const std::string path_str = j.at("path").get<std::string>();
        std::optional<fs::path> path = fs_resolve_under_root(path_str, cfg.root, err);
        if (!path) {
            return err;
        }
        const std::string content = j.at("content").get<std::string>();
        if (content.size() > cfg.max_write_bytes) {
            return fs_tool_error("content_too_large", "content exceeds AGENT_FS_MAX_WRITE_BYTES");
        }
        const bool confirm_overwrite =
            j.contains("confirm_overwrite") && j["confirm_overwrite"].is_boolean() &&
            j["confirm_overwrite"].get<bool>();
        std::error_code ec;
        if (fs::exists(*path, ec) && fs::is_regular_file(*path, ec)) {
            if (!confirm_overwrite) {
                return fs_tool_error("confirm_required",
                                     "file exists; set confirm_overwrite true to replace");
            }
        }
        if (fs::exists(*path, ec) && fs::is_directory(*path, ec)) {
            return fs_tool_error("is_directory", "cannot write to a directory path");
        }
        fs::path parent = path->parent_path();
        if (!fs::exists(parent, ec)) {
            return fs_tool_error("parent_missing", "parent directory does not exist");
        }
        const std::string tmp =
            path->string() + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return fs_tool_error("open_failed", "cannot open temp file for write");
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!out) {
                fs::remove(tmp, ec);
                return fs_tool_error("write_failed", "short write");
            }
        }
        fs::rename(tmp, *path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return fs_tool_error("rename_failed", ec.message());
        }
        return json{{"written", true}};
    }

    json do_list_dir(const json& j) const {
        json err = json::object();
        const std::string path_str = j.at("path").get<std::string>();
        std::optional<fs::path> base = fs_resolve_under_root(path_str, cfg.root, err);
        if (!base) {
            return err;
        }
        if (!fs::is_directory(*base)) {
            return fs_tool_error("not_a_directory", path_str);
        }
        int depth = 1;
        if (j.contains("depth") && j["depth"].is_number_integer()) {
            depth = j["depth"].get<int>();
        }
        if (depth < 1) {
            depth = 1;
        }
        if (static_cast<std::size_t>(depth) > cfg.max_list_depth) {
            depth = static_cast<int>(cfg.max_list_depth);
        }
        bool include_dot = false;
        if (j.contains("include_dotfiles") && j["include_dotfiles"].is_boolean()) {
            include_dot = j["include_dotfiles"].get<bool>();
        }
        json entries = json::array();
        bool truncated = false;
        std::size_t count = 0;
        std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int current_depth) {
            if (truncated || count >= cfg.max_list_entries) {
                truncated = true;
                return;
            }
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; it != end && !truncated && !ec;
                 it.increment(ec)) {
                if (ec) {
                    break;
                }
                const std::string name = it->path().filename().string();
                if (!include_dot && !name.empty() && name[0] == '.') {
                    continue;
                }
                const fs::path abs = it->path();
                std::string rel = fs::relative(abs, cfg.root, ec).generic_string();
                if (ec) {
                    continue;
                }
                json e = json{{"name", name}, {"path_rel", rel}};
                if (fs::is_directory(abs, ec)) {
                    e["type"] = "dir";
                } else if (fs::is_regular_file(abs, ec)) {
                    e["type"] = "file";
                    e["size"] = fs::file_size(abs, ec);
                } else {
                    e["type"] = "other";
                }
                entries.push_back(std::move(e));
                ++count;
                if (count >= cfg.max_list_entries) {
                    truncated = true;
                    break;
                }
                if (current_depth < depth && fs::is_directory(abs, ec)) {
                    walk(abs, current_depth + 1);
                }
            }
        };
        walk(*base, 1);
        return json{{"entries", std::move(entries)}, {"truncated", truncated}};
    }

    json do_mkdir(const json& j) const {
        json err = json::object();
        const std::string path_str = j.at("path").get<std::string>();
        std::optional<fs::path> path = fs_resolve_under_root(path_str, cfg.root, err);
        if (!path) {
            return err;
        }
        const bool parents =
            j.contains("parents") && j["parents"].is_boolean() && j["parents"].get<bool>();
        std::error_code ec;
        if (parents) {
            fs::create_directories(*path, ec);
        } else {
            fs::create_directory(*path, ec);
        }
        if (ec) {
            return fs_tool_error("mkdir_failed", ec.message());
        }
        return json{{"created", true}};
    }

    json do_delete(const json& j) const {
        json err = json::object();
        const bool confirm = j.contains("confirm") && j["confirm"].is_boolean() && j["confirm"].get<bool>();
        if (!confirm) {
            return fs_tool_error("confirm_required", "confirm must be true");
        }
        const std::string path_str = j.at("path").get<std::string>();
        std::optional<fs::path> path = fs_resolve_under_root(path_str, cfg.root, err);
        if (!path) {
            return err;
        }
        if (j.contains("expected_type") && j["expected_type"].is_string()) {
            const std::string et = j["expected_type"].get<std::string>();
            std::error_code ec;
            if (et == "file" && !fs::is_regular_file(*path, ec)) {
                return fs_tool_error("type_mismatch", "expected file");
            }
            if (et == "dir" && !fs::is_directory(*path, ec)) {
                return fs_tool_error("type_mismatch", "expected dir");
            }
        }
        std::error_code ec;
        if (fs::is_regular_file(*path, ec)) {
            if (!fs::remove(*path, ec)) {
                return fs_tool_error("delete_failed", ec.message());
            }
            return json{{"deleted", true}};
        }
        if (fs::is_directory(*path, ec)) {
            if (fs::is_empty(*path, ec)) {
                if (!fs::remove(*path, ec)) {
                    return fs_tool_error("delete_failed", ec.message());
                }
                return json{{"deleted", true}};
            }
            return fs_tool_error("directory_not_empty", "only empty directories can be deleted");
        }
        return fs_tool_error("not_found", path_str);
    }

    json do_search(const json& j) const {
        json err = json::object();
        std::string pattern = j.at("pattern").get<std::string>();
        std::size_t max_results = cfg.search_max_results;
        if (j.contains("max_results") && j["max_results"].is_number_integer()) {
            const int v = j["max_results"].get<int>();
            if (v > 0) {
                max_results = std::min(static_cast<std::size_t>(v), cfg.search_max_results);
            }
        } else if (j.contains("max_results") && j["max_results"].is_number_unsigned()) {
            max_results =
                std::min(j["max_results"].get<std::size_t>(), cfg.search_max_results);
        }
        std::optional<std::regex> exclude_re;
        if (j.contains("exclude_glob") && j["exclude_glob"].is_string()) {
            const std::string eg = j["exclude_glob"].get<std::string>();
            try {
                exclude_re = std::regex(glob_path_to_regex(eg), std::regex::ECMAScript);
            } catch (const std::regex_error& e) {
                return fs_tool_error("invalid_exclude_glob", e.what());
            }
        }
        std::regex re;
        try {
            re.assign(glob_path_to_regex(pattern), std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            return fs_tool_error("invalid_pattern", e.what());
        }
        json paths = json::array();
        bool truncated = false;
        std::size_t n = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(cfg.root, fs::directory_options::skip_permission_denied, ec),
             end;
             it != end && !ec; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!fs::is_regular_file(it->path(), ec)) {
                continue;
            }
            const fs::path rel = fs::relative(it->path(), cfg.root, ec);
            if (ec) {
                continue;
            }
            const std::string rel_s = rel.generic_string();
            if (!std::regex_match(rel_s, re)) {
                continue;
            }
            if (exclude_re && std::regex_match(rel_s, *exclude_re)) {
                continue;
            }
            paths.push_back(rel_s);
            ++n;
            if (n >= max_results) {
                truncated = true;
                break;
            }
        }
        return json{{"paths", std::move(paths)}, {"truncated", truncated}};
    }

    json do_grep(const json& j) const {
        json err = json::object();
        std::string root_rel = ".";
        if (j.contains("root_path") && j["root_path"].is_string()) {
            root_rel = j["root_path"].get<std::string>();
        }
        std::optional<fs::path> walk_root = fs_resolve_under_root(root_rel, cfg.root, err);
        if (!walk_root) {
            return err;
        }
        if (!fs::is_directory(*walk_root)) {
            return fs_tool_error("not_a_directory", root_rel);
        }
        const std::string regex_str = j.at("regex").get<std::string>();
        std::regex re;
        try {
            re.assign(regex_str, std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            return fs_tool_error("invalid_regex", e.what());
        }
        std::optional<std::regex> file_filter;
        if (j.contains("file_glob") && j["file_glob"].is_string()) {
            try {
                file_filter =
                    std::regex(glob_path_to_regex(j["file_glob"].get<std::string>()), std::regex::ECMAScript);
            } catch (const std::regex_error& e) {
                return fs_tool_error("invalid_file_glob", e.what());
            }
        }
        std::size_t max_files = cfg.max_grep_files;
        std::size_t max_matches = cfg.max_grep_matches;
        if (j.contains("max_files") && j["max_files"].is_number_integer()) {
            const int v = j["max_files"].get<int>();
            if (v > 0) {
                max_files = std::min(static_cast<std::size_t>(v), cfg.max_grep_files);
            }
        } else if (j.contains("max_files") && j["max_files"].is_number_unsigned()) {
            max_files = std::min(j["max_files"].get<std::size_t>(), cfg.max_grep_files);
        }
        if (j.contains("max_matches") && j["max_matches"].is_number_integer()) {
            const int v = j["max_matches"].get<int>();
            if (v > 0) {
                max_matches = std::min(static_cast<std::size_t>(v), cfg.max_grep_matches);
            }
        } else if (j.contains("max_matches") && j["max_matches"].is_number_unsigned()) {
            max_matches = std::min(j["max_matches"].get<std::size_t>(), cfg.max_grep_matches);
        }
        int ctx_lines = 0;
        if (j.contains("context_lines") && j["context_lines"].is_number_integer()) {
            ctx_lines = j["context_lines"].get<int>();
            if (ctx_lines < 0) {
                ctx_lines = 0;
            }
            if (ctx_lines > 2) {
                ctx_lines = 2;
            }
        }
        json matches = json::array();
        bool truncated = false;
        std::size_t files_opened = 0;
        std::size_t match_count = 0;
        std::size_t skipped_binary_lines = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(*walk_root, fs::directory_options::skip_permission_denied, ec),
             end;
             it != end && !ec; it.increment(ec)) {
            if (ec || match_count >= max_matches || files_opened >= max_files) {
                truncated = true;
                break;
            }
            if (!fs::is_regular_file(it->path(), ec)) {
                continue;
            }
            const fs::path rel = fs::relative(it->path(), cfg.root, ec);
            if (ec) {
                continue;
            }
            const std::string rel_s = rel.generic_string();
            if (file_filter && !std::regex_match(rel_s, *file_filter)) {
                continue;
            }
            ++files_opened;
            std::ifstream in(it->path());
            if (!in) {
                continue;
            }
            std::deque<std::string> prev;
            std::string line;
            std::size_t line_no = 0;
            while (match_count < max_matches && std::getline(in, line)) {
                ++line_no;
                if (line.size() > cfg.max_line_length) {
                    line = utf8_safe_truncate(line, cfg.max_line_length);
                    line += "…";
                }
                if (!utf8_validate(line)) {
                    ++skipped_binary_lines;
                    prev.clear();
                    continue;
                }
                if (std::regex_search(line, re)) {
                    json m = json{{"path", rel_s}, {"line", line_no}, {"text", line}};
                    if (ctx_lines > 0 && !prev.empty()) {
                        json arr = json::array();
                        for (const std::string& p : prev) {
                            arr.push_back(p);
                        }
                        m["context_before"] = std::move(arr);
                    }
                    matches.push_back(std::move(m));
                    ++match_count;
                }
                if (ctx_lines > 0) {
                    prev.push_back(line);
                    while (prev.size() > static_cast<std::size_t>(ctx_lines)) {
                        prev.pop_front();
                    }
                }
            }
        }
        if (files_opened >= max_files && match_count < max_matches) {
            truncated = true;
        }
        return json{{"matches", std::move(matches)},
                    {"truncated", truncated},
                    {"skipped_binary_lines", skipped_binary_lines}};
    }

    json do_replace(const json& j) const {
        json err = json::object();
        const std::string path_str = j.at("path").get<std::string>();
        std::optional<fs::path> path = fs_resolve_under_root(path_str, cfg.root, err);
        if (!path) {
            return err;
        }
        if (!fs::is_regular_file(*path)) {
            return fs_tool_error("not_a_file", path_str);
        }
        const std::string old_s = j.at("old_string").get<std::string>();
        const std::string new_s = j.at("new_string").get<std::string>();
        const bool replace_all =
            j.contains("replace_all") && j["replace_all"].is_boolean() && j["replace_all"].get<bool>();
        bool dry_run = true;
        if (j.contains("dry_run") && j["dry_run"].is_boolean()) {
            dry_run = j["dry_run"].get<bool>();
        }
        const bool confirm_write =
            j.contains("confirm_write") && j["confirm_write"].is_boolean() &&
            j["confirm_write"].get<bool>();
        if (!dry_run && !confirm_write) {
            return fs_tool_error("confirm_required", "dry_run false requires confirm_write true");
        }
        std::error_code ec;
        const auto fsize = fs::file_size(*path, ec);
        if (ec || fsize > cfg.max_write_bytes) {
            return fs_tool_error("file_too_large", "file too large for replace");
        }
        std::ifstream in(path->string(), std::ios::binary);
        std::string content(static_cast<std::size_t>(fsize), '\0');
        in.read(content.data(), static_cast<std::streamsize>(fsize));
        if (!in) {
            return fs_tool_error("read_failed", "short read");
        }
        if (!utf8_validate(content)) {
            return fs_tool_error("invalid_utf8", "file must be UTF-8 text for fs_replace");
        }
        if (old_s.empty()) {
            return fs_tool_error("invalid_old_string", "old_string must be non-empty");
        }
        std::vector<std::size_t> positions;
        for (std::size_t pos = 0;;) {
            const std::size_t found = content.find(old_s, pos);
            if (found == std::string::npos) {
                break;
            }
            positions.push_back(found);
            if (!replace_all) {
                break;
            }
            pos = found + old_s.size();
        }
        json previews = json::array();
        constexpr std::size_t k_prev = 40;
        for (std::size_t i = 0; i < positions.size() && i < 10; ++i) {
            const std::size_t pos = positions[i];
            const std::size_t start = pos > k_prev ? pos - k_prev : 0;
            const std::size_t len = std::min(content.size() - start, k_prev * 2 + old_s.size());
            previews.push_back(json{{"offset", pos}, {"snippet", content.substr(start, len)}});
        }
        if (dry_run) {
            return json{{"dry_run", true},
                        {"match_count", positions.size()},
                        {"previews", std::move(previews)}};
        }
        if (positions.empty()) {
            return fs_tool_error("no_match", "old_string not found");
        }
        std::string out_content;
        if (replace_all) {
            std::size_t p = 0;
            for (std::size_t pos : positions) {
                out_content.append(content, p, pos - p);
                out_content += new_s;
                p = pos + old_s.size();
            }
            out_content.append(content, p, std::string::npos);
        } else {
            const std::size_t pos = positions[0];
            out_content = content.substr(0, pos);
            out_content += new_s;
            out_content.append(content, pos + old_s.size(), std::string::npos);
        }
        if (out_content.size() > cfg.max_write_bytes) {
            return fs_tool_error("result_too_large", "result exceeds AGENT_FS_MAX_WRITE_BYTES");
        }
        const std::string tmp =
            path->string() + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return fs_tool_error("open_failed", "cannot open temp file");
            }
            out.write(out_content.data(), static_cast<std::streamsize>(out_content.size()));
            if (!out) {
                fs::remove(tmp, ec);
                return fs_tool_error("write_failed", "short write");
            }
        }
        fs::rename(tmp, *path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return fs_tool_error("rename_failed", ec.message());
        }
        return json{{"replaced", true}, {"match_count", positions.size()}};
    }
};

static void register_fs_tools_impl(ToolBus& bus, const FsSandboxConfig& cfg) {
    auto state = std::make_shared<FsToolsState>(cfg);

    {
        ToolMeta meta;
        meta.name = "fs_read";
        meta.description =
            "Read a file under AGENT_FS_ROOT. mode utf8 (default) or binary_preview (hex).";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "max_bytes": {"type": "integer"},
                "mode": {"type": "string"}
            },
            "required": ["path"]
        })");
        meta.side_effect = ToolSideEffect::ReadOnly;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemRead, "path", cfg.root.string(), {}});
        bus.register_local_tool(
            "fs_read",
            [state](const json& j) { return state->do_read(j); },
            meta);
    }
    {
        ToolMeta meta;
        meta.name = "fs_write";
        meta.description = "Write content to a file atomically under AGENT_FS_ROOT. confirm_overwrite "
                           "required if file exists.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "content": {"type": "string"},
                "confirm_overwrite": {"type": "boolean"}
            },
            "required": ["path", "content"]
        })");
        meta.side_effect = ToolSideEffect::Write;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemWrite, "path", cfg.root.string(), {}});
        bus.register_local_tool(
            "fs_write",
            [state](const json& j) { return state->do_write(j); },
            meta);
    }
    {
        ToolMeta meta;
        meta.name = "fs_list_dir";
        meta.description = "List directory entries under AGENT_FS_ROOT (recursive up to depth).";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "depth": {"type": "integer"},
                "include_dotfiles": {"type": "boolean"}
            },
            "required": ["path"]
        })");
        meta.side_effect = ToolSideEffect::ReadOnly;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemRead, "path", cfg.root.string(), {}});
        bus.register_local_tool(
            "fs_list_dir",
            [state](const json& j) { return state->do_list_dir(j); },
            meta);
    }
    {
        ToolMeta meta;
        meta.name = "fs_mkdir";
        meta.description = "Create directory under AGENT_FS_ROOT. parents=true for mkdir -p.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "parents": {"type": "boolean"}
            },
            "required": ["path"]
        })");
        meta.side_effect = ToolSideEffect::Write;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemWrite, "path", cfg.root.string(), {}});
        bus.register_local_tool(
            "fs_mkdir",
            [state](const json& j) { return state->do_mkdir(j); },
            meta);
    }
    {
        ToolMeta meta;
        meta.name = "fs_delete";
        meta.description =
            "Delete file or empty directory under AGENT_FS_ROOT. confirm must be true.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "confirm": {"type": "boolean"},
                "expected_type": {"type": "string"}
            },
            "required": ["path", "confirm"]
        })");
        meta.side_effect = ToolSideEffect::Write;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemWrite, "path", cfg.root.string(), {}});
        bus.register_local_tool(
            "fs_delete",
            [state](const json& j) { return state->do_delete(j); },
            meta);
    }
    {
        ToolMeta meta;
        meta.name = "fs_search";
        meta.description =
            "Find files matching glob pattern (relative to root). Optional exclude_glob.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "pattern": {"type": "string"},
                "max_results": {"type": "integer"},
                "exclude_glob": {"type": "string"}
            },
            "required": ["pattern"]
        })");
        meta.side_effect = ToolSideEffect::ReadOnly;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemRead, {}, cfg.root.string(), cfg.root.string()});
        bus.register_local_tool(
            "fs_search",
            [state](const json& j) { return state->do_search(j); },
            meta);
    }
    {
        ToolMeta meta;
        meta.name = "fs_grep";
        meta.description = "Search file contents with regex (UTF-8 lines; skips invalid UTF-8 lines).";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "root_path": {"type": "string"},
                "regex": {"type": "string"},
                "file_glob": {"type": "string"},
                "max_files": {"type": "integer"},
                "max_matches": {"type": "integer"},
                "context_lines": {"type": "integer"}
            },
            "required": ["regex"]
        })");
        meta.side_effect = ToolSideEffect::ReadOnly;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemRead, "root_path", cfg.root.string(), {}});
        bus.register_local_tool(
            "fs_grep",
            [state](const json& j) { return state->do_grep(j); },
            meta);
    }
    {
        ToolMeta meta;
        meta.name = "fs_replace";
        meta.description =
            "Replace old_string with new_string in a UTF-8 file. dry_run default true; set "
            "confirm_write true when dry_run false.";
        meta.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "old_string": {"type": "string"},
                "new_string": {"type": "string"},
                "replace_all": {"type": "boolean"},
                "dry_run": {"type": "boolean"},
                "confirm_write": {"type": "boolean"}
            },
            "required": ["path", "old_string", "new_string"]
        })");
        meta.side_effect = ToolSideEffect::Write;
        meta.permission_targets.push_back(
            {ToolMeta::PermissionTargetKind::FilesystemWrite, "path", cfg.root.string(), {}});
        bus.register_local_tool(
            "fs_replace",
            [state](const json& j) { return state->do_replace(j); },
            meta);
    }
}

void register_builtin_fs_tools_if_configured(ToolBus& bus) {
    if (bus.get_tool_info("fs_read").has_value()) {
        return;
    }
    std::optional<FsSandboxConfig> cfg = load_fs_sandbox_config_from_env();
    if (!cfg.has_value()) {
        return;
    }
    register_fs_tools_impl(bus, *cfg);
}

} // namespace agent_framework

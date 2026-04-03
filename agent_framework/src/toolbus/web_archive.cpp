/**
 * @file web_archive.cpp
 * @brief web_fetch_archive：下载 ZIP 并解压到 AGENT_FS_ROOT 下（Zip Slip 防护；deflate 需 zlib）
 */

#include <agent/fs_sandbox.hpp>
#include <agent/web_http.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef AGENT_HAVE_ZLIB
#include <zlib.h>
#endif

namespace agent_framework {
namespace fs = std::filesystem;

namespace {

std::uint16_t u16le(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8U));
}

std::uint32_t u32le(const unsigned char* p) {
    return static_cast<std::uint32_t>(
        static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
        (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U));
}

std::size_t find_eocd(const std::string& z) {
    if (z.size() < 22) {
        return std::string::npos;
    }
    const std::size_t scan = std::min<std::size_t>(65536 + 22, z.size());
    for (std::size_t back = 0; back < scan - 21 && back < z.size(); ++back) {
        const std::size_t i = z.size() - 22 - back;
        if (z[i] == 'P' && z[i + 1] == 'K' && static_cast<unsigned char>(z[i + 2]) == 5 &&
            static_cast<unsigned char>(z[i + 3]) == 6) {
            return i;
        }
    }
    return std::string::npos;
}

bool is_safe_member_name(const std::string& name, std::string& err) {
    if (name.empty() || name.back() == '/' || name.back() == '\\') {
        err = "directory_or_empty";
        return false;
    }
    fs::path p(name);
    for (const auto& comp : p) {
        const std::string part = comp.string();
        if (part == ".." || part.empty()) {
            err = "path_escape";
            return false;
        }
    }
    if (p.is_absolute()) {
        err = "absolute_forbidden";
        return false;
    }
    return true;
}

#ifdef AGENT_HAVE_ZLIB
bool inflate_raw(const unsigned char* src, std::size_t src_len, std::vector<unsigned char>& dest,
                 std::size_t dest_cap, std::string& err) {
    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
    strm.avail_in = static_cast<uInt>(src_len);
    dest.resize(dest_cap);
    strm.next_out = dest.data();
    strm.avail_out = static_cast<uInt>(dest_cap);
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        err = "zlib_init";
        return false;
    }
    const int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        err = "inflate_failed";
        return false;
    }
    dest.resize(dest_cap - strm.avail_out);
    return true;
}
#endif

std::size_t env_size_arch(const char* key, std::size_t d) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return d;
    }
    const long n = std::strtol(v, nullptr, 10);
    if (n <= 0) {
        return d;
    }
    return static_cast<std::size_t>(n);
}

std::string random_job_id() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::stringstream ss;
    ss << std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()
       << "_" << (gen() & 0xffffffffULL);
    return ss.str();
}

} // namespace

json web_fetch_archive_invoke(const json& j) {
    if (!j.contains("url") || !j["url"].is_string()) {
        return web_tool_error("invalid_url", "missing url");
    }
    const std::string url = j["url"].get<std::string>();

    std::optional<FsSandboxConfig> fs_cfg = load_fs_sandbox_config_from_env();
    if (!fs_cfg.has_value()) {
        return web_tool_error("fs_root_required",
                              "web_fetch_archive requires AGENT_FS_ROOT and a valid sandbox");
    }

    const std::size_t max_files = env_size_arch("AGENT_WEB_MAX_ARCHIVE_FILES", 1000);
    const std::size_t max_unc = env_size_arch("AGENT_WEB_MAX_ARCHIVE_UNCOMPRESSED_BYTES", 52428800);
    const std::size_t max_single = env_size_arch("AGENT_WEB_MAX_ARCHIVE_SINGLE_FILE_BYTES", 10485760);

    std::string subdir = "web_extract";
    if (j.contains("subdir") && j["subdir"].is_string()) {
        subdir = j["subdir"].get<std::string>();
    }
    const char* env_sub = std::getenv("AGENT_WEB_EXTRACT_SUBDIR");
    if (env_sub && *env_sub) {
        subdir = env_sub;
    }

    WebHttpConfig wcfg = load_web_http_config_from_env();
    wcfg.max_body_bytes = env_size_arch("AGENT_WEB_MAX_ARCHIVE_DOWNLOAD_BYTES", wcfg.max_body_bytes);
    const auto hres = web_http_get(url, wcfg, {});
    if (!hres.error_code.empty()) {
        json e = json{{"error", json{{"code", hres.error_code}}}};
        if (hres.error_http_status != 0) {
            e["error"]["status"] = hres.error_http_status;
        }
        return e;
    }

    const std::string& z = hres.body;
    const std::size_t eocd = find_eocd(z);
    if (eocd == std::string::npos) {
        return web_tool_error("unsupported_media_type", "not a zip");
    }
    const unsigned char* ez = reinterpret_cast<const unsigned char*>(z.data() + eocd);
    // EOCD (without Zip64): offset 10 = total CD records; 16-19 = offset of CD.
    const std::uint16_t cd_total = u16le(ez + 10);
    const std::uint32_t cd_off = u32le(ez + 16);
    if (cd_off >= z.size()) {
        return web_tool_error("unsupported_media_type", "bad zip eocd");
    }

    std::error_code ec;
    fs::path root_can = fs::weakly_canonical(fs_cfg->root, ec);
    if (ec || root_can.empty()) {
        return web_tool_error("fs_root_invalid");
    }
    const fs::path extract_base = root_can / subdir / random_job_id();
    fs::create_directories(extract_base, ec);
    if (ec) {
        return web_tool_error("archive_path_escape", ec.message());
    }

    json files = json::array();
    std::size_t total_unc_written = 0;
    std::size_t nfile = 0;
    std::size_t cd_pos = cd_off;
    for (std::uint16_t idx = 0; idx < cd_total; ++idx) {
        if (nfile >= max_files) {
            return web_tool_error("archive_too_many_files");
        }
        if (cd_pos + 46 > z.size()) {
            break;
        }
        const unsigned char* cd = reinterpret_cast<const unsigned char*>(z.data() + cd_pos);
        if (cd[0] != 'P' || cd[1] != 'K' || cd[2] != 1 || cd[3] != 2) {
            break;
        }
        const std::uint16_t method = u16le(cd + 10);
        const std::uint32_t comp_size_cd = u32le(cd + 20);
        const std::uint32_t unc_size_cd = u32le(cd + 24);
        (void)comp_size_cd;
        (void)unc_size_cd;
        const std::uint16_t n = u16le(cd + 28);
        const std::uint16_t m = u16le(cd + 30);
        const std::uint16_t k = u16le(cd + 32);
        const std::uint32_t local_hdr_off = u32le(cd + 42);
        if (cd_pos + 46U + n + m + k > z.size()) {
            return web_tool_error("unsupported_media_type", "bad central directory");
        }
        const std::string name(reinterpret_cast<const char*>(cd + 46), n);
        cd_pos += 46U + n + m + k;

        if (name.empty() || name.back() == '/') {
            continue;
        }
        std::string se;
        if (!is_safe_member_name(name, se)) {
            return web_tool_error("archive_path_escape", se);
        }
        if (unc_size_cd > max_single) {
            return web_tool_error("archive_uncompressed_too_large");
        }
        if (total_unc_written + unc_size_cd > max_unc) {
            return web_tool_error("archive_uncompressed_too_large");
        }
        if (local_hdr_off + 30 > z.size()) {
            return web_tool_error("unsupported_media_type", "bad local header offset");
        }
        const unsigned char* lh = reinterpret_cast<const unsigned char*>(z.data() + local_hdr_off);
        if (lh[0] != 'P' || lh[1] != 'K' || lh[2] != 3 || lh[3] != 4) {
            return web_tool_error("unsupported_media_type", "bad local header");
        }
        const std::uint16_t n2 = u16le(lh + 26);
        const std::uint16_t m2 = u16le(lh + 28);
        const std::uint32_t comp_size = u32le(lh + 18);
        const std::uint32_t unc_size = u32le(lh + 22);
        if (unc_size > max_single) {
            return web_tool_error("archive_uncompressed_too_large");
        }
        if (total_unc_written + unc_size > max_unc) {
            return web_tool_error("archive_uncompressed_too_large");
        }
        const std::size_t data_off = local_hdr_off + 30U + n2 + m2;
        if (data_off + comp_size > z.size()) {
            return web_tool_error("unsupported_media_type", "truncated zip");
        }
        const unsigned char* data = reinterpret_cast<const unsigned char*>(z.data() + data_off);

        std::vector<unsigned char> unc_bytes;
        const unsigned char* write_ptr = nullptr;
        std::size_t write_len = 0;
        if (method == 0) {
            if (comp_size != unc_size) {
                return web_tool_error("unsupported_media_type", "stored size mismatch");
            }
            write_ptr = data;
            write_len = comp_size;
        } else if (method == 8) {
#ifdef AGENT_HAVE_ZLIB
            if (unc_size > static_cast<std::uint32_t>(std::numeric_limits<uInt>::max())) {
                return web_tool_error("archive_uncompressed_too_large");
            }
            if (!inflate_raw(data, comp_size, unc_bytes, unc_size, se)) {
                return web_tool_error("unsupported_media_type", se);
            }
            write_ptr = unc_bytes.data();
            write_len = unc_bytes.size();
            if (write_len != unc_size) {
                return web_tool_error("unsupported_media_type", "inflate size mismatch");
            }
#else
            return web_tool_error("unsupported_media_type", "deflate needs zlib (AGENT_HAVE_ZLIB)");
#endif
        } else {
            return web_tool_error("unsupported_media_type", "compression method not supported");
        }

        const fs::path out_path = (extract_base / name).lexically_normal();
        if (!fs_is_path_inside_root(out_path, extract_base)) {
            return web_tool_error("archive_path_escape");
        }
        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            return web_tool_error("archive_path_escape", ec.message());
        }
        {
            std::ofstream of(out_path, std::ios::binary);
            if (!of) {
                return web_tool_error("archive_path_escape", "open failed");
            }
            of.write(reinterpret_cast<const char*>(write_ptr), static_cast<std::streamsize>(write_len));
            if (!of) {
                return web_tool_error("archive_path_escape", "write failed");
            }
        }
        total_unc_written += write_len;
        fs::path rel_to_root = fs::relative(out_path, root_can, ec);
        if (ec) {
            rel_to_root = out_path;
        }
        files.push_back(json{{"path", rel_to_root.generic_string()}, {"size", write_len}});
        ++nfile;
    }

    json out = json::object();
    out["url"] = url;
    out["extract_dir"] = (fs::path(subdir) / extract_base.filename()).generic_string();
    out["files"] = std::move(files);
    out["total_uncompressed_bytes"] = total_unc_written;
    out["total_files"] = out["files"].size();
    out["truncated_manifest"] = false;
    return out;
}

} // namespace agent_framework

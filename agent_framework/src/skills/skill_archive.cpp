#include <agent/skills/skill_archive.hpp>
#include <agent/skills/skill_lifecycle.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>

namespace agent_framework {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kEndHeader = 0x06054b50;
constexpr std::uint16_t kUtf8 = 0x0800;
constexpr std::uint16_t kDosDate = 0x0021;
constexpr std::uint32_t kRegular0644 = 0100644;
constexpr std::uint32_t kRegular0755 = 0100755;

struct PendingEntry {
    std::string path;
    std::vector<unsigned char> bytes;
    std::uint32_t crc = 0;
    bool executable = false;
    std::uint32_t offset = 0;
};

struct ParsedEntry {
    SkillArchiveMember public_entry;
    std::uint32_t data_offset = 0;
};

void set_error(SkillArchiveResult& result, const std::string& detail) {
    result.ok = false;
    result.error = std::string(kSkillArchiveInvalid) + ": " + detail;
}

void put16(std::ostream& output, std::uint16_t value) {
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
}

void put32(std::ostream& output, std::uint32_t value) {
    put16(output, static_cast<std::uint16_t>(value & 0xffff));
    put16(output, static_cast<std::uint16_t>((value >> 16) & 0xffff));
}

bool get16(const std::vector<unsigned char>& bytes, std::size_t offset, std::uint16_t& value) {
    if(offset + 2 > bytes.size()) return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
            (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
    return true;
}

bool get32(const std::vector<unsigned char>& bytes, std::size_t offset, std::uint32_t& value) {
    std::uint16_t low = 0, high = 0;
    if(!get16(bytes, offset, low) || !get16(bytes, offset + 2, high)) return false;
    value = static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 16);
    return true;
}

std::uint32_t crc32_bytes(const unsigned char* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for(std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for(int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool safe_path(const std::string& path) {
    if(path.empty() || path.front() == '/' || path.back() == '/' || path.size() > 65535) return false;
    std::size_t begin = 0;
    while(begin < path.size()) {
        auto end = path.find('/', begin);
        if(end == std::string::npos) end = path.size();
        const auto part = path.substr(begin, end - begin);
        if(part.empty() || part == "." || part == "..") return false;
        begin = end + 1;
    }
    for(unsigned char c : path) {
        if(c == '\\' || c == ':' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

std::string folded(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool read_file(const fs::path& path, std::uint64_t limit, std::vector<unsigned char>& bytes,
               std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if(ec || size > limit || size > std::numeric_limits<std::uint32_t>::max()) {
        error = "entry exceeds size limit or cannot be measured: " + path.string();
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if(!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))) {
        error = "cannot read entry: " + path.string();
        return false;
    }
    return true;
}

bool read_archive(const fs::path& path, const SkillArchiveLimits& limits,
                  std::vector<unsigned char>& bytes, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if(ec || size > limits.max_archive_bytes || size > std::numeric_limits<std::uint32_t>::max()) {
        error = "archive exceeds size limit or cannot be measured";
        return false;
    }
    return read_file(path, limits.max_archive_bytes, bytes, error);
}

bool parse_archive(const std::vector<unsigned char>& bytes, const SkillArchiveLimits& limits,
                   std::vector<ParsedEntry>& entries, std::string& error) {
    if(bytes.size() < 22) { error = "truncated end record"; return false; }
    const std::size_t eocd = bytes.size() - 22;
    std::uint32_t signature = 0, central_size = 0, central_offset = 0;
    std::uint16_t disk = 0, central_disk = 0, disk_entries = 0, total_entries = 0, comment = 0;
    if(!get32(bytes, eocd, signature) || signature != kEndHeader ||
       !get16(bytes, eocd + 4, disk) || !get16(bytes, eocd + 6, central_disk) ||
       !get16(bytes, eocd + 8, disk_entries) || !get16(bytes, eocd + 10, total_entries) ||
       !get32(bytes, eocd + 12, central_size) || !get32(bytes, eocd + 16, central_offset) ||
       !get16(bytes, eocd + 20, comment) || disk || central_disk || comment ||
       disk_entries != total_entries || total_entries > limits.max_entries ||
       static_cast<std::uint64_t>(central_offset) + central_size != eocd) {
        error = "invalid ZIP32 end record";
        return false;
    }
    std::set<std::string> names, folded_names;
    std::string previous;
    std::uint64_t expanded = 0;
    std::uint64_t expected_local = 0;
    std::size_t cursor = central_offset;
    for(std::uint32_t i = 0; i < total_entries; ++i) {
        std::uint32_t central_sig = 0, crc = 0, compressed = 0, size = 0, external = 0, local = 0;
        std::uint16_t made_by = 0, extract = 0, flags = 0, method = 0, time = 0, date = 0;
        std::uint16_t name_len = 0, extra_len = 0, comment_len = 0, start_disk = 0;
        if(cursor + 46 > eocd || !get32(bytes, cursor, central_sig) || central_sig != kCentralHeader ||
           !get16(bytes, cursor + 4, made_by) || !get16(bytes, cursor + 6, extract) ||
           !get16(bytes, cursor + 8, flags) || !get16(bytes, cursor + 10, method) ||
           !get16(bytes, cursor + 12, time) || !get16(bytes, cursor + 14, date) ||
           !get32(bytes, cursor + 16, crc) || !get32(bytes, cursor + 20, compressed) ||
           !get32(bytes, cursor + 24, size) || !get16(bytes, cursor + 28, name_len) ||
           !get16(bytes, cursor + 30, extra_len) || !get16(bytes, cursor + 32, comment_len) ||
           !get16(bytes, cursor + 34, start_disk) || !get32(bytes, cursor + 38, external) ||
           !get32(bytes, cursor + 42, local) || cursor + 46ull + name_len + extra_len + comment_len > eocd ||
           (made_by >> 8) != 3 || extract != 20 || flags != kUtf8 || method != 0 || time != 0 ||
           date != kDosDate || compressed != size || extra_len || comment_len || start_disk || !name_len) {
            error = "non-canonical central directory entry";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(bytes.data() + cursor + 46), name_len);
        const auto mode = external >> 16;
        if(!safe_path(name) || (i && name <= previous) || !names.insert(name).second ||
           !folded_names.insert(folded(name)).second || (mode != kRegular0644 && mode != kRegular0755) ||
           size > limits.max_entry_bytes || expanded + size > limits.max_expanded_bytes) {
            error = "unsafe, unsorted, colliding or oversized entry: " + name;
            return false;
        }
        previous = name;
        expanded += size;
        std::uint32_t local_sig = 0, local_crc = 0, local_compressed = 0, local_size = 0;
        std::uint16_t local_extract = 0, local_flags = 0, local_method = 0, local_time = 0,
                      local_date = 0, local_name_len = 0, local_extra_len = 0;
        if(local + 30ull > central_offset || !get32(bytes, local, local_sig) || local_sig != kLocalHeader ||
           !get16(bytes, local + 4, local_extract) || !get16(bytes, local + 6, local_flags) ||
           !get16(bytes, local + 8, local_method) || !get16(bytes, local + 10, local_time) ||
           !get16(bytes, local + 12, local_date) || !get32(bytes, local + 14, local_crc) ||
           !get32(bytes, local + 18, local_compressed) || !get32(bytes, local + 22, local_size) ||
           !get16(bytes, local + 26, local_name_len) || !get16(bytes, local + 28, local_extra_len) ||
           local != expected_local || local_extract != 20 || local_flags != flags ||
           local_method != method || local_time != time ||
           local_date != date || local_crc != crc || local_compressed != compressed || local_size != size ||
           local_name_len != name_len || local_extra_len || local + 30ull + name_len + size > central_offset ||
           std::string(reinterpret_cast<const char*>(bytes.data() + local + 30), name_len) != name) {
            error = "invalid local entry: " + name;
            return false;
        }
        const auto data_offset = local + 30u + name_len;
        if(crc32_bytes(bytes.data() + data_offset, size) != crc) {
            error = "CRC mismatch: " + name;
            return false;
        }
        entries.push_back({{name, size, crc, mode == kRegular0755}, data_offset});
        expected_local = static_cast<std::uint64_t>(data_offset) + size;
        cursor += 46ull + name_len;
    }
    if(cursor != eocd || expected_local != central_offset) {
        error = "central directory or local layout mismatch";
        return false;
    }
    return true;
}

std::atomic<std::uint64_t> temp_counter{0};

fs::path temp_path_for(const fs::path& target) {
    return target.parent_path() /
           (target.filename().string() + ".tmp." + std::to_string(++temp_counter));
}

} // namespace

SkillArchiveResult build_skill_archive(const fs::path& source_directory,
                                       const fs::path& output_archive,
                                       const std::map<std::string, std::string>& generated_files,
                                       const SkillArchiveLimits& limits) {
    SkillArchiveResult result;
    std::error_code ec;
    if(!fs::is_directory(source_directory, ec) || ec) {
        set_error(result, "source is not a directory");
        return result;
    }
    std::vector<PendingEntry> entries;
    std::set<std::string> names, folded_names;
    std::string detail;
    for(fs::recursive_directory_iterator it(source_directory, fs::directory_options::none, ec), end;
        it != end && !ec; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if(ec) break;
        if(fs::is_directory(status)) continue;
        if(!fs::is_regular_file(status)) {
            set_error(result, "links and special files are forbidden: " + it->path().string());
            return result;
        }
        auto relative = fs::relative(it->path(), source_directory, ec).generic_string();
        if(ec || !safe_path(relative) || folded(relative).rfind("meta-inf/", 0) == 0 ||
           !names.insert(relative).second || !folded_names.insert(folded(relative)).second) {
            set_error(result, "unsafe, reserved or colliding source path: " + relative);
            return result;
        }
        PendingEntry entry;
        entry.path = relative;
        if(!read_file(it->path(), limits.max_entry_bytes, entry.bytes, detail)) {
            set_error(result, detail);
            return result;
        }
        const auto permissions = status.permissions();
        entry.executable = (permissions & (fs::perms::owner_exec | fs::perms::group_exec |
                                           fs::perms::others_exec)) != fs::perms::none;
        entry.crc = crc32_bytes(entry.bytes.data(), entry.bytes.size());
        entries.push_back(std::move(entry));
    }
    if(ec) { set_error(result, "cannot enumerate source: " + ec.message()); return result; }
    for(const auto& [path, content] : generated_files) {
        if(!safe_path(path) || !names.insert(path).second || !folded_names.insert(folded(path)).second ||
           content.size() > limits.max_entry_bytes) {
            set_error(result, "unsafe, colliding or oversized generated path: " + path);
            return result;
        }
        PendingEntry entry;
        entry.path = path;
        entry.bytes.assign(content.begin(), content.end());
        entry.crc = crc32_bytes(entry.bytes.data(), entry.bytes.size());
        entries.push_back(std::move(entry));
    }
    if(entries.empty() || entries.size() > limits.max_entries) {
        set_error(result, "invalid entry count");
        return result;
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    std::uint64_t expanded = 0;
    for(const auto& entry : entries) {
        expanded += entry.bytes.size();
        if(expanded > limits.max_expanded_bytes) { set_error(result, "expanded size limit exceeded"); return result; }
    }
    if(!output_archive.parent_path().empty()) fs::create_directories(output_archive.parent_path(), ec);
    if(ec) { set_error(result, "cannot create archive directory: " + ec.message()); return result; }
    const auto temporary = temp_path_for(output_archive);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if(!output) { set_error(result, "cannot create temporary archive"); return result; }
    for(auto& entry : entries) {
        const auto position = output.tellp();
        if(position < 0 || static_cast<std::uint64_t>(position) > std::numeric_limits<std::uint32_t>::max()) {
            set_error(result, "ZIP32 offset limit exceeded"); output.close(); fs::remove(temporary, ec); return result;
        }
        entry.offset = static_cast<std::uint32_t>(position);
        put32(output, kLocalHeader); put16(output, 20); put16(output, kUtf8); put16(output, 0);
        put16(output, 0); put16(output, kDosDate); put32(output, entry.crc);
        put32(output, static_cast<std::uint32_t>(entry.bytes.size()));
        put32(output, static_cast<std::uint32_t>(entry.bytes.size()));
        put16(output, static_cast<std::uint16_t>(entry.path.size())); put16(output, 0);
        output.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
        output.write(reinterpret_cast<const char*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
    }
    const auto central_position = output.tellp();
    if(central_position < 0 || static_cast<std::uint64_t>(central_position) > std::numeric_limits<std::uint32_t>::max()) {
        set_error(result, "ZIP32 central offset limit exceeded"); output.close(); fs::remove(temporary, ec); return result;
    }
    const auto central_offset = static_cast<std::uint32_t>(central_position);
    for(const auto& entry : entries) {
        put32(output, kCentralHeader); put16(output, static_cast<std::uint16_t>((3u << 8) | 20u));
        put16(output, 20); put16(output, kUtf8); put16(output, 0); put16(output, 0); put16(output, kDosDate);
        put32(output, entry.crc); put32(output, static_cast<std::uint32_t>(entry.bytes.size()));
        put32(output, static_cast<std::uint32_t>(entry.bytes.size()));
        put16(output, static_cast<std::uint16_t>(entry.path.size())); put16(output, 0); put16(output, 0);
        put16(output, 0); put16(output, 0);
        put32(output, (entry.executable ? kRegular0755 : kRegular0644) << 16); put32(output, entry.offset);
        output.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
    }
    const auto end_position = output.tellp();
    if(end_position < 0 || static_cast<std::uint64_t>(end_position) > std::numeric_limits<std::uint32_t>::max()) {
        set_error(result, "ZIP32 size limit exceeded"); output.close(); fs::remove(temporary, ec); return result;
    }
    const auto central_size = static_cast<std::uint32_t>(end_position) - central_offset;
    put32(output, kEndHeader); put16(output, 0); put16(output, 0);
    put16(output, static_cast<std::uint16_t>(entries.size()));
    put16(output, static_cast<std::uint16_t>(entries.size()));
    put32(output, central_size); put32(output, central_offset); put16(output, 0);
    output.close();
    if(!output || fs::file_size(temporary, ec) > limits.max_archive_bytes) {
        set_error(result, "archive write failed or size limit exceeded"); fs::remove(temporary, ec); return result;
    }
    fs::rename(temporary, output_archive, ec);
    if(ec) { set_error(result, "cannot publish archive: " + ec.message()); fs::remove(temporary, ec); return result; }
    return inspect_skill_archive(output_archive, limits);
}

SkillArchiveResult inspect_skill_archive(const fs::path& archive, const SkillArchiveLimits& limits) {
    SkillArchiveResult result;
    std::vector<unsigned char> bytes;
    std::string detail;
    if(!read_archive(archive, limits, bytes, detail)) { set_error(result, detail); return result; }
    std::vector<ParsedEntry> parsed;
    if(!parse_archive(bytes, limits, parsed, detail)) { set_error(result, detail); return result; }
    for(const auto& entry : parsed) result.entries.push_back(entry.public_entry);
    auto digest = skill_sha256_file(archive, &detail);
    if(!digest) { set_error(result, detail); return result; }
    result.archive_digest = *digest;
    result.ok = true;
    return result;
}

SkillArchiveResult extract_skill_archive(const fs::path& archive, const fs::path& output_directory,
                                         const SkillArchiveLimits& limits) {
    SkillArchiveResult result;
    std::vector<unsigned char> bytes;
    std::string detail;
    if(!read_archive(archive, limits, bytes, detail)) { set_error(result, detail); return result; }
    std::vector<ParsedEntry> parsed;
    if(!parse_archive(bytes, limits, parsed, detail)) { set_error(result, detail); return result; }
    std::error_code ec;
    if(fs::exists(output_directory, ec)) { set_error(result, "output directory already exists"); return result; }
    const auto temporary = temp_path_for(output_directory);
    fs::create_directories(temporary, ec);
    if(ec) { set_error(result, "cannot create temporary extraction directory"); return result; }
    for(const auto& entry : parsed) {
        const auto destination = temporary / fs::path(entry.public_entry.path);
        fs::create_directories(destination.parent_path(), ec);
        if(ec) break;
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if(!output) { ec = std::make_error_code(std::errc::io_error); break; }
        output.write(reinterpret_cast<const char*>(bytes.data() + entry.data_offset),
                     static_cast<std::streamsize>(entry.public_entry.size));
        output.close();
        if(!output) { ec = std::make_error_code(std::errc::io_error); break; }
        fs::permissions(destination, entry.public_entry.executable ? fs::perms::owner_all |
                            fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec
                          : fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_read | fs::perms::others_read,
                        fs::perm_options::replace, ec);
        if(ec) break;
    }
    if(ec) { fs::remove_all(temporary, ec); set_error(result, "extraction failed"); return result; }
    fs::rename(temporary, output_directory, ec);
    if(ec) { fs::remove_all(temporary, ec); set_error(result, "cannot publish extraction: " + ec.message()); return result; }
    auto digest = skill_sha256_file(archive, &detail);
    if(!digest) { fs::remove_all(output_directory, ec); set_error(result, detail); return result; }
    result.archive_digest = *digest;
    for(const auto& entry : parsed) result.entries.push_back(entry.public_entry);
    result.ok = true;
    return result;
}

} // namespace agent_framework

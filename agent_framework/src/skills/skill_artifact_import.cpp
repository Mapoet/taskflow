#include <agent/skills/skill_artifact_import.hpp>

#include <agent/skills/skill_lifecycle.hpp>
#include <agent/agent/task_state_machine.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <set>

namespace agent_framework {
namespace {
namespace fs = std::filesystem;

SkillArtifactImportResult failed(const char* code, const std::string& message) {
    return {false, {{"code", code}, {"message", message}}, {}, {}};
}

bool cancelled(TaskControl* control) {
    if(!control) return false;
    control->check_deadline_now();
    return control->is_cancel_requested() || control->is_deadline_exceeded();
}

bool sha256(const std::string& value) {
    return value.size() == 64U &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
}

bool safe_path(const std::string& value) {
    if(value.empty() || value.find('\\') != std::string::npos ||
       value.find(':') != std::string::npos) return false;
    const fs::path path(value);
    if(path.is_absolute()) return false;
    for(const auto& part : path)
        if(part.empty() || part == "." || part == "..") return false;
    return true;
}

class Cleanup {
public:
    explicit Cleanup(fs::path path) : path_(std::move(path)) {}
    ~Cleanup() { std::error_code ec; fs::remove_all(path_, ec); }
private:
    fs::path path_;
};

struct StreamResult {
    bool ok = false;
    std::uint64_t bytes = 0;
    nlohmann::json error = nlohmann::json::object();
};

StreamResult write_stream(const SkillArtifactReader& reader, const fs::path& path,
                          std::uint64_t limit, TaskControl* control) {
    if(!reader) return {false, 0, {{"code", "skill_artifact_reader_missing"}}};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) return {false, 0, {{"code", "skill_artifact_write_failed"}}};
    StreamResult result{true, 0, nlohmann::json::object()};
    nlohmann::json provider_error;
    const bool provider_ok = reader([&](std::string_view chunk) {
        if(cancelled(control)) {
            result = {false, result.bytes, {{"code", "skill_artifact_cancelled"}}};
            return false;
        }
        if(chunk.size() > limit - std::min(limit, result.bytes)) {
            result = {false, result.bytes, {{"code", "skill_artifact_stream_limit"}}};
            return false;
        }
        output.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if(!output) {
            result = {false, result.bytes, {{"code", "skill_artifact_write_failed"}}};
            return false;
        }
        result.bytes += chunk.size();
        return true;
    }, &provider_error);
    output.flush();
    if(cancelled(control))
        return {false, result.bytes, {{"code", "skill_artifact_cancelled"}}};
    if(!result.ok) return result;
    if(!provider_ok)
        return {false, result.bytes, provider_error.is_object() && !provider_error.empty()
            ? provider_error : nlohmann::json{{"code", "skill_artifact_reader_failed"}}};
    if(!output) return {false, result.bytes, {{"code", "skill_artifact_write_failed"}}};
    return result;
}

} // namespace

SkillArtifactImporter::SkillArtifactImporter(std::shared_ptr<SkillResourceCache> cache,
                                             fs::path staging_root,
                                             SkillArtifactLimits limits)
    : cache_(std::move(cache)), staging_root_(std::move(staging_root)), limits_(limits) {}

SkillArtifactImportResult SkillArtifactImporter::import_archive(
    const SkillArtifactSource& source, const std::vector<SkillArchiveEntry>& entries,
    TaskControl* control) const {
    if(!cache_) return failed("skill_artifact_cache_unavailable", "artifact cache is unavailable");
    if(!sha256(source.expected_sha256))
        return failed("skill_artifact_digest_invalid", "source digest must be SHA-256");
    if(source.declared_size > limits_.max_download_bytes)
        return failed("skill_artifact_download_limit", "source exceeds download byte limit");
    if(entries.size() > limits_.max_entries)
        return failed("skill_artifact_entry_count_limit", "archive has too many entries");
    std::set<std::string> paths;
    std::uint64_t expanded = 0;
    for(const auto& entry : entries) {
        if(!safe_path(entry.path))
            return failed("skill_artifact_path_invalid", "archive entry path is unsafe");
        if(!paths.insert(entry.path).second)
            return failed("skill_artifact_duplicate_path", "archive entry path is duplicated");
        if(entry.type != SkillArchiveEntryType::Regular)
            return failed("skill_artifact_entry_type_forbidden",
                          "only regular archive entries are accepted");
        if(entry.expanded_size > limits_.max_entry_bytes)
            return failed("skill_artifact_entry_size_limit", "archive entry is too large");
        if(entry.expanded_size > limits_.max_expanded_bytes -
           std::min(limits_.max_expanded_bytes, expanded))
            return failed("skill_artifact_expanded_limit", "archive expansion limit exceeded");
        expanded += entry.expanded_size;
    }
    if(cancelled(control)) return failed("skill_artifact_cancelled", "artifact import cancelled");
    std::error_code ec;
    fs::create_directories(staging_root_, ec);
    if(ec) return failed("skill_artifact_write_failed", "staging root cannot be created");
    const auto status = fs::symlink_status(staging_root_, ec);
    if(ec || fs::is_symlink(status) || !fs::is_directory(status))
        return failed("skill_artifact_write_failed", "staging root is not trusted");
    static std::atomic<std::uint64_t> sequence{0};
    const fs::path transaction = staging_root_ /
        ("import-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    fs::create_directory(transaction, ec);
    if(ec) return failed("skill_artifact_write_failed", "staging transaction cannot be created");
    Cleanup cleanup(transaction);

    const auto downloaded = write_stream(source.reader, transaction / "source.bin",
                                          limits_.max_download_bytes, control);
    if(!downloaded.ok) {
        const std::string code = downloaded.error.value("code", "skill_artifact_reader_failed");
        if(code == "skill_artifact_stream_limit")
            return failed("skill_artifact_download_limit", "download byte limit exceeded");
        return {false, downloaded.error, {}, {}};
    }
    if(downloaded.bytes != source.declared_size)
        return failed("skill_artifact_source_size_mismatch", "source size differs from declaration");
    std::string digest_error;
    const auto actual_digest = skill_sha256_file(transaction / "source.bin", &digest_error);
    if(!actual_digest || *actual_digest != source.expected_sha256)
        return failed("skill_artifact_digest_mismatch", "source digest verification failed");
    if(expanded > 0 && (downloaded.bytes == 0 ||
       static_cast<long double>(expanded) /
       static_cast<long double>(downloaded.bytes) > limits_.max_expansion_ratio))
        return failed("skill_artifact_expansion_ratio", "archive expansion ratio exceeded");

    std::vector<fs::path> staged;
    for(const auto& entry : entries) {
        if(cancelled(control)) return failed("skill_artifact_cancelled", "artifact import cancelled");
        const fs::path target = transaction / "entries" / fs::path(entry.path);
        fs::create_directories(target.parent_path(), ec);
        if(ec) return failed("skill_artifact_write_failed", "entry directory cannot be created");
        const auto written = write_stream(entry.reader, target, limits_.max_entry_bytes, control);
        if(!written.ok) {
            const std::string code = written.error.value("code", "skill_artifact_reader_failed");
            if(code == "skill_artifact_stream_limit")
                return failed("skill_artifact_entry_size_limit", "entry byte limit exceeded");
            return {false, written.error, {}, {}};
        }
        if(written.bytes != entry.expanded_size)
            return failed("skill_artifact_entry_size_mismatch",
                          "entry size differs from declaration");
        staged.push_back(target);
    }

    SkillArtifactImportResult result;
    for(std::size_t i = 0; i < entries.size(); ++i) {
        if(cancelled(control))
            return failed("skill_artifact_cancelled", "artifact import cancelled");
        std::string error;
        const auto digest = skill_sha256_file(staged[i], &error);
        if(!digest) return failed("skill_artifact_digest_unavailable", error);
        SkillResourceHandle handle;
        handle.path = staged[i];
        handle.size = entries[i].expanded_size;
        handle.view_size = handle.size;
        handle.resource_digest = *digest;
        handle.package_digest = *actual_digest;
        handle.descriptor.id = entries[i].path;
        handle.descriptor.kind = SkillResourceType::Asset;
        handle.descriptor.media_type = "application/octet-stream";
        auto cached = cache_->acquire(handle);
        if(!cached.ok) return {false, cached.error, {}, {}};
        result.objects.push_back(*cached.object);
        result.leases.push_back(std::move(cached.lease));
    }
    result.ok = true;
    return result;
}

} // namespace agent_framework

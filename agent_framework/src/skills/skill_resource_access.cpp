#include <agent/skill_resource_access.hpp>

#include <agent/skill_lifecycle.hpp>
#include <agent/task_state_machine.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace agent_framework {
namespace {

nlohmann::json failure(const char* code, const std::string& message) {
    return {{"code", code}, {"message", message}};
}

SkillResourceResult open_failure(const char* code, const std::string& message) {
    return {false, failure(code, message), std::nullopt};
}

bool safe_relative(const std::string& value) {
    if(value.empty()) return false;
    const std::filesystem::path path(value);
    if(path.is_absolute()) return false;
    for(const auto& part : path) if(part == "..") return false;
    return true;
}

bool contains_symlink(const std::filesystem::path& base,
                      const std::filesystem::path& relative) {
    std::filesystem::path cursor = base;
    std::error_code ec;
    for(const auto& part : relative) {
        cursor /= part;
        const auto status = std::filesystem::symlink_status(cursor, ec);
        if(ec) return false;
        if(std::filesystem::is_symlink(status)) return true;
    }
    return false;
}

bool textual_media_type(const std::string& value) {
    return value.rfind("text/", 0) == 0 || value == "application/json" ||
           value == "application/yaml" || value == "application/xml" ||
           value == "application/markdown";
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool stopped(TaskControl* control, nlohmann::json* error) {
    if(!control) return false;
    control->check_deadline_now();
    if(control->is_cancel_requested()) {
        if(error) *error = failure(kSkillResourceCancelled, "resource stream was cancelled");
        return true;
    }
    if(control->is_deadline_exceeded()) {
        if(error) *error = failure("skill_resource_deadline_exceeded",
                                   "resource stream deadline was exceeded");
        return true;
    }
    return false;
}

} // namespace

SkillMappedResource::~SkillMappedResource() { reset(); }

SkillMappedResource::SkillMappedResource(SkillMappedResource&& other) noexcept {
    *this = std::move(other);
}

SkillMappedResource& SkillMappedResource::operator=(SkillMappedResource&& other) noexcept {
    if(this == &other) return *this;
    reset();
    mapping_base_ = other.mapping_base_;
    mapping_size_ = other.mapping_size_;
    data_ = other.data_;
    size_ = other.size_;
    file_descriptor_ = other.file_descriptor_;
    other.mapping_base_ = nullptr;
    other.mapping_size_ = 0;
    other.data_ = nullptr;
    other.size_ = 0;
    other.file_descriptor_ = -1;
    return *this;
}

void SkillMappedResource::reset() noexcept {
#if defined(__linux__)
    if(mapping_base_ && mapping_size_ > 0) ::munmap(mapping_base_, mapping_size_);
    if(file_descriptor_ >= 0) ::close(file_descriptor_);
#endif
    mapping_base_ = nullptr;
    mapping_size_ = 0;
    data_ = nullptr;
    size_ = 0;
    file_descriptor_ = -1;
}

SkillResourceResult SkillResourceAccess::open_snapshot(
    const SkillIndexEntry& entry, std::shared_ptr<const SkillManifest> manifest,
    const std::string& resource_id, const SkillResourceOpenOptions& options) const {
    if(!manifest)
        return open_failure("skill_resource_manifest_missing", "resource manifest is unavailable");
    const SkillResourceDescriptor* descriptor = nullptr;
    for(const auto& candidate : manifest->resources) {
        if(candidate.id == resource_id) {
            descriptor = &candidate;
            break;
        }
    }
    if(!descriptor)
        return open_failure(kSkillResourceNotFound, "resource id is not declared: " + resource_id);
    if(!safe_relative(descriptor->path))
        return open_failure("skill_resource_path_invalid", "resource path is not jail-relative");

    std::error_code ec;
    const auto package = entry.script_jail.value_or(entry.file_path.parent_path());
    const auto base = std::filesystem::weakly_canonical(package, ec);
    if(ec || base.empty())
        return open_failure("skill_resource_jail_unavailable", "package jail cannot be resolved");
    if(contains_symlink(base, descriptor->path))
        return open_failure("skill_resource_symlink_forbidden", "resource path contains a symbolic link");
    const auto target = std::filesystem::weakly_canonical(base / descriptor->path, ec);
    if(ec)
        return open_failure("skill_resource_not_regular", "resource path cannot be resolved");
    const auto relative = std::filesystem::relative(target, base, ec);
    if(ec || relative.empty() || *relative.begin() == "..")
        return open_failure("skill_resource_jail_escape", "resource resolves outside the package jail");
    if(!std::filesystem::is_regular_file(target, ec) || ec)
        return open_failure("skill_resource_not_regular", "resource must be a regular file");
    const auto file_size = std::filesystem::file_size(target, ec);
    if(ec)
        return open_failure("skill_resource_size_unavailable", "resource size cannot be read");
    if(descriptor->declared_size && file_size != *descriptor->declared_size)
        return open_failure("skill_resource_size_mismatch", "resource size differs from its descriptor");
    if(descriptor->size_limit && file_size > *descriptor->size_limit)
        return open_failure("skill_resource_size_limit_exceeded", "resource exceeds its size limit");

    std::string digest_error;
    const auto actual_digest = skill_sha256_file(target, &digest_error);
    if(!actual_digest)
        return open_failure("skill_resource_digest_unavailable",
                            "resource digest cannot be computed: " + digest_error);
    const auto recorded = entry.resource_digests.find(resource_id);
    if(recorded != entry.resource_digests.end() &&
       lowercase(recorded->second) != *actual_digest)
        return open_failure("skill_resource_digest_mismatch",
                            "resource digest differs from the pinned snapshot");
    if(!descriptor->sha256.empty() && lowercase(descriptor->sha256) != *actual_digest)
        return open_failure("skill_resource_digest_mismatch",
                            "resource digest differs from its descriptor");

    SkillResourceReadMode mode = options.mode;
    if(mode == SkillResourceReadMode::Auto) mode = descriptor->read_mode;
    if(mode == SkillResourceReadMode::Auto)
        mode = textual_media_type(descriptor->media_type)
            ? SkillResourceReadMode::Text : SkillResourceReadMode::Binary;
    if(descriptor->read_mode != SkillResourceReadMode::Auto &&
       options.mode != SkillResourceReadMode::Auto && mode != descriptor->read_mode)
        return open_failure("skill_resource_read_mode_mismatch",
                            "requested read mode differs from the descriptor");
    if(options.offset > file_size)
        return open_failure("skill_resource_range_invalid", "resource offset exceeds its size");
    const std::uint64_t remaining = file_size - options.offset;
    const std::uint64_t view_size = std::min<std::uint64_t>(remaining, options.max_bytes);

    SkillResourceHandle handle;
    handle.path = target;
    handle.descriptor = *descriptor;
    handle.package_digest = entry.package_digest;
    handle.resource_digest = *actual_digest;
    handle.size = file_size;
    handle.view_offset = options.offset;
    handle.view_size = view_size;
    handle.mode = mode;
    handle.package_lease = entry.package_lease;
    return {true, nlohmann::json::object(), std::move(handle)};
}

SkillResourceReadResult SkillResourceAccess::read(const SkillResourceHandle& handle) const {
    if(handle.view_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return {false, failure("skill_resource_budget_exceeded",
                               "resource view exceeds addressable memory"), {}};
    std::ifstream input(handle.path, std::ios::binary);
    if(!input)
        return {false, failure("skill_resource_read_failed", "resource cannot be opened"), {}};
    input.seekg(static_cast<std::streamoff>(handle.view_offset));
    if(!input)
        return {false, failure("skill_resource_read_failed", "resource seek failed"), {}};
    std::string bytes(static_cast<std::size_t>(handle.view_size), '\0');
    if(!bytes.empty()) input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if(input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return {false, failure("skill_resource_read_failed", "resource changed during read"), {}};
    return {true, nlohmann::json::object(), std::move(bytes)};
}

SkillResourceStreamResult SkillResourceAccess::stream(
    const SkillResourceHandle& handle, std::size_t chunk_bytes,
    const SkillResourceChunkConsumer& consumer, TaskControl* control) const {
    if(chunk_bytes == 0 || !consumer)
        return {false, failure("skill_resource_stream_invalid",
                               "stream chunk size and consumer are required"), 0};
    std::ifstream input(handle.path, std::ios::binary);
    if(!input)
        return {false, failure("skill_resource_read_failed", "resource cannot be opened"), 0};
    input.seekg(static_cast<std::streamoff>(handle.view_offset));
    if(!input)
        return {false, failure("skill_resource_read_failed", "resource seek failed"), 0};
    std::vector<char> buffer(static_cast<std::size_t>(
        std::min<std::uint64_t>(chunk_bytes, std::max<std::uint64_t>(1, handle.view_size))));
    std::uint64_t consumed = 0;
    while(consumed < handle.view_size) {
        nlohmann::json stop_error;
        if(stopped(control, &stop_error)) return {false, std::move(stop_error), consumed};
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), handle.view_size - consumed));
        input.read(buffer.data(), static_cast<std::streamsize>(count));
        if(input.gcount() != static_cast<std::streamsize>(count))
            return {false, failure("skill_resource_read_failed",
                                   "resource changed during stream"), consumed};
        if(!consumer(handle.view_offset + consumed,
                     std::string_view(buffer.data(), count)))
            return {false, failure("skill_resource_stream_stopped",
                                   "resource consumer stopped the stream"), consumed};
        consumed += count;
    }
    nlohmann::json stop_error;
    if(stopped(control, &stop_error)) return {false, std::move(stop_error), consumed};
    return {true, nlohmann::json::object(), consumed};
}

SkillMappedResourceResult SkillResourceAccess::map(const SkillResourceHandle& handle) const {
#if !defined(__linux__)
    (void)handle;
    return {false, failure(kSkillResourceMmapUnavailable,
                           "read-only memory mapping is unavailable on this platform"), std::nullopt};
#else
    if(handle.view_size == 0) {
        SkillMappedResource empty;
        return {true, nlohmann::json::object(), std::move(empty)};
    }
    const long page_size_raw = ::sysconf(_SC_PAGE_SIZE);
    if(page_size_raw <= 0)
        return {false, failure(kSkillResourceMmapUnavailable,
                               "system page size is unavailable"), std::nullopt};
    const std::uint64_t page_size = static_cast<std::uint64_t>(page_size_raw);
    const std::uint64_t aligned_offset = handle.view_offset - (handle.view_offset % page_size);
    const std::uint64_t delta = handle.view_offset - aligned_offset;
    if(delta > std::numeric_limits<std::size_t>::max() ||
       handle.view_size > std::numeric_limits<std::size_t>::max() - delta)
        return {false, failure("skill_resource_budget_exceeded",
                               "mapping size exceeds addressable memory"), std::nullopt};
    const std::size_t mapping_size = static_cast<std::size_t>(delta + handle.view_size);
    const int fd = ::open(handle.path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0)
        return {false, failure("skill_resource_mmap_failed", "resource cannot be opened"),
                std::nullopt};
    void* base = ::mmap(nullptr, mapping_size, PROT_READ, MAP_PRIVATE, fd,
                        static_cast<off_t>(aligned_offset));
    if(base == MAP_FAILED) {
        ::close(fd);
        return {false, failure("skill_resource_mmap_failed", "read-only mmap failed"),
                std::nullopt};
    }
    SkillMappedResource mapping;
    mapping.mapping_base_ = base;
    mapping.mapping_size_ = mapping_size;
    mapping.data_ = static_cast<const char*>(base) + delta;
    mapping.size_ = static_cast<std::size_t>(handle.view_size);
    mapping.file_descriptor_ = fd;
    return {true, nlohmann::json::object(), std::move(mapping)};
#endif
}

} // namespace agent_framework

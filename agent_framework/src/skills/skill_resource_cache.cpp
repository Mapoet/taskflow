#include <agent/skill_resource_cache.hpp>

#include <agent/skill_lifecycle.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

namespace agent_framework {
namespace {

namespace fs = std::filesystem;

nlohmann::json failure(const char* code, const std::string& message) {
    return {{"code", code}, {"message", message}};
}

SkillCacheResult failed(const char* code, const std::string& message) {
    return {false, failure(code, message), std::nullopt, nullptr};
}

SkillCacheResult succeeded(std::optional<SkillCacheObject> object = std::nullopt) {
    return {true, nlohmann::json::object(), std::move(object), nullptr};
}

bool valid_digest(const std::string& value) {
    return value.size() == 64U &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string transaction_id() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return std::to_string(ticks) + "-" + std::to_string(thread) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

class TransactionCleanup {
public:
    explicit TransactionCleanup(fs::path path) : path_(std::move(path)) {}
    ~TransactionCleanup() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
private:
    fs::path path_;
};

bool replace_file(const fs::path& temporary, const fs::path& destination,
                  std::string* error) {
    std::error_code ec;
    fs::rename(temporary, destination, ec);
#if defined(_WIN32)
    if(ec) {
        ec.clear();
        fs::remove(destination, ec);
        ec.clear();
        fs::rename(temporary, destination, ec);
    }
#endif
    if(ec) {
        if(error) *error = "atomic publish failed: " + ec.message();
        return false;
    }
    return true;
}

bool write_json(const fs::path& path, const nlohmann::json& value,
                std::string* error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        if(error) *error = "temporary file cannot be opened";
        return false;
    }
    output << value.dump(2) << '\n';
    output.flush();
    if(!output) {
        if(error) *error = "temporary file cannot be written";
        return false;
    }
    return true;
}

bool ensure_layout(const fs::path& root, std::string* error) {
    std::error_code ec;
    for(const auto& path : {root / "objects/sha256", root / "metadata",
                            root / "derived", root / "transactions"}) {
        fs::create_directories(path, ec);
        if(ec) {
            if(error) *error = "cache directory cannot be created: " + ec.message();
            return false;
        }
        const auto status = fs::symlink_status(path, ec);
        if(ec || fs::is_symlink(status) || !fs::is_directory(status)) {
            if(error) *error = "cache directory is not a trusted directory";
            return false;
        }
    }
    const fs::path state = root / "state.json";
    if(fs::exists(state, ec)) {
        const auto status = fs::symlink_status(state, ec);
        if(ec || fs::is_symlink(status) || !fs::is_regular_file(status)) {
            if(error) *error = "cache state is invalid";
            return false;
        }
        return true;
    }
    ec.clear();
    const fs::path transaction = root / "transactions" / transaction_id();
    fs::create_directory(transaction, ec);
    if(ec) {
        if(error) *error = "state transaction cannot be created";
        return false;
    }
    TransactionCleanup cleanup(transaction);
    const fs::path temporary = transaction / "state.json";
    if(!write_json(temporary, {{"schemaVersion", 1}}, error)) return false;
    if(!replace_file(temporary, state, error) && !fs::is_regular_file(state)) return false;
    return true;
}

bool load_metadata(const fs::path& path, SkillCacheObject* object) {
    std::ifstream input(path, std::ios::binary);
    if(!input) return false;
    try {
        nlohmann::json value;
        input >> value;
        if(value.value("schemaVersion", 0) != 1 ||
           value.value("digest", "") != object->digest ||
           value.value("size", std::uint64_t{0}) != object->size)
            return false;
        object->media_type = value.value("mediaType", "");
        object->source_package_digest = value.value("sourcePackageDigest", "");
        object->source_resource_id = value.value("sourceResourceId", "");
        object->last_access = value.value("lastAccess", std::uint64_t{0});
        object->pinned = value.value("pinned", false);
        return true;
    } catch(const std::exception&) {
        return false;
    }
}

bool write_metadata(const fs::path& root, const SkillCacheObject& object,
                    std::string* error) {
    const fs::path transaction = root / "transactions" / transaction_id();
    std::error_code ec;
    fs::create_directory(transaction, ec);
    if(ec) {
        if(error) *error = "metadata transaction cannot be created";
        return false;
    }
    TransactionCleanup cleanup(transaction);
    const fs::path temporary = transaction / "metadata.json";
    const nlohmann::json value = {
        {"schemaVersion", 1}, {"digest", object.digest}, {"size", object.size},
        {"mediaType", object.media_type},
        {"sourcePackageDigest", object.source_package_digest},
        {"sourceResourceId", object.source_resource_id},
        {"lastAccess", object.last_access}, {"pinned", object.pinned}
    };
    if(!write_json(temporary, value, error)) return false;
    return replace_file(temporary, object.metadata_path, error);
}

std::uint64_t next_access(const fs::path& root) {
    std::uint64_t maximum = 0;
    std::error_code ec;
    const fs::path metadata = root / "metadata";
    for(fs::directory_iterator it(metadata, ec), end; !ec && it != end; it.increment(ec)) {
        std::ifstream input(it->path(), std::ios::binary);
        try {
            nlohmann::json value;
            if(input) input >> value;
            maximum = std::max(maximum, value.value("lastAccess", std::uint64_t{0}));
        } catch(const std::exception&) {
        }
    }
    return maximum == std::numeric_limits<std::uint64_t>::max() ? maximum : maximum + 1U;
}

} // namespace

SkillResourceCache::SkillResourceCache(fs::path root, SkillCacheLimits limits)
    : root_(std::move(root)), limits_(limits) {}

SkillCacheResult SkillResourceCache::acquire(const SkillResourceHandle& handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return acquire_locked(handle);
}

SkillCacheResult SkillResourceCache::acquire_policy(const SkillResourceHandle& handle) {
    if(handle.descriptor.cache_policy == SkillCachePolicy::NoStore) {
        SkillCacheObject object;
        object.digest = lowercase(handle.resource_digest);
        object.path = handle.path;
        object.size = handle.size;
        object.media_type = handle.descriptor.media_type;
        object.source_package_digest = handle.package_digest;
        object.source_resource_id = handle.descriptor.id;
        return succeeded(std::move(object));
    }
    auto result = acquire(handle);
    if(!result.ok || handle.descriptor.cache_policy != SkillCachePolicy::Pin) return result;
    auto pinned = pin(handle.resource_digest);
    if(!pinned.ok) return pinned;
    pinned.lease = std::move(result.lease);
    if(pinned.object) pinned.object->leased = static_cast<bool>(pinned.lease);
    return pinned;
}

SkillCacheReport SkillResourceCache::inspect() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inspect_locked();
}

SkillCacheResult SkillResourceCache::pin(const std::string& digest) {
    std::lock_guard<std::mutex> lock(mutex_);
    return set_pin_locked(digest, true);
}

SkillCacheResult SkillResourceCache::unpin(const std::string& digest) {
    std::lock_guard<std::mutex> lock(mutex_);
    return set_pin_locked(digest, false);
}

SkillCacheResult SkillResourceCache::collect() {
    std::lock_guard<std::mutex> lock(mutex_);
    return collect_locked();
}

SkillCacheReport SkillResourceCache::inspect_locked() const {
    SkillCacheReport report;
    std::string layout_error;
    if(!ensure_layout(root_, &layout_error)) {
        report.error = failure("skill_cache_io_error", layout_error);
        return report;
    }
    std::error_code ec;
    const fs::path objects = root_ / "objects/sha256";
    for(fs::directory_iterator it(objects, ec), end; !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if(ec || fs::is_symlink(status) || !fs::is_regular_file(status)) {
            report.error = failure("skill_cache_object_corrupt",
                                   "cache object directory contains an invalid entry");
            return report;
        }
        SkillCacheObject object;
        object.digest = it->path().filename().string();
        if(!valid_digest(object.digest)) {
            report.error = failure("skill_cache_object_corrupt",
                                   "cache object name is not a SHA-256 digest");
            return report;
        }
        object.path = objects / object.digest;
        object.metadata_path = root_ / "metadata" / (object.digest + ".json");
        object.size = fs::file_size(object.path, ec);
        if(ec) {
            report.error = failure("skill_cache_io_error", "cache object size is unavailable");
            return report;
        }
        load_metadata(object.metadata_path, &object);
        const auto lease = leases_.find(object.digest);
        object.leased = lease != leases_.end() && !lease->second.expired();
        report.total_bytes += object.size;
        if(object.pinned) report.pinned_bytes += object.size;
        if(object.leased) ++report.leased_objects;
        report.entries.push_back(std::move(object));
    }
    if(ec) {
        report.error = failure("skill_cache_io_error", "cache object traversal failed");
        return report;
    }
    std::sort(report.entries.begin(), report.entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.digest < rhs.digest; });
    report.object_count = report.entries.size();
    report.ok = true;
    return report;
}

SkillCacheResult SkillResourceCache::set_pin_locked(const std::string& requested, bool pinned) {
    const std::string digest = lowercase(requested);
    if(!valid_digest(digest))
        return failed("skill_cache_digest_invalid", "cache digest must be SHA-256");
    const auto report = inspect_locked();
    if(!report.ok) return {false, report.error, std::nullopt, nullptr};
    const auto found = std::find_if(report.entries.begin(), report.entries.end(),
                                    [&](const auto& item) { return item.digest == digest; });
    if(found == report.entries.end())
        return failed("skill_cache_object_not_found", "cache object does not exist");
    auto object = *found;
    object.pinned = pinned;
    std::string error;
    if(!write_metadata(root_, object, &error))
        return failed("skill_cache_metadata_failed", error);
    return succeeded(std::move(object));
}

SkillCacheResult SkillResourceCache::collect_locked(std::uint64_t required_bytes,
                                                     const std::string& protected_digest) {
    auto report = inspect_locked();
    if(!report.ok) return {false, report.error, std::nullopt, nullptr};
    if(required_bytes > limits_.max_total_bytes)
        return failed("skill_cache_quota_exceeded", "object exceeds total cache quota");
    if(report.total_bytes <= limits_.max_total_bytes - required_bytes)
        return succeeded();
    std::vector<SkillCacheObject> candidates;
    for(const auto& object : report.entries) {
        if(!object.pinned && !object.leased && object.digest != protected_digest)
            candidates.push_back(object);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        if(lhs.last_access != rhs.last_access) return lhs.last_access < rhs.last_access;
        return lhs.digest < rhs.digest;
    });
    std::error_code ec;
    for(const auto& object : candidates) {
        if(report.total_bytes <= limits_.max_total_bytes - required_bytes) break;
        fs::remove(object.path, ec);
        if(ec) return failed("skill_cache_io_error", "cache object eviction failed");
        ec.clear();
        fs::remove(object.metadata_path, ec);
        ec.clear();
        report.total_bytes -= object.size;
        leases_.erase(object.digest);
    }
    if(report.total_bytes > limits_.max_total_bytes - required_bytes)
        return failed("skill_cache_quota_exceeded",
                      "cache quota is occupied by pinned or leased objects");
    return succeeded();
}

SkillCacheResult SkillResourceCache::acquire_locked(const SkillResourceHandle& handle) {
    if(!valid_digest(handle.resource_digest))
        return failed("skill_cache_digest_invalid", "resource digest must be SHA-256");
    const std::string digest = lowercase(handle.resource_digest);
    if(handle.size > limits_.max_object_bytes)
        return failed("skill_cache_object_too_large", "resource exceeds cache object limit");
    std::string layout_error;
    if(!ensure_layout(root_, &layout_error))
        return failed("skill_cache_io_error", layout_error);

    std::error_code ec;
    const auto source_status = fs::symlink_status(handle.path, ec);
    if(ec || fs::is_symlink(source_status) || !fs::is_regular_file(source_status))
        return failed("skill_cache_source_invalid", "cache source must be a regular non-link file");
    const auto source_size = fs::file_size(handle.path, ec);
    if(ec || source_size != handle.size)
        return failed("skill_cache_source_changed", "cache source size differs from its snapshot");

    SkillCacheObject object;
    object.digest = digest;
    object.path = root_ / "objects/sha256" / digest;
    object.metadata_path = root_ / "metadata" / (digest + ".json");
    object.size = handle.size;
    object.media_type = handle.descriptor.media_type;
    object.source_package_digest = handle.package_digest;
    object.source_resource_id = handle.descriptor.id;
    object.last_access = next_access(root_);

    const auto cached_status = fs::symlink_status(object.path, ec);
    const bool cached_missing =
        (!ec && cached_status.type() == fs::file_type::not_found) ||
        ec == std::errc::no_such_file_or_directory;
    if(cached_missing) ec.clear();
    if(!ec && !cached_missing) {
        if(fs::is_symlink(cached_status) || !fs::is_regular_file(cached_status))
            return failed("skill_cache_object_corrupt", "cache object is not a regular file");
        SkillCacheObject previous = object;
        if(load_metadata(object.metadata_path, &previous)) object.pinned = previous.pinned;
        const auto cached_size = fs::file_size(object.path, ec);
        std::string digest_error;
        const auto cached_digest = !ec ? skill_sha256_file(object.path, &digest_error)
                                       : std::nullopt;
        if(ec || cached_size != object.size || !cached_digest || *cached_digest != digest)
            return failed("skill_cache_object_corrupt", "cache object failed integrity verification");
    } else if(cached_missing) {
        const auto quota = collect_locked(handle.size, digest);
        if(!quota.ok) return quota;
        const fs::path transaction = root_ / "transactions" / transaction_id();
        fs::create_directory(transaction, ec);
        if(ec) return failed("skill_cache_io_error", "cache transaction cannot be created");
        TransactionCleanup cleanup(transaction);
        const fs::path temporary = transaction / "object";
        std::ifstream input(handle.path, std::ios::binary);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if(!input || !output)
            return failed("skill_cache_copy_failed", "cache copy cannot be opened");
        std::vector<char> buffer(64U * 1024U);
        std::uint64_t copied = 0;
        while(input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if(count <= 0) break;
            copied += static_cast<std::uint64_t>(count);
            if(copied > handle.size || copied > limits_.max_object_bytes)
                return failed("skill_cache_copy_failed", "cache source grew during copy");
            output.write(buffer.data(), count);
            if(!output) return failed("skill_cache_copy_failed", "cache copy cannot be written");
        }
        output.flush();
        output.close();
        if(input.bad() || copied != handle.size)
            return failed("skill_cache_copy_failed", "cache source changed during copy");
        std::string digest_error;
        const auto copied_digest = skill_sha256_file(temporary, &digest_error);
        if(!copied_digest || *copied_digest != digest)
            return failed("skill_cache_digest_mismatch", "cache copy digest verification failed");
        fs::rename(temporary, object.path, ec);
        if(ec) return failed("skill_cache_publish_failed", "cache object cannot be published");
    } else {
        return failed("skill_cache_io_error", "cache object status cannot be read");
    }

    std::string metadata_error;
    if(!write_metadata(root_, object, &metadata_error))
        return failed("skill_cache_metadata_failed", metadata_error);
    auto token = leases_[digest].lock();
    if(!token) {
        token = std::make_shared<const std::string>(digest);
        leases_[digest] = token;
    }
    auto lease = std::shared_ptr<SkillCacheLease>(new SkillCacheLease);
    object.leased = true;
    lease->object_ = object;
    lease->token_ = std::move(token);
    return {true, nlohmann::json::object(), object, std::move(lease)};
}

SkillCacheResult SkillResourceCache::verify() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string layout_error;
    if(!ensure_layout(root_, &layout_error))
        return failed("skill_cache_io_error", layout_error);
    std::error_code ec;
    const fs::path transactions = root_ / "transactions";
    for(fs::directory_iterator it(transactions, ec), end; !ec && it != end; it.increment(ec))
        fs::remove_all(it->path(), ec);
    if(ec) return failed("skill_cache_recovery_failed", "stale transactions cannot be removed");

    const fs::path objects = root_ / "objects/sha256";
    const fs::path quarantine = root_ / "quarantine";
    for(fs::directory_iterator it(objects, ec), end; !ec && it != end;) {
        const fs::path path = it->path();
        it.increment(ec);
        if(ec) break;
        const std::string digest = path.filename().string();
        const auto status = fs::symlink_status(path, ec);
        std::string digest_error;
        const auto actual = !ec && fs::is_regular_file(status) && !fs::is_symlink(status)
            ? skill_sha256_file(path, &digest_error) : std::nullopt;
        if(!valid_digest(digest) || !actual || *actual != digest) {
            fs::create_directories(quarantine, ec);
            if(ec) return failed("skill_cache_recovery_failed", "quarantine cannot be created");
            const auto quarantine_status = fs::symlink_status(quarantine, ec);
            if(ec || fs::is_symlink(quarantine_status) ||
               !fs::is_directory(quarantine_status))
                return failed("skill_cache_recovery_failed", "quarantine is not trusted");
            const fs::path destination = quarantine / (digest + "-" + transaction_id());
            fs::rename(path, destination, ec);
            if(ec) return failed("skill_cache_recovery_failed", "corrupt object cannot be quarantined");
            fs::remove(root_ / "metadata" / (digest + ".json"), ec);
            ec.clear();
            leases_.erase(digest);
            continue;
        }
        SkillCacheObject object;
        object.digest = digest;
        object.path = path;
        object.metadata_path = root_ / "metadata" / (digest + ".json");
        object.size = fs::file_size(path, ec);
        if(ec) return failed("skill_cache_recovery_failed", "object size cannot be read");
        if(!load_metadata(object.metadata_path, &object)) {
            object.last_access = next_access(root_);
            std::string metadata_error;
            if(!write_metadata(root_, object, &metadata_error))
                return failed("skill_cache_recovery_failed", metadata_error);
        }
    }
    if(ec) return failed("skill_cache_recovery_failed", "cache verification traversal failed");
    return collect_locked();
}

} // namespace agent_framework

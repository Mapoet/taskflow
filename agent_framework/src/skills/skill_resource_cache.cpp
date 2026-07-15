#include <agent/skill_resource_cache.hpp>

#include <agent/skill_lifecycle.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
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

bool atomic_metadata(const fs::path& temporary, const fs::path& destination,
                     const SkillCacheObject& object, std::string* error) {
    const nlohmann::json metadata = {
        {"schemaVersion", 1},
        {"digest", object.digest},
        {"size", object.size},
        {"mediaType", object.media_type},
        {"sourcePackageDigest", object.source_package_digest},
        {"sourceResourceId", object.source_resource_id}
    };
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if(!output) {
            if(error) *error = "cache metadata temporary file cannot be opened";
            return false;
        }
        output << metadata.dump(2) << '\n';
        output.flush();
        if(!output) {
            if(error) *error = "cache metadata cannot be written";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, destination, ec);
    if(ec) {
        if(error) *error = "cache metadata cannot be published: " + ec.message();
        return false;
    }
    return true;
}

} // namespace

SkillResourceCache::SkillResourceCache(fs::path root, SkillCacheLimits limits)
    : root_(std::move(root)), limits_(limits) {}

SkillCacheResult SkillResourceCache::acquire(const SkillResourceHandle& handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    return acquire_locked(handle);
}

SkillCacheResult SkillResourceCache::acquire_locked(const SkillResourceHandle& handle) {
    if(!valid_digest(handle.resource_digest))
        return failed("skill_cache_digest_invalid", "resource digest must be SHA-256");
    const std::string digest = lowercase(handle.resource_digest);
    if(handle.size > limits_.max_object_bytes)
        return failed("skill_cache_object_too_large", "resource exceeds cache object limit");

    std::error_code ec;
    const auto source_status = fs::symlink_status(handle.path, ec);
    if(ec || fs::is_symlink(source_status) || !fs::is_regular_file(source_status))
        return failed("skill_cache_source_invalid", "cache source must be a regular non-link file");
    const auto source_size = fs::file_size(handle.path, ec);
    if(ec || source_size != handle.size)
        return failed("skill_cache_source_changed", "cache source size differs from its snapshot");

    const fs::path objects = root_ / "objects/sha256";
    const fs::path metadata = root_ / "metadata";
    const fs::path transactions = root_ / "transactions";
    fs::create_directories(objects, ec);
    if(ec) return failed("skill_cache_io_error", "cache object directory cannot be created");
    fs::create_directories(metadata, ec);
    if(ec) return failed("skill_cache_io_error", "cache metadata directory cannot be created");
    fs::create_directories(root_ / "derived", ec);
    if(ec) return failed("skill_cache_io_error", "cache derived directory cannot be created");
    fs::create_directories(transactions, ec);
    if(ec) return failed("skill_cache_io_error", "cache transaction directory cannot be created");
    const fs::path state = root_ / "state.json";
    if(!fs::exists(state, ec)) {
        ec.clear();
        const fs::path state_transaction = transactions / transaction_id();
        fs::create_directory(state_transaction, ec);
        if(ec) return failed("skill_cache_io_error", "state transaction cannot be created");
        TransactionCleanup cleanup(state_transaction);
        const fs::path temporary = state_transaction / "state.json";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if(!output) return failed("skill_cache_io_error", "cache state cannot be opened");
            output << nlohmann::json{{"schemaVersion", 1}}.dump(2) << '\n';
            output.flush();
            if(!output) return failed("skill_cache_io_error", "cache state cannot be written");
        }
        fs::rename(temporary, state, ec);
        if(ec && !fs::is_regular_file(state))
            return failed("skill_cache_io_error", "cache state cannot be published");
    } else if(ec || !fs::is_regular_file(state)) {
        return failed("skill_cache_io_error", "cache state is invalid");
    }

    SkillCacheObject object;
    object.digest = digest;
    object.path = objects / digest;
    object.metadata_path = metadata / (digest + ".json");
    object.size = handle.size;
    object.media_type = handle.descriptor.media_type;
    object.source_package_digest = handle.package_digest;
    object.source_resource_id = handle.descriptor.id;

    const auto cached_status = fs::symlink_status(object.path, ec);
    const bool cached_missing =
        (!ec && cached_status.type() == fs::file_type::not_found) ||
        ec == std::errc::no_such_file_or_directory;
    if(cached_missing) ec.clear();
    if(!ec && !cached_missing) {
        if(fs::is_symlink(cached_status) || !fs::is_regular_file(cached_status))
            return failed("skill_cache_object_corrupt", "cache object is not a regular file");
        const auto cached_size = fs::file_size(object.path, ec);
        std::string digest_error;
        const auto cached_digest = !ec ? skill_sha256_file(object.path, &digest_error)
                                       : std::nullopt;
        if(ec || cached_size != object.size || !cached_digest || *cached_digest != digest)
            return failed("skill_cache_object_corrupt", "cache object failed integrity verification");
    } else if(cached_missing) {
        ec.clear();
        const fs::path transaction = transactions / transaction_id();
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
            if(!output)
                return failed("skill_cache_copy_failed", "cache copy cannot be written");
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
        if(ec)
            return failed("skill_cache_publish_failed", "cache object cannot be published");
    } else {
        return failed("skill_cache_io_error", "cache object status cannot be read");
    }

    const fs::path transaction = transactions / transaction_id();
    fs::create_directory(transaction, ec);
    if(ec) return failed("skill_cache_io_error", "metadata transaction cannot be created");
    TransactionCleanup cleanup(transaction);
    std::string metadata_error;
    if(!atomic_metadata(transaction / "metadata.json", object.metadata_path,
                        object, &metadata_error))
        return failed("skill_cache_metadata_failed", metadata_error);

    auto token = leases_[digest].lock();
    if(!token) {
        token = std::make_shared<const std::string>(digest);
        leases_[digest] = token;
    }
    auto lease = std::shared_ptr<SkillCacheLease>(new SkillCacheLease);
    lease->object_ = object;
    lease->token_ = std::move(token);
    return {true, nlohmann::json::object(), object, std::move(lease)};
}

} // namespace agent_framework

#include <agent/skill_resource_cache.hpp>
#include <agent/skill_lifecycle.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << content;
}

bool code_is(const nlohmann::json& error, const char* code) {
    return error.is_object() && error.value("code", "") == code;
}

SkillResourceHandle handle_for(const fs::path& path, const std::string& digest,
                               std::uint64_t size) {
    SkillResourceHandle handle;
    handle.path = path;
    handle.size = size;
    handle.view_size = size;
    handle.resource_digest = digest;
    handle.package_digest = "package-digest";
    handle.descriptor.id = "fixture";
    handle.descriptor.media_type = "application/octet-stream";
    return handle;
}

SkillResourceHandle write_handle(const fs::path& path, const std::string& content,
                                 const std::string& id) {
    write_file(path, content);
    std::string error;
    const auto digest = skill_sha256_file(path, &error);
    assert(digest);
    auto handle = handle_for(path, *digest, content.size());
    handle.descriptor.id = id;
    return handle;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_resource_cache_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path source = base / "source.bin";
    write_file(source, "0123456789");
    const std::string digest =
        "84d89877f0d4041efb6bf91a16f0248f2fd573e6af05c19f96bedb9f882f7882";

    SkillCacheLimits limits;
    limits.max_object_bytes = 64;
    SkillResourceCache cache(base / "cache", limits);
    const auto source_handle = handle_for(source, digest, 10);

    auto no_store_handle = source_handle;
    no_store_handle.descriptor.cache_policy = SkillCachePolicy::NoStore;
    const auto no_store = cache.acquire_policy(no_store_handle);
    assert(no_store.ok && no_store.object && !no_store.lease);
    assert(no_store.object->path == source);
    assert(!fs::exists(base / "cache/objects/sha256" / digest));

    auto on_demand_handle = source_handle;
    on_demand_handle.descriptor.cache_policy = SkillCachePolicy::OnDemand;
    const auto first = cache.acquire_policy(on_demand_handle);
    assert(first.ok && first.object && first.lease);
    assert(first.object->digest == digest);
    assert(first.object->size == 10U);
    assert(first.object->path == base / "cache/objects/sha256" / digest);
    assert(fs::is_regular_file(first.object->path));
    assert(fs::is_regular_file(first.object->metadata_path));
    assert(fs::is_regular_file(base / "cache/state.json"));
    assert(fs::is_directory(base / "cache/derived"));

    nlohmann::json metadata;
    {
        std::ifstream input(first.object->metadata_path);
        input >> metadata;
    }
    assert(metadata.value("schemaVersion", 0) == 1);
    assert(metadata.value("digest", "") == digest);
    assert(metadata.value("size", 0U) == 10U);
    assert(metadata.value("mediaType", "") == "application/octet-stream");
    assert(metadata.value("sourcePackageDigest", "") == "package-digest");
    assert(metadata.value("sourceResourceId", "") == "fixture");
    assert(!metadata.contains("path"));

    const auto second = cache.acquire(source_handle);
    assert(second.ok && second.object && second.lease);
    assert(second.object->path == first.object->path);
    assert(fs::hard_link_count(first.object->path, ec) == 1U && !ec);

    std::vector<std::future<SkillCacheResult>> futures;
    for(int i = 0; i < 8; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            return cache.acquire(source_handle);
        }));
    }
    for(auto& future : futures) {
        const auto result = future.get();
        assert(result.ok && result.object && result.object->digest == digest);
    }
    assert(fs::directory_iterator(base / "cache/transactions") ==
           fs::directory_iterator{});

    auto wrong_digest = source_handle;
    wrong_digest.resource_digest = std::string(64, '0');
    const auto digest_failure = cache.acquire(wrong_digest);
    assert(!digest_failure.ok &&
           code_is(digest_failure.error, "skill_cache_digest_mismatch"));
    assert(fs::directory_iterator(base / "cache/transactions") ==
           fs::directory_iterator{});

    auto too_large = source_handle;
    too_large.size = 65;
    const auto bounded = cache.acquire(too_large);
    assert(!bounded.ok && code_is(bounded.error, "skill_cache_object_too_large"));
    assert(fs::directory_iterator(base / "cache/transactions") ==
           fs::directory_iterator{});

    write_file(first.object->path, "tampered!!");
    const auto tampered = cache.acquire(source_handle);
    assert(!tampered.ok && code_is(tampered.error, "skill_cache_object_corrupt"));

    const fs::path link = base / "source-link.bin";
    fs::create_symlink(source, link, ec);
    if(!ec) {
        const auto linked = cache.acquire(handle_for(link, digest, 10));
        assert(!linked.ok && code_is(linked.error, "skill_cache_source_invalid"));
    }

#if !defined(_WIN32)
    const fs::path fifo = base / "source.fifo";
    if(::mkfifo(fifo.c_str(), 0600) == 0) {
        const auto special = cache.acquire(handle_for(fifo, digest, 10));
        assert(!special.ok && code_is(special.error, "skill_cache_source_invalid"));
    }
#endif

    const fs::path policy_base = base / "policy";
    SkillCacheLimits policy_limits;
    policy_limits.max_object_bytes = 16;
    policy_limits.max_total_bytes = 12;
    SkillResourceCache policy_cache(policy_base / "cache", policy_limits);
    const auto handle_a = write_handle(policy_base / "a.bin", "aaaaaa", "a");
    const auto handle_b = write_handle(policy_base / "b.bin", "bbbbbb", "b");
    const auto handle_c = write_handle(policy_base / "c.bin", "cccccc", "c");
    const auto handle_d = write_handle(policy_base / "d.bin", "dddddd", "d");
    const auto handle_e = write_handle(policy_base / "e.bin", "eeeeee", "e");

    auto acquired_a = policy_cache.acquire(handle_a);
    auto acquired_b = policy_cache.acquire(handle_b);
    assert(acquired_a.ok && acquired_b.ok);
    acquired_b.lease.reset();
    auto acquired_c = policy_cache.acquire(handle_c);
    assert(acquired_c.ok);
    assert(fs::exists(acquired_a.object->path));
    assert(!fs::exists(acquired_b.object->path));

    assert(policy_cache.pin(acquired_a.object->digest).ok);
    acquired_a.lease.reset();
    acquired_c.lease.reset();
    auto acquired_d = policy_cache.acquire(handle_d);
    assert(acquired_d.ok);
    assert(fs::exists(acquired_a.object->path));
    assert(!fs::exists(acquired_c.object->path));
    assert(policy_cache.pin(acquired_d.object->digest).ok);
    acquired_d.lease.reset();
    const auto quota_failure = policy_cache.acquire(handle_e);
    assert(!quota_failure.ok &&
           code_is(quota_failure.error, "skill_cache_quota_exceeded"));

    const auto report = policy_cache.inspect();
    assert(report.ok && report.object_count == 2U && report.total_bytes == 12U);
    assert(report.pinned_bytes == 12U && report.entries.size() == 2U);
    assert(policy_cache.unpin(acquired_a.object->digest).ok);
    assert(policy_cache.collect().ok);

    auto pinned_by_policy = handle_e;
    pinned_by_policy.descriptor.cache_policy = SkillCachePolicy::Pin;
    assert(policy_cache.unpin(acquired_d.object->digest).ok);
    const auto policy_pin = policy_cache.acquire_policy(pinned_by_policy);
    assert(policy_pin.ok && policy_pin.object && policy_pin.object->pinned && policy_pin.lease);

    const fs::path stale = policy_base / "cache/transactions/stale";
    write_file(stale / "partial", "partial");
    fs::remove(acquired_d.object->metadata_path, ec);
    const auto recovered = policy_cache.verify();
    assert(recovered.ok);
    assert(!fs::exists(stale));
    assert(fs::is_regular_file(acquired_d.object->metadata_path));

    write_file(acquired_d.object->path, "broken");
    const auto quarantined = policy_cache.verify();
    assert(quarantined.ok);
    assert(!fs::exists(acquired_d.object->path));
    assert(fs::is_directory(policy_base / "cache/quarantine"));
    assert(fs::directory_iterator(policy_base / "cache/quarantine") !=
           fs::directory_iterator{});

    fs::remove_all(base, ec);
    std::cout << "test_skill_resource_cache: ok\n";
    return 0;
}

#include <agent/skill_resource_cache.hpp>

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

    const auto first = cache.acquire(source_handle);
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

    fs::remove_all(base, ec);
    std::cout << "test_skill_resource_cache: ok\n";
    return 0;
}

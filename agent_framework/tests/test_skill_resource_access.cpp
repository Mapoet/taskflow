#include <agent/skills/skill_manifest.hpp>
#include <agent/skills/skill_resource_access.hpp>
#include <agent/skills/skill_resource_cache.hpp>
#include <agent/agent/task_state_machine.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << content;
}

void write_sparse_file(const fs::path& path, std::uint64_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    assert(size > 0U);
    output.seekp(static_cast<std::streamoff>(size - 1U));
    output.put('\0');
    assert(output.good());
}

SkillIndexEntry make_entry(const fs::path& package,
                           const std::shared_ptr<const SkillManifest>& manifest,
                           std::shared_ptr<const void> lease) {
    SkillIndexEntry entry;
    entry.id = manifest->name;
    entry.file_path = package / "SKILL.md";
    entry.script_jail = package;
    entry.manifest = manifest;
    entry.package_digest = "package-digest";
    entry.package_lease = std::move(lease);
    return entry;
}

std::shared_ptr<const SkillManifest> manifest_for(const fs::path& package) {
    const auto parsed = parse_skill_manifest_yaml(R"YAML(api-version: agent.taskflow/v1
kind: Skill
name: access
version: 1.0.0
description: resource access fixture
resources:
  references:
    - id: guide
      path: references/guide.txt
      media-type: text/plain
      read-mode: stream
  assets:
    - id: blob
      path: assets/blob.bin
      media-type: application/octet-stream
      read-mode: mmap
      sha256: 84d89877f0d4041efb6bf91a16f0248f2fd573e6af05c19f96bedb9f882f7882
      size: 10
      license: Apache-2.0
      source: package://assets/blob.bin
      cache-policy: on-demand
)YAML");
    assert(parsed.manifest);
    const auto issues = validate_skill_manifest(*parsed.manifest, package);
    for(const auto& issue : issues) assert(!issue.error);
    return std::make_shared<const SkillManifest>(*parsed.manifest);
}

bool code_is(const nlohmann::json& error, const char* code) {
    return error.is_object() && error.value("code", "") == code;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_resource_access_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path package = base / "access";
    write_file(package / "SKILL.md", "fixture");
    write_file(package / "references/guide.txt", "0123456789abcdef");
    write_file(package / "assets/blob.bin", "0123456789");
    auto lease_owner = std::make_shared<int>(7);
    std::weak_ptr<int> weak_lease = lease_owner;
    const auto manifest = manifest_for(package);
    auto entry = make_entry(package, manifest, lease_owner);
    lease_owner.reset();

    auto cache = std::make_shared<SkillResourceCache>(base / "cache");
    SkillResourceAccess access(cache);
    SkillResourceOpenOptions range_options;
    range_options.mode = SkillResourceReadMode::Stream;
    range_options.offset = 2;
    range_options.max_bytes = 6;
    auto opened = access.open_snapshot(entry, manifest, "guide", range_options);
    assert(opened.ok && opened.handle);
    assert(opened.handle->view_offset == 2U);
    assert(opened.handle->view_size == 6U);
    assert(opened.handle->resource_digest.size() == 64U);
    assert(!weak_lease.expired());

    const auto bytes = access.read(*opened.handle);
    assert(bytes.ok && bytes.bytes == "234567");

    std::string streamed;
    const auto stream = access.stream(
        *opened.handle, 2,
        [&](std::uint64_t offset, std::string_view chunk) {
            assert(offset == 2U + streamed.size());
            streamed.append(chunk);
            return true;
        });
    assert(stream.ok && stream.bytes_read == 6U && streamed == "234567");

    TaskControl cancelled;
    std::size_t callbacks = 0;
    const auto cancelled_stream = access.stream(
        *opened.handle, 2,
        [&](std::uint64_t, std::string_view) {
            ++callbacks;
            cancelled.request_cancel();
            return true;
        }, &cancelled);
    assert(!cancelled_stream.ok);
    assert(code_is(cancelled_stream.error, "skill_resource_cancelled"));
    assert(callbacks == 1U);

    SkillResourceOpenOptions map_options;
    map_options.mode = SkillResourceReadMode::MemoryMap;
    map_options.offset = 1;
    map_options.max_bytes = 4;
    auto blob = access.open_snapshot(entry, manifest, "blob", map_options);
    assert(blob.ok && blob.handle);
    assert(blob.handle->cache_lease);
    assert(blob.handle->path.parent_path().filename() == "sha256");
    const auto mapped = access.map(*blob.handle);
#if defined(__linux__)
    assert(mapped.ok && mapped.mapping);
    assert(mapped.mapping->as_string_view() == "1234");
#else
    assert(!mapped.ok);
    assert(code_is(mapped.error, "skill_resource_mmap_unavailable"));
#endif

    // A sparse fixture much larger than the requested window proves that the
    // public materialization and streaming paths remain bounded by max_bytes.
    constexpr std::uint64_t large_size = 8U * 1024U * 1024U;
    constexpr std::uint64_t bounded_window = 4096U;
    write_sparse_file(package / "assets/large.bin", large_size);
    auto large_manifest = std::make_shared<SkillManifest>(*manifest);
    SkillResourceDescriptor large_descriptor;
    large_descriptor.id = "large";
    large_descriptor.kind = SkillResourceType::Asset;
    large_descriptor.path = "assets/large.bin";
    large_descriptor.media_type = "application/octet-stream";
    large_descriptor.read_mode = SkillResourceReadMode::Stream;
    large_descriptor.declared_size = large_size;
    large_descriptor.license = "Apache-2.0";
    large_descriptor.source_uri = "package://assets/large.bin";
    large_manifest->resources.push_back(large_descriptor);
    large_descriptor.id = "large-map";
    large_descriptor.read_mode = SkillResourceReadMode::MemoryMap;
    large_manifest->resources.push_back(std::move(large_descriptor));

    SkillResourceOpenOptions large_options;
    large_options.mode = SkillResourceReadMode::Stream;
    large_options.max_bytes = bounded_window;
    auto large = access.open_snapshot(entry, large_manifest, "large", large_options);
    assert(large.ok && large.handle);
    assert(large.handle->descriptor.declared_size == large_size);
    assert(large.handle->view_size == bounded_window);
    const auto large_bytes = access.read(*large.handle);
    assert(large_bytes.ok && large_bytes.bytes.size() == bounded_window);

    std::uint64_t streamed_large_bytes = 0U;
    std::size_t largest_chunk = 0U;
    const auto large_stream = access.stream(
        *large.handle, 512U,
        [&](std::uint64_t offset, std::string_view chunk) {
            assert(offset == streamed_large_bytes);
            streamed_large_bytes += chunk.size();
            largest_chunk = std::max(largest_chunk, chunk.size());
            return true;
        });
    assert(large_stream.ok);
    assert(streamed_large_bytes == bounded_window);
    assert(largest_chunk <= 512U);

    SkillResourceOpenOptions large_map_options;
    large_map_options.mode = SkillResourceReadMode::MemoryMap;
    large_map_options.offset = 1024U;
    large_map_options.max_bytes = bounded_window;
    auto large_map = access.open_snapshot(entry, large_manifest, "large-map", large_map_options);
    assert(large_map.ok && large_map.handle);
    assert(large_map.handle->view_size == bounded_window);
    const auto mapped_large = access.map(*large_map.handle);
#if defined(__linux__)
    assert(mapped_large.ok && mapped_large.mapping);
    assert(mapped_large.mapping->as_string_view().size() == bounded_window);
#else
    assert(!mapped_large.ok);
    assert(code_is(mapped_large.error, "skill_resource_mmap_unavailable"));
#endif

    const auto absent = access.open_snapshot(entry, manifest, "missing", range_options);
    assert(!absent.ok && code_is(absent.error, "skill_resource_not_found"));

    auto wrong_size = std::make_shared<SkillManifest>(*manifest);
    for(auto& resource : wrong_size->resources) {
        if(resource.id == "blob") resource.declared_size = 11U;
    }
    const auto size_failure = access.open_snapshot(entry, wrong_size, "blob", map_options);
    assert(!size_failure.ok && code_is(size_failure.error, "skill_resource_size_mismatch"));

    write_file(package / "assets/blob.bin", "tampered!!");
    const auto digest_failure = access.open_snapshot(entry, manifest, "blob", map_options);
    assert(!digest_failure.ok && code_is(digest_failure.error, "skill_resource_digest_mismatch"));
    write_file(package / "assets/blob.bin", "0123456789");

#if !defined(_WIN32)
    write_file(base / "outside.txt", "outside");
    fs::create_symlink(base / "outside.txt", package / "references/link.txt", ec);
    if(!ec) {
        auto linked = std::make_shared<SkillManifest>(*manifest);
        SkillResourceDescriptor link;
        link.id = "link";
        link.kind = SkillResourceType::Reference;
        link.path = "references/link.txt";
        linked->resources.push_back(link);
        const auto link_failure = access.open_snapshot(entry, linked, "link", range_options);
        assert(!link_failure.ok);
        assert(code_is(link_failure.error, "skill_resource_symlink_forbidden") ||
               code_is(link_failure.error, "skill_resource_jail_escape"));
    }
#endif

    opened.handle.reset();
    blob.handle.reset();
    large.handle.reset();
    large_map.handle.reset();
    entry.package_lease.reset();
    assert(weak_lease.expired());
    fs::remove_all(base, ec);
    std::cout << "test_skill_resource_access: ok\n";
    return 0;
}

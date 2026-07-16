#include <agent/skill_lifecycle.hpp>
#include <agent/skill_model.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

SkillResourceHandle model_handle(const fs::path& path, std::shared_ptr<const void> lease) {
    std::ofstream output(path, std::ios::binary);
    output << "model-data";
    output.close();
    std::string error;
    const auto digest = skill_sha256_file(path, &error);
    assert(digest);
    SkillResourceHandle handle;
    handle.path = path;
    handle.size = 10;
    handle.view_size = 10;
    handle.resource_digest = *digest;
    handle.package_digest = "package-v1";
    handle.package_lease = std::move(lease);
    handle.descriptor.id = "forecast";
    handle.descriptor.kind = SkillResourceType::Model;
    handle.descriptor.media_type = "application/onnx";
    handle.descriptor.runtime = "onnxruntime";
    handle.descriptor.read_mode = SkillResourceReadMode::MemoryMap;
    handle.descriptor.cache_policy = SkillCachePolicy::OnDemand;
    handle.descriptor.model_requirements = SkillModelRequirements{
        {"cpu", "cuda"}, {"fp32", "fp16"}, 1024};
    return handle;
}

bool has(const SkillModelAdmissionResult& result, const std::string& code) {
    return std::find(result.codes.begin(), result.codes.end(), code) != result.codes.end();
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_model_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<const void> weak = owner;
    auto handle = model_handle(base / "model.onnx", owner);
    owner.reset();

    SkillCacheLimits cache_limits;
    cache_limits.max_object_bytes = 64;
    cache_limits.max_total_bytes = 128;
    auto cache = std::make_shared<SkillResourceCache>(base / "cache", cache_limits);
    SkillModelService service(cache);
    SkillModelHostCapabilities host;
    host.runtimes = {"onnxruntime"};
    host.devices = {"cpu"};
    host.precisions = {"fp32"};
    host.available_memory_bytes = 4096;
    host.max_readonly_bytes = 64;
    host.mmap_supported = true;

    const auto ready = service.check(handle, host);
    assert(ready.ok && ready.compatible && ready.codes.empty());
    auto opened = service.open(handle, host);
    assert(opened.ok && opened.handle && opened.handle->cache_lease);
    assert(opened.handle->resource.package_digest == "package-v1");
    handle.package_lease.reset();
    assert(!weak.expired());
    opened.handle.reset();
    assert(weak.expired());

    auto incompatible = host;
    incompatible.runtimes = {"tensorrt"};
    assert(has(service.check(model_handle(base / "m2", nullptr), incompatible),
               "skill_model_runtime_incompatible"));
    incompatible = host;
    incompatible.devices = {"tpu"};
    assert(has(service.check(model_handle(base / "m3", nullptr), incompatible),
               "skill_model_device_incompatible"));
    incompatible = host;
    incompatible.precisions = {"int8"};
    assert(has(service.check(model_handle(base / "m4", nullptr), incompatible),
               "skill_model_precision_incompatible"));
    incompatible = host;
    incompatible.available_memory_bytes = 512;
    assert(has(service.check(model_handle(base / "m5", nullptr), incompatible),
               "skill_model_memory_insufficient"));
    incompatible = host;
    incompatible.max_readonly_bytes = 4;
    assert(has(service.check(model_handle(base / "m6", nullptr), incompatible),
               "skill_model_open_limit"));
    incompatible = host;
    incompatible.mmap_supported = false;
    assert(has(service.check(model_handle(base / "m7", nullptr), incompatible),
               "skill_model_mmap_unavailable"));

    auto executable = model_handle(base / "m8", nullptr);
    executable.descriptor.executable = true;
    assert(has(service.check(executable, host), "skill_model_executable_forbidden"));
    SkillModelService no_cache(nullptr);
    assert(has(no_cache.check(model_handle(base / "m9", nullptr), host),
               "skill_model_cache_unavailable"));
    auto no_store_model = model_handle(base / "m10", nullptr);
    no_store_model.descriptor.cache_policy = SkillCachePolicy::NoStore;
    assert(no_cache.check(no_store_model, host).compatible);
    const auto no_store_open = no_cache.open(no_store_model, host);
    assert(no_store_open.ok && no_store_open.handle && !no_store_open.handle->cache_lease);
    assert(no_store_open.handle->cache_object.path == no_store_model.path);

    auto pinned_model = model_handle(base / "m11", nullptr);
    pinned_model.descriptor.cache_policy = SkillCachePolicy::Pin;
    const auto pinned_open = service.open(pinned_model, host);
    assert(pinned_open.ok && pinned_open.handle->cache_object.pinned);

    fs::remove_all(base, ec);
    std::cout << "test_skill_model: ok\n";
    return 0;
}

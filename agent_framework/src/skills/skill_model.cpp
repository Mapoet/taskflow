#include <agent/skills/skill_model.hpp>

#include <algorithm>

namespace agent_framework {
namespace {

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool intersects(const std::vector<std::string>& lhs,
                const std::vector<std::string>& rhs) {
    return std::any_of(lhs.begin(), lhs.end(),
                       [&](const auto& value) { return contains(rhs, value); });
}

void diagnostic(SkillModelAdmissionResult& result, const char* code,
                const std::string& message) {
    result.codes.emplace_back(code);
    result.diagnostics.push_back({{"code", code}, {"message", message}});
}

} // namespace

SkillModelAdmissionResult SkillModelService::check(
    const SkillResourceHandle& handle, const SkillModelHostCapabilities& host) const {
    SkillModelAdmissionResult result;
    result.ok = true;
    if(handle.descriptor.kind != SkillResourceType::Model)
        diagnostic(result, "skill_model_kind_invalid", "resource is not a Model");
    if(handle.descriptor.executable)
        diagnostic(result, "skill_model_executable_forbidden",
                   "Model resources cannot request executable behavior");
    if(handle.descriptor.cache_policy != SkillCachePolicy::NoStore && !cache_)
        diagnostic(result, "skill_model_cache_unavailable",
                   "content-addressed cache is unavailable");
    if(handle.descriptor.runtime.empty() ||
       !contains(host.runtimes, handle.descriptor.runtime))
        diagnostic(result, "skill_model_runtime_incompatible",
                   "model runtime is not declared by the host");
    if(handle.descriptor.model_requirements) {
        const auto& requirements = *handle.descriptor.model_requirements;
        if(!requirements.devices.empty() && !intersects(requirements.devices, host.devices))
            diagnostic(result, "skill_model_device_incompatible",
                       "host has no compatible model device");
        if(!requirements.precisions.empty() &&
           !intersects(requirements.precisions, host.precisions))
            diagnostic(result, "skill_model_precision_incompatible",
                       "host has no compatible model precision");
        if(requirements.min_memory_bytes > host.available_memory_bytes)
            diagnostic(result, "skill_model_memory_insufficient",
                       "host memory is below the model requirement");
    }
    if(handle.size > host.max_readonly_bytes)
        diagnostic(result, "skill_model_open_limit",
                   "model exceeds the read-only open byte limit");
    if(handle.descriptor.read_mode == SkillResourceReadMode::MemoryMap &&
       !host.mmap_supported)
        diagnostic(result, "skill_model_mmap_unavailable",
                   "host does not support read-only memory mapping");
    result.compatible = result.codes.empty();
    return result;
}

SkillModelOpenResult SkillModelService::open(
    const SkillResourceHandle& handle, const SkillModelHostCapabilities& host) const {
    SkillModelOpenResult result;
    result.admission = check(handle, host);
    if(!result.admission.compatible) {
        result.error = {{"code", "skill_model_incompatible"},
                        {"diagnostics", result.admission.diagnostics}};
        return result;
    }
    SkillCacheResult cached;
    if(cache_) cached = cache_->acquire_policy(handle);
    else {
        cached.ok = true;
        SkillCacheObject object;
        object.digest = handle.resource_digest;
        object.path = handle.path;
        object.size = handle.size;
        object.media_type = handle.descriptor.media_type;
        object.source_package_digest = handle.package_digest;
        object.source_resource_id = handle.descriptor.id;
        cached.object = std::move(object);
    }
    if(!cached.ok) {
        result.error = cached.error;
        return result;
    }
    SkillModelReadOnlyHandle readonly_handle;
    readonly_handle.resource = handle;
    readonly_handle.cache_object = *cached.object;
    readonly_handle.cache_lease = std::move(cached.lease);
    result.handle = std::move(readonly_handle);
    result.ok = true;
    return result;
}

} // namespace agent_framework

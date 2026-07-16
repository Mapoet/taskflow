#include <agent/skill_config.hpp>

#include <agent/schema_validate.hpp>

#include <algorithm>

namespace agent_framework {
namespace {

nlohmann::json failure(const char* code, const std::string& message,
                       nlohmann::json details = nlohmann::json::object()) {
    return {{"code", code}, {"message", message}, {"details", std::move(details)}};
}

SkillConfigResult failed(const char* code, const std::string& message,
                         nlohmann::json details = nlohmann::json::object()) {
    return {false, failure(code, message, std::move(details)), nlohmann::json::object()};
}

const SkillResourceDescriptor* find_resource(const SkillManifest& manifest,
                                             const std::string& id,
                                             SkillResourceType kind) {
    const auto found = std::find_if(manifest.resources.begin(), manifest.resources.end(),
        [&](const auto& resource) { return resource.id == id && resource.kind == kind; });
    return found == manifest.resources.end() ? nullptr : &*found;
}

} // namespace

SkillConfigResult SkillConfigService::resolve(
    const SkillIndexEntry& entry, std::shared_ptr<const SkillManifest> manifest,
    const std::string& resource_id, const SkillConfigResolveOptions& options,
    const SkillInvocationContext& context) const {
    if(!manifest) return failed("skill_config_manifest_missing", "config manifest is unavailable");
    const auto* descriptor = find_resource(*manifest, resource_id, SkillResourceType::Config);
    if(!descriptor) return failed("skill_config_not_found", "Config resource is unavailable");
    if(!descriptor->media_type.empty() && descriptor->media_type != "application/json")
        return failed("skill_config_media_type_unsupported", "Config resources must use application/json");

    SkillResourceOpenOptions open_options;
    open_options.mode = SkillResourceReadMode::Text;
    open_options.max_bytes = context.limits.max_input_bytes;
    auto opened = access_.open_snapshot(entry, manifest, resource_id, open_options);
    if(!opened.ok || !opened.handle) return {false, opened.error, nlohmann::json::object()};
    if(opened.handle->view_size != opened.handle->size)
        return failed(kSkillResourceBudgetExceeded, "Config resource exceeds the input byte budget");
    auto bytes = access_.read(*opened.handle);
    if(!bytes.ok) return {false, bytes.error, nlohmann::json::object()};

    nlohmann::json resolved;
    try { resolved = nlohmann::json::parse(bytes.bytes); }
    catch(const std::exception&) {
        return failed("skill_config_invalid", "Config resource is not valid JSON");
    }
    if(!resolved.is_object() || !options.overrides.is_object())
        return failed("skill_config_invalid", "Config defaults and overrides must be JSON objects");
    resolved.merge_patch(options.overrides);

    const auto package_root = entry.script_jail.value_or(entry.file_path.parent_path());
    SkillPolicyEngine policy(
        skill_permissions_effective(manifest->permissions, descriptor->permissions),
        context.grants, package_root);
    for(const auto& [pointer_text, reference] : options.secret_bindings) {
        const auto decision = policy.authorize_secret(reference);
        if(!decision.allowed)
            return failed(kSkillPermissionDenied, "Config secret reference is not authorized",
                          {{"pointer", pointer_text}, {"reference", reference}});
        if(!context.secret_provider)
            return failed(kSkillDependencyUnavailable, "Config secret provider is unavailable",
                          {{"pointer", pointer_text}, {"reference", reference}});
        std::optional<std::string> secret;
        try { secret = context.secret_provider(reference); }
        catch(...) { return failed(kSkillDependencyUnavailable, "Config secret provider failed"); }
        if(!secret)
            return failed(kSkillDependencyUnavailable, "Config secret reference is unavailable",
                          {{"pointer", pointer_text}, {"reference", reference}});
        try {
            const nlohmann::json::json_pointer pointer(pointer_text);
            if(!resolved.contains(pointer))
                return failed("skill_config_secret_target_missing",
                              "Config secret target must exist in defaults or overrides",
                              {{"pointer", pointer_text}});
            resolved[pointer] = *secret;
        } catch(const std::exception&) {
            return failed("skill_config_pointer_invalid", "Config secret target is not a valid JSON Pointer");
        }
    }

    if(!descriptor->input_schema.empty()) {
        const auto* schema_descriptor = find_resource(
            *manifest, descriptor->input_schema, SkillResourceType::Schema);
        if(!schema_descriptor)
            return failed(kSkillDependencyUnavailable, "Config schema resource is unavailable");
        SkillResourceOpenOptions schema_options;
        schema_options.mode = SkillResourceReadMode::Text;
        schema_options.max_bytes = context.limits.max_input_bytes;
        auto schema_opened = access_.open_snapshot(
            entry, manifest, schema_descriptor->id, schema_options);
        if(!schema_opened.ok || !schema_opened.handle)
            return {false, schema_opened.error, nlohmann::json::object()};
        auto schema_bytes = access_.read(*schema_opened.handle);
        if(!schema_bytes.ok) return {false, schema_bytes.error, nlohmann::json::object()};
        nlohmann::json schema;
        try { schema = nlohmann::json::parse(schema_bytes.bytes); }
        catch(const std::exception&) {
            return failed(kSkillDependencyUnavailable, "Config schema is not valid JSON");
        }
        nlohmann::json validation_error;
        if(!validate_json_instance(schema, resolved, validation_error))
            return failed(kSkillInputInvalid, "Resolved Config does not satisfy its schema",
                          validation_error.value("details", nlohmann::json::object()));
    }
    if(resolved.dump().size() > context.limits.max_output_bytes)
        return failed(kSkillResourceBudgetExceeded, "Resolved Config exceeds the output byte budget");
    return {true, nlohmann::json::object(), std::move(resolved)};
}

} // namespace agent_framework

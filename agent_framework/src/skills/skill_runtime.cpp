#include <agent/skill_runtime.hpp>

#include <agent/schema_validate.hpp>

#include <algorithm>
#include <stdexcept>

namespace agent_framework {
namespace {

nlohmann::json error_json(const char* code, const std::string& message,
                          nlohmann::json details = nlohmann::json::object()) {
    return {{"error", message}, {"code", code}, {"details", std::move(details)}};
}

void emit(const SkillInvocationContext& context, SkillEventType type, const char* code,
          const std::string& skill_id, const std::string& resource_id,
          nlohmann::json details = nlohmann::json::object()) noexcept {
    if (!context.event_sink) return;
    try {
        context.event_sink({type, code ? code : "", skill_id, resource_id, context.task_id,
                            context.run_id, context.attempt, context.iteration, std::move(details)});
    } catch (...) {
        // Observability is deliberately non-interfering.
    }
}

bool stopped(const SkillInvocationContext& context, bool& deadline) {
    deadline = false;
    if (!context.control) return false;
    context.control->check_deadline_now();
    deadline = context.control->is_deadline_exceeded();
    return deadline || context.control->is_cancel_requested();
}

} // namespace

SkillRuntime::SkillRuntime(std::shared_ptr<SkillRegistry> registry,
                           std::shared_ptr<SkillLoader> loader)
    : registry_(std::move(registry)), loader_(std::move(loader)) {
    if (!registry_ || !loader_) throw std::invalid_argument("SkillRuntime requires registry and loader");
}

SkillRuntimeResult SkillRuntime::begin(const std::string& skill_id,
                                       const std::string& resource_id,
                                       SkillResourceType expected_kind,
                                       const nlohmann::json& input,
                                       SkillInvocationContext context) const {
    const auto entry = registry_->get(skill_id);
    const auto manifest = registry_->get_manifest(skill_id);
    if (!entry || !manifest) {
        return {false, error_json(kSkillDependencyUnavailable, "skill is not available",
                                  {{"skill_id", skill_id}}), std::nullopt};
    }
    return begin_resolved(*entry, manifest, resource_id, expected_kind, input,
                          std::move(context));
}

SkillRuntimeResult SkillRuntime::begin_snapshot(const SkillIndexEntry& entry,
                                                std::shared_ptr<const SkillManifest> manifest,
                                                const std::string& resource_id,
                                                SkillResourceType expected_kind,
                                                const nlohmann::json& input,
                                                SkillInvocationContext context) const {
    if (!manifest) {
        return {false, error_json(kSkillDependencyUnavailable, "skill snapshot is unavailable",
                                  {{"skill_id", entry.id}}), std::nullopt};
    }
    return begin_resolved(entry, std::move(manifest), resource_id, expected_kind, input,
                          std::move(context));
}

SkillRuntimeResult SkillRuntime::begin_resolved(const SkillIndexEntry& entry,
                                                std::shared_ptr<const SkillManifest> manifest,
                                                const std::string& resource_id,
                                                SkillResourceType expected_kind,
                                                const nlohmann::json& input,
                                                SkillInvocationContext context) const {
    const std::string& skill_id = entry.id;
    bool deadline = false;
    if (stopped(context, deadline)) {
        emit(context, deadline ? SkillEventType::TimedOut : SkillEventType::Cancelled,
             kSkillCancelled, skill_id, resource_id);
        return {false, error_json(kSkillCancelled, deadline ? "skill deadline exceeded"
                                                           : "skill invocation cancelled"), std::nullopt};
    }
    const auto found = std::find_if(manifest->resources.begin(), manifest->resources.end(),
                                    [&](const SkillResourceDescriptor& resource) {
        return resource.id == resource_id && resource.kind == expected_kind;
    });
    if (found == manifest->resources.end()) {
        return {false, error_json(kSkillDependencyUnavailable, "skill resource is not available",
                                  {{"skill_id", skill_id}, {"resource_id", resource_id},
                                   {"kind", to_string(expected_kind)}}), std::nullopt};
    }
    const auto input_bytes = input.dump().size();
    if (input_bytes > context.limits.max_input_bytes) {
        emit(context, SkillEventType::BudgetExceeded, kSkillResourceBudgetExceeded,
             skill_id, resource_id, {{"direction", "input"}, {"bytes", input_bytes}});
        return {false, error_json(kSkillResourceBudgetExceeded, "skill input exceeds byte budget",
                                  {{"direction", "input"}, {"bytes", input_bytes},
                                   {"limit", context.limits.max_input_bytes}}), std::nullopt};
    }
    SkillInvocationTicket ticket;
    ticket.skill_id = skill_id;
    ticket.entry = entry;
    ticket.manifest = manifest;
    ticket.resource = *found;
    const auto package_root = entry.script_jail.value_or(entry.file_path.parent_path());
    ticket.policy = std::make_shared<SkillPolicyEngine>(manifest->permissions, context.grants,
                                                        package_root);
    ticket.context = std::move(context);
    if (!ticket.resource.input_schema.empty()) {
        auto validation = validate_schema(ticket, ticket.resource.input_schema, input, true);
        if (!validation.ok) return validation;
    }
    emit(ticket.context, SkillEventType::InvocationStarted, "", skill_id, resource_id,
         {{"input_bytes", input_bytes}, {"kind", to_string(expected_kind)}});
    return {true, nlohmann::json::object(), std::move(ticket)};
}

SkillRuntimeResult SkillRuntime::finish(const SkillInvocationTicket& ticket,
                                        const nlohmann::json& output) const {
    bool deadline = false;
    if (stopped(ticket.context, deadline)) {
        emit(ticket.context, deadline ? SkillEventType::TimedOut : SkillEventType::Cancelled,
             kSkillCancelled, ticket.skill_id, ticket.resource.id);
        return {false, error_json(kSkillCancelled, deadline ? "skill deadline exceeded"
                                                           : "skill invocation cancelled"), std::nullopt};
    }
    const auto bytes = output.dump().size();
    if (bytes > ticket.context.limits.max_output_bytes) {
        emit(ticket.context, SkillEventType::BudgetExceeded, kSkillResourceBudgetExceeded,
             ticket.skill_id, ticket.resource.id, {{"direction", "output"}, {"bytes", bytes}});
        return {false, error_json(kSkillResourceBudgetExceeded, "skill output exceeds byte budget",
                                  {{"direction", "output"}, {"bytes", bytes},
                                   {"limit", ticket.context.limits.max_output_bytes}}), std::nullopt};
    }
    if (!ticket.resource.output_schema.empty()) {
        auto validation = validate_schema(ticket, ticket.resource.output_schema, output, false);
        if (!validation.ok) return validation;
    }
    emit(ticket.context, SkillEventType::InvocationCompleted, "", ticket.skill_id,
         ticket.resource.id, {{"output_bytes", bytes}});
    return {true, nlohmann::json::object(), ticket};
}

void SkillRuntime::record_termination(const SkillInvocationTicket& ticket,
                                      bool timed_out) const noexcept {
    emit(ticket.context, timed_out ? SkillEventType::TimedOut : SkillEventType::Cancelled,
         kSkillCancelled, ticket.skill_id, ticket.resource.id);
}

SkillRuntimeResult SkillRuntime::validate_schema(const SkillInvocationTicket& ticket,
                                                 const std::string& schema_id,
                                                 const nlohmann::json& value,
                                                 bool input) const {
    const auto schema_resource = std::find_if(
        ticket.manifest->resources.begin(), ticket.manifest->resources.end(),
        [&](const SkillResourceDescriptor& resource) {
            return resource.id == schema_id && resource.kind == SkillResourceType::Schema;
        });
    if (schema_resource == ticket.manifest->resources.end()) {
        return {false, error_json(kSkillDependencyUnavailable, "schema resource is unavailable",
                                  {{"schema_id", schema_id}}), std::nullopt};
    }
    std::string load_error;
    auto content = loader_->load_resource_snapshot(
        ticket.entry, ticket.manifest, schema_resource->path, SkillResourceKind::Schema,
        1024U * 1024U, &load_error);
    if (!content) {
        return {false, error_json(kSkillDependencyUnavailable, "schema resource cannot be loaded",
                                  {{"schema_id", schema_id}, {"reason", load_error}}), std::nullopt};
    }
    nlohmann::json schema;
    try {
        schema = nlohmann::json::parse(*content);
    } catch (const std::exception& error) {
        return {false, error_json(kSkillDependencyUnavailable, "schema resource is not valid JSON",
                                  {{"schema_id", schema_id}, {"reason", error.what()}}), std::nullopt};
    }
    nlohmann::json validation_error;
    if (validate_json_instance(schema, value, validation_error)) {
        return {true, nlohmann::json::object(), ticket};
    }
    const char* code = input ? kSkillInputInvalid : kSkillOutputInvalid;
    auto details = validation_error.value("details", nlohmann::json::object());
    details["schema_id"] = schema_id;
    details["schema_location"] = schema_resource->path;
    emit(ticket.context, input ? SkillEventType::InputInvalid : SkillEventType::OutputInvalid,
         code, ticket.skill_id, ticket.resource.id, details);
    return {false, error_json(code, input ? "skill input does not satisfy schema"
                                         : "skill output does not satisfy schema",
                              std::move(details)), std::nullopt};
}

nlohmann::json SkillRuntime::permission_error(const SkillPolicyDecision& decision) {
    return error_json(kSkillPermissionDenied, decision.reason,
                      {{"permission", skill_permission_kind_cstr(decision.kind)},
                       {"action", decision.action}, {"target", decision.target}});
}

const char* skill_event_type_cstr(SkillEventType type) noexcept {
    switch (type) {
    case SkillEventType::InvocationStarted: return "invocation_started";
    case SkillEventType::InvocationCompleted: return "invocation_completed";
    case SkillEventType::PermissionDenied: return "permission_denied";
    case SkillEventType::InputInvalid: return "input_invalid";
    case SkillEventType::OutputInvalid: return "output_invalid";
    case SkillEventType::BudgetExceeded: return "budget_exceeded";
    case SkillEventType::Cancelled: return "cancelled";
    case SkillEventType::TimedOut: return "timed_out";
    }
    return "unknown";
}

} // namespace agent_framework

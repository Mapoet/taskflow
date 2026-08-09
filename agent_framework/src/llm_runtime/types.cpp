#include "agent/llm_runtime/types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

namespace agent_framework::llm_runtime {
namespace {

using json = nlohmann::json;

template <typename Enum>
std::optional<Enum> enum_from_name(const std::string& value,
                                   const std::vector<std::pair<const char*, Enum>>& names) {
    const auto found = std::find_if(names.begin(), names.end(), [&](const auto& item) {
        return value == item.first;
    });
    return found == names.end() ? std::nullopt : std::optional<Enum>(found->second);
}

json optional_u64(const std::optional<std::uint64_t>& value) {
    return value ? json(*value) : json(nullptr);
}

json optional_double(const std::optional<double>& value) {
    return value ? json(*value) : json(nullptr);
}

std::optional<std::uint64_t> read_optional_u64(const json& value, const char* key) {
    if(!value.contains(key) || value.at(key).is_null()) return std::nullopt;
    return value.at(key).get<std::uint64_t>();
}

std::optional<double> read_optional_double(const json& value, const char* key) {
    if(!value.contains(key) || value.at(key).is_null()) return std::nullopt;
    return value.at(key).get<double>();
}

json encode_usage(const UsageRecord& value) {
    return {{"input_tokens", optional_u64(value.input_tokens)},
            {"output_tokens", optional_u64(value.output_tokens)},
            {"cached_input_tokens", optional_u64(value.cached_input_tokens)},
            {"cost_usd", optional_double(value.cost_usd)},
            {"source", value.source},
            {"unknown_reason", value.unknown_reason}};
}

UsageRecord decode_usage(const json& value) {
    UsageRecord result;
    result.input_tokens = read_optional_u64(value, "input_tokens");
    result.output_tokens = read_optional_u64(value, "output_tokens");
    result.cached_input_tokens = read_optional_u64(value, "cached_input_tokens");
    result.cost_usd = read_optional_double(value, "cost_usd");
    result.source = value.value("source", "");
    result.unknown_reason = value.value("unknown_reason", "");
    return result;
}

json encode_attempt(const InvocationAttempt& value) {
    return {{"sequence", value.sequence},
            {"candidate_id", value.candidate_id},
            {"provider", value.provider},
            {"model", value.model},
            {"started_at", value.started_at},
            {"finished_at", value.finished_at},
            {"failure_class", failure_class_name(value.failure_class)},
            {"error_code", value.error_code},
            {"error_message", value.error_message},
            {"fallback", value.fallback},
            {"output_repair", value.output_repair},
            {"usage", encode_usage(value.usage)}};
}

InvocationAttempt decode_attempt(const json& value) {
    InvocationAttempt result;
    result.sequence = value.at("sequence").get<std::uint64_t>();
    result.candidate_id = value.at("candidate_id").get<std::string>();
    result.provider = value.at("provider").get<std::string>();
    result.model = value.at("model").get<std::string>();
    result.started_at = value.at("started_at").get<std::string>();
    result.finished_at = value.at("finished_at").get<std::string>();
    const auto failure = failure_class_from_name(value.at("failure_class").get<std::string>());
    if(!failure) throw std::invalid_argument("invalid failure_class");
    result.failure_class = *failure;
    result.error_code = value.at("error_code").get<std::string>();
    result.error_message = value.at("error_message").get<std::string>();
    result.fallback = value.at("fallback").get<bool>();
    result.output_repair = value.at("output_repair").get<bool>();
    result.usage = decode_usage(value.at("usage"));
    return result;
}

std::optional<contracts::TypedContractDocument> parse(
    const json& value, const char* kind, bool require_task,
    const std::set<std::string>& fields, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    auto document = contracts::parse_typed_contract(value, kind, context, issues, require_task);
    if(!document) return std::nullopt;
    if(!contracts::validate_object_fields(document->payload, fields, fields,
                                           context.unknown_fields, nullptr, issues, "/payload"))
        return std::nullopt;
    return document;
}

template <typename T, typename Builder>
std::optional<T> decode_guarded(
    const json& value, const char* kind, bool require_task,
    const std::set<std::string>& fields, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues, Builder builder) {
    auto document = parse(value, kind, require_task, fields, context, issues);
    if(!document) return std::nullopt;
    try {
        T result = builder(document->payload);
        result.metadata = std::move(document->metadata);
        return result;
    } catch(const std::exception& error) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", error.what());
        return std::nullopt;
    }
}

json encode_redactions(const std::vector<contracts::RedactionRule>& rules) {
    json result = json::array();
    for(const auto& rule : rules)
        result.push_back({{"json_pointer", rule.json_pointer}, {"replacement", rule.replacement}});
    return result;
}

std::vector<contracts::RedactionRule> decode_redactions(const json& value) {
    std::vector<contracts::RedactionRule> result;
    for(const auto& item : value)
        result.push_back({item.at("json_pointer").get<std::string>(),
                          item.at("replacement").get<std::string>()});
    return result;
}

json string_json_map(const std::map<std::string, json>& values) {
    json result = json::object();
    for(const auto& [key, value] : values) result[key] = value;
    return result;
}

std::map<std::string, json> decode_json_map(const json& value) {
    std::map<std::string, json> result;
    for(const auto& [key, item] : value.items()) result.emplace(key, item);
    return result;
}

json double_map(const std::map<std::string, double>& values) {
    json result = json::object();
    for(const auto& [key, value] : values) result[key] = value;
    return result;
}

std::map<std::string, double> decode_double_map(const json& value) {
    std::map<std::string, double> result;
    for(const auto& [key, item] : value.items()) result.emplace(key, item.get<double>());
    return result;
}

}  // namespace

std::string reasoning_effort_name(ReasoningEffort value) {
    static const char* names[] = {"minimal", "low", "medium", "high"};
    return names[static_cast<int>(value)];
}

std::optional<ReasoningEffort> reasoning_effort_from_name(const std::string& value) {
    return enum_from_name<ReasoningEffort>(value, {{"minimal", ReasoningEffort::Minimal},
        {"low", ReasoningEffort::Low}, {"medium", ReasoningEffort::Medium},
        {"high", ReasoningEffort::High}});
}

std::string evidence_authority_name(EvidenceAuthority value) {
    static const char* names[] = {"candidate", "derived", "advisory"};
    return names[static_cast<int>(value)];
}

std::optional<EvidenceAuthority> evidence_authority_from_name(const std::string& value) {
    return enum_from_name<EvidenceAuthority>(value, {{"candidate", EvidenceAuthority::Candidate},
        {"derived", EvidenceAuthority::Derived}, {"advisory", EvidenceAuthority::Advisory}});
}

std::string invocation_state_name(InvocationState value) {
    static const char* names[] = {"pending", "running", "succeeded", "failed",
                                  "cancelled", "manual_review"};
    return names[static_cast<int>(value)];
}

std::optional<InvocationState> invocation_state_from_name(const std::string& value) {
    return enum_from_name<InvocationState>(value, {{"pending", InvocationState::Pending},
        {"running", InvocationState::Running}, {"succeeded", InvocationState::Succeeded},
        {"failed", InvocationState::Failed}, {"cancelled", InvocationState::Cancelled},
        {"manual_review", InvocationState::ManualReview}});
}

std::string failure_class_name(FailureClass value) {
    static const char* names[] = {"none", "retryable", "non_retryable", "policy_denied",
                                  "output_invalid", "cancelled"};
    return names[static_cast<int>(value)];
}

std::optional<FailureClass> failure_class_from_name(const std::string& value) {
    return enum_from_name<FailureClass>(value, {{"none", FailureClass::None},
        {"retryable", FailureClass::Retryable}, {"non_retryable", FailureClass::NonRetryable},
        {"policy_denied", FailureClass::PolicyDenied}, {"output_invalid", FailureClass::OutputInvalid},
        {"cancelled", FailureClass::Cancelled}});
}

json encode(const LLMRoleProfile& value) {
    return contracts::make_typed_contract(value.metadata, "agent.llm_role_profile/v1",
        {{"profile_id", value.profile_id}, {"revision", value.revision}, {"role", value.role},
         {"provider_pool", value.provider_pool}, {"reasoning_effort", reasoning_effort_name(value.reasoning_effort)},
         {"temperature", value.temperature}, {"top_p", value.top_p},
         {"max_context_tokens", value.max_context_tokens}, {"max_output_tokens", value.max_output_tokens},
         {"prompt_id", value.prompt_id}, {"prompt_revision", value.prompt_revision},
         {"memory_view_profile", value.memory_view_profile},
         {"required_capabilities", value.required_capabilities}, {"allowed_regions", value.allowed_regions},
         {"independence_group", value.independence_group},
         {"evidence_authority", evidence_authority_name(value.evidence_authority)},
         {"timeout_ms", value.timeout_ms}, {"max_attempts", value.max_attempts},
         {"max_fallbacks", value.max_fallbacks}, {"max_cost_usd", optional_double(value.max_cost_usd)},
         {"calibration_revision", value.calibration_revision}, {"enabled", value.enabled},
         {"provider_parameters", string_json_map(value.provider_parameters)}});
}

json encode(const PromptRevision& value) {
    return contracts::make_typed_contract(value.metadata, "agent.prompt_revision/v1",
        {{"prompt_id", value.prompt_id}, {"revision", value.revision},
         {"system_template", value.system_template}, {"user_template", value.user_template},
         {"input_schema", value.input_schema}, {"output_schema", value.output_schema},
         {"structured_output_required", value.structured_output_required},
         {"max_repair_attempts", value.max_repair_attempts},
         {"redaction_rules", encode_redactions(value.redaction_rules)},
         {"compatibility_class", value.compatibility_class}, {"deprecated", value.deprecated}});
}

json encode(const ModelRouteDecision& value) {
    return contracts::make_typed_contract(value.metadata, "agent.model_route_decision/v1",
        {{"route_id", value.route_id}, {"profile_id", value.profile_id},
         {"profile_revision", value.profile_revision}, {"selected", value.selected},
         {"candidate_id", value.candidate_id}, {"provider", value.provider},
         {"model", value.model}, {"adapter_revision", value.adapter_revision},
         {"decision_code", value.decision_code}, {"decision_message", value.decision_message},
         {"rejected_candidates", value.rejected_candidates},
         {"estimated_cost_usd", optional_double(value.estimated_cost_usd)},
         {"policy_revision", value.policy_revision}});
}

json encode(const LLMInvocationManifest& value) {
    json attempts = json::array();
    for(const auto& attempt : value.attempts) attempts.push_back(encode_attempt(attempt));
    return contracts::make_typed_contract(value.metadata, "agent.llm_invocation_manifest/v1",
        {{"invocation_id", value.invocation_id}, {"state", invocation_state_name(value.state)},
         {"role", value.role}, {"profile_id", value.profile_id},
         {"profile_revision", value.profile_revision}, {"prompt_id", value.prompt_id},
         {"prompt_revision", value.prompt_revision}, {"prompt_digest", value.prompt_digest},
         {"route_decision_digest", value.route_decision_digest}, {"candidate_id", value.candidate_id},
         {"provider", value.provider}, {"model", value.model},
         {"adapter_revision", value.adapter_revision}, {"reasoning_effort", value.reasoning_effort},
         {"independence_group", value.independence_group}, {"evidence_authority", value.evidence_authority},
         {"calibration_revision", value.calibration_revision},
         {"memory_snapshot_id", value.memory_snapshot_id},
         {"memory_view_profile", value.memory_view_profile}, {"memory_view_digest", value.memory_view_digest},
         {"capabilities", value.capabilities}, {"capability_digest", value.capability_digest},
         {"input_digest", value.input_digest}, {"output_digest", value.output_digest},
         {"attempts", std::move(attempts)}, {"usage", encode_usage(value.usage)},
         {"latency_ms", value.latency_ms},
         {"started_at", value.started_at}, {"finished_at", value.finished_at},
         {"error_code", value.error_code}, {"error_message", value.error_message}});
}

json encode(const ReasoningArtifact& value) {
    return contracts::make_typed_contract(value.metadata, "agent.reasoning_artifact/v1",
        {{"artifact_id", value.artifact_id}, {"claims", value.claims},
         {"evidence_ids", value.evidence_ids}, {"assumptions", value.assumptions},
         {"unknowns", value.unknowns}, {"alternatives", value.alternatives},
         {"decision_rationale", value.decision_rationale}, {"risks", value.risks},
         {"counterexamples", value.counterexamples}, {"confidence", value.confidence}});
}

json encode(const RoleCalibrationRecord& value) {
    return contracts::make_typed_contract(value.metadata, "agent.role_calibration_record/v1",
        {{"calibration_id", value.calibration_id}, {"role", value.role},
         {"profile_id", value.profile_id}, {"profile_revision", value.profile_revision},
         {"prompt_revision", value.prompt_revision}, {"provider", value.provider},
         {"model", value.model}, {"dataset_revision", value.dataset_revision},
         {"metrics", double_map(value.metrics)}, {"thresholds", double_map(value.thresholds)},
         {"approved", value.approved}, {"decision_id", value.decision_id}});
}

std::optional<LLMRoleProfile> decode_role_profile(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"profile_id","revision","role","provider_pool",
        "reasoning_effort","temperature","top_p","max_context_tokens","max_output_tokens",
        "prompt_id","prompt_revision","memory_view_profile","required_capabilities","allowed_regions",
        "independence_group","evidence_authority","timeout_ms","max_attempts","max_fallbacks",
        "max_cost_usd","calibration_revision","enabled","provider_parameters"};
    auto result = decode_guarded<LLMRoleProfile>(value, "agent.llm_role_profile/v1", false, fields,
        context, issues, [](const json& p) {
            LLMRoleProfile v;
            v.profile_id=p.at("profile_id").get<std::string>(); v.revision=p.at("revision").get<std::string>();
            v.role=p.at("role").get<std::string>(); v.provider_pool=p.at("provider_pool").get<std::vector<std::string>>();
            const auto effort=reasoning_effort_from_name(p.at("reasoning_effort").get<std::string>());
            const auto authority=evidence_authority_from_name(p.at("evidence_authority").get<std::string>());
            if(!effort || !authority) throw std::invalid_argument("invalid role profile enum");
            v.reasoning_effort=*effort; v.evidence_authority=*authority;
            v.temperature=p.at("temperature").get<double>(); v.top_p=p.at("top_p").get<double>();
            v.max_context_tokens=p.at("max_context_tokens").get<std::uint64_t>();
            v.max_output_tokens=p.at("max_output_tokens").get<int>(); v.prompt_id=p.at("prompt_id").get<std::string>();
            v.prompt_revision=p.at("prompt_revision").get<std::string>();
            v.memory_view_profile=p.at("memory_view_profile").get<std::string>();
            v.required_capabilities=p.at("required_capabilities").get<std::vector<std::string>>();
            v.allowed_regions=p.at("allowed_regions").get<std::vector<std::string>>();
            v.independence_group=p.at("independence_group").get<std::string>();
            v.timeout_ms=p.at("timeout_ms").get<int>(); v.max_attempts=p.at("max_attempts").get<int>();
            v.max_fallbacks=p.at("max_fallbacks").get<int>(); v.max_cost_usd=read_optional_double(p,"max_cost_usd");
            v.calibration_revision=p.at("calibration_revision").get<std::string>();
            v.enabled=p.at("enabled").get<bool>(); v.provider_parameters=decode_json_map(p.at("provider_parameters"));
            return v;
        });
    if(result) {
        const auto validation = validate(*result);
        if(!validation.empty()) {
            if(issues) issues->insert(issues->end(), validation.begin(), validation.end());
            return std::nullopt;
        }
    }
    return result;
}

std::optional<PromptRevision> decode_prompt_revision(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"prompt_id","revision","system_template","user_template",
        "input_schema","output_schema","structured_output_required","max_repair_attempts",
        "redaction_rules","compatibility_class","deprecated"};
    auto result = decode_guarded<PromptRevision>(value, "agent.prompt_revision/v1", false, fields,
        context, issues, [](const json& p) {
            PromptRevision v; v.prompt_id=p.at("prompt_id").get<std::string>();
            v.revision=p.at("revision").get<std::string>(); v.system_template=p.at("system_template").get<std::string>();
            v.user_template=p.at("user_template").get<std::string>(); v.input_schema=p.at("input_schema");
            v.output_schema=p.at("output_schema"); v.structured_output_required=p.at("structured_output_required").get<bool>();
            v.max_repair_attempts=p.at("max_repair_attempts").get<int>();
            v.redaction_rules=decode_redactions(p.at("redaction_rules"));
            v.compatibility_class=p.at("compatibility_class").get<std::string>();
            v.deprecated=p.at("deprecated").get<bool>(); return v;
        });
    if(result) {
        const auto validation = validate(*result);
        if(!validation.empty()) {
            if(issues) issues->insert(issues->end(), validation.begin(), validation.end());
            return std::nullopt;
        }
    }
    return result;
}

std::optional<ModelRouteDecision> decode_route_decision(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"route_id","profile_id","profile_revision","selected",
        "candidate_id","provider","model","adapter_revision","decision_code","decision_message",
        "rejected_candidates","estimated_cost_usd","policy_revision"};
    return decode_guarded<ModelRouteDecision>(value, "agent.model_route_decision/v1", true, fields,
        context, issues, [](const json& p) {
            ModelRouteDecision v; v.route_id=p.at("route_id").get<std::string>();
            v.profile_id=p.at("profile_id").get<std::string>(); v.profile_revision=p.at("profile_revision").get<std::string>();
            v.selected=p.at("selected").get<bool>(); v.candidate_id=p.at("candidate_id").get<std::string>();
            v.provider=p.at("provider").get<std::string>(); v.model=p.at("model").get<std::string>();
            v.adapter_revision=p.at("adapter_revision").get<std::string>();
            v.decision_code=p.at("decision_code").get<std::string>(); v.decision_message=p.at("decision_message").get<std::string>();
            v.rejected_candidates=p.at("rejected_candidates").get<std::vector<std::string>>();
            v.estimated_cost_usd=read_optional_double(p,"estimated_cost_usd");
            v.policy_revision=p.at("policy_revision").get<std::string>(); return v;
        });
}

std::optional<LLMInvocationManifest> decode_invocation_manifest(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"invocation_id","state","role","profile_id",
        "profile_revision","prompt_id","prompt_revision","prompt_digest","route_decision_digest",
        "candidate_id","provider","model","adapter_revision","reasoning_effort","independence_group",
        "evidence_authority","calibration_revision","memory_snapshot_id","memory_view_profile",
        "memory_view_digest","capabilities","capability_digest","input_digest","output_digest",
        "attempts","usage","latency_ms","started_at","finished_at","error_code","error_message"};
    auto result = decode_guarded<LLMInvocationManifest>(value, "agent.llm_invocation_manifest/v1", true, fields,
        context, issues, [](const json& p) {
            LLMInvocationManifest v; v.invocation_id=p.at("invocation_id").get<std::string>();
            const auto state=invocation_state_from_name(p.at("state").get<std::string>());
            if(!state) throw std::invalid_argument("invalid invocation state"); v.state=*state;
            v.role=p.at("role").get<std::string>(); v.profile_id=p.at("profile_id").get<std::string>();
            v.profile_revision=p.at("profile_revision").get<std::string>(); v.prompt_id=p.at("prompt_id").get<std::string>();
            v.prompt_revision=p.at("prompt_revision").get<std::string>(); v.prompt_digest=p.at("prompt_digest").get<std::string>();
            v.route_decision_digest=p.at("route_decision_digest").get<std::string>();
            v.candidate_id=p.at("candidate_id").get<std::string>(); v.provider=p.at("provider").get<std::string>();
            v.model=p.at("model").get<std::string>(); v.adapter_revision=p.at("adapter_revision").get<std::string>();
            v.reasoning_effort=p.at("reasoning_effort").get<std::string>();
            v.independence_group=p.at("independence_group").get<std::string>();
            v.evidence_authority=p.at("evidence_authority").get<std::string>();
            v.calibration_revision=p.at("calibration_revision").get<std::string>();
            v.memory_snapshot_id=p.at("memory_snapshot_id").get<std::string>();
            v.memory_view_profile=p.at("memory_view_profile").get<std::string>();
            v.memory_view_digest=p.at("memory_view_digest").get<std::string>();
            v.capabilities=p.at("capabilities").get<std::vector<std::string>>();
            v.capability_digest=p.at("capability_digest").get<std::string>();
            v.input_digest=p.at("input_digest").get<std::string>(); v.output_digest=p.at("output_digest").get<std::string>();
            for(const auto& item : p.at("attempts")) v.attempts.push_back(decode_attempt(item));
            v.usage=decode_usage(p.at("usage")); v.latency_ms=p.at("latency_ms").get<std::uint64_t>();
            v.started_at=p.at("started_at").get<std::string>();
            v.finished_at=p.at("finished_at").get<std::string>(); v.error_code=p.at("error_code").get<std::string>();
            v.error_message=p.at("error_message").get<std::string>(); return v;
        });
    if(result) {
        const auto validation = validate(*result);
        if(!validation.empty()) {
            if(issues) issues->insert(issues->end(), validation.begin(), validation.end());
            return std::nullopt;
        }
    }
    return result;
}

std::optional<ReasoningArtifact> decode_reasoning_artifact(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"artifact_id","claims","evidence_ids","assumptions",
        "unknowns","alternatives","decision_rationale","risks","counterexamples","confidence"};
    return decode_guarded<ReasoningArtifact>(value, "agent.reasoning_artifact/v1", true, fields,
        context, issues, [](const json& p) {
            ReasoningArtifact v; v.artifact_id=p.at("artifact_id").get<std::string>();
            v.claims=p.at("claims").get<std::vector<std::string>>(); v.evidence_ids=p.at("evidence_ids").get<std::vector<std::string>>();
            v.assumptions=p.at("assumptions").get<std::vector<std::string>>(); v.unknowns=p.at("unknowns").get<std::vector<std::string>>();
            v.alternatives=p.at("alternatives").get<std::vector<std::string>>();
            v.decision_rationale=p.at("decision_rationale").get<std::vector<std::string>>();
            v.risks=p.at("risks").get<std::vector<std::string>>(); v.counterexamples=p.at("counterexamples").get<std::vector<std::string>>();
            v.confidence=p.at("confidence").get<double>(); return v;
        });
}

std::optional<RoleCalibrationRecord> decode_calibration_record(
    const json& value, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"calibration_id","role","profile_id","profile_revision",
        "prompt_revision","provider","model","dataset_revision","metrics","thresholds","approved","decision_id"};
    return decode_guarded<RoleCalibrationRecord>(value, "agent.role_calibration_record/v1", false, fields,
        context, issues, [](const json& p) {
            RoleCalibrationRecord v; v.calibration_id=p.at("calibration_id").get<std::string>();
            v.role=p.at("role").get<std::string>(); v.profile_id=p.at("profile_id").get<std::string>();
            v.profile_revision=p.at("profile_revision").get<std::string>();
            v.prompt_revision=p.at("prompt_revision").get<std::string>(); v.provider=p.at("provider").get<std::string>();
            v.model=p.at("model").get<std::string>(); v.dataset_revision=p.at("dataset_revision").get<std::string>();
            v.metrics=decode_double_map(p.at("metrics")); v.thresholds=decode_double_map(p.at("thresholds"));
            v.approved=p.at("approved").get<bool>(); v.decision_id=p.at("decision_id").get<std::string>(); return v;
        });
}

std::vector<contracts::ContractIssue> validate(const LLMRoleProfile& value) {
    std::vector<contracts::ContractIssue> issues;
    contracts::validate_metadata(value.metadata, &issues, false);
    if(value.profile_id.empty()) contracts::append_issue(&issues,"identity_missing","/payload/profile_id","profile_id is required");
    if(value.revision.empty()) contracts::append_issue(&issues,"revision_missing","/payload/revision","revision is required");
    if(value.role.empty()) contracts::append_issue(&issues,"role_missing","/payload/role","role is required");
    if(value.provider_pool.empty()) contracts::append_issue(&issues,"provider_pool_empty","/payload/provider_pool","provider_pool is required");
    if(value.prompt_id.empty() || value.prompt_revision.empty())
        contracts::append_issue(&issues,"prompt_missing","/payload/prompt_id","prompt id and revision are required");
    if(value.independence_group.empty()) contracts::append_issue(&issues,"independence_missing","/payload/independence_group","independence group is required");
    if(value.temperature < 0.0 || value.temperature > 2.0 || value.top_p <= 0.0 || value.top_p > 1.0)
        contracts::append_issue(&issues,"sampling_invalid","/payload/temperature","sampling values are invalid");
    if(value.max_output_tokens <= 0 || value.timeout_ms <= 0 || value.max_attempts <= 0 || value.max_fallbacks < 0)
        contracts::append_issue(&issues,"budget_invalid","/payload/max_attempts","token/time/attempt budgets must be positive");
    if(value.max_cost_usd && (!std::isfinite(*value.max_cost_usd) || *value.max_cost_usd < 0.0))
        contracts::append_issue(&issues,"cost_invalid","/payload/max_cost_usd","cost budget must be finite and non-negative");
    static const std::set<std::string> protected_request_fields = {
        "model", "messages", "system", "tools", "tool_choice", "stream",
        "temperature", "top_p", "max_tokens"};
    for(const auto& [key, parameter] : value.provider_parameters) {
        (void)parameter;
        std::string normalized=key;
        std::transform(normalized.begin(),normalized.end(),normalized.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        const bool credential_like = normalized.find("api_key")!=std::string::npos ||
            normalized.find("authorization")!=std::string::npos ||
            normalized.find("access_token")!=std::string::npos ||
            normalized.find("password")!=std::string::npos ||
            normalized.find("secret")!=std::string::npos ||
            normalized.find("credential")!=std::string::npos;
        if(protected_request_fields.count(normalized) || credential_like)
            contracts::append_issue(&issues,"provider_parameter_forbidden",
                "/payload/provider_parameters/"+key,
                "provider parameters cannot override routed request identity/content or contain credentials");
    }
    return issues;
}

std::vector<contracts::ContractIssue> validate(const PromptRevision& value) {
    std::vector<contracts::ContractIssue> issues;
    contracts::validate_metadata(value.metadata, &issues, false);
    if(value.prompt_id.empty() || value.revision.empty())
        contracts::append_issue(&issues,"prompt_identity_missing","/payload/prompt_id","prompt id and revision are required");
    if(value.system_template.empty() && value.user_template.empty())
        contracts::append_issue(&issues,"prompt_empty","/payload/system_template","at least one prompt template is required");
    if(!value.input_schema.is_object() || !value.output_schema.is_object())
        contracts::append_issue(&issues,"schema_invalid","/payload/output_schema","input/output schemas must be objects");
    if(value.max_repair_attempts < 0 || value.max_repair_attempts > 3)
        contracts::append_issue(&issues,"repair_budget_invalid","/payload/max_repair_attempts","repair attempts must be between 0 and 3");
    return issues;
}

std::vector<contracts::ContractIssue> validate(const LLMInvocationManifest& value) {
    std::vector<contracts::ContractIssue> issues;
    contracts::validate_metadata(value.metadata, &issues, true);
    if(value.invocation_id.empty()) contracts::append_issue(&issues,"invocation_id_missing","/payload/invocation_id","invocation id is required");
    if(value.role.empty() || value.profile_id.empty() || value.profile_revision.empty())
        contracts::append_issue(&issues,"profile_missing","/payload/profile_id","role profile identity is required");
    if(value.prompt_id.empty() || value.prompt_revision.empty() || value.prompt_digest.empty())
        contracts::append_issue(&issues,"prompt_missing","/payload/prompt_id","prompt identity and digest are required");
    if(value.state == InvocationState::Succeeded && value.output_digest.empty())
        contracts::append_issue(&issues,"output_digest_missing","/payload/output_digest","successful invocation requires output digest");
    return issues;
}

}  // namespace agent_framework::llm_runtime

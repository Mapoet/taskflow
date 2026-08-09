#include "agent/memory_v2/workflows/memory_workflow.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

#include "agent/observability/audit.hpp"

namespace agent_framework::memory_v2::workflows {
namespace {

using json = nlohmann::json;

std::string default_now() { return audit_timestamp_now(); }

json scope_json(const MemoryScope& value) {
    static const char* levels[] = {"system", "organization", "principal", "project", "task", "turn"};
    return {{"tenant_id", value.tenant_id}, {"organization_id", value.organization_id},
            {"principal_id", value.principal_id}, {"agent_id", value.agent_id},
            {"project_id", value.project_id}, {"workspace_id", value.workspace_id},
            {"path_scope", value.path_scope}, {"task_id", value.task_id},
            {"run_id", value.run_id}, {"turn_id", value.turn_id},
            {"level", levels[static_cast<std::size_t>(value.level)]}};
}

json source_json(const MemorySourceArtifact& value, bool instruction_authority) {
    return {{"artifact_id", value.artifact_id}, {"source_kind", value.source_kind},
            {"source_locator", value.source_locator}, {"source_digest", value.source_digest},
            {"scope", scope_json(value.scope)}, {"content", value.content},
            {"evidence_ids", value.evidence_ids}, {"event_time", value.event_time},
            {"instruction_authority", instruction_authority}};
}

bool scope_visible(const MemorySourceArtifact& source, const MemoryWorkflowInput& input) {
    const auto& scope = source.scope;
    const auto& subject = input.subject;
    if(scope.tenant_id.empty() || scope.tenant_id != subject.tenant_id) return false;
    if(!source.acl_principals.empty() &&
       std::find(source.acl_principals.begin(), source.acl_principals.end(),
                 subject.principal_id) == source.acl_principals.end()) return false;
    switch(scope.level) {
        case MemoryLevel::System: return true;
        case MemoryLevel::Organization:
            return !scope.organization_id.empty() && scope.organization_id == subject.organization_id;
        case MemoryLevel::Principal:
            return !scope.principal_id.empty() && scope.principal_id == subject.principal_id;
        case MemoryLevel::Project:
            return !scope.project_id.empty() && scope.project_id == subject.project_id;
        case MemoryLevel::Task:
            return !scope.task_id.empty() && scope.task_id == subject.task_id;
        case MemoryLevel::Turn:
            return !scope.task_id.empty() && scope.task_id == subject.task_id &&
                   !scope.turn_id.empty() && scope.turn_id == subject.turn_id;
    }
    return false;
}

bool trusted_instruction(const MemorySourceArtifact& source) {
    return source.trusted_instruction &&
        (source.source_kind == "system_policy" || source.source_kind == "organization_policy" ||
         source.source_kind == "project_instruction" || source.source_kind == "user_instruction");
}

json input_document(const MemoryWorkflowInput& input) {
    json sources = json::array();
    for(const auto& source : input.sources) {
        auto value = source_json(source, trusted_instruction(source));
        value["acl_principals"] = source.acl_principals;
        sources.push_back(std::move(value));
    }
    return {{"identity", contracts::identity_to_json(input.metadata.identity)},
            {"workflow_id", input.workflow_id}, {"subject", scope_json(input.subject)},
            {"workflow_phase", input.workflow_phase}, {"workflow_event", input.workflow_event},
            {"risk_level", input.risk_level}, {"query", input.query},
            {"sources", std::move(sources)}};
}

json view_document(const MemoryView& view) {
    json records = json::array();
    for(const auto& record : view.records) records.push_back(memory_v2::encode(record));
    return {{"snapshot", memory_v2::encode(view.snapshot)},
            {"manifest", memory_v2::encode(view.manifest)},
            {"records", std::move(records)}, {"fail_closed", view.fail_closed},
            {"error", view.error}};
}

std::optional<MemoryView> decode_view_document(const json& document) {
    try {
        static const std::set<std::string> fields = {
            "snapshot", "manifest", "records", "fail_closed", "error"};
        if(!document.is_object()) return std::nullopt;
        for(const auto& [key, value] : document.items()) {
            (void)value;
            if(!fields.count(key)) return std::nullopt;
        }
        for(const auto& field : fields) if(!document.contains(field)) return std::nullopt;
        MemoryView view;
        auto snapshot = decode_memory_snapshot(document.at("snapshot"));
        auto manifest = decode_memory_view_manifest(document.at("manifest"));
        if(!snapshot || !manifest || !document.at("records").is_array()) return std::nullopt;
        view.snapshot = std::move(*snapshot);
        view.manifest = std::move(*manifest);
        for(const auto& encoded : document.at("records")) {
            auto record = decode_memory_record(encoded);
            if(!record) return std::nullopt;
            view.records.push_back(std::move(*record));
        }
        view.fail_closed = document.at("fail_closed").get<bool>();
        view.error = document.at("error").get<std::string>();
        if(view.snapshot.snapshot_id != view.manifest.snapshot_id ||
           view.manifest.view_digest.empty()) return std::nullopt;
        return view;
    } catch(...) {
        return std::nullopt;
    }
}

const MemoryStageArtifact* latest(const MemoryWorkflowCheckpoint& checkpoint,
                                  MemoryWorkflowStage stage) {
    for(auto iterator = checkpoint.artifacts.rbegin(); iterator != checkpoint.artifacts.rend(); ++iterator)
        if(iterator->stage == stage) return &*iterator;
    return nullptr;
}

bool required_fields(const json& value, const std::set<std::string>& fields, std::string* error) {
    if(!value.is_object()) {
        if(error) *error = "memory stage output must be an object";
        return false;
    }
    for(const auto& field : fields) {
        if(!value.contains(field)) {
            if(error) *error = "memory stage output missing field: " + field;
            return false;
        }
    }
    for(const auto& [field, item] : value.items()) {
        (void)item;
        if(!fields.count(field)) {
            if(error) *error = "memory stage output contains unknown field: " + field;
            return false;
        }
    }
    return true;
}

std::vector<std::string> strings(const json& value, const char* field) {
    if(!value.contains(field) || !value.at(field).is_array())
        throw std::invalid_argument(std::string(field) + " must be an array");
    return value.at(field).get<std::vector<std::string>>();
}

bool unique_nonempty(const std::vector<std::string>& values) {
    std::set<std::string> unique;
    for(const auto& value : values) if(value.empty() || !unique.insert(value).second) return false;
    return true;
}

std::optional<MemoryKind> kind_from_string(std::string_view value) {
    static const std::vector<std::string> names = {
        "instruction", "semantic", "episodic", "procedural", "evidentiary",
        "working", "operational", "conversational"};
    for(std::size_t index = 0; index < names.size(); ++index)
        if(names[index] == value) return static_cast<MemoryKind>(index);
    return std::nullopt;
}

std::optional<MemoryLevel> level_from_string(std::string_view value) {
    static const std::vector<std::string> names = {
        "system", "organization", "principal", "project", "task", "turn"};
    for(std::size_t index = 0; index < names.size(); ++index)
        if(names[index] == value) return static_cast<MemoryLevel>(index);
    return std::nullopt;
}

bool extraction_valid(const json& output, const std::set<std::string>& sources,
                      const std::set<std::string>& evidence,
                      std::string* error) {
    try {
        if(!required_fields(output,
            {"candidates", "discarded_source_ids", "clarification_questions"}, error)) return false;
        if(!output.at("candidates").is_array()) throw std::invalid_argument("candidates must be an array");
        (void)strings(output, "discarded_source_ids");
        (void)strings(output, "clarification_questions");
        std::set<std::string> candidate_ids;
        for(const auto& candidate : output.at("candidates")) {
            if(!required_fields(candidate,
                {"candidate_id", "kind", "content", "source_artifact_ids", "evidence_ids",
                 "confidence", "target_level", "rationale"}, error)) return false;
            const auto id = candidate.at("candidate_id").get<std::string>();
            if(id.empty() || !candidate_ids.insert(id).second)
                throw std::invalid_argument("candidate_id must be non-empty and unique");
            if(!kind_from_string(candidate.at("kind").get<std::string>()) ||
               !level_from_string(candidate.at("target_level").get<std::string>()))
                throw std::invalid_argument("candidate kind or target_level is invalid");
            if(!candidate.at("content").is_object() || !candidate.at("rationale").is_string())
                throw std::invalid_argument("candidate content/rationale has invalid type");
            const auto confidence = candidate.at("confidence").get<double>();
            if(confidence < 0.0 || confidence > 1.0)
                throw std::invalid_argument("candidate confidence must be within [0,1]");
            const auto refs = strings(candidate, "source_artifact_ids");
            if(refs.empty()) throw std::invalid_argument("candidate requires provenance");
            for(const auto& reference : refs) if(!sources.count(reference))
                throw std::invalid_argument("candidate references invisible source: " + reference);
            for(const auto& evidence_id : strings(candidate, "evidence_ids"))
                if(!evidence.count(evidence_id))
                    throw std::invalid_argument("candidate references unknown evidence: " + evidence_id);
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool normalization_valid(const json& output, const json& extraction, std::string* error) {
    try {
        if(!required_fields(output, {"candidates", "entity_links"}, error)) return false;
        if(!output.at("candidates").is_array() || !output.at("entity_links").is_array())
            throw std::invalid_argument("normalized candidates/entity_links must be arrays");
        std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> extracted;
        for(const auto& value : extraction.at("candidates")) {
            const auto id = value.at("candidate_id").get<std::string>();
            const auto source_ids = strings(value, "source_artifact_ids");
            const auto evidence_ids = strings(value, "evidence_ids");
            extracted.emplace(id, std::make_pair(std::set<std::string>(source_ids.begin(), source_ids.end()),
                                                 std::set<std::string>(evidence_ids.begin(), evidence_ids.end())));
        }
        std::set<std::string> normalized;
        for(const auto& candidate : output.at("candidates")) {
            if(!required_fields(candidate,
                {"normalized_candidate_id", "extracted_candidate_id", "canonical_entities",
                 "normalized_claim", "kind", "valid_from", "valid_until",
                 "source_artifact_ids", "evidence_ids", "confidence"}, error)) return false;
            const auto id = candidate.at("normalized_candidate_id").get<std::string>();
            const auto extracted_id = candidate.at("extracted_candidate_id").get<std::string>();
            const auto extraction_found = extracted.find(extracted_id);
            if(id.empty() || !normalized.insert(id).second || extraction_found == extracted.end())
                throw std::invalid_argument("normalized candidate identity is invalid");
            if(!kind_from_string(candidate.at("kind").get<std::string>()) ||
               !candidate.at("normalized_claim").is_string() ||
               candidate.at("normalized_claim").get<std::string>().empty())
                throw std::invalid_argument("normalized candidate claim/kind is invalid");
            (void)strings(candidate, "canonical_entities");
            for(const auto& source_id : strings(candidate, "source_artifact_ids"))
                if(!extraction_found->second.first.count(source_id))
                    throw std::invalid_argument("normalization introduced new provenance: " + source_id);
            for(const auto& evidence_id : strings(candidate, "evidence_ids"))
                if(!extraction_found->second.second.count(evidence_id))
                    throw std::invalid_argument("normalization introduced new evidence: " + evidence_id);
            const auto confidence = candidate.at("confidence").get<double>();
            if(confidence < 0.0 || confidence > 1.0)
                throw std::invalid_argument("normalized confidence must be within [0,1]");
            if(!candidate.at("valid_from").is_string() || !candidate.at("valid_until").is_string())
                throw std::invalid_argument("normalized validity must be strings");
        }
        for(const auto& link : output.at("entity_links")) {
            if(!required_fields(link, {"mention", "canonical_entity", "confidence"}, error)) return false;
            if(!link.at("mention").is_string() || !link.at("canonical_entity").is_string() ||
               !link.at("confidence").is_number())
                throw std::invalid_argument("entity link fields are invalid");
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool consolidation_valid(const json& output, const std::set<std::string>& records,
                         std::string* error) {
    try {
        if(!required_fields(output, {"clusters", "unclustered_record_ids"}, error)) return false;
        if(!output.at("clusters").is_array()) throw std::invalid_argument("clusters must be an array");
        for(const auto& id : strings(output, "unclustered_record_ids"))
            if(!records.count(id)) throw std::invalid_argument("unknown unclustered record: " + id);
        for(const auto& cluster : output.at("clusters")) {
            if(!required_fields(cluster,
                {"cluster_id", "member_record_ids", "canonical_statement",
                 "supersede_record_ids", "rationale", "confidence"}, error)) return false;
            if(cluster.at("cluster_id").get<std::string>().empty() ||
               !cluster.at("canonical_statement").is_string() ||
               !cluster.at("rationale").is_string())
                throw std::invalid_argument("cluster fields are invalid");
            const auto members = strings(cluster, "member_record_ids");
            if(members.empty()) throw std::invalid_argument("cluster has no members");
            for(const auto& id : members) if(!records.count(id))
                throw std::invalid_argument("cluster references unknown record: " + id);
            for(const auto& id : strings(cluster, "supersede_record_ids")) if(!records.count(id))
                throw std::invalid_argument("cluster supersedes unknown record: " + id);
            const auto confidence = cluster.at("confidence").get<double>();
            if(confidence < 0.0 || confidence > 1.0)
                throw std::invalid_argument("cluster confidence must be within [0,1]");
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool conflict_valid(const json& output, const std::set<std::string>& records,
                    std::string* error) {
    try {
        if(!required_fields(output, {"conflicts", "clarification_questions"}, error)) return false;
        if(!output.at("conflicts").is_array()) throw std::invalid_argument("conflicts must be an array");
        (void)strings(output, "clarification_questions");
        for(const auto& conflict : output.at("conflicts")) {
            if(!required_fields(conflict,
                {"conflict_id", "record_ids", "preferred_record_id", "rationale",
                 "authority_comparison", "requires_clarification"}, error)) return false;
            const auto ids = strings(conflict, "record_ids");
            if(ids.size() < 2) throw std::invalid_argument("conflict requires at least two records");
            for(const auto& id : ids) if(!records.count(id))
                throw std::invalid_argument("conflict references unknown record: " + id);
            const auto preferred = conflict.at("preferred_record_id").get<std::string>();
            if(!preferred.empty() && std::find(ids.begin(), ids.end(), preferred) == ids.end())
                throw std::invalid_argument("preferred conflict record is not a member");
            if(!conflict.at("rationale").is_string() ||
               !conflict.at("authority_comparison").is_string() ||
               !conflict.at("requires_clarification").is_boolean())
                throw std::invalid_argument("conflict fields are invalid");
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool task_state_valid(const json& output, const std::set<std::string>& evidence,
                      std::string* error) {
    try {
        if(!required_fields(output,
            {"goal", "status", "attempts", "results", "evidence_ids", "blockers"}, error)) return false;
        if(!output.at("goal").is_string() || output.at("goal").get<std::string>().empty() ||
           !output.at("status").is_string())
            throw std::invalid_argument("task state goal/status are invalid");
        (void)strings(output, "attempts");
        (void)strings(output, "results");
        for(const auto& evidence_id : strings(output, "evidence_ids"))
            if(!evidence.count(evidence_id))
                throw std::invalid_argument("task state references unknown evidence: " + evidence_id);
        (void)strings(output, "blockers");
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool query_plan_valid(const json& output, std::string* error) {
    try {
        if(!required_fields(output, {"queries", "filters", "max_results", "rationale"}, error)) return false;
        const auto queries = strings(output, "queries");
        if(!unique_nonempty(queries)) throw std::invalid_argument("queries must be non-empty and unique");
        if(!output.at("filters").is_object() ||
           !required_fields(output.at("filters"), {"levels", "kinds", "fresh_only"}, error)) return false;
        for(const auto& level : strings(output.at("filters"), "levels"))
            if(!level_from_string(level)) throw std::invalid_argument("query filter has invalid level");
        for(const auto& kind : strings(output.at("filters"), "kinds"))
            if(!kind_from_string(kind)) throw std::invalid_argument("query filter has invalid kind");
        if(!output.at("filters").at("fresh_only").is_boolean() ||
           (!output.at("max_results").is_number_unsigned() &&
            !output.at("max_results").is_number_integer()) ||
           output.at("max_results").get<std::int64_t>() <= 0 ||
           !output.at("rationale").is_string())
            throw std::invalid_argument("query plan fields are invalid");
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool reranking_valid(const json& output, const std::set<std::string>& records,
                     std::uint64_t max_results,
                     std::string* error) {
    try {
        if(!required_fields(output, {"ranking", "excluded"}, error)) return false;
        if(!output.at("ranking").is_array() || !output.at("excluded").is_array())
            throw std::invalid_argument("ranking/excluded must be arrays");
        if(output.at("ranking").size() > max_results)
            throw std::invalid_argument("ranking exceeds deterministic max_results");
        std::set<std::string> seen;
        for(const auto& rank : output.at("ranking")) {
            if(!required_fields(rank, {"record_id", "score", "reason"}, error)) return false;
            const auto id = rank.at("record_id").get<std::string>();
            if(!records.count(id) || !seen.insert(id).second)
                throw std::invalid_argument("ranking references unknown or duplicate record: " + id);
            const auto score = rank.at("score").get<double>();
            if(score < 0.0 || score > 1.0 || !rank.at("reason").is_string())
                throw std::invalid_argument("ranking score/reason is invalid");
        }
        for(const auto& item : output.at("excluded")) {
            if(!required_fields(item, {"record_id", "reason"}, error)) return false;
            const auto id = item.at("record_id").get<std::string>();
            if(!records.count(id) || seen.count(id))
                throw std::invalid_argument("excluded record is unknown or already ranked: " + id);
            seen.insert(id);
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool dynamic_view_valid(const json& output, const std::set<std::string>& records,
                        std::string* error) {
    try {
        if(!required_fields(output,
            {"recommended_mode", "byte_budget", "token_budget", "include_procedural_skills",
             "rationale", "selected_record_ids", "excluded_record_ids"}, error)) return false;
        if(!memory_view_mode(output.at("recommended_mode").get<std::string>()) ||
           (!output.at("byte_budget").is_number_unsigned() &&
            !output.at("byte_budget").is_number_integer()) ||
           (!output.at("token_budget").is_number_unsigned() &&
            !output.at("token_budget").is_number_integer()) ||
           output.at("byte_budget").get<std::int64_t>() <= 0 ||
           output.at("token_budget").get<std::int64_t>() <= 0 ||
           !output.at("include_procedural_skills").is_boolean() ||
           !output.at("rationale").is_string())
            throw std::invalid_argument("dynamic view recommendation fields are invalid");
        const auto selected = strings(output, "selected_record_ids");
        const auto excluded = strings(output, "excluded_record_ids");
        if(!unique_nonempty(selected) || !unique_nonempty(excluded))
            throw std::invalid_argument("dynamic view record IDs must be non-empty and unique");
        std::set<std::string> selected_set(selected.begin(), selected.end());
        for(const auto& id : selected)
            if(!records.count(id)) throw std::invalid_argument("dynamic view selects unknown record: " + id);
        for(const auto& id : excluded) {
            if(!records.count(id)) throw std::invalid_argument("dynamic view excludes unknown record: " + id);
            if(selected_set.count(id))
                throw std::invalid_argument("dynamic view selects and excludes the same record: " + id);
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

bool recommendation_valid(const json& output, const std::set<std::string>& records,
                          const std::set<std::string>& evidence,
                          MemoryStore& store, std::string* error) {
    try {
        if(!required_fields(output, {"recommendations"}, error) ||
           !output.at("recommendations").is_array()) return false;
        std::set<std::string> recommendation_ids;
        for(const auto& item : output.at("recommendations")) {
            if(!required_fields(item,
                {"recommendation_id", "action", "record_id", "expected_revision",
                 "target_level", "rationale", "evidence_ids", "risk"}, error)) return false;
            const auto action = item.at("action").get<std::string>();
            const auto recommendation_id = item.at("recommendation_id").get<std::string>();
            if(recommendation_id.empty() || !recommendation_ids.insert(recommendation_id).second)
                throw std::invalid_argument("recommendation_id must be non-empty and unique");
            if(action != "promote_verified" && action != "promote_authoritative" &&
               action != "forget")
                throw std::invalid_argument("unsupported governance recommendation action");
            const auto id = item.at("record_id").get<std::string>();
            if(!records.count(id)) throw std::invalid_argument("recommendation references invisible record: " + id);
            const auto current = store.current(id);
            if(!current || current->revision != item.at("expected_revision").get<std::uint64_t>())
                throw std::invalid_argument("recommendation has stale record revision: " + id);
            if(!level_from_string(item.at("target_level").get<std::string>()) ||
               !item.at("rationale").is_string() || !item.at("risk").is_string())
                throw std::invalid_argument("governance recommendation fields are invalid");
            for(const auto& evidence_id : strings(item, "evidence_ids"))
                if(!evidence.count(evidence_id))
                    throw std::invalid_argument("recommendation references unknown evidence: " + evidence_id);
        }
        return true;
    } catch(const std::exception& exception) {
        if(error) *error = exception.what();
        return false;
    }
}

MemoryRecord normalized_record(const json& candidate, const MemoryWorkflowInput& input,
                               std::string_view now) {
    MemoryRecord record;
    record.metadata = input.metadata;
    const json identity = {{"tenant_id", input.subject.tenant_id},
                           {"workflow_id", input.workflow_id},
                           {"candidate_id", candidate.at("normalized_candidate_id")}};
    auto digest = contracts::canonical_digest(identity).value();
    std::replace(digest.begin(), digest.end(), ':', '-');
    record.record_id = "memory-candidate-" + digest;
    record.metadata.identity.memory_id = record.record_id;
    record.scope = input.subject;
    if(record.scope.level != MemoryLevel::Turn) record.scope.level = MemoryLevel::Task;
    record.kind = *kind_from_string(candidate.at("kind").get<std::string>());
    record.authority = Authority::Candidate;
    record.status = MemoryStatus::Candidate;
    record.source_kind = "llm_memory_workflow";
    record.source_locator = input.workflow_id + ":normalization";
    record.source_digest = contracts::canonical_digest(candidate).value();
    record.content_type = "application/json";
    record.content = {{"claim", candidate.at("normalized_claim")},
                      {"entities", candidate.at("canonical_entities")},
                      {"evidence_ids", candidate.at("evidence_ids")},
                      {"confidence", candidate.at("confidence")},
                      {"source_artifact_ids", candidate.at("source_artifact_ids")}};
    record.trust_class = "llm-candidate";
    record.purpose = "task-memory-candidate";
    if(!input.subject.principal_id.empty()) record.acl_principals = {input.subject.principal_id};
    record.supports = candidate.at("evidence_ids").get<std::vector<std::string>>();
    record.valid_from = candidate.at("valid_from").get<std::string>();
    record.valid_until = candidate.at("valid_until").get<std::string>();
    record.event_time = std::string(now);
    record.ingest_time = std::string(now);
    record.created_at = std::string(now);
    return record;
}

MemoryRecord task_state_record(const json& state, const MemoryWorkflowInput& input,
                               std::string_view now) {
    json wrapped = {{"tenant_id", input.subject.tenant_id}, {"workflow_id", input.workflow_id},
                    {"kind", "task_state"}};
    auto digest = contracts::canonical_digest(wrapped).value();
    std::replace(digest.begin(), digest.end(), ':', '-');
    MemoryRecord record;
    record.metadata = input.metadata;
    record.record_id = "memory-candidate-" + digest;
    record.metadata.identity.memory_id = record.record_id;
    record.scope = input.subject;
    record.scope.level = MemoryLevel::Task;
    record.kind = MemoryKind::Operational;
    record.authority = Authority::Candidate;
    record.status = MemoryStatus::Candidate;
    record.source_kind = "llm_task_state";
    record.source_locator = input.workflow_id + ":task_state_update";
    record.source_digest = contracts::canonical_digest(state).value();
    record.content_type = "application/json";
    record.content = state;
    record.trust_class = "llm-candidate";
    record.purpose = "task-state-candidate";
    if(!input.subject.principal_id.empty()) record.acl_principals = {input.subject.principal_id};
    record.supports = state.at("evidence_ids").get<std::vector<std::string>>();
    record.event_time = std::string(now);
    record.ingest_time = std::string(now);
    record.created_at = std::string(now);
    return record;
}

bool append_idempotent(MemoryStore& store, const MemoryRecord& record, std::string* error) {
    const auto commit = store.append(record);
    if(commit) return true;
    if(commit.status != CommitStatus::AlreadyExists) {
        if(error) *error = commit.error;
        return false;
    }
    const auto existing = store.current(record.record_id);
    if(!existing || existing->scope.tenant_id != record.scope.tenant_id ||
       existing->source_digest != record.source_digest ||
       existing->status != MemoryStatus::Candidate ||
       existing->authority != Authority::Candidate) {
        if(error) *error = "candidate record id collision or authority mismatch";
        return false;
    }
    return true;
}

std::set<std::string> visible_record_ids(const MemoryView& view,
                                         const std::vector<std::string>& candidates) {
    std::set<std::string> ids(candidates.begin(), candidates.end());
    for(const auto& record : view.records) ids.insert(record.record_id);
    return ids;
}

std::optional<MemoryViewMode> initial_mode(const MemoryWorkflowInput& input) {
    if(const auto mode = memory_view_mode(input.workflow_phase)) return mode;
    return MemoryViewMode::Intake;
}

}  // namespace

MultiLayerMemoryWorkflow::MultiLayerMemoryWorkflow(
    MemoryViewEngine& views, std::shared_ptr<MemoryStore> store,
    MemoryWorkflowCheckpointStore& checkpoints, MemoryStageModel& model)
    : views_(views), store_(std::move(store)), checkpoints_(checkpoints), model_(model) {
    if(!store_) throw std::invalid_argument("memory store is required");
}

MemoryWorkflowResult MultiLayerMemoryWorkflow::run(
    const MemoryWorkflowInput& input, const MemoryWorkflowOptions& options) {
    MemoryWorkflowResult result;
    if(input.workflow_id.empty() || input.metadata.identity.tenant_id.empty() ||
       input.metadata.identity.task_id.empty() || input.subject.tenant_id.empty() ||
       input.subject.tenant_id != input.metadata.identity.tenant_id ||
       input.subject.task_id != input.metadata.identity.task_id ||
       !memory_view_mode(input.workflow_phase) ||
       options.maximum_view_budget.bytes == 0 ||
       options.maximum_view_budget.tokens == 0) {
        result.error_code = "memory_workflow_input_invalid";
        result.error_message = "workflow, tenant, task, matching subject, known phase and positive budgets are required";
        return result;
    }
    const auto now = options.now ? options.now : default_now;
    const auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    const auto input_digest = contracts::canonical_digest(input_document(input)).value_or("");
    auto stored = checkpoints_.load(input.metadata.identity.tenant_id, input.workflow_id);
    MemoryWorkflowCheckpoint checkpoint;
    std::uint64_t store_revision = 0;
    if(stored) {
        checkpoint = stored->checkpoint;
        store_revision = stored->revision;
        if(checkpoint.metadata.identity.task_id != input.metadata.identity.task_id) {
            result.error_code = "memory_checkpoint_scope_mismatch";
            result.error_message = "memory workflow checkpoint belongs to another task";
            return result;
        }
        if(checkpoint.input_digest != input_digest) {
            result.error_code = "memory_checkpoint_input_mismatch";
            result.error_message = "memory workflow checkpoint was created for different input";
            return result;
        }
    } else {
        checkpoint.metadata = input.metadata;
        checkpoint.workflow_id = input.workflow_id;
        checkpoint.input_digest = input_digest;
        checkpoint.updated_at = now();
        const auto commit = checkpoints_.create(checkpoint);
        if(!commit) {
            result.error_code = "memory_checkpoint_create_failed";
            result.error_message = commit.error;
            return result;
        }
        store_revision = commit.revision;
    }

    auto emit = [&](std::string event_type, MemoryWorkflowStage stage,
                    json payload = json::object()) {
        if(options.event_sink)
            options.event_sink({input.workflow_id, store_revision, stage,
                                std::move(event_type), std::move(payload)});
    };
    auto persist = [&]() -> bool {
        checkpoint.revision = store_revision + 1;
        checkpoint.updated_at = now();
        const auto commit = checkpoints_.compare_exchange(checkpoint, store_revision);
        if(!commit) {
            checkpoint.state = MemoryWorkflowState::ManualReview;
            checkpoint.error_code = commit.status == MemoryWorkflowStoreStatus::RevisionConflict
                ? "memory_checkpoint_revision_conflict" : "memory_checkpoint_write_failed";
            checkpoint.error_message = commit.error;
            return false;
        }
        store_revision = commit.revision;
        return true;
    };
    auto fail = [&](std::string code, std::string message,
                    MemoryWorkflowState state = MemoryWorkflowState::Failed) {
        checkpoint.state = state;
        checkpoint.error_code = std::move(code);
        checkpoint.error_message = std::move(message);
        (void)persist();
        emit("memory_workflow_failed", checkpoint.next_stage,
             {{"error_code", checkpoint.error_code}, {"error_message", checkpoint.error_message}});
    };
    auto finish = [&]() {
        result.state = checkpoint.state;
        result.checkpoint = checkpoint;
        result.recommendations = checkpoint.recommendations;
        result.error_code = checkpoint.error_code;
        result.error_message = checkpoint.error_message;
        if(auto view = decode_view_document(checkpoint.base_view_document)) result.base_view = *view;
        if(auto view = decode_view_document(checkpoint.dynamic_view_document)) result.dynamic_view = *view;
        for(const auto& id : checkpoint.candidate_record_ids) {
            auto record = store_->current(id);
            if(record && record->scope.tenant_id == input.subject.tenant_id)
                result.candidates.push_back(std::move(*record));
        }
        return result;
    };

    if(checkpoint.state == MemoryWorkflowState::Completed ||
       checkpoint.state == MemoryWorkflowState::Failed ||
       checkpoint.state == MemoryWorkflowState::Cancelled ||
       checkpoint.state == MemoryWorkflowState::ManualReview) return finish();

    if(checkpoint.state == MemoryWorkflowState::AwaitingClarification) {
        if(options.clarification_answers.empty()) return finish();
        checkpoint.state = MemoryWorkflowState::Running;
        checkpoint.error_code.clear();
        checkpoint.error_message.clear();
        if(!persist()) return finish();
        emit("clarification_received", checkpoint.next_stage,
             {{"answers_digest", contracts::canonical_digest(options.clarification_answers).value_or("")}});
    }
    if(checkpoint.state == MemoryWorkflowState::AwaitingApproval) {
        if(options.approval_decision_id.empty() || !options.governance_validator) return finish();
        for(const auto& recommendation : checkpoint.recommendations) {
            if(!options.governance_validator(recommendation, options.approval_decision_id)) {
                fail("memory_governance_approval_invalid", "governance approval validation failed",
                     MemoryWorkflowState::ManualReview);
                return finish();
            }
        }
        checkpoint.approval_decision_id = options.approval_decision_id;
        checkpoint.state = MemoryWorkflowState::Completed;
        checkpoint.next_stage = MemoryWorkflowStage::Complete;
        checkpoint.completed_stages.push_back("complete");
        if(!persist()) return finish();
        emit("governance_recommendations_approved", MemoryWorkflowStage::Complete,
             {{"decision_id", options.approval_decision_id}});
        return finish();
    }

    MemoryView base_view;
    if(checkpoint.base_view_document.empty()) {
        auto spec = make_view_spec(*initial_mode(input), input.metadata, input.subject,
                                  options.maximum_view_budget);
        base_view = views_.build(spec, now());
        if(base_view.fail_closed) {
            fail("memory_base_view_failed", base_view.error);
            return finish();
        }
        checkpoint.base_snapshot_id = base_view.snapshot.snapshot_id;
        checkpoint.base_view_digest = base_view.manifest.view_digest;
        checkpoint.base_view_document = view_document(base_view);
        if(!persist()) return finish();
        emit("scope_first_view_built", checkpoint.next_stage,
             {{"snapshot_id", checkpoint.base_snapshot_id},
              {"view_digest", checkpoint.base_view_digest}});
    } else {
        auto restored = decode_view_document(checkpoint.base_view_document);
        if(!restored || restored->manifest.view_digest != checkpoint.base_view_digest ||
           restored->snapshot.snapshot_id != checkpoint.base_snapshot_id) {
            fail("memory_pinned_view_invalid", "persisted scope-first Memory View is invalid",
                 MemoryWorkflowState::ManualReview);
            return finish();
        }
        base_view = std::move(*restored);
    }

    std::set<std::string> visible_sources;
    std::set<std::string> visible_evidence;
    json source_context = json::array();
    for(const auto& source : input.sources) {
        if(!scope_visible(source, input)) {
            emit("source_excluded", checkpoint.next_stage,
                 {{"artifact_id", source.artifact_id}, {"reason", "scope_or_acl"}});
            continue;
        }
        if(source.artifact_id.empty() || !visible_sources.insert(source.artifact_id).second) {
            fail("memory_source_identity_invalid", "visible source artifact ids must be non-empty and unique");
            return finish();
        }
        source_context.push_back(source_json(source, trusted_instruction(source)));
        visible_evidence.insert(source.evidence_ids.begin(), source.evidence_ids.end());
    }
    for(const auto& record : base_view.records)
        visible_evidence.insert(record.supports.begin(), record.supports.end());

    auto stage_input = [&](MemoryWorkflowStage stage) {
        json previous = json::object();
        for(const auto& artifact : checkpoint.artifacts)
            previous[memory_workflow_stage_name(artifact.stage)] = artifact.output;
        return json{{"workflow_phase", input.workflow_phase}, {"workflow_event", input.workflow_event},
                    {"risk_level", input.risk_level}, {"query", input.query},
                    {"subject", scope_json(input.subject)}, {"sources", source_context},
                    {"memory_view", {{"snapshot_id", base_view.snapshot.snapshot_id},
                                     {"view_digest", base_view.manifest.view_digest},
                                     {"record_ids", [&] { json ids = json::array();
                                         for(const auto& r : base_view.records) ids.push_back(r.record_id);
                                         return ids; }()}}},
                    {"candidate_record_ids", checkpoint.candidate_record_ids},
                    {"reranked_record_ids", checkpoint.reranked_record_ids},
                    {"clarification_answers", options.clarification_answers},
                    {"stage", memory_workflow_stage_name(stage)},
                    {"previous", std::move(previous)}};
    };

    auto invoke = [&](MemoryWorkflowStage stage, json request_input,
                      std::string* error) -> std::optional<json> {
        const auto name = memory_workflow_stage_name(stage);
        const auto attempt = ++checkpoint.stage_attempts[name];
        if(attempt > options.max_stage_attempts) {
            if(error) *error = "memory stage retry budget exhausted";
            return std::nullopt;
        }
        checkpoint.next_stage = stage;
        if(!persist()) {
            if(error) *error = checkpoint.error_message;
            return std::nullopt;
        }
        if(cancelled()) {
            if(error) *error = "memory workflow cancelled";
            return std::nullopt;
        }
        llm_runtime::IndependenceRequirement independence;
        if(stage == MemoryWorkflowStage::Reranking ||
           stage == MemoryWorkflowStage::GovernanceRecommendation) {
            const auto compared = stage == MemoryWorkflowStage::Reranking
                ? MemoryWorkflowStage::QueryPlanning : MemoryWorkflowStage::Consolidation;
            if(const auto* prior = latest(checkpoint, compared)) {
                if(!prior->independence_group.empty())
                    independence.forbidden_groups.push_back(prior->independence_group);
                if(!prior->provider.empty()) independence.forbidden_providers.push_back(prior->provider);
                if(!prior->model.empty()) independence.forbidden_models.push_back(prior->model);
            }
        }
        MemoryStageRequest request{input.metadata, input.workflow_id, stage, attempt,
                                   base_view, std::move(request_input), independence,
                                   options.cancelled};
        MemoryStageResponse response;
        try {
            response = model_.invoke(request);
        } catch(const std::exception& exception) {
            emit("stage_interrupted", stage, {{"attempt", attempt}, {"error", exception.what()}});
            throw;
        }
        if(!response.ok) {
            if(error) *error = response.error_code + ":" + response.error_message;
            return std::nullopt;
        }
        MemoryStageArtifact artifact;
        artifact.stage = stage;
        artifact.attempt = attempt;
        artifact.invocation_id = response.manifest.invocation_id;
        artifact.manifest_digest = llm_runtime::encode(response.manifest).at("canonical_digest");
        artifact.output_digest = contracts::canonical_digest(response.output).value_or("");
        artifact.provider = response.manifest.provider;
        artifact.model = response.manifest.model;
        artifact.independence_group = response.manifest.independence_group;
        artifact.output = response.output;
        checkpoint.artifacts.push_back(std::move(artifact));
        checkpoint.completed_stages.push_back(name);
        if(!persist()) {
            if(error) *error = checkpoint.error_message;
            return std::nullopt;
        }
        emit("stage_completed", stage, {{"attempt", attempt}});
        return response.output;
    };

    auto advance = [&](MemoryWorkflowStage stage) {
        checkpoint.next_stage = stage;
        return persist();
    };
    auto invoke_or_latest = [&](MemoryWorkflowStage stage,
                                std::string* error) -> std::optional<json> {
        if(const auto* artifact = latest(checkpoint, stage)) return artifact->output;
        return invoke(stage, stage_input(stage), error);
    };

    std::string stage_error;
    auto extraction = invoke_or_latest(MemoryWorkflowStage::Extraction, &stage_error);
    if(!extraction || !extraction_valid(*extraction, visible_sources, visible_evidence, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_extraction_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    if(!advance(MemoryWorkflowStage::Normalization)) return finish();

    auto normalization = invoke_or_latest(MemoryWorkflowStage::Normalization, &stage_error);
    if(!normalization || !normalization_valid(*normalization, *extraction, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_normalization_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    if(checkpoint.candidate_record_ids.empty()) {
        for(const auto& item : normalization->at("candidates")) {
            auto record = normalized_record(item, input, now());
            if(!append_idempotent(*store_, record, &stage_error)) {
                fail("memory_candidate_commit_failed", stage_error, MemoryWorkflowState::ManualReview);
                return finish();
            }
            checkpoint.candidate_record_ids.push_back(record.record_id);
            checkpoint.candidate_record_digests.push_back(record.source_digest);
        }
        if(!persist()) return finish();
    }
    if(!advance(MemoryWorkflowStage::Consolidation)) return finish();

    auto record_ids = visible_record_ids(base_view, checkpoint.candidate_record_ids);
    auto consolidation = invoke_or_latest(MemoryWorkflowStage::Consolidation, &stage_error);
    if(!consolidation || !consolidation_valid(*consolidation, record_ids, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_consolidation_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    if(!advance(MemoryWorkflowStage::ConflictResolution)) return finish();

    auto conflicts = invoke_or_latest(MemoryWorkflowStage::ConflictResolution, &stage_error);
    if(!conflicts || !conflict_valid(*conflicts, record_ids, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_conflict_resolution_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    bool clarification_required = false;
    for(const auto& conflict : conflicts->at("conflicts"))
        clarification_required = clarification_required ||
                                 conflict.at("requires_clarification").get<bool>();
    if(clarification_required && options.clarification_answers.empty()) {
        checkpoint.state = MemoryWorkflowState::AwaitingClarification;
        checkpoint.next_stage = MemoryWorkflowStage::TaskStateUpdate;
        checkpoint.error_code = "memory_clarification_required";
        checkpoint.error_message = conflicts->at("clarification_questions").dump();
        if(!persist()) return finish();
        emit("clarification_required", MemoryWorkflowStage::ConflictResolution,
             {{"questions", conflicts->at("clarification_questions")}});
        return finish();
    }
    if(!advance(MemoryWorkflowStage::TaskStateUpdate)) return finish();

    auto task_state = invoke_or_latest(MemoryWorkflowStage::TaskStateUpdate, &stage_error);
    if(!task_state || !task_state_valid(*task_state, visible_evidence, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_task_state_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    bool task_state_committed = false;
    for(const auto& id : checkpoint.candidate_record_ids) {
        const auto current = store_->current(id);
        if(current && current->source_kind == "llm_task_state") task_state_committed = true;
    }
    if(!task_state_committed) {
        auto record = task_state_record(*task_state, input, now());
        if(!append_idempotent(*store_, record, &stage_error)) {
            fail("memory_task_state_commit_failed", stage_error, MemoryWorkflowState::ManualReview);
            return finish();
        }
        checkpoint.candidate_record_ids.push_back(record.record_id);
        checkpoint.candidate_record_digests.push_back(record.source_digest);
        if(!persist()) return finish();
    }
    if(!advance(MemoryWorkflowStage::QueryPlanning)) return finish();

    record_ids = visible_record_ids(base_view, checkpoint.candidate_record_ids);
    auto query_plan = invoke_or_latest(MemoryWorkflowStage::QueryPlanning, &stage_error);
    if(!query_plan || !query_plan_valid(*query_plan, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_query_planning_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    if(!advance(MemoryWorkflowStage::Reranking)) return finish();

    auto ranking = invoke_or_latest(MemoryWorkflowStage::Reranking, &stage_error);
    if(!ranking || !reranking_valid(*ranking, record_ids,
                                    query_plan->at("max_results").get<std::uint64_t>(),
                                    &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_reranking_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    checkpoint.reranked_record_ids.clear();
    for(const auto& rank : ranking->at("ranking"))
        checkpoint.reranked_record_ids.push_back(rank.at("record_id").get<std::string>());
    if(!persist() || !advance(MemoryWorkflowStage::DynamicView)) return finish();

    auto dynamic = invoke_or_latest(MemoryWorkflowStage::DynamicView, &stage_error);
    if(!dynamic || !dynamic_view_valid(*dynamic, record_ids, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_dynamic_view_failed", stage_error,
             cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    if(checkpoint.dynamic_view_document.empty()) {
        auto current_mode = *initial_mode(input);
        auto recommended = *memory_view_mode(dynamic->at("recommended_mode").get<std::string>());
        MemoryViewRouter router;
        if(recommended != current_mode) {
            const auto allowed = router.route(current_mode, input.workflow_event);
            if(!allowed || *allowed != recommended) {
                fail("memory_dynamic_view_transition_denied",
                     "LLM recommended a view transition not allowed by deterministic router");
                return finish();
            }
        }
        ViewBudget budget;
        budget.bytes = std::min(dynamic->at("byte_budget").get<std::uint64_t>(),
                                options.maximum_view_budget.bytes);
        budget.tokens = std::min(dynamic->at("token_budget").get<std::uint64_t>(),
                                 options.maximum_view_budget.tokens);
        auto spec = make_view_spec(recommended, input.metadata, input.subject, budget);
        spec.include_procedural_skills = dynamic->at("include_procedural_skills").get<bool>();
        auto view = views_.build(spec, now());
        if(view.fail_closed) {
            fail("memory_dynamic_view_build_failed", view.error);
            return finish();
        }
        checkpoint.dynamic_snapshot_id = view.snapshot.snapshot_id;
        checkpoint.dynamic_view_digest = view.manifest.view_digest;
        checkpoint.dynamic_view_document = view_document(view);
        if(!persist()) return finish();
    }
    if(!advance(MemoryWorkflowStage::GovernanceRecommendation)) return finish();

    auto recommendations = invoke_or_latest(MemoryWorkflowStage::GovernanceRecommendation, &stage_error);
    if(!recommendations || !recommendation_valid(*recommendations, record_ids, visible_evidence,
                                                 *store_, &stage_error)) {
        fail(cancelled() ? "memory_workflow_cancelled" : "memory_governance_recommendation_failed",
             stage_error, cancelled() ? MemoryWorkflowState::Cancelled : MemoryWorkflowState::Failed);
        return finish();
    }
    checkpoint.recommendations = recommendations->at("recommendations");
    checkpoint.next_stage = MemoryWorkflowStage::Complete;
    if(checkpoint.recommendations.empty()) {
        checkpoint.state = MemoryWorkflowState::Completed;
        checkpoint.completed_stages.push_back("complete");
        if(!persist()) return finish();
        emit("memory_workflow_completed", MemoryWorkflowStage::Complete);
        return finish();
    }
    checkpoint.state = MemoryWorkflowState::AwaitingApproval;
    checkpoint.error_code = "memory_governance_approval_required";
    checkpoint.error_message = "LLM recommendations require deterministic policy and HITL approval";
    if(!persist()) return finish();
    emit("governance_approval_required", MemoryWorkflowStage::GovernanceRecommendation,
         {{"recommendation_count", checkpoint.recommendations.size()}});
    return finish();
}

}  // namespace agent_framework::memory_v2::workflows

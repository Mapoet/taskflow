#include "agent/memory_v2/workflows/legacy_memory_provider.hpp"

#include <algorithm>
#include <stdexcept>

namespace agent_framework::memory_v2::workflows {
namespace {

std::string stable_id(std::string_view provider, const nlohmann::json& content) {
    auto digest = contracts::canonical_digest(
        {{"provider", provider}, {"content", content}}).value();
    std::replace(digest.begin(), digest.end(), ':', '-');
    return "legacy-v1-" + digest;
}

bool scope_matches(const LegacyMemoryProviderConfig& config, const MemoryQuery& query) {
    const auto& fixed = config.fixed_scope;
    const auto& subject = query.subject;
    return !fixed.tenant_id.empty() && fixed.tenant_id == subject.tenant_id &&
        (fixed.organization_id.empty() || fixed.organization_id == subject.organization_id) &&
        (fixed.principal_id.empty() || fixed.principal_id == subject.principal_id) &&
        (fixed.project_id.empty() || fixed.project_id == subject.project_id) &&
        (fixed.task_id.empty() || fixed.task_id == subject.task_id) &&
        (query.principal_id.empty() || fixed.principal_id.empty() ||
         query.principal_id == fixed.principal_id);
}

MemoryRecord base_record(const LegacyMemoryProviderConfig& config,
                         nlohmann::json content, MemoryKind kind,
                         std::string locator) {
    MemoryRecord record;
    record.metadata.identity.tenant_id = config.fixed_scope.tenant_id;
    record.metadata.identity.organization_id = config.fixed_scope.organization_id;
    record.metadata.identity.principal_id = config.fixed_scope.principal_id;
    record.metadata.identity.project_id = config.fixed_scope.project_id;
    record.metadata.identity.task_id = config.fixed_scope.task_id;
    record.metadata.identity.run_id = config.fixed_scope.run_id;
    record.record_id = stable_id(config.provider_id, content);
    record.metadata.identity.memory_id = record.record_id;
    record.scope = config.fixed_scope;
    record.kind = kind;
    record.authority = Authority::Observed;
    record.status = MemoryStatus::Candidate;
    record.source_kind = "legacy_memory_v1";
    record.source_locator = std::move(locator);
    record.source_digest = contracts::canonical_digest(content).value();
    record.content_type = "application/json";
    record.content = std::move(content);
    record.trust_class = "legacy-unverified";
    record.purpose = "dual-read-shadow";
    if(!record.scope.principal_id.empty()) record.acl_principals = {record.scope.principal_id};
    return record;
}

}  // namespace

LegacyMemoryProvider::LegacyMemoryProvider(
    std::shared_ptr<agent_framework::MemoryStore> legacy,
    LegacyMemoryProviderConfig config)
    : legacy_(std::move(legacy)), config_(std::move(config)) {
    if(!legacy_ || config_.provider_id.empty() || config_.fixed_scope.tenant_id.empty() ||
       config_.session_id.empty())
        throw std::invalid_argument("legacy store, provider, tenant and session are required");
}

std::string LegacyMemoryProvider::id() const { return config_.provider_id; }

ProviderResult LegacyMemoryProvider::fetch(const MemoryQuery& query) {
    ProviderResult result;
    result.provider_id = config_.provider_id;
    result.generation = 1;
    if(!scope_matches(config_, query)) return result;
    try {
        std::size_t message_index = 0;
        for(const auto& message : legacy_->get_conversation_history(
                config_.session_id, config_.max_messages)) {
            auto record = base_record(config_,
                {{"role", message.role}, {"content", message.content},
                 {"timestamp", message.timestamp}, {"message_index", message_index++},
                 {"instruction_authority", false}},
                MemoryKind::Conversational,
                "legacy-session:" + config_.session_id);
            record.scope.level = MemoryLevel::Turn;
            result.records.push_back(std::move(record));
        }
        std::size_t summary_index = 0;
        for(const auto& summary : legacy_->query_long_term_memory(
                config_.summary_query, config_.top_k_summaries)) {
            auto record = base_record(config_,
                {{"session_id", summary.session_id}, {"summary", summary.summary},
                 {"keywords", summary.keywords}, {"summary_index", summary_index++},
                 {"instruction_authority", false}},
                MemoryKind::Semantic,
                "legacy-summary:" + summary.session_id);
            record.scope.level = config_.fixed_scope.level;
            result.records.push_back(std::move(record));
        }
        if(result.records.size() > query.limit) result.records.resize(query.limit);
    } catch(const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

}  // namespace agent_framework::memory_v2::workflows

#include "agent/assurance/production_oracles.hpp"

#include <fstream>

#include "agent/contracts/contract.hpp"
#include "agent/sandbox/workspace.hpp"

namespace agent_framework::assurance {
namespace {
std::optional<std::filesystem::path> safe_path(const std::filesystem::path& root,
                                                const std::filesystem::path& relative) {
    if(relative.empty()||relative.is_absolute()) return std::nullopt;
    for(const auto& part:relative) if(part=="..") return std::nullopt;
    std::error_code ec;const auto canonical_root=std::filesystem::weakly_canonical(root,ec);
    if(ec)return std::nullopt;
    const auto candidate=std::filesystem::weakly_canonical(root/relative,ec);
    if(ec)return std::nullopt;
    const auto root_text=canonical_root.generic_string()+"/";
    const auto candidate_text=candidate.generic_string();
    if(candidate_text!=canonical_root.generic_string()&&candidate_text.rfind(root_text,0)!=0)
        return std::nullopt;
    return candidate;
}
std::string digest_file(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);std::string content(
        (std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
    return contracts::embedded_digest(content).value_or("");
}
VerificationEvidence evidence(std::string id,std::string criterion,std::string kind,
    std::string locator,std::string digest,std::string observed,std::string fresh,
    OracleStrength strength,FindingOutcome outcome) {
    return {std::move(id),std::move(criterion),std::move(kind),std::move(locator),
        std::move(digest),std::move(observed),std::move(fresh),strength,outcome,true};
}
}

RepositoryEvidenceOracle::RepositoryEvidenceOracle(std::filesystem::path root,
    std::vector<RepositoryOracleRule> rules,std::string revision)
    :root_(std::move(root)),rules_(std::move(rules)),revision_(std::move(revision)) {
    std::error_code ec;ready_=!revision_.empty()&&!rules_.empty()&&
        std::filesystem::is_directory(root_,ec)&&!ec;
    nlohmann::json values=nlohmann::json::array();for(const auto&r:rules_)values.push_back(
        {{"criterion_id",r.criterion_id},{"source_kind",r.source_kind},
         {"relative_path",r.relative_path.generic_string()},{"must_exist",r.must_exist},
         {"must_be_nonempty",r.must_be_nonempty},{"expected_digest",r.expected_digest.value_or("")}});
    manifest_digest_=contracts::canonical_digest({{"schema","agent.repository_oracle/v1"},
        {"root",std::filesystem::weakly_canonical(root_,ec).generic_string()},
        {"revision",revision_},{"rules",std::move(values)}}).value_or("");
    ready_=ready_&&!manifest_digest_.empty();
}
std::vector<std::string> RepositoryEvidenceOracle::source_kinds() const {
    std::set<std::string> unique;for(const auto&r:rules_)unique.insert(r.source_kind);
    return {unique.begin(),unique.end()};
}
bool RepositoryEvidenceOracle::supports(std::string_view criterion_id,
                                        std::string_view source_kind) const {
    return std::any_of(rules_.begin(),rules_.end(),[&](const auto& rule){
        return rule.criterion_id==criterion_id&&rule.source_kind==source_kind;
    });
}
OracleResult RepositoryEvidenceOracle::collect(const OracleContext& context) {
    OracleResult out;if(!ready_){out.error="repository oracle is not production ready";return out;}
    for(const auto&r:rules_){const auto path=safe_path(root_,r.relative_path);if(!path){out.error="unsafe repository oracle path";return out;}
        std::error_code ec;const bool exists=std::filesystem::is_regular_file(*path,ec);const auto size=exists?std::filesystem::file_size(*path,ec):0;const auto digest=exists?digest_file(*path):contracts::embedded_digest("missing:"+r.relative_path.generic_string()).value_or("");const bool pass=(!r.must_exist||exists)&&(!r.must_be_nonempty||size>0)&&(!r.expected_digest||*r.expected_digest==digest);const auto id="repo:"+r.criterion_id+":"+r.relative_path.generic_string()+":"+digest;out.evidence.push_back(evidence(id,r.criterion_id,r.source_kind,"file://"+path->generic_string(),digest,context.now,context.now,OracleStrength::RealSystem,pass?FindingOutcome::Pass:FindingOutcome::Fail));if(!pass)out.findings.push_back({"repository:"+r.criterion_id,r.criterion_id,"mandatory",FindingOutcome::Fail,1.0,{id},"restore the required repository artifact"});}
    return out;
}

SandboxCommandOracle::SandboxCommandOracle(sandbox::SandboxProvider& provider,
    std::filesystem::path workspace,std::vector<SandboxOracleRule> rules,
    std::string policy_revision,std::string revision)
    :provider_(provider),workspace_(std::move(workspace)),rules_(std::move(rules)),
     policy_revision_(std::move(policy_revision)),revision_(std::move(revision)) {
    std::string reason;ready_=!rules_.empty()&&!policy_revision_.empty()&&!revision_.empty()&&
        provider_.available(&reason);nlohmann::json values=nlohmann::json::array();for(const auto&r:rules_)values.push_back({{"id",r.rule_id},{"criterion_id",r.criterion_id},{"source_kind",r.source_kind},{"command",r.command},{"wall_time_ms",r.wall_time_ms}});manifest_digest_=contracts::canonical_digest({{"schema","agent.sandbox_command_oracle/v1"},{"provider",provider_.id()},{"provider_version",provider_.version()},{"policy_revision",policy_revision_},{"revision",revision_},{"rules",std::move(values)}}).value_or("");ready_=ready_&&!manifest_digest_.empty();
}
std::vector<std::string> SandboxCommandOracle::source_kinds() const {std::set<std::string>unique;for(const auto&r:rules_)unique.insert(r.source_kind);return{unique.begin(),unique.end()};}
bool SandboxCommandOracle::supports(std::string_view criterion_id,
                                    std::string_view source_kind) const {
    return std::any_of(rules_.begin(),rules_.end(),[&](const auto& rule){
        return rule.criterion_id==criterion_id&&rule.source_kind==source_kind;
    });
}
OracleResult SandboxCommandOracle::collect(const OracleContext& context) {
    OracleResult out;if(!ready_){out.error="sandbox command oracle is not production ready";return out;}
    std::string error;const auto workspace=sandbox::snapshot_workspace(workspace_,512ULL*1024ULL*1024ULL,&error);if(!workspace){out.error="workspace snapshot failed:"+error;return out;}
    for(const auto&r:rules_){sandbox::SandboxSpec spec;spec.metadata=context.metadata;spec.provider=provider_.id();spec.workspace_base_digest=workspace->digest;spec.command=r.command;spec.read_only_mounts={workspace_.string()+":/workspace"};spec.cpu_millis=r.wall_time_ms;spec.memory_bytes=1024ULL*1024ULL*1024ULL;spec.wall_time_ms=r.wall_time_ms;spec.policy_revision=policy_revision_;spec.memory_view_digest="sha256:oracle-view";auto handle=provider_.create(spec,&error);if(!handle){out.error=r.rule_id+":create:"+error;return out;}auto result=provider_.exec(*handle,&error);const bool destroyed=provider_.destroy(*handle,&error);if(!result||!destroyed){out.error=r.rule_id+":execute:"+error;return out;}const bool pass=!result->timed_out&&r.passing_exit_codes.count(result->exit_code);const auto digest=contracts::canonical_digest(sandbox::encode(result->manifest)).value_or("");const auto id="sandbox:"+r.rule_id+":"+digest;out.evidence.push_back(evidence(id,r.criterion_id,r.source_kind,"sandbox://"+result->manifest.sandbox_id,digest,context.now,context.now,OracleStrength::RealSystem,pass?FindingOutcome::Pass:FindingOutcome::Fail));if(!pass)out.findings.push_back({"sandbox:"+r.rule_id,r.criterion_id,"mandatory",FindingOutcome::Fail,1.0,{id},result->timed_out?"command timed out":"command exited with failure"});}
    return out;
}

PolicyBoundDomainOracle::PolicyBoundDomainOracle(std::string source_kind,
    std::shared_ptr<DomainOracleAdapter> adapter,std::string policy_revision)
    :source_kind_(std::move(source_kind)),policy_revision_(std::move(policy_revision)),adapter_(std::move(adapter)) {
    ready_=adapter_&&!source_kind_.empty()&&!adapter_->id().empty()&&!adapter_->revision().empty()&&!policy_revision_.empty();manifest_digest_=contracts::canonical_digest({{"schema","agent.domain_oracle/v1"},{"source_kind",source_kind_},{"adapter_id",adapter_?adapter_->id():""},{"adapter_revision",adapter_?adapter_->revision():""},{"policy_revision",policy_revision_}}).value_or("");ready_=ready_&&!manifest_digest_.empty();
}
std::string PolicyBoundDomainOracle::id()const{return "production.domain."+(adapter_?adapter_->id():"missing");}
OracleResult PolicyBoundDomainOracle::collect(const OracleContext& context){OracleResult out;if(!ready_){out.error="domain oracle adapter is unavailable";return out;}for(const auto&criterion:context.contract.criteria)if(std::find(criterion.required_evidence.begin(),criterion.required_evidence.end(),source_kind_)!=criterion.required_evidence.end()){auto result=adapter_->verify(context,criterion);out.evidence.insert(out.evidence.end(),std::make_move_iterator(result.evidence.begin()),std::make_move_iterator(result.evidence.end()));out.findings.insert(out.findings.end(),std::make_move_iterator(result.findings.begin()),std::make_move_iterator(result.findings.end()));if(!result.error.empty()){out.error=result.error;return out;}}return out;}

}  // namespace agent_framework::assurance

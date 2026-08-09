#include "agent/telemetry/types.hpp"

#include <set>

namespace agent_framework::telemetry {
namespace {
using json=nlohmann::json;
template<typename T, typename Builder> std::optional<T> decode_value(const json& value,const char* kind,const std::set<std::string>& fields,const contracts::ParseContext& context,std::vector<contracts::ContractIssue>* issues,Builder builder){
 auto d=contracts::parse_typed_contract(value,kind,context,issues); if(!d||!contracts::validate_object_fields(d->payload,fields,fields,contracts::UnknownFieldPolicy::Reject,nullptr,issues,"/payload")) return std::nullopt;
 try { T r=builder(d->payload); r.metadata=std::move(d->metadata); return r; } catch(const std::exception& e){contracts::append_issue(issues,"payload_decode_failed","/payload",e.what()); return std::nullopt;}}
}
json encode(const CorrelationContext& v){return contracts::make_typed_contract(v.metadata,"agent.correlation_context/v1",{{"trace_id",v.trace_id},{"span_id",v.span_id},{"parent_span_id",v.parent_span_id},{"node_id",v.node_id},{"evidence_id",v.evidence_id},{"artifact_digest",v.artifact_digest},{"approval_id",v.approval_id},{"memory_snapshot_id",v.memory_snapshot_id},{"memory_view_digest",v.memory_view_digest},{"sandbox_id",v.sandbox_id}});}
json encode(const MetricResult& v){return contracts::make_typed_contract(v.metadata,"agent.metric_result/v1",{{"metric_name",v.metric_name},{"value",v.value},{"unit",v.unit},{"threshold",v.threshold},{"outcome",v.outcome},{"sample_count",v.sample_count},{"evidence_ids",v.evidence_ids}});}
std::optional<CorrelationContext> decode_correlation_context(const json& value,const contracts::ParseContext& context,std::vector<contracts::ContractIssue>* issues){
 static const std::set<std::string> f={"trace_id","span_id","parent_span_id","node_id","evidence_id","artifact_digest","approval_id","memory_snapshot_id","memory_view_digest","sandbox_id"};
 return decode_value<CorrelationContext>(value,"agent.correlation_context/v1",f,context,issues,[](const json&p){CorrelationContext v;v.trace_id=p.at("trace_id").get<std::string>();v.span_id=p.at("span_id").get<std::string>();v.parent_span_id=p.at("parent_span_id").get<std::string>();v.node_id=p.at("node_id").get<std::string>();v.evidence_id=p.at("evidence_id").get<std::string>();v.artifact_digest=p.at("artifact_digest").get<std::string>();v.approval_id=p.at("approval_id").get<std::string>();v.memory_snapshot_id=p.at("memory_snapshot_id").get<std::string>();v.memory_view_digest=p.at("memory_view_digest").get<std::string>();v.sandbox_id=p.at("sandbox_id").get<std::string>();return v;});}
std::optional<MetricResult> decode_metric_result(const json& value,const contracts::ParseContext& context,std::vector<contracts::ContractIssue>* issues){
 static const std::set<std::string> f={"metric_name","value","unit","threshold","outcome","sample_count","evidence_ids"};
 return decode_value<MetricResult>(value,"agent.metric_result/v1",f,context,issues,[](const json&p){MetricResult v;v.metric_name=p.at("metric_name").get<std::string>();v.value=p.at("value").get<double>();v.unit=p.at("unit").get<std::string>();v.threshold=p.at("threshold").get<std::string>();v.outcome=p.at("outcome").get<std::string>();v.sample_count=p.at("sample_count").get<std::uint64_t>();v.evidence_ids=p.at("evidence_ids").get<std::vector<std::string>>();return v;});}
}  // namespace agent_framework::telemetry

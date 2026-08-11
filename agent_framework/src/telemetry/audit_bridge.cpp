#include "agent/telemetry/audit_bridge.hpp"

#include <stdexcept>

namespace agent_framework::telemetry {
AuditBridgeTelemetrySink::AuditBridgeTelemetrySink(std::shared_ptr<TelemetrySink> downstream,
                                                   std::shared_ptr<AuditSink> audit)
    : downstream_(std::move(downstream)), audit_(std::move(audit)) {
    if (!downstream_ || !audit_) throw std::invalid_argument("telemetry and audit sinks are required");
}
bool AuditBridgeTelemetrySink::export_span(const SpanRecord& span) {
    if (!downstream_->export_span(span)) return false;
    AuditEvent event; event.timestamp = audit_timestamp_now(); event.trace_id = span.context.trace_id;
    event.tenant_id = span.context.metadata.identity.tenant_id;
    event.task_id = span.context.metadata.identity.task_id;
    event.component = "telemetry"; event.event_kind = "span"; event.capability_id = span.name;
    event.outcome = span.status; event.payload = redact_audit_payload({{"span_id", span.context.span_id},
        {"parent_span_id", span.context.parent_span_id}, {"attributes", span.attributes}});
    event.payload_digest = audit_payload_digest(event.payload); audit_->write(event); return true;
}
bool AuditBridgeTelemetrySink::export_metric(const MetricResult& metric) {
    if (!downstream_->export_metric(metric)) return false;
    AuditEvent event; event.timestamp = audit_timestamp_now();
    event.tenant_id = metric.metadata.identity.tenant_id; event.task_id = metric.metadata.identity.task_id;
    event.component = "telemetry"; event.event_kind = "metric"; event.capability_id = metric.metric_name;
    event.outcome = metric.outcome; event.payload = {{"value", metric.value}, {"unit", metric.unit},
        {"sample_count", metric.sample_count}, {"evidence_ids", metric.evidence_ids}};
    event.payload_digest = audit_payload_digest(event.payload); audit_->write(event); return true;
}
bool AuditBridgeTelemetrySink::flush() { return downstream_->flush(); }
}  // namespace agent_framework::telemetry

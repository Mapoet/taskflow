#include "agent/telemetry/runtime.hpp"

#include <algorithm>
#include <stdexcept>

namespace agent_framework::telemetry {

bool InMemoryTelemetrySink::export_span(const SpanRecord& span) {
    std::lock_guard lock(mutex_); spans_.push_back(span); return true;
}
bool InMemoryTelemetrySink::export_metric(const MetricResult& metric) {
    std::lock_guard lock(mutex_); metrics_.push_back(metric); return true;
}
std::vector<SpanRecord> InMemoryTelemetrySink::spans() const {
    std::lock_guard lock(mutex_); return spans_;
}
std::vector<MetricResult> InMemoryTelemetrySink::metrics() const {
    std::lock_guard lock(mutex_); return metrics_;
}

TelemetryRuntime::TelemetryRuntime(std::shared_ptr<TelemetrySink> sink, TelemetryPolicy policy)
    : sink_(std::move(sink)), policy_(std::move(policy)) {
    if(!sink_) throw std::invalid_argument("telemetry sink is required");
}

bool TelemetryRuntime::attributes_safe(
    const std::map<std::string, std::string>& attributes, std::string* error) const {
    for(const auto& [key, value] : attributes) {
        if(std::find(policy_.allowed_attribute_keys.begin(), policy_.allowed_attribute_keys.end(), key) ==
           policy_.allowed_attribute_keys.end()) {
            if(error) *error = "attribute key is not allowlisted: " + key;
            return false;
        }
        if(value.size() > policy_.max_attribute_length || value.find("-----BEGIN") != std::string::npos ||
           value.find("Bearer ") != std::string::npos || value.find("password=") != std::string::npos) {
            if(error) *error = "attribute value violates privacy policy";
            return false;
        }
    }
    return true;
}

bool TelemetryRuntime::emit_span(SpanRecord span, std::string* error) {
    if(span.context.trace_id.empty() || span.context.span_id.empty() || span.name.empty() ||
       !attributes_safe(span.attributes, error)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if(!sink_->export_span(span)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if(error) *error = "span export failed";
        return false;
    }
    return true;
}

bool TelemetryRuntime::emit_metric(MetricResult metric, std::string* error) {
    if(metric.metric_name.empty() || metric.metadata.identity.tenant_id.empty()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if(error) *error = "metric name and tenant are required";
        return false;
    }
    if(!sink_->export_metric(metric)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if(error) *error = "metric export failed";
        return false;
    }
    return true;
}

}  // namespace agent_framework::telemetry

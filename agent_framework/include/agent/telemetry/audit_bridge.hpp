#pragma once

#include "agent/observability/audit.hpp"
#include "agent/telemetry/runtime.hpp"

namespace agent_framework::telemetry {

class AuditBridgeTelemetrySink final : public TelemetrySink {
public:
    AuditBridgeTelemetrySink(std::shared_ptr<TelemetrySink> downstream,
                             std::shared_ptr<AuditSink> audit);
    bool export_span(const SpanRecord& span) override;
    bool export_metric(const MetricResult& metric) override;
    bool flush() override;
private:
    std::shared_ptr<TelemetrySink> downstream_;
    std::shared_ptr<AuditSink> audit_;
};
}  // namespace agent_framework::telemetry

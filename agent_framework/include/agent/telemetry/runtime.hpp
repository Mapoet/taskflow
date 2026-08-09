#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/telemetry/types.hpp"

namespace agent_framework::telemetry {

struct SpanRecord {
    CorrelationContext context;
    std::string name;
    std::string started_at;
    std::string finished_at;
    std::string status;
    std::map<std::string, std::string> attributes;
};

class TelemetrySink {
public:
    virtual ~TelemetrySink() = default;
    virtual bool export_span(const SpanRecord& span) = 0;
    virtual bool export_metric(const MetricResult& metric) = 0;
    virtual bool flush() = 0;
};

class InMemoryTelemetrySink final : public TelemetrySink {
public:
    bool export_span(const SpanRecord& span) override;
    bool export_metric(const MetricResult& metric) override;
    bool flush() override { return true; }
    std::vector<SpanRecord> spans() const;
    std::vector<MetricResult> metrics() const;
private:
    mutable std::mutex mutex_;
    std::vector<SpanRecord> spans_;
    std::vector<MetricResult> metrics_;
};

struct TelemetryPolicy {
    std::vector<std::string> allowed_attribute_keys;
    std::size_t max_attribute_length{256};
};

class TelemetryRuntime {
public:
    TelemetryRuntime(std::shared_ptr<TelemetrySink> sink, TelemetryPolicy policy);
    bool emit_span(SpanRecord span, std::string* error = nullptr);
    bool emit_metric(MetricResult metric, std::string* error = nullptr);
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    bool attributes_safe(const std::map<std::string, std::string>& attributes,
                         std::string* error) const;
    std::shared_ptr<TelemetrySink> sink_;
    TelemetryPolicy policy_;
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace agent_framework::telemetry

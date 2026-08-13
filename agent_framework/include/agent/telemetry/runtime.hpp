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

enum class SamplingDecision { Drop, Record };
struct SamplingContext { std::string trace_id; std::string span_name; bool parent_sampled{true}; };
class TelemetrySampler {
public:
    virtual ~TelemetrySampler() = default;
    virtual SamplingDecision decide(const SamplingContext&) const = 0;
};
class DeterministicRatioSampler final : public TelemetrySampler {
public:
    explicit DeterministicRatioSampler(double ratio);
    SamplingDecision decide(const SamplingContext&) const override;
private: double ratio_{1.0};
};

struct LogRecord {
    CorrelationContext context;
    std::string timestamp;
    std::string severity;
    std::string body;
    std::map<std::string, std::string> attributes;
};

class TelemetrySink {
public:
    virtual ~TelemetrySink() = default;
    virtual bool export_span(const SpanRecord& span) = 0;
    virtual bool export_metric(const MetricResult& metric) = 0;
    virtual bool export_log(const LogRecord&) { return false; }
    virtual bool flush() = 0;
};

class InMemoryTelemetrySink final : public TelemetrySink {
public:
    bool export_span(const SpanRecord& span) override;
    bool export_metric(const MetricResult& metric) override;
    bool export_log(const LogRecord& log) override;
    bool flush() override { return true; }
    std::vector<SpanRecord> spans() const;
    std::vector<MetricResult> metrics() const;
    std::vector<LogRecord> logs() const;
private:
    mutable std::mutex mutex_;
    std::vector<SpanRecord> spans_;
    std::vector<MetricResult> metrics_;
    std::vector<LogRecord> logs_;
};

struct TelemetryPolicy {
    std::vector<std::string> allowed_attribute_keys;
    std::size_t max_attribute_length{256};
    std::size_t max_log_body_length{1024};
    std::shared_ptr<TelemetrySampler> sampler;
};

class TelemetryRuntime {
public:
    TelemetryRuntime(std::shared_ptr<TelemetrySink> sink, TelemetryPolicy policy);
    bool emit_span(SpanRecord span, std::string* error = nullptr);
    bool emit_metric(MetricResult metric, std::string* error = nullptr);
    bool emit_log(LogRecord log, std::string* error = nullptr);
    bool allows_attribute(std::string_view key) const;
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    bool attributes_safe(const std::map<std::string, std::string>& attributes,
                         std::string* error) const;
    std::shared_ptr<TelemetrySink> sink_;
    TelemetryPolicy policy_;
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace agent_framework::telemetry

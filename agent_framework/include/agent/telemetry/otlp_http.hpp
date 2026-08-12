#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "agent/telemetry/runtime.hpp"

namespace agent_framework::telemetry {

struct OtlpHttpOptions {
    std::string endpoint;
    std::size_t batch_size{64};
    std::string service_name{"taskflow-agent"};
    int connect_timeout_ms{2000};
    int request_timeout_ms{5000};
    std::string ca_certificate_path;
    std::string client_certificate_path;
    std::string client_private_key_path;
    bool verify_server_certificate{true};
    std::map<std::string, std::string> resource_attributes;
};

class OtlpHttpTelemetrySink final : public TelemetrySink {
public:
    explicit OtlpHttpTelemetrySink(OtlpHttpOptions options);
    bool export_span(const SpanRecord& span) override;
    bool export_metric(const MetricResult& metric) override;
    bool export_log(const LogRecord& log) override;
    bool flush() override;
    std::string last_error() const;
private:
    bool flush_locked();
    OtlpHttpOptions options_;
    mutable std::mutex mutex_;
    std::vector<SpanRecord> spans_;
    std::vector<MetricResult> metrics_;
    std::vector<LogRecord> logs_;
    std::string last_error_;
};
}  // namespace agent_framework::telemetry

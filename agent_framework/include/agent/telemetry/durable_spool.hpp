#pragma once

#include <mutex>
#include <string>
#include <functional>

#include "agent/telemetry/runtime.hpp"

namespace agent_framework::telemetry {

struct TelemetrySpoolOptions {
    std::uint32_t maximum_attempts{8};
    std::uint64_t initial_backoff_ms{1000};
    std::uint64_t maximum_backoff_ms{60000};
    std::size_t maximum_records{100000};
    std::uint64_t retention_ms{7ULL * 24ULL * 60ULL * 60ULL * 1000ULL};
    std::function<std::uint64_t()> now_ms;
};

class SQLiteTelemetrySpool final : public TelemetrySink {
public:
    SQLiteTelemetrySpool(std::string path, std::shared_ptr<TelemetrySink> downstream,
                         TelemetrySpoolOptions options = {});
    ~SQLiteTelemetrySpool();
    SQLiteTelemetrySpool(const SQLiteTelemetrySpool&) = delete;
    SQLiteTelemetrySpool& operator=(const SQLiteTelemetrySpool&) = delete;
    bool export_span(const SpanRecord& span) override;
    bool export_metric(const MetricResult& metric) override;
    bool export_log(const LogRecord& log) override;
    bool flush() override;
    std::size_t pending() const;
    std::size_t dead_letters() const;
private:
    bool append(std::string_view kind, const nlohmann::json& document);
    void* db_{nullptr};
    std::shared_ptr<TelemetrySink> downstream_;
    TelemetrySpoolOptions options_;
    mutable std::mutex mutex_;
};
}  // namespace agent_framework::telemetry

#pragma once

#include <mutex>
#include <string>

#include "agent/telemetry/runtime.hpp"

namespace agent_framework::telemetry {

class SQLiteTelemetrySpool final : public TelemetrySink {
public:
    SQLiteTelemetrySpool(std::string path, std::shared_ptr<TelemetrySink> downstream);
    ~SQLiteTelemetrySpool();
    SQLiteTelemetrySpool(const SQLiteTelemetrySpool&) = delete;
    SQLiteTelemetrySpool& operator=(const SQLiteTelemetrySpool&) = delete;
    bool export_span(const SpanRecord& span) override;
    bool export_metric(const MetricResult& metric) override;
    bool export_log(const LogRecord& log) override;
    bool flush() override;
    std::size_t pending() const;
private:
    bool append(std::string_view kind, const nlohmann::json& document);
    void* db_{nullptr};
    std::shared_ptr<TelemetrySink> downstream_;
    mutable std::mutex mutex_;
};
}  // namespace agent_framework::telemetry

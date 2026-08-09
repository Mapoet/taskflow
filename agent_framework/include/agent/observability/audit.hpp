#ifndef AGENT_OBSERVABILITY_AUDIT_HPP
#define AGENT_OBSERVABILITY_AUDIT_HPP

#include <agent/core/types.hpp>
#include <chrono>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

struct AuditEvent {
    std::string timestamp;
    std::string trace_id;
    std::string tenant_id;
    std::string session_id;
    std::string task_id;
    std::size_t attempt = 0;
    std::string component;
    std::string event_kind;
    std::string capability_id;
    std::string capability_revision;
    std::string outcome;
    std::string error_code;
    std::uint64_t sequence = 0;
    std::optional<std::uint64_t> source_sequence;
    std::optional<std::uint64_t> a2a_sequence;
    std::optional<std::uint64_t> latency_ms;
    std::string payload_digest;
    json payload = json::object();
};

json redact_audit_payload(json payload);
std::string audit_payload_digest(const json& payload);
json audit_event_to_json(const AuditEvent& event);
std::string audit_timestamp_now();

/** Environment-backed latency thresholds. Zero disables a component warning. */
class AuditLatencyPolicy {
public:
    AuditLatencyPolicy();
    explicit AuditLatencyPolicy(std::map<std::string, std::uint64_t> thresholds_ms);
    static AuditLatencyPolicy from_environment();
    std::uint64_t threshold_ms(const std::string& component) const noexcept;
    bool is_slow(const std::string& component, std::uint64_t latency_ms) const noexcept;
private:
    std::map<std::string, std::uint64_t> thresholds_ms_;
};

class AuditSink {
public:
    virtual ~AuditSink() = default;
    virtual void write(const AuditEvent& event) noexcept = 0;
};

class JsonlAuditSink final : public AuditSink {
public:
    explicit JsonlAuditSink(std::string path);
    void write(const AuditEvent& event) noexcept override;
private:
    std::string path_;
    std::mutex mutex_;
};

class StderrJsonAuditSink final : public AuditSink {
public:
    StderrJsonAuditSink();
    explicit StderrJsonAuditSink(std::ostream& stream);
    void write(const AuditEvent& event) noexcept override;
private:
    std::ostream* stream_;
    std::mutex mutex_;
};

class CompositeAuditSink final : public AuditSink {
public:
    explicit CompositeAuditSink(std::vector<std::shared_ptr<AuditSink>> sinks = {});
    void add(std::shared_ptr<AuditSink> sink);
    void write(const AuditEvent& event) noexcept override;
private:
    std::mutex mutex_;
    std::vector<std::shared_ptr<AuditSink>> sinks_;
};

class TestAuditSink final : public AuditSink {
public:
    void write(const AuditEvent& event) noexcept override;
    std::vector<AuditEvent> events() const;
    std::vector<AuditEvent> events_for_trace(const std::string& trace_id) const;
    void clear() noexcept;
private:
    mutable std::mutex mutex_;
    std::vector<AuditEvent> events_;
};

}  // namespace agent_framework
#endif

#ifndef AGENT_OBSERVABILITY_AUDIT_HPP
#define AGENT_OBSERVABILITY_AUDIT_HPP

#include <agent/core/types.hpp>
#include <functional>
#include <mutex>
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
    std::string outcome;
    std::string error_code;
    std::uint64_t sequence = 0;
    std::string payload_digest;
    json payload = json::object();
};

json redact_audit_payload(json payload);
std::string audit_payload_digest(const json& payload);

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

class TestAuditSink final : public AuditSink {
public:
    void write(const AuditEvent& event) noexcept override;
    std::vector<AuditEvent> events() const;
private:
    mutable std::mutex mutex_;
    std::vector<AuditEvent> events_;
};

}  // namespace agent_framework
#endif

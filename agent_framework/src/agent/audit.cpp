#include <agent/observability/audit.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace agent_framework {
namespace {
bool sensitive_key(const std::string& key) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return lower.find("token") != std::string::npos || lower.find("secret") != std::string::npos ||
           lower.find("password") != std::string::npos || lower == "authorization" ||
           lower == "api_key" || lower == "x-api-key";
}
json to_json(const AuditEvent& e) {
    return {{"v", 1}, {"timestamp", e.timestamp}, {"trace_id", e.trace_id},
            {"tenant_id", e.tenant_id}, {"session_id", e.session_id}, {"task_id", e.task_id},
            {"attempt", e.attempt}, {"component", e.component}, {"outcome", e.outcome},
            {"error_code", e.error_code}, {"sequence", e.sequence},
            {"payload_digest", e.payload_digest}, {"payload", e.payload}};
}
}

json redact_audit_payload(json value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (sensitive_key(it.key())) it.value() = "<redacted>";
            else it.value() = redact_audit_payload(std::move(it.value()));
        }
    } else if (value.is_array()) {
        for (auto& item : value) item = redact_audit_payload(std::move(item));
    }
    return value;
}

std::string audit_payload_digest(const json& payload) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : payload.dump()) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

JsonlAuditSink::JsonlAuditSink(std::string path) : path_(std::move(path)) {}
void JsonlAuditSink::write(const AuditEvent& event) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
        std::ofstream out(path_, std::ios::app);
        if (out) out << to_json(event).dump() << '\n';
    } catch (...) {}
}
void TestAuditSink::write(const AuditEvent& event) noexcept { try { std::lock_guard<std::mutex> lock(mutex_); events_.push_back(event); } catch (...) {} }
std::vector<AuditEvent> TestAuditSink::events() const { std::lock_guard<std::mutex> lock(mutex_); return events_; }
}  // namespace agent_framework

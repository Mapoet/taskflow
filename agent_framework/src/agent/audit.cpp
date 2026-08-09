#include <agent/observability/audit.hpp>
#include <agent/skills/skill_supply_chain.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

namespace agent_framework {
namespace {
bool sensitive_key(const std::string& key) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return lower.find("token") != std::string::npos || lower.find("secret") != std::string::npos ||
           lower.find("password") != std::string::npos || lower.find("credential") != std::string::npos ||
           lower == "authorization" || lower == "proxy-authorization" || lower == "cookie" ||
           lower == "set-cookie" || lower == "api_key" || lower == "x-api-key";
}
bool sensitive_value(const std::string& value) {
    static const std::regex bearer(R"(^\s*(bearer|basic)\s+\S+\s*$)", std::regex::icase);
    static const std::regex token_assignment(
        R"((access[_-]?token|refresh[_-]?token|api[_-]?key|client[_-]?secret)\s*[:=]\s*\S+)",
        std::regex::icase);
    return std::regex_search(value, bearer) || std::regex_search(value, token_assignment);
}
std::uint64_t env_threshold(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        return consumed == std::string(value).size() ? parsed : fallback;
    } catch (...) { return fallback; }
}
std::string canonical_component(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::replace(value.begin(), value.end(), '-', '_');
    return value;
}
}

json audit_event_to_json(const AuditEvent& e) {
    json result = {{"v", 2}, {"timestamp", e.timestamp}, {"trace_id", e.trace_id},
            {"tenant_id", e.tenant_id}, {"session_id", e.session_id}, {"task_id", e.task_id},
            {"attempt", e.attempt}, {"component", e.component}, {"event_kind", e.event_kind},
            {"capability_id", e.capability_id}, {"capability_revision", e.capability_revision},
            {"outcome", e.outcome},
            {"error_code", e.error_code}, {"sequence", e.sequence},
            {"payload_digest", e.payload_digest}, {"payload", e.payload}};
    if (e.source_sequence) result["source_sequence"] = *e.source_sequence;
    if (e.a2a_sequence) result["a2a_sequence"] = *e.a2a_sequence;
    if (e.latency_ms) result["latency_ms"] = *e.latency_ms;
    return result;
}

json redact_audit_payload(json value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (sensitive_key(it.key())) it.value() = "<redacted>";
            else it.value() = redact_audit_payload(std::move(it.value()));
        }
    } else if (value.is_array()) {
        for (auto& item : value) item = redact_audit_payload(std::move(item));
    } else if (value.is_string() && sensitive_value(value.get_ref<const std::string&>())) {
        value = "<redacted>";
    }
    return value;
}

std::string audit_payload_digest(const json& payload) {
    return skill_sha256_bytes(payload.dump()).value_or("sha256-unavailable");
}

std::string audit_timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << millis << 'Z';
    return out.str();
}

AuditLatencyPolicy::AuditLatencyPolicy()
    : AuditLatencyPolicy({{"tool_hook", 100}, {"mcp", 1000}, {"llm", 30000}, {"renderer", 2000}}) {}
AuditLatencyPolicy::AuditLatencyPolicy(std::map<std::string, std::uint64_t> thresholds_ms)
    : thresholds_ms_(std::move(thresholds_ms)) {}
AuditLatencyPolicy AuditLatencyPolicy::from_environment() {
    return AuditLatencyPolicy({
        {"tool_hook", env_threshold("AGENT_TOOL_HOOK_WARN_MS", 100)},
        {"mcp", env_threshold("AGENT_MCP_WARN_MS", 1000)},
        {"llm", env_threshold("AGENT_LLM_WARN_MS", 30000)},
        {"renderer", env_threshold("AGENT_RENDERER_WARN_MS", 2000)}});
}
std::uint64_t AuditLatencyPolicy::threshold_ms(const std::string& component) const noexcept {
    const auto it = thresholds_ms_.find(canonical_component(component));
    return it == thresholds_ms_.end() ? 0 : it->second;
}
bool AuditLatencyPolicy::is_slow(const std::string& component, std::uint64_t latency_ms) const noexcept {
    const auto threshold = threshold_ms(component);
    return threshold != 0 && latency_ms > threshold;
}

JsonlAuditSink::JsonlAuditSink(std::string path) : path_(std::move(path)) {}
void JsonlAuditSink::write(const AuditEvent& event) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
        std::ofstream out(path_, std::ios::app);
        if (out) out << audit_event_to_json(event).dump() << '\n';
    } catch (...) {}
}
StderrJsonAuditSink::StderrJsonAuditSink() : stream_(&std::cerr) {}
StderrJsonAuditSink::StderrJsonAuditSink(std::ostream& stream) : stream_(&stream) {}
void StderrJsonAuditSink::write(const AuditEvent& event) noexcept {
    try { std::lock_guard<std::mutex> lock(mutex_); *stream_ << audit_event_to_json(event).dump() << '\n'; }
    catch (...) {}
}
CompositeAuditSink::CompositeAuditSink(std::vector<std::shared_ptr<AuditSink>> sinks)
    : sinks_(std::move(sinks)) {}
void CompositeAuditSink::add(std::shared_ptr<AuditSink> sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}
void CompositeAuditSink::write(const AuditEvent& event) noexcept {
    std::vector<std::shared_ptr<AuditSink>> sinks;
    try { std::lock_guard<std::mutex> lock(mutex_); sinks = sinks_; } catch (...) { return; }
    for (const auto& sink : sinks) if (sink) sink->write(event);
}
void TestAuditSink::write(const AuditEvent& event) noexcept { try { std::lock_guard<std::mutex> lock(mutex_); events_.push_back(event); } catch (...) {} }
std::vector<AuditEvent> TestAuditSink::events() const { std::lock_guard<std::mutex> lock(mutex_); return events_; }
std::vector<AuditEvent> TestAuditSink::events_for_trace(const std::string& trace_id) const {
    std::vector<AuditEvent> result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::copy_if(events_.begin(), events_.end(), std::back_inserter(result),
                     [&](const AuditEvent& event) { return event.trace_id == trace_id; });
    }
    std::stable_sort(result.begin(), result.end(), [](const AuditEvent& lhs, const AuditEvent& rhs) {
        if (lhs.sequence != rhs.sequence) return lhs.sequence < rhs.sequence;
        return lhs.timestamp < rhs.timestamp;
    });
    return result;
}
void TestAuditSink::clear() noexcept { try { std::lock_guard<std::mutex> lock(mutex_); events_.clear(); } catch (...) {} }
}  // namespace agent_framework

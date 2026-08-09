#include <agent/observability/audit.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace agent_framework;

int main() {
    json payload = {
        {"authorization", "Bearer abc"},
        {"headers", {{"Cookie", "session=top-secret"}, {"ordinary", "Bearer hidden"}}},
        {"nested", {{"refresh_token", "secret"}, {"ok", 1}}},
        {"generic_value", "client_secret=never-print-this"}};
    const auto redacted = redact_audit_payload(payload);
    const auto dumped = redacted.dump();
    assert(dumped.find("abc") == std::string::npos);
    assert(dumped.find("top-secret") == std::string::npos);
    assert(dumped.find("never-print-this") == std::string::npos);
    assert(redacted["nested"]["ok"] == 1);

    AuditEvent later;
    later.timestamp = audit_timestamp_now();
    later.trace_id = "trace";
    later.task_id = "task";
    later.sequence = 2;
    later.component = "tool";
    later.event_kind = "tool_completed";
    later.capability_id = "tool:search";
    later.capability_revision = "sha256:revision";
    later.latency_ms = 150;
    later.payload = redacted;
    later.payload_digest = audit_payload_digest(later.payload);
    assert(later.payload_digest.size() == 64);

    AuditEvent earlier = later;
    earlier.sequence = 1;
    earlier.event_kind = "tool_started";

    const auto path = (std::filesystem::temp_directory_path() / "agent-audit-wp38.jsonl").string();
    std::filesystem::remove(path);
    JsonlAuditSink file(path);
    file.write(later);
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    assert(line.find("never-print-this") == std::string::npos);
    const auto serialized = json::parse(line);
    assert(serialized["v"] == 2);
    assert(serialized["latency_ms"] == 150);
    assert(serialized["capability_revision"] == "sha256:revision");

    auto collector = std::make_shared<TestAuditSink>();
    std::ostringstream stderr_capture;
    auto stderr_sink = std::make_shared<StderrJsonAuditSink>(stderr_capture);
    CompositeAuditSink composite({collector, stderr_sink});
    composite.write(later);
    composite.write(earlier);
    assert(collector->events().size() == 2);
    const auto replay = collector->events_for_trace("trace");
    assert(replay.size() == 2 && replay[0].sequence == 1 && replay[1].sequence == 2);
    assert(json::parse(stderr_capture.str().substr(0, stderr_capture.str().find('\n')))["v"] == 2);

    const AuditLatencyPolicy policy({{"tool_hook", 10}, {"mcp", 20}});
    assert(policy.is_slow("tool-hook", 11));
    assert(!policy.is_slow("mcp", 20));
    assert(!policy.is_slow("unknown", 100000));
    std::filesystem::remove(path);
}

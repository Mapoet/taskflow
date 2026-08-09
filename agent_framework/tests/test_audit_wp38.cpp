#include <agent/observability/audit.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
using namespace agent_framework;
int main() {
  auto redacted=redact_audit_payload(json{{"authorization","Bearer secret"},{"nested",{{"token","x"},{"safe",1}}}});
  assert(redacted["authorization"] == "<redacted>" && redacted["nested"]["token"] == "<redacted>");
  const auto path=(std::filesystem::temp_directory_path()/"agent-audit-wp38.jsonl").string(); std::filesystem::remove(path);
  AuditEvent event; event.trace_id="trace"; event.task_id="task"; event.sequence=1; event.component="tool"; event.payload=redacted; event.payload_digest=audit_payload_digest(event.payload);
  JsonlAuditSink file(path); file.write(event); std::ifstream in(path); std::string line; std::getline(in,line); assert(line.find("secret")==std::string::npos && line.find("<redacted>")!=std::string::npos);
  TestAuditSink collector; collector.write(event); assert(collector.events().size()==1); std::filesystem::remove(path);
}

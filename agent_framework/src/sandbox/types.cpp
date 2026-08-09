#include "agent/sandbox/types.hpp"

#include <set>

namespace agent_framework::sandbox {
namespace {
using json = nlohmann::json;
template <typename T, typename Builder>
std::optional<T> decode_value(const json& value, const char* kind,
    const std::set<std::string>& fields, const contracts::ParseContext& context,
    std::vector<contracts::ContractIssue>* issues, Builder builder) {
    auto document = contracts::parse_typed_contract(value, kind, context, issues);
    if(!document || !contracts::validate_object_fields(document->payload, fields, fields,
        contracts::UnknownFieldPolicy::Reject, nullptr, issues, "/payload")) return std::nullopt;
    try {
        T result = builder(document->payload);
        result.metadata = std::move(document->metadata);
        return result;
    } catch(const std::exception& error) {
        contracts::append_issue(issues, "payload_decode_failed", "/payload", error.what());
        return std::nullopt;
    }
}
}  // namespace
json encode(const SandboxSpec& v) {
    return contracts::make_typed_contract(v.metadata, "agent.sandbox_spec/v1",
        {{"provider", v.provider}, {"image_digest", v.image_digest},
         {"workspace_base_digest", v.workspace_base_digest}, {"command", v.command},
         {"read_only_mounts", v.read_only_mounts}, {"writable_mounts", v.writable_mounts},
         {"network_allowlist", v.network_allowlist}, {"credential_refs", v.credential_refs},
         {"cpu_millis", v.cpu_millis}, {"memory_bytes", v.memory_bytes},
         {"wall_time_ms", v.wall_time_ms}, {"policy_revision", v.policy_revision},
         {"memory_view_digest", v.memory_view_digest}});
}
json encode(const SandboxManifest& v) {
    return contracts::make_typed_contract(v.metadata, "agent.sandbox_manifest/v1",
        {{"sandbox_id", v.sandbox_id}, {"spec_digest", v.spec_digest},
         {"provider_version", v.provider_version}, {"workspace_input_digest", v.workspace_input_digest},
         {"workspace_output_digest", v.workspace_output_digest}, {"stdout_digest", v.stdout_digest},
         {"stderr_digest", v.stderr_digest}, {"exit_code", v.exit_code},
         {"cpu_millis", v.cpu_millis}, {"peak_memory_bytes", v.peak_memory_bytes},
         {"wall_time_ms", v.wall_time_ms}, {"started_at", v.started_at},
         {"finished_at", v.finished_at}});
}
std::optional<SandboxSpec> decode_sandbox_spec(const json& value,
    const contracts::ParseContext& context, std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"provider", "image_digest", "workspace_base_digest",
        "command", "read_only_mounts", "writable_mounts", "network_allowlist", "credential_refs",
        "cpu_millis", "memory_bytes", "wall_time_ms", "policy_revision", "memory_view_digest"};
    return decode_value<SandboxSpec>(value, "agent.sandbox_spec/v1", fields, context, issues,
        [](const json& p) {
            SandboxSpec v;
            v.provider = p.at("provider").get<std::string>(); v.image_digest = p.at("image_digest").get<std::string>();
            v.workspace_base_digest = p.at("workspace_base_digest").get<std::string>();
            v.command = p.at("command").get<std::vector<std::string>>();
            v.read_only_mounts = p.at("read_only_mounts").get<std::vector<std::string>>();
            v.writable_mounts = p.at("writable_mounts").get<std::vector<std::string>>();
            v.network_allowlist = p.at("network_allowlist").get<std::vector<std::string>>();
            v.credential_refs = p.at("credential_refs").get<std::vector<std::string>>();
            v.cpu_millis = p.at("cpu_millis").get<std::uint64_t>();
            v.memory_bytes = p.at("memory_bytes").get<std::uint64_t>();
            v.wall_time_ms = p.at("wall_time_ms").get<std::uint64_t>();
            v.policy_revision = p.at("policy_revision").get<std::string>();
            v.memory_view_digest = p.at("memory_view_digest").get<std::string>(); return v;
        });
}
std::optional<SandboxManifest> decode_sandbox_manifest(const json& value,
    const contracts::ParseContext& context, std::vector<contracts::ContractIssue>* issues) {
    static const std::set<std::string> fields = {"sandbox_id", "spec_digest", "provider_version",
        "workspace_input_digest", "workspace_output_digest", "stdout_digest", "stderr_digest",
        "exit_code", "cpu_millis", "peak_memory_bytes", "wall_time_ms", "started_at", "finished_at"};
    return decode_value<SandboxManifest>(value, "agent.sandbox_manifest/v1", fields, context, issues,
        [](const json& p) {
            SandboxManifest v;
            v.sandbox_id=p.at("sandbox_id").get<std::string>(); v.spec_digest=p.at("spec_digest").get<std::string>();
            v.provider_version=p.at("provider_version").get<std::string>(); v.workspace_input_digest=p.at("workspace_input_digest").get<std::string>();
            v.workspace_output_digest=p.at("workspace_output_digest").get<std::string>(); v.stdout_digest=p.at("stdout_digest").get<std::string>();
            v.stderr_digest=p.at("stderr_digest").get<std::string>(); v.exit_code=p.at("exit_code").get<int>();
            v.cpu_millis=p.at("cpu_millis").get<std::uint64_t>(); v.peak_memory_bytes=p.at("peak_memory_bytes").get<std::uint64_t>();
            v.wall_time_ms=p.at("wall_time_ms").get<std::uint64_t>(); v.started_at=p.at("started_at").get<std::string>();
            v.finished_at=p.at("finished_at").get<std::string>(); return v;
        });
}
}  // namespace agent_framework::sandbox

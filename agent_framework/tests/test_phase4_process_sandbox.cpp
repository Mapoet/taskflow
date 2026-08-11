#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "agent/contracts/contract.hpp"
#include "agent/internal/platform_io.hpp"
#include "agent/sandbox/process_provider.hpp"
#include "agent/sandbox/workspace.hpp"

int main() {
#if defined(_WIN32)
    return 0;
#else
    namespace fs = std::filesystem;
    using namespace agent_framework;
    const auto root = fs::temp_directory_path() /
        ("phase4-process-sandbox-" + std::to_string(internal::current_process_id()));
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root);
    std::ofstream(root / "input.txt") << "input";
    auto before = sandbox::snapshot_workspace(root, 1024 * 1024); assert(before);
    auto credentials = std::make_shared<sandbox::CredentialBroker>(
        [](std::string_view reference) -> std::optional<sandbox::CredentialLease> {
            if (reference != "vault://tenant/key") return std::nullopt;
            return sandbox::CredentialLease{std::string(reference), "sandbox-secret-value", "2099"};
        });
    sandbox::BubblewrapSandboxProvider provider({}, credentials);
    std::string error; assert(provider.available(&error));
    sandbox::SandboxSpec spec;
    spec.metadata.identity.tenant_id = "tenant-a";
    spec.metadata.identity.task_id = "task-a";
    spec.metadata.identity.run_id = "run-a";
    spec.provider = "bubblewrap"; spec.workspace_base_digest = before->digest;
    spec.command = {"/bin/sh", "-c",
        "cat /run/secrets/credential-0; printf changed >/workspace/output.txt; "
        "test ! -e /workspace/../outside-sentinel"};
    spec.writable_mounts = {root.string() + ":/workspace"};
    spec.credential_refs = {"vault://tenant/key"};
    spec.cpu_millis = 2000; spec.memory_bytes = 128 * 1024 * 1024;
    spec.wall_time_ms = 5000; spec.policy_revision = "policy-v1";
    spec.memory_view_digest = "sha256:view";
    auto handle = provider.create(spec, &error);
    if (!handle) { std::cerr << "sandbox create: " << error << '\n'; return 3; }
    auto result = provider.exec(*handle, &error);
    if (!result) return 2;
    assert(result->exit_code == 0 && !result->timed_out);
    assert(result->stdout_text.find("sandbox-secret-value") == std::string::npos);
    assert(result->stdout_text.find("[REDACTED]") != std::string::npos);
    assert(fs::is_regular_file(root / "output.txt"));
    assert(result->manifest.workspace_input_digest == before->digest);
    assert(result->manifest.workspace_output_digest != before->digest);
    auto after = sandbox::snapshot_workspace(root, 1024 * 1024); assert(after);
    const auto diff = sandbox::diff_workspace(*before, *after);
    assert(diff.added == std::vector<std::string>{"output.txt"});
    assert(diff.modified.empty() && diff.removed.empty() && !diff.digest.empty());
    assert(!result->manifest.stdout_digest.empty() && result->manifest.peak_memory_bytes > 0);
    assert(provider.destroy(*handle, &error));
    assert(!provider.exec(*handle, &error));
    spec.command = {"/bin/sh", "-c", "sleep 2"};
    spec.wall_time_ms = 50;
    auto timeout_handle = provider.create(spec, &error); assert(timeout_handle);
    auto timeout_result = provider.exec(*timeout_handle, &error); assert(timeout_result);
    assert(timeout_result->timed_out && timeout_result->exit_code == -1);
    assert(timeout_result->manifest.wall_time_ms < 1000);
    assert(provider.destroy(*timeout_handle, &error));
    spec.network_allowlist = {"https://example.org"};
    assert(!provider.create(spec, &error));
    fs::remove_all(root, ec);
    return 0;
#endif
}

#include <agent/ui/presentation_model.hpp>
#include <agent/ui/rich_renderer.hpp>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace agent_framework;

namespace {
std::string tiny_png() {
    return std::string("\x89PNG\r\n\x1a\n", 8) + "test-payload";
}
class FakeWorker final : public RendererWorker {
public:
    RendererWorkerResult execute(RenderKind, const std::string&, const RenderLimits&) override {
        ++calls;
        if (delay.count()) std::this_thread::sleep_for(delay);
        return response;
    }
    int calls{0};
    std::chrono::milliseconds delay{0};
    RendererWorkerResult response{true, false, tiny_png(), "image/png", {}};
};
}

int main(int argc, char** argv) {
    // The same binary acts as the controlled worker for direct exec/no-shell verification.
    if (argc > 1) {
        if (argc > 2 && std::string(argv[2]) == "exit-early") return 3;
        if (argc > 2 && std::string(argv[2]) == "sleep")
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto bytes = tiny_png();
        std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return 0;
    }

    const auto root = std::filesystem::temp_directory_path() / "agent-renderer-wp39";
    std::filesystem::remove_all(root);
    auto artifacts = std::make_shared<ArtifactStore>(root, 1024 * 1024);

    RendererRegistry plain_only(artifacts);
    const auto plain = plain_only.render({RenderKind::Plain, "plain response"});
    assert(plain.status == RenderStatus::Plain && plain.display_text == "plain response");
    const auto unavailable = plain_only.render({RenderKind::Mermaid, "graph LR\nA-->B"});
    assert(unavailable.status == RenderStatus::Fallback);
    assert(unavailable.diagnostic_code == "renderer_unavailable");

    auto fake = std::make_shared<FakeWorker>();
    auto audit = std::make_shared<TestAuditSink>();
    RendererRegistry registry(artifacts, {"plain", "mermaid-fake", "latex-fake"}, audit,
                              AuditLatencyPolicy({{"renderer", 1}}));
    registry.register_renderer(std::make_shared<WorkerRenderer>(
        "mermaid-fake", RenderKind::Mermaid, fake));
    registry.register_renderer(std::make_shared<WorkerRenderer>(
        "latex-fake", RenderKind::Latex, fake));
    RenderRequest mermaid{RenderKind::Mermaid, "graph LR\nA-->B", "trace", "citation-1"};
    const auto rendered = registry.render(mermaid);
    assert(rendered.status == RenderStatus::Rendered && rendered.artifact);
    assert(rendered.artifact->sha256.size() == 64);
    assert(rendered.artifact->citation_id == "citation-1");
    assert(artifacts->resolve(rendered.artifact->relative_path));
    assert(!artifacts->resolve("../outside.png"));
    fake->delay = std::chrono::milliseconds(3);
    const auto slow = registry.render(mermaid);
    assert(slow.status == RenderStatus::Rendered && slow.latency_ms >= 1);
    const auto audit_events = audit->events_for_trace("trace");
    assert(audit_events.size() == 2);
    assert(audit_events.back().event_kind == "renderer_slow");
    assert(audit_events.back().payload.find("source") == audit_events.back().payload.end());
    fake->delay = std::chrono::milliseconds(0);

    const int before_malicious = fake->calls;
    auto malicious = mermaid;
    malicious.source = "%%{init: {securityLevel: 'loose'}}%%\ngraph LR\nA-->B\nclick A javascript:alert(1)";
    const auto rejected_mermaid = registry.render(malicious);
    assert(rejected_mermaid.status == RenderStatus::Fallback);
    assert(rejected_mermaid.diagnostic_code == "renderer_active_content_rejected" ||
           rejected_mermaid.diagnostic_code == "mermaid_directive_rejected");
    assert(fake->calls == before_malicious);

    RenderRequest latex{RenderKind::Latex, "\\input{/etc/passwd}"};
    const auto rejected_latex = registry.render(latex);
    assert(rejected_latex.status == RenderStatus::Fallback);
    assert(rejected_latex.diagnostic_code == "latex_command_rejected");
    assert(fake->calls == before_malicious);

    auto excessive = mermaid;
    excessive.limits.max_nodes = 2;
    excessive.source = "graph LR\nA-->B\nB-->C";
    assert(registry.render(excessive).diagnostic_code == "mermaid_node_limit");
    auto oversized = mermaid;
    oversized.limits.max_source_bytes = 4;
    assert(registry.render(oversized).diagnostic_code == "renderer_source_limit");

#ifndef _WIN32
    const auto self = std::filesystem::canonical("/proc/self/exe");
    RendererRegistry process_registry(artifacts, {"plain", "mermaid-process"});
    process_registry.register_renderer(std::make_shared<WorkerRenderer>(
        "mermaid-process", RenderKind::Mermaid,
        std::make_shared<SubprocessRendererWorker>(self)));
    assert(process_registry.render(mermaid).status == RenderStatus::Rendered);

    RendererRegistry timeout_registry(artifacts, {"plain", "mermaid-timeout"});
    timeout_registry.register_renderer(std::make_shared<WorkerRenderer>(
        "mermaid-timeout", RenderKind::Mermaid,
        std::make_shared<SubprocessRendererWorker>(self, std::vector<std::string>{"sleep"})));
    auto timeout_request = mermaid;
    // Exceed the deliberately small socket buffer: the parent must multiplex
    // input and output under the same deadline even when the worker never reads.
    timeout_request.source.assign(32 * 1024, 'A');
    timeout_request.limits.timeout = std::chrono::milliseconds(20);
    const auto timed_out = timeout_registry.render(timeout_request);
    assert(timed_out.status == RenderStatus::Fallback);
    assert(timed_out.diagnostic_code == "renderer_timeout");

    // A worker exiting without reading stdin must not SIGPIPE the host.
    RendererRegistry early_exit_registry(artifacts, {"plain", "mermaid-exit-early"});
    early_exit_registry.register_renderer(std::make_shared<WorkerRenderer>(
        "mermaid-exit-early", RenderKind::Mermaid,
        std::make_shared<SubprocessRendererWorker>(self,
            std::vector<std::string>{"exit-early"})));
    auto large_request = mermaid;
    large_request.source.assign(32 * 1024, 'A');
    const auto early_exit = early_exit_registry.render(large_request);
    assert(early_exit.status == RenderStatus::Fallback);
    assert(early_exit.diagnostic_code == "worker_failed");
#endif

    UiPresentationModel model;
    model.begin_user_turn("render this");
    assert(model.observe_artifact({{"id", rendered.artifact->id},
        {"mime", rendered.artifact->mime}, {"path", rendered.artifact->relative_path},
        {"sha256", rendered.artifact->sha256}, {"citation_id", rendered.artifact->citation_id},
        {"byte_size", rendered.artifact->byte_size}}));
    const auto snapshot = model.snapshot();
    assert(snapshot.turns.back().attachments.back().sha256 == rendered.artifact->sha256);
    assert(snapshot.turns.back().attachments.back().citation_id == "citation-1");
    std::filesystem::remove_all(root);
}

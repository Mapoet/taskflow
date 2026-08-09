#ifndef AGENT_UI_RICH_RENDERER_HPP
#define AGENT_UI_RICH_RENDERER_HPP

#include <agent/core/types.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace agent_framework {

enum class RenderKind { Plain, Mermaid, Latex };
enum class RenderStatus { Rendered, Plain, Fallback, Rejected, TimedOut, Failed, Disabled };

struct RenderLimits {
    std::size_t max_source_bytes{64 * 1024};
    std::size_t max_nodes{256};
    std::size_t max_output_bytes{8 * 1024 * 1024};
    std::chrono::milliseconds timeout{2000};
    std::size_t max_latex_depth{32};
};

struct RenderRequest {
    RenderKind kind{RenderKind::Plain};
    std::string source;
    std::string trace_id;
    std::string citation_id;
    RenderLimits limits;
};

struct RenderArtifact {
    std::string id;
    std::string mime;
    std::string relative_path;
    std::string sha256;
    std::size_t byte_size{0};
    std::string citation_id;
};

struct RenderResult {
    RenderStatus status{RenderStatus::Failed};
    std::string backend;
    std::string display_text;
    std::optional<RenderArtifact> artifact;
    std::string diagnostic_code;
    std::uint64_t latency_ms{0};
};

class ArtifactStore {
public:
    explicit ArtifactStore(std::filesystem::path root, std::size_t max_artifact_bytes = 8 * 1024 * 1024);
    RenderArtifact put(const std::string& bytes, const std::string& mime,
                       const std::string& citation_id = {});
    std::optional<std::filesystem::path> resolve(const std::string& relative_path) const;
    const std::filesystem::path& root() const noexcept { return root_; }
private:
    std::filesystem::path root_;
    std::size_t max_artifact_bytes_;
    mutable std::mutex mutex_;
};

struct RendererWorkerResult {
    bool success{false};
    bool timed_out{false};
    std::string bytes;
    std::string mime{"image/png"};
    std::string diagnostic_code;
};

class RendererWorker {
public:
    virtual ~RendererWorker() = default;
    virtual RendererWorkerResult execute(RenderKind kind, const std::string& source,
                                         const RenderLimits& limits) = 0;
};

/** Executes a fixed absolute worker binary directly (never through a shell), with POSIX limits. */
class SubprocessRendererWorker final : public RendererWorker {
public:
    explicit SubprocessRendererWorker(std::filesystem::path executable,
                                      std::vector<std::string> fixed_arguments = {});
    RendererWorkerResult execute(RenderKind kind, const std::string& source,
                                 const RenderLimits& limits) override;
private:
    std::filesystem::path executable_;
    std::vector<std::string> fixed_arguments_;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual std::string name() const = 0;
    virtual RenderKind kind() const noexcept = 0;
    virtual RenderResult render(const RenderRequest& request, ArtifactStore& artifacts) = 0;
};

class PlainRenderer final : public Renderer {
public:
    std::string name() const override { return "plain"; }
    RenderKind kind() const noexcept override { return RenderKind::Plain; }
    RenderResult render(const RenderRequest& request, ArtifactStore& artifacts) override;
};

class WorkerRenderer final : public Renderer {
public:
    WorkerRenderer(std::string name, RenderKind kind, std::shared_ptr<RendererWorker> worker);
    std::string name() const override { return name_; }
    RenderKind kind() const noexcept override { return kind_; }
    RenderResult render(const RenderRequest& request, ArtifactStore& artifacts) override;
private:
    std::string name_;
    RenderKind kind_;
    std::shared_ptr<RendererWorker> worker_;
};

class RendererRegistry {
public:
    explicit RendererRegistry(std::shared_ptr<ArtifactStore> artifacts,
                              std::set<std::string> allowlist = {"plain"});
    void register_renderer(std::shared_ptr<Renderer> renderer);
    void set_allowlist(std::set<std::string> allowlist);
    RenderResult render(const RenderRequest& request);
    static std::set<std::string> allowlist_from_environment();
private:
    static std::optional<std::string> validate(const RenderRequest& request);
    RenderResult fallback(const RenderRequest& request, std::string diagnostic);
    std::shared_ptr<ArtifactStore> artifacts_;
    std::map<RenderKind, std::shared_ptr<Renderer>> renderers_;
    std::set<std::string> allowlist_;
    mutable std::mutex mutex_;
};

const char* render_kind_name(RenderKind kind) noexcept;

} // namespace agent_framework
#endif

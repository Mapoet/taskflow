#include <agent/ui/rich_renderer.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent_framework {
namespace {

bool safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& component : path)
        if (component.empty() || component == "." || component == "..") return false;
    return true;
}
bool png_bytes(const std::string& bytes) {
    static const unsigned char signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return bytes.size() >= sizeof(signature) &&
        std::equal(std::begin(signature), std::end(signature),
                   reinterpret_cast<const unsigned char*>(bytes.data()));
}
std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
std::size_t mermaid_complexity(const std::string& source) {
    std::size_t count = 0;
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find_first_not_of(" \t\r") != std::string::npos) ++count;
        std::size_t offset = 0;
        while ((offset = line.find("-->", offset)) != std::string::npos) { ++count; offset += 3; }
    }
    return count;
}
std::size_t latex_depth(const std::string& source) {
    std::size_t depth = 0, maximum = 0;
    bool escaped = false;
    for (char value : source) {
        if (escaped) { escaped = false; continue; }
        if (value == '\\') { escaped = true; continue; }
        if (value == '{') maximum = std::max(maximum, ++depth);
        else if (value == '}' && depth > 0) --depth;
    }
    return maximum;
}

} // namespace

const char* render_kind_name(RenderKind kind) noexcept {
    switch (kind) {
        case RenderKind::Plain: return "plain";
        case RenderKind::Mermaid: return "mermaid";
        case RenderKind::Latex: return "latex";
    }
    return "plain";
}

ArtifactStore::ArtifactStore(std::filesystem::path root, std::size_t max_artifact_bytes)
    : root_(std::move(root)), max_artifact_bytes_(max_artifact_bytes) {
    if (root_.empty() || max_artifact_bytes_ == 0)
        throw std::invalid_argument("artifact store requires root and positive size limit");
    std::filesystem::create_directories(root_ / "sha256");
    root_ = std::filesystem::weakly_canonical(root_);
}
RenderArtifact ArtifactStore::put(const std::string& bytes, const std::string& mime,
                                  const std::string& citation_id) {
    if (bytes.empty() || bytes.size() > max_artifact_bytes_)
        throw std::runtime_error("renderer artifact size limit exceeded");
    std::string extension;
    if (mime == "image/png") {
        if (!png_bytes(bytes)) throw std::runtime_error("renderer returned invalid PNG");
        extension = ".png";
    } else if (mime == "text/plain") extension = ".txt";
    else throw std::runtime_error("renderer artifact MIME is not allowed");
    const auto digest = skill_sha256_bytes(bytes);
    if (!digest) throw std::runtime_error("renderer artifact digest unavailable");
    const std::filesystem::path relative = std::filesystem::path("sha256") / (*digest + extension);
    const auto target = root_ / relative;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::filesystem::exists(target)) {
        const auto temporary = target.string() + ".tmp." + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("cannot write renderer artifact");
        out.close();
        std::filesystem::rename(temporary, target);
    }
    return {*digest, mime, relative.generic_string(), *digest, bytes.size(), citation_id};
}
std::optional<std::filesystem::path> ArtifactStore::resolve(const std::string& relative_path) const {
    const std::filesystem::path relative(relative_path);
    if (!safe_relative_path(relative)) return std::nullopt;
    const auto candidate = std::filesystem::weakly_canonical(root_ / relative);
    const auto root_text = root_.generic_string() + '/';
    if (candidate.generic_string().rfind(root_text, 0) != 0 || !std::filesystem::is_regular_file(candidate))
        return std::nullopt;
    return candidate;
}

SubprocessRendererWorker::SubprocessRendererWorker(
    std::filesystem::path executable, std::vector<std::string> fixed_arguments)
    : executable_(std::move(executable)), fixed_arguments_(std::move(fixed_arguments)) {
    if (!executable_.is_absolute())
        throw std::invalid_argument("renderer worker executable must be absolute");
}
RendererWorkerResult SubprocessRendererWorker::execute(
    RenderKind kind, const std::string& source, const RenderLimits& limits) {
#ifdef _WIN32
    (void)kind; (void)source; (void)limits;
    return {false, false, {}, {}, "subprocess_renderer_unsupported"};
#else
    int input_pipe[2]{-1, -1}, output_pipe[2]{-1, -1};
    if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0)
        return {false, false, {}, {}, "worker_pipe_failed"};
    const pid_t child = ::fork();
    if (child < 0) return {false, false, {}, {}, "worker_fork_failed"};
    if (child == 0) {
        (void)::dup2(input_pipe[0], STDIN_FILENO);
        (void)::dup2(output_pipe[1], STDOUT_FILENO);
        const int null_fd = ::open("/dev/null", O_WRONLY);
        if (null_fd >= 0) (void)::dup2(null_fd, STDERR_FILENO);
        ::close(input_pipe[0]); ::close(input_pipe[1]);
        ::close(output_pipe[0]); ::close(output_pipe[1]);
        struct rlimit memory{256U * 1024U * 1024U, 256U * 1024U * 1024U};
        struct rlimit output{limits.max_output_bytes, limits.max_output_bytes};
        const auto cpu_seconds = std::max<rlim_t>(1, (limits.timeout.count() + 999) / 1000 + 1);
        struct rlimit cpu{cpu_seconds, cpu_seconds};
        struct rlimit files{32, 32};
        (void)::setrlimit(RLIMIT_AS, &memory); (void)::setrlimit(RLIMIT_FSIZE, &output);
        (void)::setrlimit(RLIMIT_CPU, &cpu); (void)::setrlimit(RLIMIT_NOFILE, &files);
        (void)::clearenv(); (void)::setenv("PATH", "/usr/bin:/bin", 1);
        std::vector<std::string> arguments{executable_.string(), render_kind_name(kind)};
        arguments.insert(arguments.end(), fixed_arguments_.begin(), fixed_arguments_.end());
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        ::execv(executable_.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(input_pipe[0]); ::close(output_pipe[1]);
    std::size_t offset = 0;
    while (offset < source.size()) {
        const auto count = ::write(input_pipe[1], source.data() + offset, source.size() - offset);
        if (count < 0) { if (errno == EINTR) continue; break; }
        offset += static_cast<std::size_t>(count);
    }
    ::close(input_pipe[1]);
    (void)::fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
    std::string bytes;
    int status = 0;
    bool exited = false, too_large = false;
    std::array<char, 8192> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        for (;;) {
            const auto count = ::read(output_pipe[0], buffer.data(), buffer.size());
            if (count > 0) {
                bytes.append(buffer.data(), static_cast<std::size_t>(count));
                if (bytes.size() > limits.max_output_bytes) { too_large = true; break; }
            } else break;
        }
        if (too_large) break;
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) { exited = true; break; }
        struct pollfd descriptor{output_pipe[0], POLLIN, 0};
        (void)::poll(&descriptor, 1, 20);
    }
    if (!exited) { (void)::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
    for (;;) {
        const auto count = ::read(output_pipe[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(output_pipe[0]);
    if (too_large || bytes.size() > limits.max_output_bytes)
        return {false, false, {}, {}, "worker_output_limit"};
    if (!exited) return {false, true, {}, {}, "worker_timeout"};
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {false, false, {}, {}, "worker_failed"};
    return {true, false, std::move(bytes), "image/png", {}};
#endif
}

RenderResult PlainRenderer::render(const RenderRequest& request, ArtifactStore&) {
    return {RenderStatus::Plain, name(), request.source, std::nullopt, {}, 0};
}
WorkerRenderer::WorkerRenderer(std::string name, RenderKind kind, std::shared_ptr<RendererWorker> worker)
    : name_(std::move(name)), kind_(kind), worker_(std::move(worker)) {
    if (name_.empty() || kind_ == RenderKind::Plain || !worker_)
        throw std::invalid_argument("worker renderer requires name, rich kind, and worker");
}
RenderResult WorkerRenderer::render(const RenderRequest& request, ArtifactStore& artifacts) {
    const auto started = std::chrono::steady_clock::now();
    auto worker = worker_->execute(kind_, request.source, request.limits);
    const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    if (worker.timed_out) return {RenderStatus::TimedOut, name_, request.source, std::nullopt,
                                  "renderer_timeout", elapsed};
    if (!worker.success) return {RenderStatus::Failed, name_, request.source, std::nullopt,
                                 worker.diagnostic_code.empty() ? "renderer_failed" : worker.diagnostic_code,
                                 elapsed};
    try {
        auto artifact = artifacts.put(worker.bytes, worker.mime, request.citation_id);
        return {RenderStatus::Rendered, name_, {}, std::move(artifact), {}, elapsed};
    } catch (...) {
        return {RenderStatus::Failed, name_, request.source, std::nullopt,
                "artifact_rejected", elapsed};
    }
}

RendererRegistry::RendererRegistry(std::shared_ptr<ArtifactStore> artifacts,
                                   std::set<std::string> allowlist)
    : artifacts_(std::move(artifacts)), allowlist_(std::move(allowlist)) {
    if (!artifacts_) throw std::invalid_argument("renderer registry requires artifact store");
    allowlist_.insert("plain");
    renderers_[RenderKind::Plain] = std::make_shared<PlainRenderer>();
}
void RendererRegistry::register_renderer(std::shared_ptr<Renderer> renderer) {
    if (!renderer) throw std::invalid_argument("renderer cannot be null");
    std::lock_guard<std::mutex> lock(mutex_);
    renderers_[renderer->kind()] = std::move(renderer);
}
void RendererRegistry::set_allowlist(std::set<std::string> allowlist) {
    allowlist.insert("plain");
    std::lock_guard<std::mutex> lock(mutex_);
    allowlist_ = std::move(allowlist);
}
std::set<std::string> RendererRegistry::allowlist_from_environment() {
    std::set<std::string> result{"plain"};
    const char* raw = std::getenv("AGENT_RENDERER_ALLOWLIST");
    if (!raw) return result;
    std::istringstream values(raw);
    std::string value;
    while (std::getline(values, value, ',')) {
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
            return std::isspace(c);
        }), value.end());
        if (!value.empty()) result.insert(lower_ascii(value));
    }
    return result;
}
std::optional<std::string> RendererRegistry::validate(const RenderRequest& request) {
    if (request.source.size() > request.limits.max_source_bytes) return "renderer_source_limit";
    if (request.source.find('\0') != std::string::npos) return "renderer_nul_rejected";
    const auto lower = lower_ascii(request.source);
    if (lower.find("<script") != std::string::npos || lower.find("javascript:") != std::string::npos)
        return "renderer_active_content_rejected";
    if (request.kind == RenderKind::Mermaid) {
        if (lower.find("%%{") != std::string::npos || lower.find("click ") != std::string::npos ||
            lower.find("href ") != std::string::npos || lower.find("link ") != std::string::npos)
            return "mermaid_directive_rejected";
        if (mermaid_complexity(request.source) > request.limits.max_nodes)
            return "mermaid_node_limit";
    }
    if (request.kind == RenderKind::Latex) {
        for (const char* denied : {"\\write18", "\\input", "\\include", "\\openout", "\\read"})
            if (lower.find(denied) != std::string::npos) return "latex_command_rejected";
        if (latex_depth(request.source) > request.limits.max_latex_depth)
            return "latex_depth_limit";
    }
    return std::nullopt;
}
RenderResult RendererRegistry::fallback(const RenderRequest& request, std::string diagnostic) {
    RenderResult result = renderers_.at(RenderKind::Plain)->render(request, *artifacts_);
    result.status = RenderStatus::Fallback;
    result.diagnostic_code = std::move(diagnostic);
    return result;
}
RenderResult RendererRegistry::render(const RenderRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto error = validate(request)) return fallback(request, *error);
    const auto it = renderers_.find(request.kind);
    if (it == renderers_.end()) return fallback(request, "renderer_unavailable");
    if (!allowlist_.contains(it->second->name())) return fallback(request, "renderer_disabled");
    auto result = it->second->render(request, *artifacts_);
    if (result.status != RenderStatus::Rendered && result.status != RenderStatus::Plain)
        return fallback(request, result.diagnostic_code.empty() ? "renderer_failed" : result.diagnostic_code);
    return result;
}

} // namespace agent_framework

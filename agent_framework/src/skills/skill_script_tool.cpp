/**
 * @file skill_script_tool.cpp
 * @brief run_skill_script：jail + allowlist + 子进程（POSIX）；Windows 返回不支持错误
 */

#include <agent/skill_script_tool.hpp>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace agent_framework {

namespace {

constexpr std::size_t kOutCap = 65536;

std::vector<std::string> parse_allowlist() {
    std::vector<std::string> out;
    const char* raw = std::getenv("AGENT_SKILL_SCRIPT_ALLOWLIST");
    if (!raw || !*raw) {
        return out;
    }
    std::string chunk;
    for (const char* p = raw; *p != '\0'; ++p) {
        if (*p == ',') {
            while (!chunk.empty() && std::isspace(static_cast<unsigned char>(chunk.front()))) {
                chunk.erase(chunk.begin());
            }
            while (!chunk.empty() && std::isspace(static_cast<unsigned char>(chunk.back()))) {
                chunk.pop_back();
            }
            if (!chunk.empty()) {
                out.push_back(std::move(chunk));
                chunk.clear();
            }
        } else {
            chunk.push_back(*p);
        }
    }
    while (!chunk.empty() && std::isspace(static_cast<unsigned char>(chunk.front()))) {
        chunk.erase(chunk.begin());
    }
    while (!chunk.empty() && std::isspace(static_cast<unsigned char>(chunk.back()))) {
        chunk.pop_back();
    }
    if (!chunk.empty()) {
        out.push_back(std::move(chunk));
    }
    return out;
}

bool allowlist_contains(const std::vector<std::string>& list, const std::string& path) {
    for (const auto& e : list) {
        if (e == path) {
            return true;
        }
    }
    return false;
}

std::string pick_interpreter(const std::filesystem::path& script_path,
                             const std::vector<std::string>& allow) {
    const std::string ext = script_path.extension().string();
    if (ext == ".sh" && allowlist_contains(allow, "/bin/sh")) {
        return "/bin/sh";
    }
    if (ext == ".py" && allowlist_contains(allow, "/usr/bin/python3")) {
        return "/usr/bin/python3";
    }
    if (ext == ".py" && allowlist_contains(allow, "/bin/python3")) {
        return "/bin/python3";
    }
    return {};
}

int script_timeout_sec() {
    const char* e = std::getenv("AGENT_SKILL_SCRIPT_TIMEOUT_SEC");
    if (!e || !*e) {
        return 30;
    }
    const int v = std::atoi(e);
    return (v <= 0) ? 30 : v;
}

json tool_error(const std::string& code, const std::string& message) {
    return json{{"error", json{{"code", code}, {"message", message}}}};
}

} // namespace

void register_skill_script_tool(ToolBus& bus, const std::shared_ptr<SkillServices>& services) {
    if (!services || !services->registry || !services->loader) {
        return;
    }
    constexpr const char* k_name = "run_skill_script";
    if (bus.get_tool_info(k_name).has_value()) {
        return;
    }

    ToolMeta meta;
    meta.name = k_name;
    meta.description =
        "Run a script inside the indexed skill package directory (jail). "
        "Parameter skill_id is the canonical skill key (Cursor frontmatter name, legacy id, or folder name). "
        "Requires AGENT_SKILL_SCRIPT_ALLOWLIST.";
    meta.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "skill_id": { "type": "string" },
            "relative_path": { "type": "string" }
        },
        "required": ["skill_id", "relative_path"]
    })");
    meta.side_effect = ToolSideEffect::Write;

    auto reg = services->registry;

    bus.register_local_tool(
        k_name,
        [reg](const json& args) -> json {
            if (!args.contains("skill_id") || !args["skill_id"].is_string()) {
                return tool_error("validation_failed", "skill_id required");
            }
            if (!args.contains("relative_path") || !args["relative_path"].is_string()) {
                return tool_error("validation_failed", "relative_path required");
            }
            const std::string skill_id = args["skill_id"].get<std::string>();
            const std::string rel = args["relative_path"].get<std::string>();
            if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
                return tool_error("validation_failed", "relative_path must be relative without ..");
            }
            const auto ent_opt = reg->get(skill_id);
            if (!ent_opt.has_value()) {
                return tool_error("validation_failed", "unknown skill_id");
            }

            std::error_code ec;
            std::filesystem::path base;
            if (ent_opt->script_jail.has_value()) {
                base = std::filesystem::weakly_canonical(*ent_opt->script_jail, ec);
            } else {
                const std::filesystem::path root = std::filesystem::weakly_canonical(reg->root(), ec);
                if (ec) {
                    return tool_error("tool_internal_error", "canonical root failed");
                }
                base = std::filesystem::weakly_canonical(root / skill_id, ec);
            }
            if (ec || !std::filesystem::exists(base)) {
                return tool_error("validation_failed", "skill directory missing");
            }
            const std::filesystem::path target = std::filesystem::weakly_canonical(base / rel, ec);
            if (ec) {
                return tool_error("validation_failed", "path resolution failed");
            }
            const std::string base_s = base.string() + std::filesystem::path::preferred_separator;
            const std::string tgt_s = target.string();
            if (tgt_s.size() < base_s.size() || tgt_s.compare(0, base_s.size(), base_s) != 0) {
                return tool_error("validation_failed", "path outside skill jail");
            }
            if (!std::filesystem::is_regular_file(target)) {
                return tool_error("validation_failed", "not a regular file");
            }

            const std::vector<std::string> allow = parse_allowlist();
            if (allow.empty()) {
                return tool_error(
                    "validation_failed",
                    "AGENT_SKILL_SCRIPT_ALLOWLIST is empty; refusing to execute");
            }

            const std::string interpreter = pick_interpreter(target, allow);
            if (interpreter.empty()) {
                return tool_error("validation_failed", "no allowlisted interpreter for script type");
            }

#if defined(_WIN32)
            return tool_error("unsupported", "run_skill_script is POSIX-only in this build");
#else
            int out_pipe[2];
            int err_pipe[2];
            if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
                return tool_error("tool_internal_error", "pipe failed");
            }

            const pid_t pid = fork();
            if (pid < 0) {
                close(out_pipe[0]);
                close(out_pipe[1]);
                close(err_pipe[0]);
                close(err_pipe[1]);
                return tool_error("tool_internal_error", "fork failed");
            }

            if (pid == 0) {
                close(out_pipe[0]);
                close(err_pipe[0]);
                dup2(out_pipe[1], STDOUT_FILENO);
                dup2(err_pipe[1], STDERR_FILENO);
                close(out_pipe[1]);
                close(err_pipe[1]);
                if (chdir(base.string().c_str()) != 0) {
                    _exit(126);
                }
                const std::string script_str = target.string();
                std::vector<char> argv0(interpreter.begin(), interpreter.end());
                argv0.push_back('\0');
                std::vector<char> argv1(script_str.begin(), script_str.end());
                argv1.push_back('\0');
                char* argv[] = {argv0.data(), argv1.data(), nullptr};
                execve(argv0.data(), argv, environ);
                _exit(127);
            }

            close(out_pipe[1]);
            close(err_pipe[1]);
            fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
            fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

            std::string stdout_acc;
            std::string stderr_acc;
            auto append_cap = [](std::string& acc, const char* buf, std::size_t n) {
                if (acc.size() >= kOutCap) {
                    return;
                }
                const std::size_t room = kOutCap - acc.size();
                acc.append(buf, n > room ? room : n);
            };

            const int timeout = script_timeout_sec();
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
            int status = 0;
            bool killed = false;
            bool child_done = false;

            auto try_read_both = [&]() {
                char buf[4096];
                for (;;) {
                    const ssize_t n = read(out_pipe[0], buf, sizeof(buf));
                    if (n <= 0) {
                        break;
                    }
                    append_cap(stdout_acc, buf, static_cast<std::size_t>(n));
                }
                for (;;) {
                    const ssize_t n = read(err_pipe[0], buf, sizeof(buf));
                    if (n <= 0) {
                        break;
                    }
                    append_cap(stderr_acc, buf, static_cast<std::size_t>(n));
                }
            };

            while (!child_done) {
                struct pollfd pf[2];
                pf[0].fd = out_pipe[0];
                pf[0].events = POLLIN;
                pf[0].revents = 0;
                pf[1].fd = err_pipe[0];
                pf[1].events = POLLIN;
                pf[1].revents = 0;
                (void)poll(pf, 2, 50);
                try_read_both();

                const int w = waitpid(pid, &status, WNOHANG);
                if (w == pid) {
                    child_done = true;
                } else if (w < 0) {
                    close(out_pipe[0]);
                    close(err_pipe[0]);
                    return tool_error("tool_internal_error", "waitpid failed");
                }

                if (std::chrono::steady_clock::now() >= deadline && !child_done) {
                    kill(pid, SIGKILL);
                    (void)waitpid(pid, &status, 0);
                    killed = true;
                    child_done = true;
                }
            }

            try_read_both();

            close(out_pipe[0]);
            close(err_pipe[0]);

            json out;
            out["stdout"] = stdout_acc;
            out["stderr"] = stderr_acc;
            out["truncated"] = (stdout_acc.size() >= kOutCap || stderr_acc.size() >= kOutCap);
            if (killed) {
                out["exit_code"] = -1;
                out["timed_out"] = true;
                return out;
            }
            if (WIFEXITED(status)) {
                out["exit_code"] = WEXITSTATUS(status);
            } else {
                out["exit_code"] = -1;
                out["signaled"] = true;
            }
            return out;
#endif
        },
        meta);
}

} // namespace agent_framework

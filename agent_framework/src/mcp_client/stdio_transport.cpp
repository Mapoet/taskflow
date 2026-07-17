/**
 * @file stdio_transport.cpp
 * @brief MCP stdio 传输（JSON Lines 默认，兼容旧 Content-Length 帧）
 */

#include <agent/mcp_client/mcp_client.hpp>
#include "agent/internal/stdio_framing.hpp"

#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#else
#error "StdioMCPTransport is not implemented on Windows (WP1.3 Linux/WSL only)."
#endif

namespace agent_framework {

struct StdioMCPTransport::StdioPipes {
    pid_t pid = -1;
    bool owns_process_group = false;
    int to_child = -1;
    int from_child = -1;
};

namespace {

int mcp_timeout_ms() {
    const char* e = std::getenv("AGENT_MCP_REQUEST_TIMEOUT_MS");
    if (e == nullptr || e[0] == '\0') {
        return 60000;
    }
    int ms = std::atoi(e);
    return ms > 0 ? ms : 60000;
}

void writen(int fd, const char* buf, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("StdioMCPTransport: write failed: " + std::string(std::strerror(errno)));
        }
        off += static_cast<std::size_t>(n);
    }
}

} // namespace

namespace {

std::map<std::string, std::string> parse_environ(char** envp) {
    std::map<std::string, std::string> out;
    if (envp == nullptr) {
        return out;
    }
    for (char** p = envp; *p != nullptr; ++p) {
        std::string s(*p);
        std::size_t eq = s.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        out[s.substr(0, eq)] = s.substr(eq + 1);
    }
    return out;
}

std::vector<char*> build_envp(const std::map<std::string, std::string>& merged,
                              std::vector<std::string>& store) {
    store.clear();
    store.reserve(merged.size());
    for (const auto& kv : merged) {
        store.push_back(kv.first + "=" + kv.second);
    }
    std::vector<char*> envp;
    envp.reserve(store.size() + 1);
    for (auto& s : store) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);
    return envp;
}

} // namespace

StdioMCPTransport::StdioMCPTransport(std::string command, std::vector<std::string> args,
                                     std::map<std::string, std::string> extra_env,
                                     MCPStdioFraming framing)
    : command_(std::move(command)), args_(std::move(args)), extra_env_(std::move(extra_env)),
      framing_(framing) {}

StdioMCPTransport::~StdioMCPTransport() {
    disconnect();
}

bool StdioMCPTransport::connect(const std::string& /*endpoint*/) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (connected_) {
        return true;
    }
    pending_read_.clear();
    io_ = std::make_unique<StdioPipes>();

    int in_pipe[2]{-1, -1};
    int out_pipe[2]{-1, -1};
    if (::pipe(in_pipe) != 0) {
        throw std::runtime_error("StdioMCPTransport: pipe() failed");
    }
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        throw std::runtime_error("StdioMCPTransport: pipe() failed");
    }

    posix_spawn_file_actions_t fa;
    if (::posix_spawn_file_actions_init(&fa) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        throw std::runtime_error("StdioMCPTransport: posix_spawn_file_actions_init failed");
    }
    ::posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
    ::posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
    // MCP servers may log to stderr; only stdout carries protocol messages.
    ::posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    ::posix_spawn_file_actions_addclose(&fa, out_pipe[0]);

    std::vector<std::string> argv_store;
    argv_store.reserve(args_.size() + 1);
    argv_store.push_back(command_);
    for (const auto& a : args_) {
        argv_store.push_back(a);
    }
    std::vector<char*> argv;
    argv.reserve(argv_store.size() + 1);
    for (auto& s : argv_store) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    std::map<std::string, std::string> merged_env = parse_environ(environ);
    for (const auto& kv : extra_env_) {
        merged_env[kv.first] = kv.second;
    }
    std::vector<std::string> env_store;
    std::vector<char*> envp = build_envp(merged_env, env_store);

    posix_spawnattr_t attr;
    if (::posix_spawnattr_init(&attr) != 0) {
        ::posix_spawn_file_actions_destroy(&fa);
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        io_.reset();
        throw std::runtime_error("StdioMCPTransport: posix_spawnattr_init failed");
    }
    const short spawn_flags = POSIX_SPAWN_SETPGROUP;
    const int flags_rc = ::posix_spawnattr_setflags(&attr, spawn_flags);
    const int pgroup_rc = ::posix_spawnattr_setpgroup(&attr, 0);
    if (flags_rc != 0 || pgroup_rc != 0) {
        ::posix_spawnattr_destroy(&attr);
        ::posix_spawn_file_actions_destroy(&fa);
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        io_.reset();
        const int error = flags_rc != 0 ? flags_rc : pgroup_rc;
        throw std::runtime_error("StdioMCPTransport: process group setup failed: " +
                                 std::string(std::strerror(error)));
    }
    int rc = ::posix_spawnp(&pid, command_.c_str(), &fa, &attr, argv.data(), envp.data());
    ::posix_spawnattr_destroy(&attr);
    ::posix_spawn_file_actions_destroy(&fa);

    ::close(in_pipe[0]);
    ::close(out_pipe[1]);

    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        io_.reset();
        throw std::runtime_error("StdioMCPTransport: posix_spawnp failed: " + std::string(std::strerror(rc)));
    }

    io_->pid = pid;
    io_->owns_process_group = true;
    io_->to_child = in_pipe[1];
    io_->from_child = out_pipe[0];
    connected_ = true;
    return true;
}

void StdioMCPTransport::disconnect() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!connected_ && !io_) {
        return;
    }
    pending_read_.clear();
    if (io_) {
        if (io_->to_child >= 0) {
            ::close(io_->to_child);
            io_->to_child = -1;
        }
        if (io_->from_child >= 0) {
            ::close(io_->from_child);
            io_->from_child = -1;
        }
        if (io_->pid > 0) {
            const pid_t signal_target = io_->owns_process_group ? -io_->pid : io_->pid;
            (void)::kill(signal_target, SIGTERM);
            int st = 0;
            for (int i = 0; i < 20; ++i) {
                pid_t w = ::waitpid(io_->pid, &st, WNOHANG);
                if (w == io_->pid) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            (void)::waitpid(io_->pid, &st, WNOHANG);
            if (::kill(signal_target, 0) == 0) {
                (void)::kill(signal_target, SIGKILL);
                (void)::waitpid(io_->pid, &st, 0);
            }
            io_->pid = -1;
        }
    }
    io_.reset();
    connected_ = false;
}

void StdioMCPTransport::write_framed_message(const json& msg) {
    std::string body = msg.dump();
    if (framing_ == MCPStdioFraming::JsonLines) {
        body.push_back('\n');
        writen(io_->to_child, body.data(), body.size());
        return;
    }
    std::ostringstream head;
    head << "Content-Length: " << body.size() << "\r\n\r\n";
    std::string h = head.str();
    writen(io_->to_child, h.data(), h.size());
    writen(io_->to_child, body.data(), body.size());
}

json StdioMCPTransport::read_framed_message() {
    return read_framed_message({});
}

json StdioMCPTransport::read_framed_message(
    const std::function<bool()>& cancellation_requested) {
    int tmo = mcp_timeout_ms();
    constexpr std::size_t k_max_message_bytes = 16U * 1024U * 1024U;
    std::string body_text;
    if (framing_ == MCPStdioFraming::JsonLines) {
        body_text = internal::read_one_json_line_text(
            io_->from_child, pending_read_, tmo, k_max_message_bytes, cancellation_requested);
    } else {
        constexpr std::size_t k_max_scan_bytes = 256U * 1024U;
        body_text = internal::read_one_framed_body_text(
            io_->from_child, pending_read_, tmo, k_max_scan_bytes, cancellation_requested);
    }
    try {
        return json::parse(body_text);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("StdioMCPTransport: non-JSON stdout message: ") +
                                 e.what());
    }
}

json StdioMCPTransport::transceive(const json& jsonrpc_request) {
    return transceive_cancellable(jsonrpc_request, {});
}

json StdioMCPTransport::transceive_cancellable(
    const json& jsonrpc_request, const std::function<bool()>& cancellation_requested) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!connected_ || !io_) {
        throw std::runtime_error("StdioMCPTransport: not connected");
    }
    write_framed_message(jsonrpc_request);
    return read_framed_message(cancellation_requested);
}

void StdioMCPTransport::send_notification(const json& jsonrpc_notification) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!connected_ || !io_) {
        throw std::runtime_error("StdioMCPTransport: not connected");
    }
    write_framed_message(jsonrpc_notification);
}

bool StdioMCPTransport::is_connected() const {
    return connected_;
}

MCPTransport StdioMCPTransport::get_transport_type() const {
    return MCPTransport::STDIO;
}

} // namespace agent_framework

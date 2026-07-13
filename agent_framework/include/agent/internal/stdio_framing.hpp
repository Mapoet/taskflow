/**
 * @file stdio_framing.hpp
 * @brief MCP stdio framing helpers (internal, testable)
 * @author Mapoet
 * @version 0.1
 * @date 2026-03-31
 */
#ifndef __AGENT_INTERNAL_STDIO_FRAMING_HPP__
#define __AGENT_INTERNAL_STDIO_FRAMING_HPP__

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <chrono>

#if !defined(_WIN32)
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#else
#error "agent_framework::internal::stdio_framing is not available on Windows"
#endif

namespace agent_framework {
namespace internal {

struct FramedHeaderRead {
    std::string headers;
    std::string buffered_body;
};

inline bool poll_readable(int fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int r = ::poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) {
            return poll_readable(fd, timeout_ms);
        }
        throw std::runtime_error("StdioMCPTransport: poll failed: " + std::string(std::strerror(errno)));
    }
    if (r == 0) {
        return false;
    }
    return (pfd.revents & (POLLIN | POLLHUP)) != 0;
}

inline bool poll_readable_cancellable(int fd, int timeout_ms,
                                      const std::function<bool()>& cancellation_requested) {
    if (!cancellation_requested) return poll_readable(fd, timeout_ms);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (cancellation_requested()) throw std::runtime_error("MCP request cancelled");
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (poll_readable(fd, static_cast<int>(std::min<long long>(remaining, 50)))) return true;
    }
}

inline std::size_t parse_content_length(const std::string& headers) {
    constexpr const char* kpref = "content-length:";
    std::istringstream in(headers);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        std::string lower;
        lower.reserve(line.size());
        for (char c : line) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower.rfind(kpref, 0) != 0) {
            continue;
        }
        std::size_t pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        std::stringstream ss(line.substr(pos + 1));
        std::size_t v = 0;
        ss >> v;
        return v;
    }
    throw std::runtime_error("StdioMCPTransport: missing Content-Length");
}

inline bool starts_with_icase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

inline std::size_t find_content_length_line_start(std::string_view buf) {
    constexpr std::string_view k = "Content-Length:";
    for (std::size_t i = 0; i + k.size() <= buf.size(); ++i) {
        if (i > 0) {
            const char prev = buf[i - 1];
            if (prev != '\n' && prev != '\r') {
                continue;
            }
        }
        if (starts_with_icase(buf.substr(i, k.size()), k)) {
            return i;
        }
    }
    return std::string_view::npos;
}

inline std::size_t find_header_end(std::string_view buf, std::size_t start) {
    const std::size_t p = buf.find("\r\n\r\n", start);
    if (p != std::string_view::npos) {
        return p;
    }
    return buf.find("\n\n", start);
}

inline std::size_t header_delim_len(std::string_view buf, std::size_t end_pos) {
    if (end_pos + 4 <= buf.size() && buf.substr(end_pos, 4) == "\r\n\r\n") {
        return 4;
    }
    return 2;
}

inline FramedHeaderRead read_http_style_headers_scanning(int fd, std::string& pending_read,
                                                         int timeout_ms,
                                                         std::size_t max_scan_bytes,
                                                         const std::function<bool()>& cancellation_requested = {}) {
    std::string buf;
    buf.swap(pending_read);

    std::size_t scanned = 0;
    std::size_t header_start = find_content_length_line_start(buf);
    if (header_start != std::string_view::npos && header_start > 0) {
        buf.erase(0, header_start);
        header_start = 0;
    }

    while (true) {
        if (header_start == std::string_view::npos) {
            header_start = find_content_length_line_start(buf);
            if (header_start != std::string_view::npos && header_start > 0) {
                buf.erase(0, header_start);
                header_start = 0;
            }
        }

        if (header_start != std::string_view::npos) {
            const std::size_t end_pos = find_header_end(buf, 0);
            if (end_pos != std::string_view::npos) {
                const std::size_t dlen = header_delim_len(buf, end_pos);
                FramedHeaderRead out;
                out.headers = buf.substr(0, end_pos + dlen);
                out.buffered_body = buf.substr(end_pos + dlen);
                pending_read.clear();
                return out;
            }
        }

        if (scanned > max_scan_bytes) {
            pending_read.clear();
            throw std::runtime_error(
                "StdioMCPTransport: framing header not found (stdout noise?)");
        }

        if (!poll_readable_cancellable(fd, timeout_ms, cancellation_requested)) {
            throw std::runtime_error("StdioMCPTransport: read timeout");
        }
        char tmp[4096];
        const ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("StdioMCPTransport: read failed: " +
                                     std::string(std::strerror(errno)));
        }
        if (n == 0) {
            throw std::runtime_error("StdioMCPTransport: unexpected EOF");
        }
        buf.append(tmp, static_cast<std::size_t>(n));
        scanned += static_cast<std::size_t>(n);
    }
}

inline std::string read_exact_with_pending(int fd, std::string& pending_read, std::size_t n,
                                           int timeout_ms,
                                           const std::function<bool()>& cancellation_requested = {}) {
    std::string out;
    out.reserve(n);

    if (!pending_read.empty()) {
        const std::size_t take = std::min(n, pending_read.size());
        out.append(pending_read.data(), take);
        pending_read.erase(0, take);
        if (out.size() == n) {
            return out;
        }
    }

    while (out.size() < n) {
        if (!poll_readable_cancellable(fd, timeout_ms, cancellation_requested)) {
            throw std::runtime_error("StdioMCPTransport: read timeout");
        }
        char tmp[4096];
        const std::size_t want = std::min<std::size_t>(sizeof(tmp), n - out.size());
        const ssize_t r = ::read(fd, tmp, want);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("StdioMCPTransport: read failed: " +
                                     std::string(std::strerror(errno)));
        }
        if (r == 0) {
            throw std::runtime_error("StdioMCPTransport: unexpected EOF");
        }
        out.append(tmp, static_cast<std::size_t>(r));
    }
    return out;
}

inline std::string read_one_framed_body_text(int fd, std::string& pending_read, int timeout_ms,
                                             std::size_t max_scan_bytes,
                                             const std::function<bool()>& cancellation_requested = {}) {
    FramedHeaderRead hdr = read_http_style_headers_scanning(fd, pending_read, timeout_ms,
                                                            max_scan_bytes, cancellation_requested);
    const std::size_t n = parse_content_length(hdr.headers);

    std::string body;
    if (hdr.buffered_body.size() >= n) {
        body = hdr.buffered_body.substr(0, n);
        pending_read = hdr.buffered_body.substr(n);
        return body;
    }
    body = std::move(hdr.buffered_body);
    const std::string rest = read_exact_with_pending(fd, pending_read, n - body.size(), timeout_ms,
                                                     cancellation_requested);
    body += rest;
    return body;
}

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_STDIO_FRAMING_HPP__

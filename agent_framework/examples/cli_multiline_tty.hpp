/**
 * @file cli_multiline_tty.hpp
 * @brief REPL 多行输入：Enter 换行；Ctrl+Enter（CSI 13;5u 等）或 Ctrl+O 提交
 *
 * POSIX/WSL：termios 非规范模式。Windows 下 read_multiline_repl_input 返回 false，调用方退回 std::getline。
 */
#ifndef AGENT_EXAMPLES_CLI_MULTILINE_TTY_HPP
#define AGENT_EXAMPLES_CLI_MULTILINE_TTY_HPP

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

namespace cli_multiline_tty {

#ifndef _WIN32

inline bool isatty_stdin() {
    return isatty(STDIN_FILENO) != 0;
}

/** true 若 CSI 表示 Enter + Ctrl（提交）。支持 13;5u、13;5~、1;5u 部分终端变种。 */
inline bool csi_is_ctrl_enter_submit(const std::string& seq) {
    if (seq.size() < 4 || seq[0] != '\x1b' || seq[1] != '[') {
        return false;
    }
    // 末尾字母
    char fin = seq.back();
    if (fin != 'u' && fin != 'U' && fin != '~') {
        return false;
    }
    // 找 "13" 与修饰键 5（Ctrl）；允许 1;5（少数实现用 1 表示 Enter）
    bool has_key_13 = (seq.find(";13;") != std::string::npos || seq.find("[13;") != std::string::npos ||
                       seq.find(";13~") != std::string::npos);
    bool has_key_1_enter = (seq.find("[1;5") != std::string::npos || seq.find(";1;5") != std::string::npos);
    bool has_ctrl_mod = (seq.find(";5u") != std::string::npos || seq.find(";5U") != std::string::npos ||
                         seq.find(";5~") != std::string::npos || seq.find(";5;") != std::string::npos);
    if (!has_ctrl_mod) {
        return false;
    }
    return has_key_13 || has_key_1_enter;
}

struct TtyRawModeGuard {
    int fd{};
    bool active{false};
    termios saved{};

    explicit TtyRawModeGuard(int fd_in = STDIN_FILENO) : fd(fd_in) {
        if (!isatty(fd)) {
            return;
        }
        if (tcgetattr(fd, &saved) != 0) {
            return;
        }
        termios t = saved;
        t.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
        t.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF));
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSADRAIN, &t) == 0) {
            active = true;
        }
    }

    ~TtyRawModeGuard() {
        if (active) {
            (void)tcsetattr(fd, TCSADRAIN, &saved);
        }
    }

    TtyRawModeGuard(const TtyRawModeGuard&) = delete;
    TtyRawModeGuard& operator=(const TtyRawModeGuard&) = delete;
};

/**
 * @brief 从 TTY 读入多行，直至 Ctrl+Enter（CSI）或 Ctrl+O（0x0f）
 * @param out 提交时的完整文本（含用户通过 Enter 产生的 '\n'）
 * @param interrupt 若为 true 则放弃读取（返回 false）
 * @return true 提交；false EOF / EINTR 中断 / 错误
 */
inline bool read_multiline_from_tty(std::string& out, const std::atomic<bool>* interrupt) {
    out.clear();
    TtyRawModeGuard guard(STDIN_FILENO);
    if (!guard.active) {
        return false;
    }

    auto interrupted = [&]() -> bool {
        return interrupt && interrupt->load();
    };

    while (!interrupted()) {
        unsigned char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                if (interrupted()) {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }

        // Ctrl+O：备用提交（不依赖终端 CSI）
        if (c == 0x0f) {
            (void)write(STDOUT_FILENO, "\r\n", 2);
            return true;
        }

        // Enter：换行写入缓冲并回显
        if (c == '\r' || c == '\n') {
            out.push_back('\n');
            (void)write(STDOUT_FILENO, "\r\n", 2);
            continue;
        }

        // Backspace / Ctrl+H
        if (c == 0x7f || c == 0x08) {
            if (!out.empty()) {
                out.pop_back();
                const char bs[] = "\b \b";
                (void)write(STDOUT_FILENO, bs, 3);
            }
            continue;
        }

        // ESC：CSI / 其它
        if (c == 0x1b) {
            std::string seq;
            seq.push_back(static_cast<char>(c));
            const int k_csi_max = 48;
            for (int i = 0; i < k_csi_max; ++i) {
                unsigned char ch = 0;
                ssize_t m = read(STDIN_FILENO, &ch, 1);
                if (m <= 0) {
                    break;
                }
                seq.push_back(static_cast<char>(ch));
                if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '~') {
                    break;
                }
            }
            if (csi_is_ctrl_enter_submit(seq)) {
                (void)write(STDOUT_FILENO, "\r\n", 2);
                return true;
            }
            // 忽略方向键等 ESC 序列
            continue;
        }

        // 可打印与控制字符：写入缓冲并回显
        if (c >= 0x20 && c < 0x7f) {
            out.push_back(static_cast<char>(c));
            (void)write(STDOUT_FILENO, &c, 1);
        } else if (c == '\t') {
            out.push_back('\t');
            (void)write(STDOUT_FILENO, "\t", 1);
        }
        // 其余控制符忽略
    }
    return false;
}

#endif // !_WIN32

/**
 * @return true 且 out 已填充：应用多行模式；false：调用方应用 std::getline
 */
inline bool read_multiline_repl_input(std::string& out, const std::atomic<bool>* interrupt) {
#ifdef _WIN32
    (void)out;
    (void)interrupt;
    return false;
#else
    if (!isatty_stdin()) {
        return false;
    }
    return read_multiline_from_tty(out, interrupt);
#endif
}

} // namespace cli_multiline_tty

#endif // AGENT_EXAMPLES_CLI_MULTILINE_TTY_HPP

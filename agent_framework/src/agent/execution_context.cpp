/**
 * @file execution_context.cpp
 * @brief ExecutionContext 实现
 */

#include <agent/agent/execution_context.hpp>

#include <cstdlib>
#include <filesystem>

namespace agent_framework {

namespace fs = std::filesystem;

ExecutionContext ExecutionContext::from_environment() {
    ExecutionContext ctx;
    ctx.input_policy_version = "wp27-v1";

    const char* pwd = std::getenv("PWD");
    if (pwd && pwd[0] != '\0') {
        std::error_code ec;
        const fs::path p(pwd);
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            ctx.cwd = fs::weakly_canonical(p, ec).string();
        }
    }
    if (ctx.cwd.empty()) {
        std::error_code ec;
        ctx.cwd = fs::current_path(ec).string();
        if (ec) {
            ctx.cwd.clear();
        }
    }

    return ctx;
}

} // namespace agent_framework

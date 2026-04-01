/**
 * @file cli_agent_demo.cpp
 * @brief WP1.6 CLI demo: argv, REPL, stream + terminal sink, SIGINT (cooperative)
 *
 * Stream tokens go to stdout via CLIHandler; final JSON summary via Sink → handle_final_result
 * (see CLIHandler: avoids duplicating full final_answer when streaming). Use AGENT_LOG_LEVEL or -v.
 *
 * `LLMClient::from_env` 前会调用与 `tests/test_agent_loop_wp5.cpp` 相同的 live 默认值
 * （如 DEEPSEEK_API_KEY → OPENAI_API_KEY、默认 base URL / model / 超时）；不覆盖已存在 env。
 */

#include "CLI11.hpp"

#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>
#include <agent/ui_manager.hpp>

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define ISATTY(_fd) _isatty(_fd)
#else
#include <unistd.h>
#define ISATTY(_fd) isatty(_fd)
#endif

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::atomic<bool> g_shutdown_requested{false};

void on_sigint_handler(int /*sig*/) {
    g_shutdown_requested.store(true);
}

ToolMeta make_add_meta() {
    ToolMeta m;
    m.name = "add";
    m.description = "sum two integers";
    m.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "a": { "type": "integer" },
            "b": { "type": "integer" }
        },
        "required": ["a", "b"]
    })");
    return m;
}

void register_demo_tools(ToolBus& bus) {
    bus.register_local_tool(
        "add",
        [](const json& j) {
            return json{{"result", j.at("a").get<int>() + j.at("b").get<int>()}};
        },
        make_add_meta());
}

std::string env_or(const char* key, const char* default_val) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(default_val);
}

void set_env_if_absent(const char* key, const char* val) {
    if (std::getenv(key) != nullptr) {
        return;
    }
#if defined(_WIN32)
    (void)_putenv_s(key, val);
#else
    (void)::setenv(key, val, 0);
#endif
}

/**
 * @brief 与 tests/test_agent_loop_wp5.cpp::apply_live_llm_env_defaults 一致：便于本地仅有
 *        DeepSeek 等键时直接跑 demo（不覆盖已设置的环境变量）。
 */
void apply_live_llm_env_defaults() {
    if (std::getenv("OPENAI_API_KEY") == nullptr) {
        const char* dk = std::getenv("DEEPSEEK_API_KEY");
        if (dk && *dk) {
#if defined(_WIN32)
            (void)_putenv_s("OPENAI_API_KEY", dk);
#else
            (void)::setenv("OPENAI_API_KEY", dk, 0);
#endif
        }
    }
    set_env_if_absent("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1");
    set_env_if_absent("AGENT_LLM_PROVIDER", "openai");
    set_env_if_absent("AGENT_HTTP_TIMEOUT_SEC", "120");
    set_env_if_absent("AGENT_LLM_MAX_RETRIES", "1");
    if (std::getenv("AGENT_LLM_MODEL") == nullptr) {
        const std::string m = env_or("DEEPSEEK_MODEL", "deepseek-chat");
#if defined(_WIN32)
        (void)_putenv_s("AGENT_LLM_MODEL", m.c_str());
#else
        (void)::setenv("AGENT_LLM_MODEL", m.c_str(), 0);
#endif
    }
    set_env_if_absent("AGENT_MCP_REQUEST_TIMEOUT_MS", "20000");
}

int run_graph_once(tf::Executor& executor,
                   const AgentConfig& cfg,
                   const AgentWorkflowDeps& deps,
                   const std::shared_ptr<internal::AgentThreadState>& state,
                   CLIHandler& cli) {
    workflow::GraphBuilder builder("cli_agent_demo");
    CliAgentTerminalSinkOptions sink;
    sink.sink_node_name = "CliSink";
    sink.on_final_json = [&cli](const json& j) { cli.handle_final_result(j); };

    CliAgentGraphOptions gopts;
    gopts.stream_callback = [&cli](std::string_view tok) {
        if (!g_shutdown_requested.load()) {
            cli.handle_stream_token(tok);
        }
    };

    try {
        build_cli_agent_graph_with_terminal_sink(builder, cfg, deps, state, sink, "AgentLoop",
                                                 gopts);
    } catch (const std::exception& e) {
        cli.handle_error(std::string("build graph: ") + e.what());
        return 1;
    }

    try {
        auto fut = builder.run_async(executor);
        fut.wait();
    } catch (const std::exception& e) {
        cli.handle_error(std::string("run: ") + e.what());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app("cli_agent_demo — Agent Framework WP1.6 (stdio CLI)\n"
                 "Streaming prints tokens to stdout; final line is a short [result] summary "
                 "(full answer often already streamed). Set AGENT_LOG_LEVEL=debug or -v for more.");
    app.get_formatter()->column_width(32);

    std::string prompt_arg;
    std::string provider_arg;
    int max_iterations = -1;
    bool verbose = false;
    bool mock = false;
    app.add_flag("-v,--verbose", verbose, "Same as AGENT_LOG_LEVEL=debug for this process");
    app.add_option("-p,--prompt", prompt_arg, "Single-turn user message; then exit");
    app.add_option("--provider", provider_arg, "Override AGENT_LLM_PROVIDER for this run");
    app.add_option("--max-iterations", max_iterations,
                   "Override AgentConfig::max_iterations (default: keep env/config)");
    app.add_flag("--mock", mock, "Reserved for WP1.7 (offline mock); not implemented yet");
    app.set_help_flag("-h,--help", "Print this help and environment hints");

    CLI11_PARSE(app, argc, argv);

    if (mock) {
        std::cerr << "[cli_agent_demo] --mock is reserved for WP1.7; use offline tests or unset "
                     "--mock.\n";
        return 2;
    }

    if (verbose) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_LOG_LEVEL", "debug");
#else
        (void)::setenv("AGENT_LOG_LEVEL", "debug", 1);
#endif
    }

    if (!provider_arg.empty()) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_LLM_PROVIDER", provider_arg.c_str());
#else
        (void)::setenv("AGENT_LLM_PROVIDER", provider_arg.c_str(), 1);
#endif
    }

    std::signal(SIGINT, on_sigint_handler);

    apply_live_llm_env_defaults();

    CLIHandler cli;
    std::shared_ptr<LLMClient> llm;
    try {
        llm = std::make_shared<LLMClient>(LLMClient::from_env());
    } catch (const std::exception& e) {
        std::cerr << "[error] LLM init: " << e.what() << "\n"
                  << "Set AGENT_LLM_PROVIDER, OPENAI_API_KEY / ANTHROPIC_API_KEY, or DEEPSEEK_API_KEY "
                     "(demo mirrors test_agent_loop_wp5 DeepSeek defaults), etc. "
                     "(see docs/guides/getting_started.md)\n";
        return 1;
    }
    auto renderer = std::make_shared<PromptRenderer>();
    llm->set_prompt_renderer(renderer);

    auto bus = std::make_shared<ToolBus>();
    try {
        register_demo_tools(*bus);
    } catch (const std::exception& e) {
        std::cerr << "[error] register tools: " << e.what() << '\n';
        return 1;
    }

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    AgentConfig cfg;
    cfg.name = "cli_agent_demo";
    cfg.system_prompt =
        "You are a helpful assistant. You may use the add tool for integer sums when relevant. "
        "Answer concisely.";
    if (const char* m = std::getenv("AGENT_LLM_MODEL")) {
        cfg.model_config.model_name = m;
    }
    if (max_iterations > 0) {
        cfg.max_iterations = max_iterations;
    }

    tf::Executor executor;

    auto state = std::make_shared<internal::AgentThreadState>();

    auto exec_line = [&](const std::string& line) -> int {
        if (g_shutdown_requested.load()) {
            return 130;
        }
        state->initial_user_prompt = line;
        return run_graph_once(executor, cfg, deps, state, cli);
    };

    if (!prompt_arg.empty()) {
        return exec_line(prompt_arg);
    }

    if (ISATTY(STDIN_FILENO)) {
        std::cout << "cli_agent_demo REPL (EOF or :quit to exit). Empty line skipped.\n";
        std::string line;
        while (!g_shutdown_requested.load() && std::cout << "> " && std::getline(std::cin, line)) {
            if (g_shutdown_requested.load()) {
                std::cout << "\n[interrupt]\n";
                break;
            }
            if (line == ":quit" || line == ":q") {
                break;
            }
            if (line.empty()) {
                continue;
            }
            const int rc = exec_line(line);
            if (rc != 0) {
                return rc;
            }
        }
        if (g_shutdown_requested.load()) {
            std::cout << "\n[interrupt]\n";
        }
        std::cout << std::flush;
        std::clog << std::flush;
        return g_shutdown_requested.load() ? 130 : 0;
    }

    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "[error] no input (pipe mode needs one line)\n";
        return 1;
    }
    return exec_line(line);
}

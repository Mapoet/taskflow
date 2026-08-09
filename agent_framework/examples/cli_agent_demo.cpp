/**
 * @file cli_agent_demo.cpp
 * @brief WP1.6 CLI demo: argv, REPL, stream + terminal sink, SIGINT (cooperative)
 *
 * 交互 REPL（TTY）：**Enter** 插入换行；**Ctrl+Enter**（终端发送 CSI 13;5u 等时）或 **Ctrl+O** 提交并执行。
 * 非 TTY / Windows：仍为单行 getline。
 *
 * Stream tokens go to stdout via CLIHandler; final JSON summary via Sink → handle_final_result
 * (see CLIHandler: avoids duplicating full final_answer when streaming). Use AGENT_LOG_LEVEL or -v.
 *
 * `LLMClient::from_env` 前会调用与 `tests/test_agent_loop_wp5.cpp` 相同的 live 默认值
 * （如 DEEPSEEK_API_KEY → OPENAI_API_KEY、默认 base URL / model / 超时）；不覆盖已存在 env。
 *
 * Cursor MCP：默认尝试从 mcp.json 注册工具（与 wp5 一致：`--cursor-mcp-json` →
 * `AGENT_TEST_CURSOR_MCP_JSON` → 空则 `ToolBus` 使用 `AGENT_MCP_CONFIG_PATH` 或 `~/.cursor/mcp.json`）。
 * 使用 `--no-cursor-mcp` 或环境变量 `AGENT_TEST_SKIP_CURSOR_MCP` / `AGENT_CLI_SKIP_CURSOR_MCP` 可跳过。
 */

#include "CLI11.hpp"
#include "cli_multiline_tty.hpp"
#include "common/agent_example_bootstrap.hpp"

#include <agent/graph_executor/graph_executor.hpp>
#include <agent/agent/execution_context.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/agent/user_input_preprocessor.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <cstddef>
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

bool env_truthy(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

/**
 * @brief 与 wp5 一致：`AGENT_TEST_CURSOR_MCP_JSON` 优先；否则空串交给 ToolBus（`AGENT_MCP_CONFIG_PATH` / ~/.cursor/mcp.json）
 */
std::string resolve_cursor_mcp_config_path(const std::string& cli_path) {
    return example::cursor_mcp_config_path(cli_path);
}

void import_cursor_mcp_tools(ToolBus& bus, const std::string& config_path_arg, bool dbg,
                            std::size_t* out_mcp_services) {
    example::BootstrapOptions options;
    options.cursor_mcp_config = config_path_arg;
    options.use_cursor_skill_roots = false;
    options.import_cursor_mcp = true;
    options.verbose = dbg;
    const auto boot = example::bootstrap_agent_services(bus, options);
    *out_mcp_services = boot.mcp_services;
    if(dbg) {
        for(const auto& diagnostic : boot.diagnostics) std::clog << "[bootstrap] " << diagnostic << '\n';
    }
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
    // npx 冷启动常 >20s；过短会导致 Context7/filesystem/playwright 等 stdio MCP 在握手阶段 read timeout
}

int run_graph_once(tf::Executor& executor,
                   const AgentConfig& cfg,
                   const AgentWorkflowDeps& deps,
                   const std::shared_ptr<internal::AgentThreadState>& state,
                   CLIHandler& cli) {
    GraphExecutor gx;
    ReactCliRunRequest req;
    req.config = cfg;
    req.deps = deps;
    req.session = state;
    req.options.sink.sink_node_name = "CliSink";
    req.options.sink.on_final_json = [&cli](const json& j) { cli.handle_final_result(j); };
    req.options.graph_options.stream_callback = [&cli](std::string_view tok) {
        if (!g_shutdown_requested.load()) {
            cli.handle_stream_token(tok);
        }
    };
    req.options.graph_options.skill_event_sink = [](const SkillEvent& event) {
        std::clog << example::skill_event_json(event).dump() << '\n';
    };
    try {
        WorkflowResult wr = gx.run_react_cli_sync(executor, req);
        if (!wr.success) {
            cli.handle_error(wr.error_message.value_or("run_react_cli_sync failed"));
            return wr.exit_code != 0 ? wr.exit_code : 1;
        }
    } catch (const std::exception& e) {
        cli.handle_error(std::string("run_react_cli_sync: ") + e.what());
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
    std::string cursor_mcp_json_arg;
    int max_iterations = -1;
    bool verbose = false;
    bool mock = false;
    bool no_cursor_mcp = false;
    app.add_flag("-v,--verbose", verbose, "Same as AGENT_LOG_LEVEL=debug for this process");
    app.add_option("-p,--prompt", prompt_arg, "Single-turn user message; then exit");
    app.add_option("--provider", provider_arg, "Override AGENT_LLM_PROVIDER for this run");
    app.add_option("--max-iterations", max_iterations,
                   "Override AgentConfig::max_iterations (default: keep env/config)");
    app.add_option("--cursor-mcp-json", cursor_mcp_json_arg,
                   "Path to Cursor mcp.json (else AGENT_TEST_CURSOR_MCP_JSON, else ToolBus default ~/.cursor/mcp.json)");
    app.add_flag("--no-cursor-mcp", no_cursor_mcp,
                 "Skip Cursor MCP import (or env AGENT_TEST_SKIP_CURSOR_MCP / AGENT_CLI_SKIP_CURSOR_MCP)");
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

    CLIHandler cli;
    example::LiveRuntime runtime;
    try {
        example::LiveRuntimeOptions options;
        options.agent_name = "cli_agent_demo";
        options.cursor_mcp_config = cursor_mcp_json_arg;
        options.provider = provider_arg;
        options.max_iterations = max_iterations;
        options.use_cursor_skill_roots = false;
        options.import_cursor_mcp = !(no_cursor_mcp || env_truthy("AGENT_TEST_SKIP_CURSOR_MCP") ||
                                      env_truthy("AGENT_CLI_SKIP_CURSOR_MCP"));
        options.verbose = verbose || env_truthy("AGENT_TEST_AGENT_LOOP_DEBUG");
        runtime = example::build_live_runtime(options);
    } catch (const std::exception& e) {
        std::cerr << "[error] LLM init: " << e.what() << "\n"
                  << "Set AGENT_LLM_PROVIDER, OPENAI_API_KEY / ANTHROPIC_API_KEY, or DEEPSEEK_API_KEY "
                     "(demo mirrors test_agent_loop_wp5 DeepSeek defaults), etc. "
                     "(see docs/guides/getting_started.md)\n";
        return 1;
    }
    AgentWorkflowDeps deps{runtime.llm, runtime.toolbus, runtime.skills,
                           runtime.memory_compaction_llm};
    AgentConfig cfg = runtime.config;

    tf::Executor executor;

    auto state = std::make_shared<internal::AgentThreadState>();

    auto exec_line = [&](const std::string& line) -> int {
        if (g_shutdown_requested.load()) {
            return 130;
        }
        state->skill_prompt_cache.reset();
        state->active_skill_id.reset();
        state->pending_injected_context.clear();
        state->pending_control_actions.clear();
        state->pending_input_violations.clear();
        ExecutionContext ectx = ExecutionContext::from_environment();
        PreprocessOptions popts;
        popts.toolbus = deps.toolbus;
        UserInputPreprocessor prep(popts);
        ProcessedUserInput proc = prep.process(line, ectx);
        if (env_input_strict_enabled() && !proc.tier_a_violations.empty()) {
            for (const auto& v : proc.tier_a_violations) {
                std::cerr << v << '\n';
            }
            return 3;
        }
        apply_processed_to_agent_state(std::move(proc), ectx, *state);
        // LLM/MCP 可能阻塞较久且无首 token；提示走 clog，避免误以为 REPL 卡死
        std::clog << "[cli_agent_demo] running agent loop (streaming to stdout; "
                     "wait up to AGENT_HTTP_TIMEOUT_SEC)...\n"
                  << std::flush;
        return run_graph_once(executor, cfg, deps, state, cli);
    };

    if (!prompt_arg.empty()) {
        return exec_line(prompt_arg);
    }

    if (ISATTY(STDIN_FILENO)) {
        std::clog << "[cli_agent_demo] REPL ready (LLM + ToolBus/MCP loaded).\n" << std::flush;
        std::cout << "cli_agent_demo REPL — Enter=newline, Ctrl+Enter or Ctrl+O=submit, :quit / :q, EOF.\n"
                  << std::flush;
        std::string line;
        while (!g_shutdown_requested.load()) {
            std::cout << "> " << std::flush;
            bool got = cli_multiline_tty::read_multiline_repl_input(line, &g_shutdown_requested);
            if (!got) {
                if (g_shutdown_requested.load()) {
                    std::cout << "\n[interrupt]\n";
                    break;
                }
                if (!std::getline(std::cin, line)) {
                    break;
                }
            }
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
            std::cout << std::flush;
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

/**
 * @file cli_agent_skills_demo.cpp
 * @brief CLI 示例：与 `cli_agent_demo` 等价全链路，默认从 **Cursor 技能目录**加载 Skills。
 *
 * **技能路径**（合并扫描，仅已存在目录参与）：每个技能为 **`<root>/<skill-folder>/SKILL.md`**。
 * - `$HOME/.cursor/skills`
 * - `$HOME/.cursor/skills-cursor`（Windows：`%USERPROFILE%` 下相同相对路径）
 *
 * 若设置 **`AGENT_SKILLS_DIR`**，则 **仅**使用该单目录（`SkillServices::from_env()`），便于覆写或 CI。
 *
 * 流式、ToolBus、`run_skill_script`、MCP 等与 `cli_agent_demo` 一致。
 *
 * TTY REPL：**Enter** 换行；**Ctrl+Enter** 或 **Ctrl+O** 提交（与 `cli_agent_demo` 相同；非 TTY 为单行）。
 */

#include "CLI11.hpp"
#include "cli_multiline_tty.hpp"

#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/skill_services.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>
#include <agent/ui_manager.hpp>

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <memory>
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

std::string resolve_cursor_mcp_config_path(const std::string& cli_path) {
    if (!cli_path.empty()) {
        return cli_path;
    }
    const char* test_env = std::getenv("AGENT_TEST_CURSOR_MCP_JSON");
    if (test_env && *test_env) {
        return std::string(test_env);
    }
    return "";
}

void import_cursor_mcp_tools(ToolBus& bus, const std::string& config_path_arg, bool dbg,
                            std::size_t* out_mcp_services) {
    *out_mcp_services = 0;
    const std::string path_for_display = config_path_arg.empty()
                                              ? std::string("<ToolBus default: AGENT_MCP_CONFIG_PATH or ~/.cursor/mcp.json>")
                                              : config_path_arg;
    ToolBus::CursorMcpImportResult r = bus.register_mcp_from_cursor_config(config_path_arg, true);
    *out_mcp_services = r.registered_services.size();
    auto tools = bus.export_as_llm_tools();

    if (dbg) {
        std::clog << "cursor_mcp config_path=\"" << path_for_display << "\"\n";
        std::clog << "  registered_services=" << r.registered_services.size()
                  << " failures=" << r.failures.size() << "\n";
        for (const auto& name : r.registered_services) {
            std::clog << "    ok: " << name << '\n';
        }
        for (const auto& f : r.failures) {
            std::clog << "    fail: " << f.service_name << " - " << f.reason << '\n';
        }
        std::clog << "toolbus export_as_llm_tools count=" << tools.size() << '\n';
    } else if (!r.failures.empty() && *out_mcp_services == 0) {
        std::clog << "[cli_agent_skills_demo] cursor_mcp: no services registered ("
                  << r.failures.size()
                  << " failure(s); use -v or AGENT_TEST_AGENT_LOOP_DEBUG=1 for details)\n";
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
    // npx 冷启动较慢；过短会导致 stdio MCP 握手阶段 read timeout
    set_env_if_absent("AGENT_MCP_REQUEST_TIMEOUT_MS", "1500");
}

/** `AGENT_SKILLS_DIR` 优先；否则合并扫描 Cursor 默认双路径。 */
std::shared_ptr<SkillServices> resolve_skills_services(bool dbg) {
    const char* override_dir = std::getenv("AGENT_SKILLS_DIR");
    std::shared_ptr<SkillServices> svc;
    if (override_dir && *override_dir) {
        svc = SkillServices::from_env();
        if (dbg) {
            std::clog << "[cli_agent_skills_demo] skills: AGENT_SKILLS_DIR=\"" << override_dir << "\"\n";
        }
    } else {
        svc = SkillServices::from_cursor_default_skill_roots();
        if (dbg && svc && svc->registry) {
            std::clog << "[cli_agent_skills_demo] skills: Cursor roots (merge scan):\n";
            for (const auto& r : svc->registry->roots()) {
                std::clog << "  - " << r.string() << '\n';
            }
            std::clog << "  indexed_skills=" << svc->registry->entries().size() << '\n';
        } else if (dbg && !svc) {
            std::clog << "[cli_agent_skills_demo] skills: no AGENT_SKILLS_DIR and "
                         "~/.cursor/skills / ~/.cursor/skills-cursor missing — skills disabled\n";
        }
    }
    return svc;
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
    try {
        WorkflowResult wr = gx.run_react_cli_sync(executor, req);
        if (!wr.success) {
            cli.handle_error(wr.error_message.value_or("run_react_cli_sync failed"));
            return 1;
        }
    } catch (const std::exception& e) {
        cli.handle_error(std::string("run_react_cli_sync: ") + e.what());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app("cli_agent_skills_demo — Cursor ~/.cursor/skills{,-cursor} + WP1.6 CLI\n"
                 "Override with AGENT_SKILLS_DIR. Streaming to stdout; -v for skill/MCP details.");
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
        std::cerr << "[cli_agent_skills_demo] --mock is reserved for WP1.7; use offline tests or unset "
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
                     "(see docs/guides/getting_started.md)\n";
        return 1;
    }
    auto renderer = std::make_shared<PromptRenderer>();
    llm->set_prompt_renderer(renderer);

    const bool skip_cursor_mcp = no_cursor_mcp || env_truthy("AGENT_TEST_SKIP_CURSOR_MCP") ||
                                 env_truthy("AGENT_CLI_SKIP_CURSOR_MCP");
    const bool mcp_dbg = verbose || env_truthy("AGENT_TEST_AGENT_LOOP_DEBUG");

    auto bus = std::make_shared<ToolBus>();
    try {
        register_demo_tools(*bus);
    } catch (const std::exception& e) {
        std::cerr << "[error] register tools: " << e.what() << '\n';
        return 1;
    }

    std::size_t mcp_services = 0;
    if (!skip_cursor_mcp) {
        std::clog << "[cli_agent_skills_demo] loading Cursor MCP config (--no-cursor-mcp to skip)...\n"
                     "  (stdio/HTTP 单服务最长等待 AGENT_MCP_REQUEST_TIMEOUT_MS；未设置时本 demo 为 1500 ms)\n"
                  << std::flush;
        const std::string mcp_cfg = resolve_cursor_mcp_config_path(cursor_mcp_json_arg);
        import_cursor_mcp_tools(*bus, mcp_cfg, mcp_dbg, &mcp_services);
        if (!mcp_dbg && mcp_services > 0) {
            std::clog << "[cli_agent_skills_demo] cursor_mcp: " << mcp_services
                      << " service(s) registered\n";
        }
    } else if (mcp_dbg) {
        std::clog << "cursor_mcp: skipped (--no-cursor-mcp or skip env)\n";
    }

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;
    deps.skills = resolve_skills_services(mcp_dbg);

    AgentConfig cfg;
    cfg.name = "cli_agent_skills_demo";
    if (!skip_cursor_mcp && mcp_services > 0) {
        cfg.system_prompt =
            "你是一个能够调用外部工具的助手。\n"
            "若有与问题直接相关的工具，优先调用工具获取可核对的信息；若无完全对口工具，可结合现有工具输出与常识推理补全结论。\n"
            "不要编造无法核对的细节；若信息不足，请明确假设并给出合理区间。\n"
            "若系统提示中带有 Active skill，请优先遵循该技能说明；run_skill_script 的 skill_id 须为已索引技能的 canonical 名（勿编造；与 Cursor SKILL 的 name/目录名一致）。需配置 AGENT_SKILL_SCRIPT_ALLOWLIST。\n"
            "回答使用简体中文，结构清晰。\n";
    } else {
        cfg.system_prompt =
            "你是一个助手。当前未加载 MCP 工具；请基于常识与公开典型情况回答，并明确标注为估算。\n"
            "不要编造无法核对的细节；信息不足时请说明假设并给出合理区间。\n"
            "若带有 Active skill 段，请优先遵循；run_skill_script 的 skill_id 须为已索引 canonical；需 allowlist。\n"
            "回答使用简体中文，结构清晰。\n";
    }
    if (env_truthy("AGENT_SKILL_INJECT_CATALOG") && deps.skills && deps.skills->registry) {
        std::size_t cap = 2048;
        if (const char* c = std::getenv("AGENT_SKILL_CATALOG_MAX_CHARS")) {
            const int v = std::atoi(c);
            if (v > 0) {
                cap = static_cast<std::size_t>(v);
            }
        }
        cfg.system_prompt += format_skill_catalog_l1(*deps.skills->registry, cap);
    }
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
        state->skill_prompt_cache.reset();
        state->active_skill_id.reset();
        state->initial_user_prompt = line;
        std::clog << "[cli_agent_skills_demo] running agent loop (streaming to stdout; "
                     "wait up to AGENT_HTTP_TIMEOUT_SEC)...\n"
                  << std::flush;
        return run_graph_once(executor, cfg, deps, state, cli);
    };

    if (!prompt_arg.empty()) {
        return exec_line(prompt_arg);
    }

    if (ISATTY(STDIN_FILENO)) {
        std::clog << "[cli_agent_skills_demo] REPL ready (LLM + ToolBus/MCP + Cursor skills dirs).\n"
                  << std::flush;
        std::cout << "cli_agent_skills_demo REPL — Enter=newline, Ctrl+Enter or Ctrl+O=submit, :quit / :q, EOF.\n"
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

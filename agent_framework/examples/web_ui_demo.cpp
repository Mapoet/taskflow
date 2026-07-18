/**
 * @file web_ui_demo.cpp
 * @brief WP2.U Track W：httplib 静态资源 + SSE（/ui/sse）+ POST /ui/run
 *
 * 构建：-DAGENT_BUILD_WEB_UI=ON。静态根目录由 CMake 定义 AGENT_WEB_UI_STATIC_ROOT。
 */

#include "CLI11.hpp"
#include "common/agent_example_bootstrap.hpp"

#include <agent/agent/execution_context.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>
#include <agent/agent/user_input_preprocessor.hpp>

#include <httplib.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifndef AGENT_WEB_UI_STATIC_ROOT
#define AGENT_WEB_UI_STATIC_ROOT "."
#endif

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::atomic<bool> g_agent_busy{false};
std::mutex g_control_mutex;
std::shared_ptr<TaskControl> g_active_control;

std::mutex g_sse_slot_mutex;
bool g_sse_slot_taken = false;

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

bool env_truthy(const char* key) {
    return example::env_truthy(key);
}

std::string resolve_cursor_mcp_config_path(const std::string& cli_path) {
    return example::cursor_mcp_config_path(cli_path);
}

example::BootstrapResult import_cursor_mcp_tools(ToolBus& bus, const std::string& config_path_arg,
                                                 bool dbg) {
    example::BootstrapOptions options;
    options.cursor_mcp_config = config_path_arg;
    options.use_cursor_skill_roots = false;
    options.import_cursor_mcp = true;
    options.verbose = dbg;
    const auto boot = example::bootstrap_agent_services(bus, options);
    if(dbg) {
        for(const auto& diagnostic : boot.diagnostics) std::clog << "[bootstrap] " << diagnostic << '\n';
    }
    return boot;
}

std::shared_ptr<SkillServices> resolve_skills_services(bool dbg) {
    example::BootstrapOptions options;
    options.use_cursor_skill_roots = true;
    options.verbose = dbg;
    auto services = example::discover_skill_services(options);
    const auto status = example::skill_ui_status(services);
    std::clog << "[bootstrap] skills=" << (status.enabled ? "enabled" : "disabled")
              << " root=" << (status.root.empty() ? "-" : status.root)
              << " indexed=" << status.count << " generation=" << status.generation
              << " diagnostics=" << status.diagnostics << " errors=" << status.errors << '\n';
    if(dbg && services && services->registry)
        for(const auto& diagnostic : services->registry->diagnostics())
            std::clog << "[bootstrap] skill " << diagnostic.code << ": "
                      << diagnostic.path.string() << '\n';
    return services;
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
        const char* m = std::getenv("DEEPSEEK_MODEL");
        const std::string model = (m && *m) ? std::string(m) : "deepseek-chat";
#if defined(_WIN32)
        (void)_putenv_s("AGENT_LLM_MODEL", model.c_str());
#else
        (void)::setenv("AGENT_LLM_MODEL", model.c_str(), 0);
#endif
    }
}

int run_graph_ui(tf::Executor& executor,
                 const AgentConfig& cfg,
                 const AgentWorkflowDeps& deps,
                 const std::shared_ptr<internal::AgentThreadState>& state,
                 UIManager& ui,
                 const std::shared_ptr<TaskControl>& control) {
    GraphExecutor gx;
    ReactCliRunRequest req;
    req.config = cfg;
    req.deps = deps;
    req.session = state;
    req.options.sink.sink_node_name = "CliSink";
    req.options.sink.on_final_json = [&ui](const json& j) { ui.dispatch_final_result(j); };
    req.options.graph_options.stream_callback = [&ui](std::string_view tok) {
        ui.stream_token("default", tok);
    };
    req.options.graph_options.task_control = control;
    req.options.graph_options.tool_execution_observer = [&ui](const ToolExecutionEvent& event) {
        json payload{{"tool_name", event.tool_name},
                     {"tool_call_id", event.tool_call_id},
                     {"arguments", event.arguments}};
        if (event.phase == ToolExecutionPhase::Completed) payload["result"] = event.result;
        ui.dispatch_message(event.phase == ToolExecutionPhase::Started ? "tool_started"
                                                                       : "tool_completed",
                            payload);
    };
    req.options.graph_options.skill_event_sink = [](const SkillEvent& event) {
        std::clog << example::skill_event_json(event).dump() << '\n';
    };
    try {
        WorkflowResult wr = gx.run_react_cli_sync(executor, req);
        if (!wr.success) {
            if (control && control->is_cancel_requested()) {
                ui.dispatch_message("run_cancelled", json{{"message", "Run cancelled by user"}});
                return 0;
            }
            ui.dispatch_error(wr.error_message.value_or("run_react_cli_sync failed"));
            return wr.exit_code != 0 ? wr.exit_code : 1;
        }
    } catch (const std::exception& e) {
        ui.dispatch_error(std::string("run_react_cli_sync: ") + e.what());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app("web_ui_demo — WP2.U httplib + SSE + static UI");
    int port = 8080;
    std::string prompt_arg;
    std::string provider_arg;
    std::string cursor_mcp_json_arg;
    std::string skills_root_arg;
    std::string skill_authoring_root_arg;
    int max_iterations = -1;
    bool verbose = false;
    bool no_cursor_mcp = false;
    bool demo_state = false;
    bool no_skills = false;
    app.add_option("--port", port, "Listen port")->check(CLI::PositiveNumber);
    app.add_option("-p,--prompt", prompt_arg, "Optional single-turn on startup");
    app.add_option("--provider", provider_arg, "Override AGENT_LLM_PROVIDER");
    app.add_option("--max-iterations", max_iterations, "Override max_iterations");
    app.add_option("--cursor-mcp-json", cursor_mcp_json_arg,
                   "Cursor mcp.json (else AGENT_TEST_CURSOR_MCP_JSON, else ToolBus default)");
    app.add_option("--skills-root", skills_root_arg, "Installed read-only Skill root")
        ->check(CLI::ExistingDirectory);
    app.add_option("--skill-authoring-root", skill_authoring_root_arg,
                   "Writable root used by /skills create");
    app.add_flag("--no-skills", no_skills, "Disable Skill discovery and management");
    app.add_flag("--no-cursor-mcp", no_cursor_mcp,
                 "Skip MCP (or AGENT_TEST_SKIP_CURSOR_MCP / AGENT_CLI_SKIP_CURSOR_MCP)");
    app.add_flag("--demo-state", demo_state,
                 "Load deterministic Scientific Console content without invoking the LLM");
    app.add_flag("-v,--verbose", verbose, "AGENT_LOG_LEVEL=debug");
    CLI11_PARSE(app, argc, argv);

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
    example::apply_skill_cli_options(skills_root_arg, skill_authoring_root_arg, no_skills);

    apply_live_llm_env_defaults();

    std::shared_ptr<LLMClient> llm;
    try {
        llm = std::make_shared<LLMClient>(LLMClient::from_env());
    } catch (const std::exception& e) {
        std::cerr << "[web_ui_demo] LLM init: " << e.what() << "\n";
        return 1;
    }
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());

    const bool skip_cursor_mcp = no_cursor_mcp || env_truthy("AGENT_TEST_SKIP_CURSOR_MCP") ||
                                 env_truthy("AGENT_CLI_SKIP_CURSOR_MCP");
    const bool mcp_dbg = verbose || env_truthy("AGENT_TEST_AGENT_LOOP_DEBUG");

    auto bus = std::make_shared<ToolBus>();
    register_demo_tools(*bus);

    std::size_t mcp_services = 0;
    example::BootstrapResult mcp_boot;
    if (!skip_cursor_mcp) {
        std::clog << "[web_ui_demo] loading Cursor MCP config (--no-cursor-mcp to skip)...\n"
 "  (AGENT_MCP_REQUEST_TIMEOUT_MS per request; transport default 60000 ms)\n"
                  << std::flush;
        const std::string mcp_cfg = resolve_cursor_mcp_config_path(cursor_mcp_json_arg);
        mcp_boot = import_cursor_mcp_tools(*bus, mcp_cfg, mcp_dbg);
        mcp_services = mcp_boot.mcp_services;
        if (!mcp_dbg && mcp_services > 0) {
            std::clog << "[web_ui_demo] cursor_mcp: " << mcp_services << " service(s) registered\n";
        }
    } else if (mcp_dbg) {
        std::clog << "cursor_mcp: skipped (--no-cursor-mcp or skip env)\n";
    }

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;
    deps.skills = resolve_skills_services(mcp_dbg);

    AgentConfig cfg;
    cfg.name = "web_ui_demo";
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

    // AgentLoop / OpenAIAdapter treat AGENT_TEST_AGENT_LOOP_DEBUG as permission to print
    // iterations and full final_answer to std::cout — fine for CLI, but it looks like the Web
    // UI is "hijacked" to the terminal. MCP import above already ran with the prior env value.
#if defined(_WIN32)
    (void)_putenv_s("AGENT_TEST_AGENT_LOOP_DEBUG", "0");
#else
    (void)unsetenv("AGENT_TEST_AGENT_LOOP_DEBUG");
#endif

    auto conn = std::make_shared<WebConnectionInfo>();
    conn->session_id = "default";
    conn->connection_type = "SSE";
    conn->is_active = true;

    auto web_handler = std::make_unique<WebHandler>("default", conn);
    WebHandler* web_h = web_handler.get();

    UIManager ui;
    ui.register_web_connection("default", std::move(web_handler));

    auto state = std::make_shared<internal::AgentThreadState>();
    auto executor = std::make_shared<tf::Executor>();

    auto run_line = [&](const std::string& line) {
        auto control = std::make_shared<TaskControl>();
        {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            g_active_control = control;
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
                ui.dispatch_error(v);
            }
            std::lock_guard<std::mutex> lock(g_control_mutex);
            if (g_active_control == control) g_active_control.reset();
            return;
        }
        apply_processed_to_agent_state(std::move(proc), ectx, *state);
        (void)run_graph_ui(*executor, cfg, deps, state, ui, control);
        {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            if (g_active_control == control) g_active_control.reset();
        }
    };

    const char* provider_env = std::getenv("AGENT_LLM_PROVIDER");
    auto emit_bootstrap = [&]() {
        ui.dispatch_message("runtime", json{{"session", "orbital-analysis"},
                                            {"provider", provider_env && *provider_env ? provider_env : "OpenAI"},
                                            {"model", cfg.model_config.model_name.empty() ? "provider default" : cfg.model_config.model_name},
                                            {"connection", skip_cursor_mcp ? "Core tools ready" :
                                             mcp_boot.diagnostics.empty() ? "MCP connected" : "MCP partial"}});
        const auto skills = example::skill_ui_status(deps.skills);
        ui.dispatch_message("skills_status", json{{"enabled", skills.enabled},
                                                   {"count", skills.count},
                                                   {"generation", skills.generation},
                                                   {"diagnostics", skills.diagnostics},
                                                   {"errors", skills.errors},
                                                   {"root", skills.root},
                                                   {"active", skills.active}});
        for (const auto& diagnostic : mcp_boot.diagnostics)
            ui.dispatch_message("mcp_status", json{{"level", "error"}, {"message", "MCP unavailable: " + diagnostic}});
        for (const auto& service : mcp_boot.skipped_mcp_services)
            ui.dispatch_message("mcp_status", json{{"level", "info"}, {"message", "MCP skipped by policy: " + service}});
        if (!demo_state) return;
        ui.dispatch_message("demo_user", json{{"content", "Plot sin(x) from 0 to 2π and find its maximum value and location."}});
        ui.stream_token("default", "I evaluated the expression over [0, 2π] and generated a plot.\n\nThe maximum value is 1.0000 at x = π/2 ≈ 1.5708 rad.");
        ui.dispatch_message("tool_started", json{{"tool_name", "fs_search"}, {"tool_call_id", "demo-fs"}, {"arguments", json{{"query", "*.cpp"}, {"path", "/workspace/src"}}}});
        ui.dispatch_message("tool_completed", json{{"tool_name", "fs_search"}, {"tool_call_id", "demo-fs"}, {"arguments", json{{"query", "*.cpp"}}}, {"result", json{{"matches", 18}, {"top", "agent.cpp, tool_mgr.cpp, plot_tool.cpp"}}}});
        ui.dispatch_message("tool_started", json{{"tool_name", "web_search"}, {"tool_call_id", "demo-web"}, {"arguments", json{{"query", "sin(x) maximum 0 to 2pi"}}}});
        ui.dispatch_message("tool_completed", json{{"tool_name", "web_search"}, {"tool_call_id", "demo-web"}, {"arguments", json{{"query", "sin(x) maximum 0 to 2pi"}}}, {"result", json{{"sources", 5}, {"status", "verified"}}}});
        ui.dispatch_message("tool_started", json{{"tool_name", "expr_eval"}, {"tool_call_id", "demo-expr"}, {"arguments", json{{"expression", "max(sin(x))"}}}});
        ui.dispatch_message("tool_completed", json{{"tool_name", "expr_eval"}, {"tool_call_id", "demo-expr"}, {"arguments", json{{"expression", "max(sin(x))"}}}, {"result", json{{"value", 1.0}, {"x", 1.5708}}}});
        ui.dispatch_final_result(json{{"final_answer", "demo"}, {"iteration", 3}, {"history_size", 6}});
    };

    if (!demo_state && !prompt_arg.empty()) {
        g_agent_busy = true;
        std::thread([run_line, prompt_arg]() { run_line(prompt_arg); g_agent_busy = false; }).detach();
    }

    httplib::Server svr;

    svr.Post("/ui/run", [&](const httplib::Request& req, httplib::Response& res) {
        if (g_agent_busy.load()) {
            res.status = 429;
            res.set_content(R"({"error":"agent busy"})", "application/json");
            return;
        }
        try {
            json body = json::parse(req.body);
            std::string prompt = body.at("prompt").get<std::string>();
            if (prompt.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"empty prompt"})", "application/json");
                return;
            }
            g_agent_busy = true;
            std::thread([run_line, prompt]() {
                run_line(prompt);
                g_agent_busy = false;
            }).detach();
            res.status = 202;
            res.set_content(R"({"accepted":true})", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            json err{{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    svr.Post("/ui/cancel", [&](const httplib::Request&, httplib::Response& res) {
        std::shared_ptr<TaskControl> control;
        {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            control = g_active_control;
        }
        if (!control || !g_agent_busy.load()) {
            res.status = 409;
            res.set_content(R"({"cancelled":false,"reason":"no active run"})", "application/json");
            return;
        }
        control->request_cancel();
        res.status = 202;
        res.set_content(R"({"cancelled":true})", "application/json");
    });

    svr.Get("/ui/sse", [web_h, &emit_bootstrap](const httplib::Request& req, httplib::Response& res) {
        std::string session = req.get_param_value("session");
        if (session.empty()) {
            session = "default";
        }
        if (session != "default") {
            res.status = 404;
            res.set_content(R"({"error":"unknown session"})", "application/json");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_sse_slot_mutex);
            if (g_sse_slot_taken) {
                res.status = 503;
                res.set_content(R"({"error":"sse slot busy; use one browser tab"})",
                               "application/json");
                return;
            }
            g_sse_slot_taken = true;
        }
        emit_bootstrap();
        res.status = 200;
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [web_h, last_heartbeat = std::chrono::steady_clock::now()]
            (std::size_t /*offset*/, httplib::DataSink& sink) mutable {
                if (!sink.is_writable()) return false;
                std::string chunk;
                if (web_h->try_pop_sse_chunk(chunk)) {
                    sink.write(chunk.data(), chunk.size());
                } else {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_heartbeat >= std::chrono::seconds(1)) {
                        static constexpr char heartbeat[] = ": heartbeat\n\n";
                        sink.write(heartbeat, sizeof(heartbeat) - 1);
                        last_heartbeat = now;
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                return sink.is_writable();
            },
            [] {
                std::lock_guard<std::mutex> lk(g_sse_slot_mutex);
                g_sse_slot_taken = false;
            });
    });

    const std::string mount = AGENT_WEB_UI_STATIC_ROOT;
    if (!svr.set_mount_point("/", mount.c_str())) {
        std::cerr << "[web_ui_demo] set_mount_point failed for " << mount << "\n";
        return 1;
    }

    std::cerr << "[web_ui_demo] http://127.0.0.1:" << port << "/ (static from " << mount << ")\n";
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "[web_ui_demo] listen failed\n";
        return 1;
    }
    return 0;
}

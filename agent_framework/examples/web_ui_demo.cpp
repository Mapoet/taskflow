/**
 * @file web_ui_demo.cpp
 * @brief WP2.U Track W：httplib 静态资源 + SSE（/ui/sse）+ POST /ui/run
 *
 * 构建：-DAGENT_BUILD_WEB_UI=ON。静态根目录由 CMake 定义 AGENT_WEB_UI_STATIC_ROOT。
 */

#include "CLI11.hpp"
#include "common/agent_example_bootstrap.hpp"
#include "common/phase4_operations_bootstrap.hpp"

#include <agent/agent/execution_context.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/toolbus/fs_sandbox.hpp>
#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>
#include <agent/ui/live_operations_projection.hpp>
#include <agent/agent/user_input_preprocessor.hpp>
#include <agent/approval/executor.hpp>

#include <httplib.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
                 const std::shared_ptr<TaskControl>& control,
                 const std::shared_ptr<LiveOperationsProjection>& operations,
                 const example::LiveRuntime& runtime) {
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
    req.options.graph_options.thinking_stream_callback = [&ui](std::string_view tok) {
        ui.stream_thinking("default", tok);
    };
    req.options.graph_options.task_control = control;
    req.options.graph_options.tool_execution_observer = [&ui, operations](const ToolExecutionEvent& event) {
        json payload{{"tool_name", event.tool_name},
                     {"tool_call_id", event.tool_call_id},
                     {"arguments", event.arguments}};
        if (event.phase == ToolExecutionPhase::Completed) payload["result"] = event.result;
        ui.dispatch_message(event.phase == ToolExecutionPhase::Started ? "tool_started"
                                                                       : "tool_completed",
                            payload);
        if (operations) operations->observe_tool(event);
    };
    req.options.graph_options.skill_event_sink = [](const SkillEvent& event) {
        std::clog << example::skill_event_json(event).dump() << '\n';
    };
    try {
        WorkflowResult wr{};
        auto turn = example::run_conversation_turn(runtime, "web_ui_demo",
            state->initial_user_prompt,
            [&] { wr = gx.run_react_cli_sync(executor, req); return wr; },
            [&ui](const conversation::RuntimeEventEnvelope& event) {
                ui.dispatch_message("runtime_event", conversation::encode(event));
            });
        if (!turn.error.empty() || turn.outcome.reason != conversation::ModelTurnStopReason::EndTurn) {
            if (control && control->is_cancel_requested()) {
                ui.dispatch_message("run_cancelled", json{{"message", "Run cancelled by user"}});
                return 0;
            }
            ui.dispatch_error(!turn.error.empty() ? turn.error :
                wr.error_message.value_or(turn.outcome.candidate_answer));
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
    std::string phase4_state_dir_arg;
    std::string operations_db_arg;
    std::string operations_tenant_arg{"demo-tenant"};
    std::string operations_run_arg{"run-orbit-042"};
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
    app.add_option("--phase4-state-dir", phase4_state_dir_arg,
                   "Persistent directory for accountable Phase 4 UI state");
    app.add_option("--operations-db", operations_db_arg, "Operations snapshot SQLite database");
    app.add_option("--operations-tenant", operations_tenant_arg, "Operations tenant identity");
    app.add_option("--operations-run", operations_run_arg, "Operations run identity");
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
    const bool import_cursor_mcp =
        !(no_cursor_mcp || env_truthy("AGENT_TEST_SKIP_CURSOR_MCP") ||
          env_truthy("AGENT_CLI_SKIP_CURSOR_MCP"));

    example::LiveRuntime runtime;
    try {
        example::LiveRuntimeOptions options;
        options.agent_name = "web_ui_demo";
        options.skills_root = skills_root_arg;
        options.cursor_mcp_config = cursor_mcp_json_arg;
        options.provider = provider_arg;
        options.max_iterations = max_iterations;
        options.use_cursor_skill_roots = true;
        options.enable_skills = !no_skills;
        options.import_cursor_mcp = import_cursor_mcp;
        options.verbose = verbose || env_truthy("AGENT_TEST_AGENT_LOOP_DEBUG");
        runtime = example::build_live_runtime(options);
        example::require_direct_demo_execution(runtime);
    } catch (const std::exception& e) {
        std::cerr << "[web_ui_demo] LLM init: " << e.what() << "\n";
        return 1;
    }
    AgentWorkflowDeps deps{runtime.llm, runtime.toolbus, runtime.skills,
                           runtime.memory_compaction_llm};
    AgentConfig cfg = runtime.config;

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
    if (phase4_state_dir_arg.empty()) {
        phase4_state_dir_arg = (std::filesystem::temp_directory_path() /
            ("taskflow-web-ui-phase4-" + std::to_string(port))).string();
    }
    std::filesystem::create_directories(phase4_state_dir_arg);
    if (operations_db_arg.empty()) {
        operations_db_arg = (std::filesystem::path(phase4_state_dir_arg) /
                             "operations.sqlite3").string();
    }
    auto operations_bootstrap = example::load_phase4_operations({
        operations_db_arg, operations_tenant_arg, operations_run_arg, demo_state, true});
    auto operations_store = operations_bootstrap.store;
    auto operations = std::make_shared<Phase4OperationsSnapshot>(
        std::move(operations_bootstrap.snapshot));
    auto operations_mutex = std::make_shared<std::mutex>();
    auto live_operations = std::make_shared<LiveOperationsProjection>(
        *operations, operations_store,
        [&ui, operations, operations_mutex](const Phase4OperationsSnapshot& snapshot) {
            {
                std::lock_guard<std::mutex> lock(*operations_mutex);
                *operations = snapshot;
            }
            ui.publish_phase4_operations(snapshot);
        });
    auto approval_store = std::make_shared<approval::SQLiteApprovalStore>(
        (std::filesystem::path(phase4_state_dir_arg) / "approval.sqlite3").string());
    auto approval_executor = std::make_shared<approval::AccountableApprovalExecutor>(
        (std::filesystem::path(phase4_state_dir_arg) / "approval-votes.sqlite3").string(),
        *approval_store);
    auto approval_actions = std::make_shared<approval::AuthenticatedApprovalActionService>(
        *approval_executor);
    if (demo_state && !operations->hitl.empty()) {
        approval::ApprovalRequest request;
        request.metadata.identity.tenant_id = "demo-tenant";
        request.metadata.identity.task_id = operations->task_id;
        request.metadata.identity.run_id = operations->run_id;
        request.approval_id = operations->hitl.front().id;
        request.request_kind = operations->hitl.front().kind;
        request.requester_id = operations->hitl.front().requested_by;
        request.scope = "release:" + operations->run_id;
        request.reason = "Accountable release decision";
        request.risk_level = "medium";
        request.policy_revision = "phase4-policy-v1";
        request.plan_digest = "sha256:demo-plan";
        request.arguments_digest = "sha256:demo-release-arguments";
        request.artifact_digest = "sha256:demo-artifact";
        request.memory_view_digest = "sha256:demo-memory-view";
        request.created_at = "2026-08-10T12:00:00+08:00";
        request.expires_at = "2027-08-10T18:00:00+08:00";
        (void)approval_store->put_request(request);
    }

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
        (void)run_graph_ui(*executor, cfg, deps, state, ui, control, live_operations, runtime);
        {
            std::lock_guard<std::mutex> lock(g_control_mutex);
            if (g_active_control == control) g_active_control.reset();
        }
    };

    const char* provider_env = std::getenv("AGENT_LLM_PROVIDER");
    auto emit_bootstrap = [&]() {
        const std::string connection = !import_cursor_mcp ? "Core tools ready" :
            runtime.bootstrap.diagnostics.empty()
                ? (runtime.bootstrap.mcp_services > 0 ? "MCP connected" : "Core tools ready")
                : "MCP partial";
        ui.dispatch_message("runtime", json{{"session", "orbital-analysis"},
                                            {"provider", provider_env && *provider_env ? provider_env : "OpenAI"},
                                            {"model", cfg.model_config.model_name.empty() ? "provider default" : cfg.model_config.model_name},
                                            {"connection", connection}});
        const auto skills = example::skill_ui_status(deps.skills);
        ui.dispatch_message("skills_status", json{{"enabled", skills.enabled},
                                                   {"count", skills.count},
                                                   {"generation", skills.generation},
                                                   {"diagnostics", skills.diagnostics},
                                                   {"errors", skills.errors},
                                                   {"root", skills.root},
                                                   {"active", skills.active}});
        for (const auto& diagnostic : runtime.bootstrap.diagnostics)
            ui.dispatch_message("mcp_status", json{{"level", "error"}, {"message", "MCP unavailable: " + diagnostic}});
        for (const auto& service : runtime.bootstrap.skipped_mcp_services)
            ui.dispatch_message("mcp_status", json{{"level", "info"}, {"message", "MCP skipped by policy: " + service}});
        if (!demo_state) return;
        {
            std::lock_guard<std::mutex> lock(*operations_mutex);
            ui.publish_phase4_operations(*operations);
        }
        ui.dispatch_message("demo_user", json{{"content", "分析 sin(x) 在 [0, 2π] 的极值，并给出可复核结果。"}});
        ui.stream_thinking("default", "已完成符号分析与数值交叉验证；以下仅展示可公开的推理摘要。\n");
        ui.stream_token("default",
            "## 计算结论\n\n函数在区间内的最大值为 **1.0000**，位置为 $x = \\pi/2 \\approx 1.5708$ rad。\n\n"
            "| 指标 | 数值 |\n|---|---:|\n| 最大值 | 1.0000 |\n| 位置 | π/2 |\n\n"
            "```cpp\ndouble y = std::sin(x);\n```\n\n"
            "$$\\max_{x \\in [0,2\\pi]} \\sin(x)=1$$\n\n"
            "```mermaid\ngraph LR\n  Sample --> Evaluate --> Verify\n```\n");
        ui.dispatch_message("tool_started", json{{"tool_name", "fs_search"}, {"tool_call_id", "demo-fs"}, {"arguments", json{{"query", "*.cpp"}, {"path", "/workspace/src"}}}});
        ui.dispatch_message("tool_completed", json{{"tool_name", "fs_search"}, {"tool_call_id", "demo-fs"}, {"arguments", json{{"query", "*.cpp"}}}, {"result", json{{"matches", 18}, {"top", "agent.cpp, tool_mgr.cpp, plot_tool.cpp"}}}});
        ui.dispatch_message("tool_started", json{{"tool_name", "web_search"}, {"tool_call_id", "demo-web"}, {"arguments", json{{"query", "sin(x) maximum 0 to 2pi"}}}});
        ui.dispatch_message("tool_completed", json{{"tool_name", "web_search"}, {"tool_call_id", "demo-web"}, {"arguments", json{{"query", "sin(x) maximum 0 to 2pi"}}}, {"result", json{{"sources", 5}, {"status", "verified"}}}});
        ui.dispatch_message("tool_started", json{{"tool_name", "expr_eval"}, {"tool_call_id", "demo-expr"}, {"arguments", json{{"expression", "max(sin(x))"}}}});
        ui.dispatch_message("tool_completed", json{{"tool_name", "expr_eval"}, {"tool_call_id", "demo-expr"}, {"arguments", json{{"expression", "max(sin(x))"}}}, {"result", json{{"value", 1.0}, {"x", 1.5708}}}});
        for (const auto& event : std::vector<ToolExecutionEvent>{
                 {ToolExecutionPhase::Started, "fs_search", "demo-fs", json::object(), json::object()},
                 {ToolExecutionPhase::Completed, "fs_search", "demo-fs", json::object(), {{"ok", true}}},
                 {ToolExecutionPhase::Started, "web_search", "demo-web", json::object(), json::object()},
                 {ToolExecutionPhase::Completed, "web_search", "demo-web", json::object(), {{"ok", true}}},
                 {ToolExecutionPhase::Started, "expr_eval", "demo-expr", json::object(), json::object()},
                 {ToolExecutionPhase::Completed, "expr_eval", "demo-expr", json::object(), {{"ok", true}}}})
            live_operations->observe_tool(event);
        ui.dispatch_message("artifact", json{{"id", "demo-console-reference"},
                                               {"mime", "image/png"},
                                               {"path", "agent_framework/docs/assets/ui/scientific-console-reference.png"},
                                               {"caption", "Scientific console reference artifact"}});
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

    svr.Post("/ui/operations/hitl", [&](const httplib::Request& req, httplib::Response& res) {
        if (!demo_state) {
            res.status = 409;
            res.set_content(R"({"error":"no accountable HITL executor is attached to this run"})",
                            "application/json");
            return;
        }
        try {
            const char* configured_reviewer = std::getenv("AGENT_WEB_REVIEWER_ID");
            const char* configured_session = std::getenv("AGENT_WEB_HITL_SESSION_TOKEN");
            if (!configured_reviewer || !*configured_reviewer || !configured_session || !*configured_session) {
                res.status = 503;
                res.set_content(R"({"error":"authenticated HITL identity provider is not configured"})", "application/json");
                return;
            }
            if (!req.has_header("X-Agent-Session") ||
                req.get_header_value("X-Agent-Session") != configured_session) {
                res.status = 403;
                res.set_content(R"({"error":"invalid authenticated HITL session"})", "application/json");
                return;
            }
            const json body = json::parse(req.body);
            const std::string request_id = body.at("request_id").get<std::string>();
            const std::string action = body.at("action").get<std::string>();
            const std::string reviewer_id = configured_reviewer;
            auto persisted_request = approval_store->request(request_id);
            if (!persisted_request) {
                res.status = 404;
                res.set_content(R"({"error":"approval request is not persisted"})", "application/json");
                return;
            }
            std::lock_guard<std::mutex> lock(*operations_mutex);
            auto request = std::find_if(operations->hitl.begin(), operations->hitl.end(),
                                        [&](const auto& item) { return item.id == request_id; });
            if (request == operations->hitl.end() ||
                std::find(request->allowed_actions.begin(), request->allowed_actions.end(), action) ==
                    request->allowed_actions.end()) {
                res.status = 422;
                res.set_content(R"({"error":"request or action is not allowed"})", "application/json");
                return;
            }
            approval::AuthenticatedPrincipal principal{reviewer_id, {"approver"},
                "server-session:" + reviewer_id};
            approval::ApprovalActionIntent intent{request_id,
                approval::encode(*persisted_request).at("canonical_digest"), action,
                action == "request_remediation" ?
                    "Reviewer rejected this revision and requested remediation" :
                    "Accountable decision submitted from operations UI",
                "2026-08-10T14:35:00+08:00", approval_executor->vote_revision(request_id)};
            const auto result = approval_actions->submit(principal, intent);
            if (result.outcome != approval::ReviewOutcome::Approved &&
                result.outcome != approval::ReviewOutcome::Rejected) {
                res.status = 409;
                res.set_content(json{{"error", result.error_code},
                                     {"detail", result.error_message}}.dump(), "application/json");
                return;
            }
            if (action == "approve") {
                request->status = OperationsStatus::Passed;
                request->summary = "Approved by " + reviewer_id + " · durable decision r" +
                    std::to_string(result.decision_revision);
                operations->overall_status = OperationsStatus::Warning;
                operations->blocker.clear();
            } else if (action == "request_remediation") {
                request->status = OperationsStatus::Running;
                request->summary = "Revision rejected by " + reviewer_id + "; remediation required";
                operations->overall_status = OperationsStatus::Running;
                operations->blocker = "Remediation workflow is active.";
            } else if (action == "reject") {
                request->status = OperationsStatus::Failed;
                request->summary = "Release rejected by " + reviewer_id + " · durable decision r" +
                    std::to_string(result.decision_revision);
                operations->overall_status = OperationsStatus::Failed;
                operations->blocker = "Accountable reviewer rejected release.";
            }
            auto release = std::find_if(operations->stages.begin(), operations->stages.end(),
                [](const auto& stage) { return stage.id == "release"; });
            if (release != operations->stages.end()) {
                release->status = action == "approve" ? OperationsStatus::Passed :
                    action == "request_remediation" ? OperationsStatus::Running : OperationsStatus::Failed;
                release->summary = request->summary;
                ++release->revision;
            }
            operations->snapshot_id += ".next";
            operations->updated_at = "2026-08-10T14:35:00+08:00";
            std::string snapshot_error;
            if (!operations_store || !operations_store->save(*operations, &snapshot_error)) {
                throw std::runtime_error("cannot persist operations snapshot: " + snapshot_error);
            }
            ui.publish_phase4_operations(*operations);
            res.status = 202;
            res.set_content(json{{"accepted", true}, {"snapshot_id", operations->snapshot_id},
                                 {"decision_digest", result.decision_digest}}.dump(),
                            "application/json");
        } catch (const std::exception& error) {
            res.status = 400;
            res.set_content(json{{"error", error.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/ui/operations/snapshot", [&](const httplib::Request&, httplib::Response& res) {
        if (!demo_state) {
            res.status = 404;
            res.set_content(R"({"error":"no operations snapshot is attached to this run"})",
                            "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(*operations_mutex);
        res.set_header("Cache-Control", "no-store");
        res.set_content(Phase4OperationsProjection::to_json(*operations).dump(),
                        "application/json");
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

    // Artifact files are served only from the canonical AGENT_FS_ROOT and only
    // as a conservative image allowlist. This route intentionally does not
    // expose arbitrary fs_read capability to the browser.
    svr.Get(R"(/ui/files/(.*))", [](const httplib::Request& req, httplib::Response& res) {
        const auto config = load_fs_sandbox_config_from_env();
        if (!config || req.matches.size() < 2) {
            res.status = 404;
            return;
        }
        json error;
        const auto resolved = fs_resolve_under_root(req.matches[1].str(), config->root, error);
        std::error_code ec;
        if (!resolved || !std::filesystem::is_regular_file(*resolved, ec) || ec) {
            res.status = 404;
            return;
        }
        const auto size = std::filesystem::file_size(*resolved, ec);
        constexpr std::uintmax_t kMaxUiArtifactBytes = 16U * 1024U * 1024U;
        if (ec || size > kMaxUiArtifactBytes) {
            res.status = 413;
            return;
        }
        std::string mime;
        const auto ext = resolved->extension().string();
        if (ext == ".png") mime = "image/png";
        else if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
        else if (ext == ".webp") mime = "image/webp";
        else if (ext == ".gif") mime = "image/gif";
        else {
            res.status = 415;
            return;
        }
        std::ifstream input(*resolved, std::ios::binary);
        if (!input) {
            res.status = 404;
            return;
        }
        std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        res.set_header("Cache-Control", "private, max-age=60");
        res.set_header("Content-Security-Policy", "default-src 'none'; sandbox");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_content(bytes, mime.c_str());
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

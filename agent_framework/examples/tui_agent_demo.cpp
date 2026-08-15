/**
 * @file tui_agent_demo.cpp
 * @brief WP2.U Track T：FTXUI 全屏 + TuiHandler + 同 cli 图路径
 *
 * 构建：-DAGENT_BUILD_TUI=ON（需初始化 3rd-party/FTXUI 子模块）
 */

#include "CLI11.hpp"
#include "common/agent_example_bootstrap.hpp"
#include "common/phase4_operations_bootstrap.hpp"
#include "common/ftxui_console_view.hpp"

#include <agent/agent/execution_context.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/skills/skill_control.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/ui/tui_handler.hpp>
#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>
#include <agent/ui/presentation_model.hpp>
#include <agent/ui/live_operations_projection.hpp>
#include <agent/agent/user_input_preprocessor.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::atomic<bool> g_shutdown{false};

/** 串行化 ReAct 运行，避免多线程同时 stream到同一 TuiHandler 导致输出交错 */
std::mutex g_tui_agent_run_mutex;

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
                 const std::shared_ptr<UiPresentationModel>& presentation,
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
        if (!g_shutdown.load()) {
            ui.stream_token("default", tok);
        }
    };
    req.options.graph_options.thinking_stream_callback = [&ui](std::string_view tok) {
        if (!g_shutdown.load()) ui.stream_thinking("default", tok);
    };
    req.options.graph_options.task_control = control;
    req.options.graph_options.tool_execution_observer = [presentation, operations](const ToolExecutionEvent& event) {
        if (presentation) presentation->observe_tool(event);
        if (operations) operations->observe_tool(event);
    };
    req.options.graph_options.skill_event_sink = [](const SkillEvent& event) {
        std::clog << example::skill_event_json(event).dump() << '\n';
    };
    try {
        WorkflowResult wr{};
        auto turn = example::run_conversation_turn(runtime, "tui_agent_demo",
            state->initial_user_prompt,
            [&] { wr = gx.run_react_cli_sync(executor, req); return wr; },
            [operations](const conversation::RuntimeEventEnvelope& event) {
                if(operations) operations->observe_runtime(event);
            });
        if (!turn.error.empty() || turn.outcome.reason != conversation::ModelTurnStopReason::EndTurn) {
            if (control && control->is_cancel_requested()) {
                if (presentation) presentation->cancel();
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

int tui_agent_demo_main(int argc, char** argv) {
    CLI::App app("tui_agent_demo — FTXUI 7.0.1 + TuiHandler + MCP/Skills");
    std::string prompt_arg;
    std::string provider_arg;
    std::string cursor_mcp_json_arg;
    std::string skills_root_arg;
    std::string skill_authoring_root_arg;
    std::string operations_db_arg;
    std::string operations_tenant_arg{"demo-tenant"};
    std::string operations_run_arg{"run-orbit-042"};
    int max_iterations = -1;
    bool verbose = false;
    bool no_cursor_mcp = false;
    bool demo_state = false;
    bool no_skills = false;
    app.add_option("-p,--prompt", prompt_arg, "Optional single-turn then interactive");
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
    app.add_option("--operations-db", operations_db_arg, "Operations snapshot SQLite database");
    app.add_option("--operations-tenant", operations_tenant_arg, "Operations tenant identity");
    app.add_option("--operations-run", operations_run_arg, "Operations run identity");
    app.add_flag("-v,--verbose", verbose, "AGENT_LOG_LEVEL=debug");
    CLI11_PARSE(app, argc, argv);

    std::setlocale(LC_ALL, "");

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

    example::LiveRuntime runtime;
    try {
        example::LiveRuntimeOptions options;
        options.agent_name = "tui_agent_demo";
        options.skills_root = skills_root_arg;
        options.cursor_mcp_config = cursor_mcp_json_arg;
        options.provider = provider_arg;
        options.max_iterations = max_iterations;
        options.use_cursor_skill_roots = true;
        options.enable_skills = !no_skills;
        options.import_cursor_mcp = !(no_cursor_mcp || env_truthy("AGENT_TEST_SKIP_CURSOR_MCP") ||
                                      env_truthy("AGENT_CLI_SKIP_CURSOR_MCP"));
        options.verbose = verbose || env_truthy("AGENT_TEST_AGENT_LOOP_DEBUG");
        runtime = example::build_live_runtime(options);
        example::require_harness_supported_execution(runtime);
    } catch (const std::exception& e) {
        std::cerr << "[tui_agent_demo] LLM init: " << e.what() << "\n";
        return 1;
    }
    const bool skip_cursor_mcp = !runtime.bootstrap.mcp_services;
    const auto& mcp_boot = runtime.bootstrap;
    AgentWorkflowDeps deps{runtime.llm, runtime.toolbus, runtime.skills,
                           runtime.memory_compaction_llm};
    AgentConfig cfg = runtime.config;

    std::clog << "[tui_agent_demo] starting fullscreen TUI (FTXUI 7.0.1 / UTF-8)...\n" << std::flush;

    auto presentation = std::make_shared<UiPresentationModel>();
    const char* provider_env = std::getenv("AGENT_LLM_PROVIDER");
    presentation->set_runtime_metadata("orbital-analysis",
                                       provider_env && *provider_env ? provider_env : "OpenAI",
                                       cfg.model_config.model_name.empty() ? "provider default" : cfg.model_config.model_name,
                                       skip_cursor_mcp ? "Core tools ready" :
                                       mcp_boot.diagnostics.empty() ? "MCP connected" : "MCP partial");
    std::shared_ptr<SQLiteOperationsSnapshotStore> operations_store;
    Phase4OperationsSnapshot initial_operations;
    if (demo_state) {
        auto loaded = example::load_phase4_operations({
            operations_db_arg, operations_tenant_arg, operations_run_arg, true});
        operations_store = loaded.store;
        initial_operations = std::move(loaded.snapshot);
        presentation->observe_operations(initial_operations);
    } else {
        if(operations_db_arg.empty())
            operations_db_arg = example::default_operations_database("tui_agent_demo");
        operations_store = std::make_shared<SQLiteOperationsSnapshotStore>(operations_db_arg);
        initial_operations.tenant_id = operations_tenant_arg;
        initial_operations.run_id = operations_run_arg;
        initial_operations.task_id = "tui-live-task";
    }
    auto operations = std::make_shared<LiveOperationsProjection>(
        std::move(initial_operations), operations_store,
        [presentation](const Phase4OperationsSnapshot& snapshot) {
            presentation->observe_operations(snapshot);
        });
    for (const auto& diagnostic : mcp_boot.diagnostics)
        presentation->add_system_notice("MCP unavailable: " + diagnostic, true);
    for (const auto& service : mcp_boot.skipped_mcp_services)
        presentation->add_system_notice("MCP skipped by policy: " + service);

    auto tui_handler = std::make_unique<TuiHandler>(presentation);
    TuiHandler* tui_h = tui_handler.get();

    UIManager ui;
    ui.register_handler(std::move(tui_handler));

    auto state = std::make_shared<internal::AgentThreadState>();
    tf::Executor executor;

    std::atomic<bool> agent_busy{false};
    std::mutex control_mutex;
    std::shared_ptr<TaskControl> active_control;

    auto run_line = [&](const std::string& line) {
        std::lock_guard<std::mutex> run_lk(g_tui_agent_run_mutex);
        presentation->begin_user_turn(line);
        auto control = std::make_shared<TaskControl>();
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            active_control = control;
        }
        state->skill_prompt_cache.reset();
        state->active_skill_id.reset();
        state->pending_injected_context.clear();
        state->pending_control_actions.clear();
        state->pending_input_violations.clear();
        ExecutionContext ectx = ExecutionContext::from_environment();
        if (deps.skills) {
            ectx.resources = deps.skills->pin_resource_context();
            ectx.skill_loader = deps.skills->loader;
        }
        PreprocessOptions popts;
        popts.toolbus = deps.toolbus;
        UserInputPreprocessor prep(popts);
        ProcessedUserInput proc = prep.process(line, ectx);
        if (env_input_strict_enabled() && !proc.tier_a_violations.empty()) {
            for (const auto& v : proc.tier_a_violations) {
                ui.dispatch_error(v);
            }
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control == control) active_control.reset();
            return;
        }
        std::vector<ControlAction> pending;
        bool handled_skill_control = false;
        for (const auto& action : proc.control_actions) {
            auto result = dispatch_skill_control(action, deps.skills ? deps.skills->manager : nullptr);
            if (!result.handled) {
                pending.push_back(action);
                continue;
            }
            handled_skill_control = true;
            if (result.ok) ui.stream_token("default", "\n[skills]\n" + result.text + "\n");
            else ui.dispatch_error(result.text);
        }
        proc.control_actions = std::move(pending);
        if (handled_skill_control && proc.llm_user_text.find_first_not_of(" \t\r\n") == std::string::npos &&
            proc.control_actions.empty()) {
            presentation->complete(json{{"final_answer", "skill control completed"},
                                        {"kind", "skill_control"}});
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control == control) active_control.reset();
            return;
        }
        if (deps.skills && deps.skills->manager) {
            const auto active = deps.skills->manager->active_skill_id();
            if (!active.empty()) state->active_skill_id = active;
        }
        apply_processed_to_agent_state(std::move(proc), ectx, *state);
        (void)run_graph_ui(executor, cfg, deps, state, ui, control, presentation, operations, runtime);
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control == control) active_control.reset();
        }
    };

    std::mutex worker_mutex;
    std::thread worker;
    example::FtxuiConsoleView* view_ptr = nullptr;

    auto cancel_active = [&] {
        std::lock_guard<std::mutex> lock(control_mutex);
        if (active_control) active_control->request_cancel();
    };

    auto launch_line = [&](std::string line) {
        if (line.empty() || agent_busy.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(worker_mutex);
            if (worker.joinable()) worker.join();
            worker = std::thread([&, line = std::move(line)] {
                run_line(line);
                agent_busy.store(false);
                if (view_ptr) {
                    view_ptr->set_busy(false);
                    view_ptr->request_refresh();
                }
            });
        }
        if (view_ptr) view_ptr->set_busy(true);
    };

    auto skill_status_provider = [&]() {
        example::FtxuiSkillStatus result;
        const auto status = example::skill_ui_status(deps.skills);
        result.enabled = status.enabled;
        result.count = status.count;
        result.generation = status.generation;
        result.diagnostics = status.diagnostics;
        result.errors = status.errors;
        result.root = status.root;
        result.active = status.active;
        return result;
    };

    example::FtxuiConsoleCallbacks callbacks;
    callbacks.on_submit = [&](std::string line) { launch_line(std::move(line)); };
    callbacks.on_cancel = cancel_active;
    callbacks.on_quit = [&] {
        g_shutdown.store(true);
        cancel_active();
    };
    example::FtxuiConsoleView view(
        [tui_h] { return tui_h->presentation_snapshot(); },
        skill_status_provider,
        std::move(callbacks));
    view_ptr = &view;

    if (!prompt_arg.empty()) launch_line(prompt_arg);
    const int rc = view.run();

    g_shutdown.store(true);
    cancel_active();
    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        if (worker.joinable()) worker.join();
    }
    view_ptr = nullptr;
    return rc;
}

int main(int argc, char** argv) {
    try { return tui_agent_demo_main(argc, argv); }
    catch(const std::exception& error) {
        std::cerr << "[tui_agent_demo] fatal startup/runtime error: " << error.what() << '\n';
        return 2;
    }
}

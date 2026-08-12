/**
 * @file imgui_agent_demo.cpp
 * @brief WP2.U Track I：GLFW + OpenGL3 + Dear ImGui + 同 cli图路径（UIManager 队列）
 *
 * 主线程：GLFW/ImGui + drain StreamMessage；工作线程：run_react_cli_sync。
 * 构建：-DAGENT_BUILD_IMGUI=ON。Dear ImGui：优先 `3rd-party/imgui` 子模块，否则 FetchContent。
 * ImPlot / ImPlot3D：可选 `3rd-party/implot`、`3rd-party/implot3d` 子模块（见仓库 .gitmodules）。
 */

#include "CLI11.hpp"
#include "common/agent_example_bootstrap.hpp"
#include "common/phase4_operations_bootstrap.hpp"
#include "common/imgui_console_view.hpp"
#include "common/imgui_text_input_support.hpp"

#include <agent/agent/execution_context.hpp>
#include <agent/agent/task_state_machine.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/ui/thread_safe_queue.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>
#include <agent/ui/presentation_model.hpp>
#include <agent/agent/user_input_preprocessor.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#if defined(AGENT_HAS_IMPLOT)
#include <implot.h>
#endif
#if defined(AGENT_HAS_IMPLOT3D)
#include <implot3d.h>
#endif

#include <GLFW/glfw3.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

#ifndef AGENT_IMG_LSAN_SUPP_PATH
#define AGENT_IMG_LSAN_SUPP_PATH ""
#endif

/** Before GLFW/X11 init: merge LSan suppressions for known libX11 XIM leaks (ASan+Linux). */
static void agent_imgui_merge_lsan_suppressions() {
#if defined(__SANITIZE_ADDRESS__) && !defined(_WIN32)
    const char* path = AGENT_IMG_LSAN_SUPP_PATH;
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    const char* cur = std::getenv("LSAN_OPTIONS");
    if (cur != nullptr && std::strstr(cur, "suppressions=") != nullptr) {
        return;
    }
    std::string s = std::string("suppressions=") + path;
    if (cur != nullptr && cur[0] != '\0') {
        s += ':';
        s += cur;
    }
    (void)::setenv("LSAN_OPTIONS", s.c_str(), 1);
#endif
}

/** System CJK font for ImGui; set AGENT_IMGUI_FONT_PATH to override. */
static bool try_load_imgui_cjk_font(ImGuiIO& io) {
    const char* env_path = std::getenv("AGENT_IMGUI_FONT_PATH");
    const char* candidates[] = {
        env_path,
#if defined(_WIN32)
        R"(C:\Windows\Fonts\msyh.ttc)",
        R"(C:\Windows\Fonts\simsun.ttc)",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
#endif
        nullptr,
    };
    for (const char* p : candidates) {
        if (!p || !*p) {
            continue;
        }
#if defined(_WIN32)
        if (_access(p, 0) != 0) {
            continue;
        }
#else
        if (access(p, R_OK) != 0) {
            continue;
        }
#endif
        ImFontConfig fc;
        fc.OversampleH = 2;
        fc.OversampleV = 1;
        fc.FontNo = 0; // NotoSansCJK-Regular.ttc face 0 is Simplified Chinese.
        // ImGui 1.92 dynamically rasterizes requested glyphs when GlyphRanges is null. This
        // avoids the legacy 2,500-character SimplifiedCommon restriction without eagerly
        // allocating a very large 21k-glyph atlas.
        ImFont* f = io.Fonts->AddFontFromFileTTF(p, 20.0f, &fc, nullptr);
        if (f) {
            io.FontDefault = f;
            std::clog << "[imgui_agent_demo] CJK font: " << p
                      << " (20 px, dynamic full CJK glyphs)\n";
            return true;
        }
    }
    return false;
}

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::atomic<bool> g_shutdown{false};

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
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
    req.options.graph_options.tool_execution_observer = [presentation](const ToolExecutionEvent& event) {
        if (presentation) presentation->observe_tool(event);
    };
    req.options.graph_options.skill_event_sink = [](const SkillEvent& event) {
        std::clog << example::skill_event_json(event).dump() << '\n';
    };
    try {
        WorkflowResult wr{};
        auto turn = example::run_conversation_turn(runtime, "imgui_agent_demo",
            state->initial_user_prompt,
            [&] { wr = gx.run_react_cli_sync(executor, req); return wr; });
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

// Some third-party stacks (e.g. SDL-style wrappers) may `#define main`; keep the real entry symbol.
#if defined(main)
#undef main
#endif

int main(int argc, char** argv) {
    agent_imgui_merge_lsan_suppressions();

    CLI::App app("imgui_agent_demo — WP2.U ImGui + same ReAct graph as cli_agent_demo");
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
    app.add_option("-p,--prompt", prompt_arg, "Optional single-turn: run then keep window open");
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
        options.agent_name = "imgui_agent_demo";
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
        example::require_direct_demo_execution(runtime);
    } catch (const std::exception& e) {
        std::cerr << "[imgui_agent_demo] LLM init: " << e.what() << "\n";
        return 1;
    }
    AgentWorkflowDeps deps{runtime.llm, runtime.toolbus, runtime.skills,
                           runtime.memory_compaction_llm};
    AgentConfig cfg = runtime.config;
    const bool skip_cursor_mcp = !runtime.bootstrap.mcp_services;
    const auto& mcp_boot = runtime.bootstrap;

    // Same rationale as web_ui_demo: avoid AgentLoop/OpenAIAdapter std::cout spam in GUI apps.
#if defined(_WIN32)
    (void)_putenv_s("AGENT_TEST_AGENT_LOOP_DEBUG", "0");
#else
    (void)unsetenv("AGENT_TEST_AGENT_LOOP_DEBUG");
#endif

    glfwSetErrorCallback(glfw_error_callback);
    const auto text_locale = example::initialize_imgui_text_input_locale();
    std::clog << "[imgui_agent_demo] text input LC_CTYPE: "
              << (text_locale.before.empty() ? "<unavailable>" : text_locale.before)
              << " -> " << (text_locale.after.empty() ? "<unavailable>" : text_locale.after)
              << (text_locale.unicode_ready ? " (Unicode/XIM ready)" : " (CJK IME unavailable)")
              << '\n';
    if (!text_locale.unicode_ready) {
        std::clog << "[imgui_agent_demo] warning: set LANG or LC_CTYPE to a UTF-8 locale "
                     "before launching (for example C.UTF-8 or zh_CN.UTF-8).\n";
    }
    if (!glfwInit()) {
        std::cerr << "glfwInit failed\n";
        return 1;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1280, 720, "imgui_agent_demo (WP2.U)", nullptr, nullptr);
    if (!window) {
        std::cerr << "glfwCreateWindow failed\n";
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    std::clog << "[imgui_agent_demo] GLFW: " << glfwGetVersionString() << '\n';

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
#if defined(AGENT_HAS_IMPLOT)
    ImPlot::CreateContext();
#endif
#if defined(AGENT_HAS_IMPLOT3D)
    ImPlot3D::CreateContext();
#endif
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    example::apply_scientific_console_theme();
    if (!try_load_imgui_cjk_font(io)) {
        std::clog << "[imgui_agent_demo] CJK font not loaded (Chinese may show as ?). Set "
                     "AGENT_IMGUI_FONT_PATH to a .ttf/.ttc with CJK, or install fonts-noto-cjk / wqy.\n";
    }
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    auto queue = std::make_shared<ThreadSafeQueue<StreamMessage>>();
    auto presentation = std::make_shared<UiPresentationModel>();
    const char* provider_env = std::getenv("AGENT_LLM_PROVIDER");
    presentation->set_runtime_metadata("orbital-analysis",
                                       provider_env && *provider_env ? provider_env : "OpenAI",
                                       cfg.model_config.model_name.empty() ? "provider default" : cfg.model_config.model_name,
                                       skip_cursor_mcp ? "Core tools ready" :
                                       mcp_boot.diagnostics.empty() ? "MCP connected" : "MCP partial");
    for (const auto& diagnostic : mcp_boot.diagnostics)
        presentation->add_system_notice("MCP unavailable: " + diagnostic, true);
    for (const auto& service : mcp_boot.skipped_mcp_services)
        presentation->add_system_notice("MCP skipped by policy: " + service);
    auto imgui_handler = std::make_unique<ImGuiHandler>(queue, "default", presentation);
    ImGuiHandler* imgui_h = imgui_handler.get();

    UIManager ui;
    ui.register_gui_handler(std::move(imgui_handler));

    auto state = std::make_shared<internal::AgentThreadState>();
    tf::Executor executor;

    std::atomic<bool> agent_busy{false};
    std::mutex control_mutex;
    std::shared_ptr<TaskControl> active_control;

    auto run_line = [&](const std::string& line) {
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
        PreprocessOptions popts;
        popts.toolbus = deps.toolbus;
        UserInputPreprocessor prep(popts);
        ProcessedUserInput proc = prep.process(line, ectx);
        if (env_input_strict_enabled() && !proc.tier_a_violations.empty()) {
            for (const auto& v : proc.tier_a_violations) {
                ui.dispatch_error(v);
            }
            return;
        }
        apply_processed_to_agent_state(std::move(proc), ectx, *state);
        (void)run_graph_ui(executor, cfg, deps, state, ui, control, presentation, runtime);
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control == control) active_control.reset();
        }
    };

    if (demo_state) {
        presentation->observe_operations(example::load_phase4_operations({
            operations_db_arg, operations_tenant_arg, operations_run_arg, true}).snapshot);
        presentation->add_system_notice(
            "中文显示验证：GNSS 掩星、电离层建模、数据同化、轨道与气象卫星；扩展字：龘。");
    } else if (!prompt_arg.empty()) {
        agent_busy = true;
        run_line(prompt_arg);
        agent_busy = false;
    }

    char input_buf[4096] = {};

    while (!glfwWindowShouldClose(window) && !g_shutdown.load()) {
        glfwPollEvents();

        std::vector<StreamMessage> drained;
        (void)imgui_h->drain_messages(drained, 256);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const auto runtime_skills = example::skill_ui_status(deps.skills);
        example::ImGuiSkillStatus imgui_skills;
        imgui_skills.enabled = runtime_skills.enabled;
        imgui_skills.count = runtime_skills.count;
        imgui_skills.generation = runtime_skills.generation;
        imgui_skills.diagnostics = runtime_skills.diagnostics;
        imgui_skills.errors = runtime_skills.errors;
        imgui_skills.root = runtime_skills.root;
        imgui_skills.active = runtime_skills.active;
        auto action = example::render_scientific_console(
            presentation->snapshot(), imgui_skills, input_buf, sizeof(input_buf), agent_busy.load());
        if (action.send && !agent_busy.exchange(true)) {
            std::thread([&, line = std::move(action.prompt)]() {
                run_line(line);
                agent_busy = false;
            }).detach();
        }
        if (action.cancel) {
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control) active_control->request_cancel();
        }
        if (action.quit) glfwSetWindowShouldClose(window, 1);

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    agent_framework::example::clear_imgui_artifact_textures();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
#if defined(AGENT_HAS_IMPLOT3D)
    ImPlot3D::DestroyContext();
#endif
#if defined(AGENT_HAS_IMPLOT)
    ImPlot::DestroyContext();
#endif
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

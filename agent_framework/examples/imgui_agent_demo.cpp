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

#include <agent/agent/execution_context.hpp>
#include <agent/graph_executor/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/skills/skill_services.hpp>
#include <agent/ui/thread_safe_queue.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>
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
        fc.OversampleV = 2;
        ImFont* f = io.Fonts->AddFontFromFileTTF(
            p, 20.0f, &fc, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (f) {
            io.FontDefault = f;
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

std::shared_ptr<SkillServices> resolve_skills_services(bool dbg) {
    example::BootstrapOptions options;
    options.use_cursor_skill_roots = true;
    options.verbose = dbg;
    auto services = example::discover_skill_services(options);
    if(dbg && services && services->registry)
        std::clog << "[bootstrap] indexed_skills=" << services->registry->entries().size() << '\n';
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
                 UIManager& ui) {
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
    req.options.graph_options.skill_event_sink = [](const SkillEvent& event) {
        std::clog << example::skill_event_json(event).dump() << '\n';
    };
    try {
        WorkflowResult wr = gx.run_react_cli_sync(executor, req);
        if (!wr.success) {
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
    int max_iterations = -1;
    bool verbose = false;
    bool no_cursor_mcp = false;
    app.add_option("-p,--prompt", prompt_arg, "Optional single-turn: run then keep window open");
    app.add_option("--provider", provider_arg, "Override AGENT_LLM_PROVIDER");
    app.add_option("--max-iterations", max_iterations, "Override max_iterations");
    app.add_option("--cursor-mcp-json", cursor_mcp_json_arg,
                   "Cursor mcp.json (else AGENT_TEST_CURSOR_MCP_JSON, else ToolBus default)");
    app.add_flag("--no-cursor-mcp", no_cursor_mcp,
                 "Skip MCP (or AGENT_TEST_SKIP_CURSOR_MCP / AGENT_CLI_SKIP_CURSOR_MCP)");
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

    apply_live_llm_env_defaults();

    std::shared_ptr<LLMClient> llm;
    try {
        llm = std::make_shared<LLMClient>(LLMClient::from_env());
    } catch (const std::exception& e) {
        std::cerr << "[imgui_agent_demo] LLM init: " << e.what() << "\n";
        return 1;
    }
    llm->set_prompt_renderer(std::make_shared<PromptRenderer>());

    const bool skip_cursor_mcp = no_cursor_mcp || env_truthy("AGENT_TEST_SKIP_CURSOR_MCP") ||
                                 env_truthy("AGENT_CLI_SKIP_CURSOR_MCP");
    const bool mcp_dbg = verbose || env_truthy("AGENT_TEST_AGENT_LOOP_DEBUG");

    auto bus = std::make_shared<ToolBus>();
    register_demo_tools(*bus);

    std::size_t mcp_services = 0;
    if (!skip_cursor_mcp) {
        std::clog << "[imgui_agent_demo] loading Cursor MCP config (--no-cursor-mcp to skip)...\n"
                      "  (AGENT_MCP_REQUEST_TIMEOUT_MS per request; transport default 60000 ms)\n"
                  << std::flush;
        const std::string mcp_cfg = resolve_cursor_mcp_config_path(cursor_mcp_json_arg);
        import_cursor_mcp_tools(*bus, mcp_cfg, mcp_dbg, &mcp_services);
        if (!mcp_dbg && mcp_services > 0) {
            std::clog << "[imgui_agent_demo] cursor_mcp: " << mcp_services << " service(s) registered\n";
        }
    } else if (mcp_dbg) {
        std::clog << "cursor_mcp: skipped (--no-cursor-mcp or skip env)\n";
    }

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;
    deps.skills = resolve_skills_services(mcp_dbg);

    AgentConfig cfg;
    cfg.name = "imgui_agent_demo";
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

    // Same rationale as web_ui_demo: avoid AgentLoop/OpenAIAdapter std::cout spam in GUI apps.
#if defined(_WIN32)
    (void)_putenv_s("AGENT_TEST_AGENT_LOOP_DEBUG", "0");
#else
    (void)unsetenv("AGENT_TEST_AGENT_LOOP_DEBUG");
#endif

    glfwSetErrorCallback(glfw_error_callback);
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
    if (!try_load_imgui_cjk_font(io)) {
        std::clog << "[imgui_agent_demo] CJK font not loaded (Chinese may show as ?). Set "
                     "AGENT_IMGUI_FONT_PATH to a .ttf/.ttc with CJK, or install fonts-noto-cjk / wqy.\n";
    }
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    auto queue = std::make_shared<ThreadSafeQueue<StreamMessage>>();
    auto imgui_handler = std::make_unique<ImGuiHandler>(queue, "default");
    ImGuiHandler* imgui_h = imgui_handler.get();

    UIManager ui;
    ui.register_gui_handler(std::move(imgui_handler));

    auto state = std::make_shared<internal::AgentThreadState>();
    tf::Executor executor;

    std::string stream_text;
    std::string aux_text;
    std::atomic<bool> agent_busy{false};

    auto run_line = [&](const std::string& line) {
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
        (void)run_graph_ui(executor, cfg, deps, state, ui);
    };

    if (!prompt_arg.empty()) {
        run_line(prompt_arg);
    }

    char input_buf[4096] = {};

    while (!glfwWindowShouldClose(window) && !g_shutdown.load()) {
        glfwPollEvents();

        std::vector<StreamMessage> drained;
        const std::size_t n = imgui_h->drain_messages(drained, 256);
        for (std::size_t i = 0; i < n; ++i) {
            const StreamMessage& m = drained[i];
            if (m.message_type == "token") {
                stream_text += m.content;
            } else if (m.message_type == "final") {
                stream_text += std::string("\n") + m.content + "\n";
            } else if (m.message_type == "error") {
                stream_text += std::string("\n[error] ") + m.content + "\n";
            } else if (m.message_type.size() > 4 && m.message_type.compare(0, 4, "aux:") == 0) {
                aux_text += m.content;
                aux_text += "\n";
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(720.0f, 400.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Output");
        ImGui::BeginChild("scroll", ImVec2(0, -120), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(stream_text.c_str());
        ImGui::EndChild();
        if (!aux_text.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(aux_text.c_str());
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(24.0f, 440.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(720.0f, 220.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Input");
        ImGui::InputTextMultiline("##user", input_buf, sizeof(input_buf), ImVec2(-1, 80));
        if (ImGui::Button("Send") && !agent_busy.load()) {
            std::string line(input_buf);
            if (!line.empty()) {
                agent_busy = true;
                std::thread([&, line]() {
                    run_line(line);
                    agent_busy = false;
                }).detach();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit")) {
            glfwSetWindowShouldClose(window, 1);
        }
        ImGui::Text("agent_busy=%s", agent_busy.load() ? "yes" : "no");
        ImGui::End();

#if defined(AGENT_HAS_IMPLOT) || defined(AGENT_HAS_IMPLOT3D)
        ImGui::SetNextWindowPos(ImVec2(760.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(500.0f, 520.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Plots (submodule demo)");
#if defined(AGENT_HAS_IMPLOT)
        if (ImPlot::BeginPlot("2D")) {
            static float xs[64];
            static float ys[64];
            static bool inited_2d = false;
            if (!inited_2d) {
                for (int i = 0; i < 64; ++i) {
                    xs[i] = static_cast<float>(i) * 0.1f;
                    ys[i] = std::sin(xs[i]);
                }
                inited_2d = true;
            }
            ImPlot::PlotLine("sin", xs, ys, 64);
            ImPlot::EndPlot();
        }
#endif
#if defined(AGENT_HAS_IMPLOT3D)
        if (ImPlot3D::BeginPlot("3D")) {
            static float xa[64];
            static float ya[64];
            static float za[64];
            static bool inited_3d = false;
            if (!inited_3d) {
                for (int i = 0; i < 64; ++i) {
                    const float t = static_cast<float>(i) / 63.0F * 6.2831855F * 2.0F;
                    xa[i] = std::cos(t);
                    ya[i] = std::sin(t);
                    za[i] = t * 0.08F;
                }
                inited_3d = true;
            }
            ImPlot3D::PlotLine("helix", xa, ya, za, 64);
            ImPlot3D::EndPlot();
        }
#endif
        ImGui::End();
#endif

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

/**
 * @file imgui_agent_demo.cpp
 * @brief WP2.U Track I：GLFW + OpenGL3 + Dear ImGui + 同 cli图路径（UIManager 队列）
 *
 * 主线程：GLFW/ImGui + drain StreamMessage；工作线程：run_react_cli_sync。
 * 构建：-DAGENT_BUILD_IMGUI=ON（FetchContent GLFW+ImGui）
 */

#include "CLI11.hpp"

#include <agent/execution_context.hpp>
#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/skill_services.hpp>
#include <agent/thread_safe_queue.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>
#include <agent/ui_manager.hpp>
#include <agent/user_input_preprocessor.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif

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
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
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
    set_env_if_absent("AGENT_MCP_REQUEST_TIMEOUT_MS", "1500");
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

int main(int argc, char** argv) {
    CLI::App app("imgui_agent_demo — WP2.U ImGui + same ReAct graph as cli_agent_demo");
    std::string prompt_arg;
    std::string provider_arg;
    int max_iterations = -1;
    bool verbose = false;
    app.add_option("-p,--prompt", prompt_arg, "Optional single-turn: run then keep window open");
    app.add_option("--provider", provider_arg, "Override AGENT_LLM_PROVIDER");
    app.add_option("--max-iterations", max_iterations, "Override max_iterations");
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

    auto bus = std::make_shared<ToolBus>();
    register_demo_tools(*bus);

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;
    deps.skills = SkillServices::from_env();

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
    if (const char* m = std::getenv("AGENT_LLM_MODEL")) {
        cfg.model_config.model_name = m;
    }
    if (max_iterations > 0) {
        cfg.max_iterations = max_iterations;
    }

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
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();
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

        ImGui::Begin("Output");
        ImGui::BeginChild("scroll", ImVec2(0, -120), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(stream_text.c_str());
        ImGui::EndChild();
        if (!aux_text.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(aux_text.c_str());
        }
        ImGui::End();

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
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

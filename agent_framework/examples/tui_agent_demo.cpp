/**
 * @file tui_agent_demo.cpp
 * @brief WP2.U Track T：ncurses 全屏 + TuiHandler + 同 cli 图路径
 *
 * 构建：-DAGENT_BUILD_TUI=ON（需系统 ncurses / ncursesw 开发包）
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
#include <agent/skills/skill_control.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/ui/tui_handler.hpp>
#include <agent/core/types.hpp>
#include <agent/ui/ui_manager.hpp>
#include <agent/ui/presentation_model.hpp>
#include <agent/agent/user_input_preprocessor.hpp>

#include <clocale>
#include <cwchar>
#include <cwctype>

#if defined(__has_include)
#if __has_include(<ncursesw/ncurses.h>)
#include <ncursesw/ncurses.h>
#else
#include <curses.h>
#endif
#else
#include <curses.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
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
                 UIManager& ui,
                 const std::shared_ptr<TaskControl>& control,
                 const std::shared_ptr<UiPresentationModel>& presentation) {
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
    req.options.graph_options.task_control = control;
    req.options.graph_options.tool_execution_observer = [presentation](const ToolExecutionEvent& event) {
        if (presentation) presentation->observe_tool(event);
    };
    req.options.graph_options.skill_event_sink = [](const SkillEvent& event) {
        std::clog << example::skill_event_json(event).dump() << '\n';
    };
    try {
        WorkflowResult wr = gx.run_react_cli_sync(executor, req);
        if (!wr.success) {
            if (control && control->is_cancel_requested()) {
                if (presentation) presentation->cancel();
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

/**
 * @brief UTF-8 → 显示行（wcwidth 计宽），供 CJK 与 ncursesw 使用
 */
void utf8_wrap_to_wlines(const std::string& text, int max_cols, std::vector<std::wstring>* out_lines) {
    out_lines->clear();
    if (max_cols < 1) {
        max_cols = 1;
    }
    std::wstring cur;
    int col = 0;
    std::mbstate_t st{};
    std::memset(&st, 0, sizeof(st));
    for (std::size_t i = 0; i < text.size();) {
        wchar_t wc = 0;
        const std::size_t n = std::mbrtowc(&wc, text.data() + i, text.size() - i, &st);
        if (n == static_cast<std::size_t>(-2)) {
            break;
        }
        if (n == static_cast<std::size_t>(-1) || n == 0) {
            ++i;
            std::memset(&st, 0, sizeof(st));
            continue;
        }
        if (wc == L'\n') {
            out_lines->push_back(std::move(cur));
            cur.clear();
            col = 0;
            i += n;
            continue;
        }
        int w = wcwidth(wc);
        if (w < 0) {
            w = 0;
        }
        if (col + w > max_cols && !cur.empty()) {
            out_lines->push_back(std::move(cur));
            cur.clear();
            col = 0;
        }
        cur.push_back(wc);
        col += w;
        i += n;
    }
    if (!cur.empty()) {
        out_lines->push_back(std::move(cur));
    }
}

std::string wstring_to_utf8(const std::wstring& w) {
    std::string line;
    std::mbstate_t st{};
    std::memset(&st, 0, sizeof(st));
    char buf[16];
    for (wchar_t wc : w) {
        const std::size_t n = std::wcrtomb(buf, wc, &st);
        if (n != static_cast<std::size_t>(-1) && n > 0) {
            line.append(buf, n);
        }
    }
    return line;
}

std::string conversation_text(const UiPresentationSnapshot& snap) {
    std::string out;
    for (const auto& turn : snap.turns) {
        if (!out.empty()) out += "\n";
        out += turn.role == UiTurnRole::User ? "YOU" : turn.role == UiTurnRole::Assistant ? "AGENT" : "SYSTEM";
        if (turn.streaming) out += "  [streaming]";
        out += "\n" + turn.content + "\n";
    }
    if (out.empty()) out = "READY\n\nStart a verifiable research task. Tool activity will remain visible beside the answer.";
    return out;
}

std::string activity_text(const UiPresentationSnapshot& snap) {
    std::string out;
    for (const auto& tool : snap.tools) {
        out += std::string(UiPresentationModel::state_name(tool.state)) + "  " + tool.tool_name;
        if (tool.duration_ms > 0) out += "  " + std::to_string(tool.duration_ms) + " ms";
        out += "\n";
        if (!tool.arguments.empty()) out += "  args: " + tool.arguments.dump() + "\n";
        if (!tool.result.empty()) out += "  result: " + tool.result.dump() + "\n";
        out += "\n";
    }
    return out.empty() ? "No tool calls yet.\n\nArguments, results, and duration appear here." : out;
}

void draw_panel(WINDOW* w, const char* title, const std::string& text) {
    werase(w);
    box(w, 0, 0);
    wattron(w, A_BOLD | COLOR_PAIR(1));
    mvwprintw(w, 0, 2, " %s ", title);
    wattroff(w, A_BOLD | COLOR_PAIR(1));
    int rows = 0, cols = 0;
    getmaxyx(w, rows, cols);
    std::vector<std::wstring> lines;
    utf8_wrap_to_wlines(text, std::max(1, cols - 2), &lines);
    const int room = std::max(0, rows - 2);
    const int skip = std::max(0, static_cast<int>(lines.size()) - room);
    for (int r = 0; r < room && skip + r < static_cast<int>(lines.size()); ++r) {
        mvwaddnwstr(w, r + 1, 1, lines[static_cast<std::size_t>(skip + r)].c_str(), -1);
    }
    wrefresh(w);
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app("tui_agent_demo — WP2.U ncursesw + TuiHandler + MCP/Skills");
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
    app.add_option("-p,--prompt", prompt_arg, "Optional single-turn then interactive");
    app.add_option("--provider", provider_arg, "Override AGENT_LLM_PROVIDER");
    app.add_option("--max-iterations", max_iterations, "Override max_iterations");
    app.add_option("--cursor-mcp-json", cursor_mcp_json_arg,
                   "Cursor mcp.json (else AGENT_TEST_CURSOR_MCP_JSON, else ToolBus default)");
    app.add_option("--skills-root", skills_root_arg, "Installed read-only Skill root");
    app.add_option("--skill-authoring-root", skill_authoring_root_arg,
                   "Writable root used by /skills create");
    app.add_flag("--no-skills", no_skills, "Disable Skill discovery and management");
    app.add_flag("--no-cursor-mcp", no_cursor_mcp,
                 "Skip MCP (or AGENT_TEST_SKIP_CURSOR_MCP / AGENT_CLI_SKIP_CURSOR_MCP)");
    app.add_flag("--demo-state", demo_state,
                 "Load deterministic Scientific Console content without invoking the LLM");
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
    if (!skills_root_arg.empty()) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_SKILLS_DIR", skills_root_arg.c_str());
#else
        (void)::setenv("AGENT_SKILLS_DIR", skills_root_arg.c_str(), 1);
#endif
    }
    if (!skill_authoring_root_arg.empty()) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_SKILL_AUTHORING_DIR", skill_authoring_root_arg.c_str());
#else
        (void)::setenv("AGENT_SKILL_AUTHORING_DIR", skill_authoring_root_arg.c_str(), 1);
#endif
    }
    if (no_skills) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_SKILLS_DISABLED", "1");
#else
        (void)::setenv("AGENT_SKILLS_DISABLED", "1", 1);
#endif
    }

    apply_live_llm_env_defaults();

    std::shared_ptr<LLMClient> llm;
    try {
        llm = std::make_shared<LLMClient>(LLMClient::from_env());
    } catch (const std::exception& e) {
        std::cerr << "[tui_agent_demo] LLM init: " << e.what() << "\n";
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
        std::clog << "[tui_agent_demo] loading Cursor MCP config (--no-cursor-mcp to skip)...\n"
 "  (AGENT_MCP_REQUEST_TIMEOUT_MS per request; transport default 60000 ms)\n"
                  << std::flush;
        const std::string mcp_cfg = resolve_cursor_mcp_config_path(cursor_mcp_json_arg);
        mcp_boot = import_cursor_mcp_tools(*bus, mcp_cfg, mcp_dbg);
        mcp_services = mcp_boot.mcp_services;
        if (!mcp_dbg && mcp_services > 0) {
            std::clog << "[tui_agent_demo] cursor_mcp: " << mcp_services << " service(s) registered\n";
        }
    } else if (mcp_dbg) {
        std::clog << "cursor_mcp: skipped (--no-cursor-mcp or skip env)\n";
    }

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;
    deps.skills = resolve_skills_services(mcp_dbg);
    if (deps.skills && deps.skills->manager) deps.skills->manager->attach_toolbus(bus);

    AgentConfig cfg;
    cfg.name = "tui_agent_demo";
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

    std::clog << "[tui_agent_demo] starting fullscreen TUI (UTF-8 / wide ncurses)...\n" << std::flush;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
        init_pair(2, COLOR_GREEN, -1);
        init_pair(3, COLOR_RED, -1);
    }

    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 8 || cols < 40) {
        endwin();
        std::cerr << "[tui_agent_demo] terminal too small\n";
        return 1;
    }

    const int header_h = 2;
    const int input_h = 5;
    const int status_h = 1;
    const int body_h = rows - header_h - input_h - status_h;
    const bool wide_layout = cols >= 110;
    const int left_w = wide_layout ? 22 : 0;
    const int right_w = wide_layout ? 34 : 0;
    WINDOW* left_win = wide_layout ? newwin(body_h, left_w, header_h, 0) : nullptr;
    WINDOW* out_win = newwin(body_h, cols - left_w - right_w, header_h, left_w);
    WINDOW* activity_win = wide_layout ? newwin(body_h, right_w, header_h, cols - right_w) : nullptr;
    WINDOW* in_win = newwin(input_h, cols, rows - input_h - status_h, 0);
    wtimeout(in_win, 50);

    auto presentation = std::make_shared<UiPresentationModel>();
    const char* provider_env = std::getenv("AGENT_LLM_PROVIDER");
    presentation->set_runtime_metadata("orbital-analysis",
                                       provider_env && *provider_env ? provider_env : "OpenAI",
                                       cfg.model_config.model_name.empty() ? "provider default" : cfg.model_config.model_name,
                                       skip_cursor_mcp ? "Core tools ready" :
                                       mcp_boot.diagnostics.empty() ? "MCP connected" : "MCP partial");
    if (demo_state) presentation->load_demo_state();
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
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control == control) active_control.reset();
            return;
        }
        if (deps.skills && deps.skills->manager) {
            const auto active = deps.skills->manager->active_skill_id();
            if (!active.empty()) state->active_skill_id = active;
        }
        apply_processed_to_agent_state(std::move(proc), ectx, *state);
        (void)run_graph_ui(executor, cfg, deps, state, ui, control, presentation);
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control == control) active_control.reset();
        }
    };

    if (!prompt_arg.empty()) {
        agent_busy = true;
        std::thread([&, prompt_arg]() {
            run_line(prompt_arg);
            agent_busy = false;
        }).detach();
    }

    std::wstring input_wline;
    bool running = true;
    while (running && !g_shutdown.load()) {
        const UiPresentationSnapshot snap = tui_h->presentation_snapshot();
        attron(A_BOLD | COLOR_PAIR(1));
        mvprintw(0, 1, "SCIENTIFIC CONSOLE");
        attroff(A_BOLD | COLOR_PAIR(1));
        mvprintw(0, 23, "Session: %s  Model: %s  Provider: %s", snap.session_id.c_str(),
                 snap.model.c_str(), snap.provider.c_str());
        mvprintw(1, 1, "State: %s  Connection: %s", UiPresentationModel::state_name(snap.run_state),
                 snap.connection_label.c_str());
        clrtoeol(); refresh();
        std::string capabilities =
            "SESSION\n  orbital-analysis\n\nTOOLS\n  FS\n  WEB\n  EXPR\n  DRAW\n  SKILLS\n  MCP";
        std::string skill_line = "Skills disabled  (configure --skills-root to enable)";
        if (deps.skills && deps.skills->manager) {
            const auto skill_status = deps.skills->manager->status();
            const std::string active = skill_status["activeSkill"].is_string()
                ? skill_status["activeSkill"].get<std::string>() : "-";
            capabilities += "\n\nSKILLS\n  indexed " +
                            std::to_string(skill_status.value("skills", std::size_t{0})) +
                            "\n  active " + active;
            skill_line = "Skills gen=" +
                         std::to_string(skill_status.value("generation", 0ULL)) +
                         " count=" +
                         std::to_string(skill_status.value("skills", std::size_t{0})) +
                         " active=" + active + "  /skills status";
        }
        if (left_win) draw_panel(left_win, "CAPABILITIES", capabilities);
        draw_panel(out_win, "CONVERSATION", conversation_text(snap));
        if (activity_win) draw_panel(activity_win, "TOOL ACTIVITY", activity_text(snap));

        werase(in_win); box(in_win, 0, 0);
        wattron(in_win, A_BOLD | COLOR_PAIR(1)); mvwprintw(in_win, 0, 2, " COMPOSER "); wattroff(in_win, A_BOLD | COLOR_PAIR(1));
        mvwaddwstr(in_win, 1, 2, L"> ");
        mvwaddnwstr(in_win, 1, 4, input_wline.c_str(), -1);
        mvwprintw(in_win, 2, 2, "%.*s", std::max(0, cols - 4), skill_line.c_str());
        mvwprintw(in_win, 3, 2, "Enter send  Esc stop  Ctrl+C quit  %s",
                  agent_busy.load() ? "RUNNING" : "READY");
        wrefresh(in_win);
        move(rows - 1, 1);
        clrtoeol();
        printw("%zu turns  %zu tools  %s", snap.turns.size(), snap.tools.size(),
               wide_layout ? "three-panel" : "compact");
        refresh();

        wint_t ch = 0;
        const int ret = wget_wch(in_win, &ch);
        if (ret == ERR) {
            continue;
        }
        if (ret == KEY_CODE_YES) {
            if (ch == 3) { // Ctrl+C
                running = false;
                break;
            }
            if (ch == KEY_RESIZE) continue;
            if (ch == KEY_BACKSPACE || ch == KEY_DC || ch == 127) {
                if (!input_wline.empty()) {
                    input_wline.pop_back();
                }
                continue;
            }
            if (ch == KEY_ENTER || ch == L'\n' || ch == L'\r' || ch == 10 || ch == 13) {
                if (!input_wline.empty() && !agent_busy.load()) {
                    std::string line = wstring_to_utf8(input_wline);
                    input_wline.clear();
                    agent_busy = true;
                    std::thread([&, line]() {
                        run_line(line);
                        agent_busy = false;
                    }).detach();
                }
                continue;
            }
            continue;
        }
        if (ch == 27) {
            std::lock_guard<std::mutex> lock(control_mutex);
            if (active_control) active_control->request_cancel();
            continue;
        }
        if (ch == L'\n' || ch == L'\r' || ch == 10 || ch == 13) {
            if (!input_wline.empty() && !agent_busy.load()) {
                std::string line = wstring_to_utf8(input_wline);
                input_wline.clear();
                agent_busy = true;
                std::thread([&, line]() {
                    run_line(line);
                    agent_busy = false;
                }).detach();
            }
            continue;
        }
        if (ch >= 32) {
            input_wline.push_back(static_cast<wchar_t>(ch));
        }
    }

    g_shutdown = true;
    delwin(out_win);
    if (left_win) delwin(left_win);
    if (activity_win) delwin(activity_win);
    delwin(in_win);
    endwin();
    return 0;
}

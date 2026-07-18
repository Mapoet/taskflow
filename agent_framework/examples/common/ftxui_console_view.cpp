#include "ftxui_console_view.hpp"

#include <agent/context_budget/context_budget.hpp>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace agent_framework::example {
namespace {

using namespace ftxui;

const Color kAccent = Color::Cyan;
const Color kMuted = Color::GrayDark;

std::string truncate(std::string value, std::size_t max_bytes = 480) {
    if (value.size() <= max_bytes) return value;
    return utf8_safe_truncate(value, max_bytes) + " ...";
}

Color state_color(UiRunState state) {
    switch (state) {
        case UiRunState::Completed: return Color::Green;
        case UiRunState::Running: return Color::Yellow;
        case UiRunState::Failed: return Color::Red;
        case UiRunState::Cancelled: return Color::Magenta;
        case UiRunState::Idle: return Color::GrayLight;
    }
    return Color::GrayLight;
}

std::string role_name(UiTurnRole role) {
    switch (role) {
        case UiTurnRole::User: return "YOU";
        case UiTurnRole::Assistant: return "AGENT";
        case UiTurnRole::System: return "SYSTEM";
    }
    return "SYSTEM";
}

Elements conversation_rows(const UiPresentationSnapshot& snapshot) {
    Elements rows;
    if (snapshot.turns.empty()) {
        rows.push_back(paragraph("Ready. Start a verifiable research task; tool activity remains visible beside the answer.") | color(kMuted));
        return rows;
    }
    for (const auto& turn : snapshot.turns) {
        Color role_color = turn.error ? Color::Red : turn.role == UiTurnRole::User ? Color::BlueLight : kAccent;
        std::string label = role_name(turn.role);
        if (turn.streaming) label += "  streaming";
        rows.push_back(text(label) | bold | color(role_color));
        rows.push_back(paragraph(turn.content.empty() ? " " : turn.content));
        rows.push_back(separatorEmpty());
    }
    return rows;
}

Elements activity_rows(const UiPresentationSnapshot& snapshot) {
    Elements rows;
    if (snapshot.tools.empty()) {
        rows.push_back(paragraph("No tool calls yet. Arguments, results, duration, and failures appear here.") | color(kMuted));
        return rows;
    }
    for (const auto& tool : snapshot.tools) {
        std::string title = std::string(UiPresentationModel::state_name(tool.state)) + "  " + tool.tool_name;
        if (tool.duration_ms > 0) title += "  " + std::to_string(tool.duration_ms) + " ms";
        rows.push_back(text(title) | bold | color(state_color(tool.state)));
        if (!tool.arguments.empty()) rows.push_back(paragraph("args  " + truncate(tool.arguments.dump())) | color(Color::GrayLight));
        if (!tool.result.empty()) rows.push_back(paragraph("result  " + truncate(tool.result.dump())));
        rows.push_back(separatorEmpty());
    }
    return rows;
}

Element capabilities(const UiPresentationSnapshot& snapshot, const FtxuiSkillStatus& skills) {
    Elements tools;
    for (const char* item : {"FS", "WEB", "EXPR", "DRAW", "SKILLS", "MCP"}) {
        tools.push_back(hbox({text("  "), text("● ") | color(Color::Green), text(item)}));
    }
    Elements rows{
        text("SESSION") | bold | color(kAccent),
        paragraph("  " + snapshot.session_id),
        separatorEmpty(),
        text("CAPABILITIES") | bold | color(kAccent),
    };
    rows.insert(rows.end(), tools.begin(), tools.end());
    rows.push_back(separatorEmpty());
    rows.push_back(text("SKILLS") | bold | color(kAccent));
    if (skills.enabled) {
        rows.push_back(text("  indexed " + std::to_string(skills.count)));
        rows.push_back(text("  generation " + std::to_string(skills.generation)));
        rows.push_back(text("  diagnostics " + std::to_string(skills.diagnostics) +
                            " / errors " + std::to_string(skills.errors)) |
                       color(skills.errors == 0 ? kMuted : Color::RedLight));
        if (!skills.root.empty()) rows.push_back(paragraph("  root " + skills.root));
        rows.push_back(paragraph("  active " + skills.active));
    } else {
        rows.push_back(text("  disabled") | color(kMuted));
    }
    return vbox(std::move(rows));
}

Element framed_panel(std::string title, Element content, float scroll, bool selected) {
    auto title_element = text(" " + std::move(title) + " ") | bold | color(selected ? Color::Yellow : kAccent);
    return window(title_element,
                  std::move(content) | focusPositionRelative(0.0F, scroll) |
                      vscroll_indicator | frame | flex) |
           (selected ? borderHeavy : border);
}

struct RenderState {
    UiPresentationSnapshot snapshot;
    FtxuiSkillStatus skills;
    bool busy{false};
    int width{120};
    int height{30};
    int active_tab{1};
    float conversation_scroll{1.0F};
    float activity_scroll{1.0F};
    Element composer;
};

Element render_document(RenderState state) {
    const auto run_color = state.busy ? Color::Yellow : state_color(state.snapshot.run_state);
    auto header = hbox({
        text(" SCIENTIFIC CONSOLE ") | bold | bgcolor(Color::Blue) | color(Color::White),
        text("  " + state.snapshot.session_id) | bold,
        filler(),
        text(state.snapshot.model + "  ") | color(kMuted),
        text(UiPresentationModel::state_name(state.snapshot.run_state)) | bold | color(run_color),
        text("  " + state.snapshot.connection_label + " ") | color(kMuted),
    });

    auto conversation = framed_panel("CONVERSATION", vbox(conversation_rows(state.snapshot)),
                                     state.conversation_scroll, state.active_tab == 1);
    auto activity = framed_panel("TOOL ACTIVITY", vbox(activity_rows(state.snapshot)),
                                 state.activity_scroll, state.active_tab == 2);
    auto caps = framed_panel("CAPABILITIES", capabilities(state.snapshot, state.skills),
                             0.0F, state.active_tab == 0);

    Element body;
    std::string layout;
    if (state.width >= 120) {
        body = hbox({caps | size(WIDTH, EQUAL, 24), conversation | flex,
                     activity | size(WIDTH, EQUAL, 40)});
        layout = "wide · three panels";
    } else if (state.width >= 100) {
        body = hbox({caps | size(WIDTH, EQUAL, 22),
                     (state.active_tab == 2 ? activity : conversation) | flex});
        layout = "medium · 1/2 conversation · 2/2 activity";
    } else {
        body = state.active_tab == 0 ? caps : state.active_tab == 2 ? activity : conversation;
        layout = "compact · [1] capabilities  [2] conversation  [3] activity";
    }

    std::string skill_line = state.skills.enabled
        ? "Skills gen=" + std::to_string(state.skills.generation) +
              " count=" + std::to_string(state.skills.count) +
              " errors=" + std::to_string(state.skills.errors) + " active=" + state.skills.active
        : "Skills disabled";
    auto composer = window(text(" COMPOSER ") | bold | color(kAccent),
                           vbox({
                               hbox({text("> ") | bold | color(kAccent), std::move(state.composer) | flex}),
                               hbox({text(skill_line) | color(kMuted), filler(),
                                     text(state.busy ? "RUNNING" : "READY") | bold | color(run_color)}),
                               text("Enter send · Esc cancel · Ctrl+C quit · 1/2/3 panes · PgUp/PgDn scroll") | color(kMuted),
                           })) |
                    size(HEIGHT, EQUAL, 5);
    auto footer = hbox({
        text(" " + std::to_string(state.snapshot.turns.size()) + " turns"),
        text("  " + std::to_string(state.snapshot.tools.size()) + " tools"),
        filler(),
        text(layout + " ") | color(kMuted),
    });
    return vbox({std::move(header), std::move(body) | flex, std::move(composer), std::move(footer)}) |
           size(HEIGHT, EQUAL, state.height);
}

}  // namespace

struct FtxuiConsoleView::Impl {
    Impl(SnapshotProvider snapshot_provider,
         SkillStatusProvider skill_status_provider,
         FtxuiConsoleCallbacks callbacks)
        : snapshot_provider(std::move(snapshot_provider)),
          skill_status_provider(std::move(skill_status_provider)),
          callbacks(std::move(callbacks)) {}

    SnapshotProvider snapshot_provider;
    SkillStatusProvider skill_status_provider;
    FtxuiConsoleCallbacks callbacks;
    std::atomic<bool> busy{false};
    std::mutex app_mutex;
    App* app{nullptr};
    std::string composer;
    int active_tab{1};
    float conversation_scroll{1.0F};
    float activity_scroll{1.0F};

    void scroll(float delta) {
        float& value = active_tab == 2 ? activity_scroll : conversation_scroll;
        value = std::clamp(value + delta, 0.0F, 1.0F);
    }

    Element render(Element input) {
        const auto dimensions = Terminal::Size();
        return render_document(RenderState{
            snapshot_provider ? snapshot_provider() : UiPresentationSnapshot{},
            skill_status_provider ? skill_status_provider() : FtxuiSkillStatus{},
            busy.load(),
            dimensions.dimx,
            dimensions.dimy,
            active_tab,
            conversation_scroll,
            activity_scroll,
            std::move(input),
        });
    }
};

FtxuiConsoleView::FtxuiConsoleView(SnapshotProvider snapshot_provider,
                                   SkillStatusProvider skill_status_provider,
                                   FtxuiConsoleCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(snapshot_provider),
                                  std::move(skill_status_provider),
                                  std::move(callbacks))) {}

FtxuiConsoleView::~FtxuiConsoleView() = default;

int FtxuiConsoleView::run() {
    auto app = App::Fullscreen();
    app.ForceHandleCtrlC(false);
    {
        std::lock_guard<std::mutex> lock(impl_->app_mutex);
        impl_->app = &app;
    }

    InputOption input_options;
    input_options.content = &impl_->composer;
    input_options.placeholder = "Ask a question or use /skills ...";
    input_options.multiline = false;
    input_options.on_enter = [this] {
        if (impl_->busy.load() || impl_->composer.empty()) return;
        std::string line = std::move(impl_->composer);
        impl_->composer.clear();
        if (impl_->callbacks.on_submit) impl_->callbacks.on_submit(std::move(line));
    };
    auto input = Input(input_options);
    auto root = Renderer(input, [this, input] { return impl_->render(input->Render()); });
    root |= CatchEvent([this, &app](ftxui::Event event) {
        if (event == ftxui::Event::CtrlC) {
            if (impl_->callbacks.on_quit) impl_->callbacks.on_quit();
            app.Exit();
            return true;
        }
        if (event == ftxui::Event::Escape) {
            if (impl_->callbacks.on_cancel) impl_->callbacks.on_cancel();
            return true;
        }
        if (event == ftxui::Event::Character('1') && impl_->composer.empty()) { impl_->active_tab = 0; return true; }
        if (event == ftxui::Event::Character('2') && impl_->composer.empty()) { impl_->active_tab = 1; return true; }
        if (event == ftxui::Event::Character('3') && impl_->composer.empty()) { impl_->active_tab = 2; return true; }
        if (event == ftxui::Event::PageUp) { impl_->scroll(-0.15F); return true; }
        if (event == ftxui::Event::PageDown) { impl_->scroll(0.15F); return true; }
        if (event.is_mouse()) {
            const auto& mouse = event.mouse();
            if (mouse.button == Mouse::WheelUp) { impl_->scroll(-0.10F); return true; }
            if (mouse.button == Mouse::WheelDown) { impl_->scroll(0.10F); return true; }
        }
        return false;
    });

    std::atomic<bool> refresh_running{true};
    std::thread refresh_thread([this, &refresh_running] {
        while (refresh_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            request_refresh();
        }
    });
    app.Loop(root);
    refresh_running.store(false);
    if (refresh_thread.joinable()) refresh_thread.join();
    {
        std::lock_guard<std::mutex> lock(impl_->app_mutex);
        impl_->app = nullptr;
    }
    return 0;
}

void FtxuiConsoleView::set_busy(bool busy) noexcept {
    impl_->busy.store(busy);
    request_refresh();
}

void FtxuiConsoleView::request_refresh() {
    std::lock_guard<std::mutex> lock(impl_->app_mutex);
    if (impl_->app) impl_->app->PostEvent(ftxui::Event::Custom);
}

std::string FtxuiConsoleView::render_for_test(const UiPresentationSnapshot& snapshot,
                                              const FtxuiSkillStatus& skills,
                                              bool busy,
                                              int width,
                                              int height) {
    Screen screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    const int active_tab = 1;
    auto document = render_document(RenderState{snapshot, skills, busy, width, height, active_tab,
                                                 1.0F, 1.0F, text("test input")});
    Render(screen, document);
    return screen.ToString();
}

}  // namespace agent_framework::example

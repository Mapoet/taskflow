#include "ftxui_console_view.hpp"

#include <agent/context_budget/context_budget.hpp>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>

namespace agent_framework::example {

std::vector<std::string> wrap_terminal_text(std::string_view value,
                                            int display_columns,
                                            bool preserve_leading_space) {
    const int limit = std::max(1, display_columns);
    std::vector<std::string> output;
    std::string logical_line;
    auto emit_logical_line = [&](std::string_view line) {
        if (line.empty()) {
            output.emplace_back();
            return;
        }
        std::string current;
        int cells = 0;
        for (const auto& glyph : ftxui::Utf8ToGlyphs(line)) {
            if (glyph.empty()) continue;  // second display cell of a wide glyph
            const int glyph_cells = std::max(0, ftxui::string_width(glyph));
            if (cells > 0 && cells + glyph_cells > limit) {
                output.push_back(std::move(current));
                current.clear();
                cells = 0;
                if (!preserve_leading_space && glyph == " ") continue;
            }
            current += glyph;
            cells += glyph_cells;
        }
        output.push_back(std::move(current));
    };
    for (char ch : value) {
        if (ch == '\n') {
            emit_logical_line(logical_line);
            logical_line.clear();
        } else if (ch != '\r') {
            logical_line.push_back(ch);
        }
    }
    if (!logical_line.empty() || value.empty() || value.back() == '\n')
        emit_logical_line(logical_line);
    return output;
}

namespace {

using namespace ftxui;

const Color kAccent = Color::Cyan;
const Color kMuted = Color::GrayDark;

Element wrapped_text(std::string_view value, int width,
                     bool preserve_leading_space = false) {
    Elements lines;
    for (auto& line : wrap_terminal_text(value, width, preserve_leading_space))
        lines.push_back(text(std::move(line)));
    if (lines.empty()) lines.push_back(text(""));
    return vbox(std::move(lines));
}

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

Color operations_color(OperationsStatus state) {
    switch (state) {
        case OperationsStatus::Passed: return Color::Green;
        case OperationsStatus::Running: return Color::Yellow;
        case OperationsStatus::Warning: return Color::YellowLight;
        case OperationsStatus::Blocked:
        case OperationsStatus::Failed: return Color::Red;
        case OperationsStatus::Pending: return Color::BlueLight;
        case OperationsStatus::Unknown: return Color::GrayLight;
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

Elements conversation_rows(const UiPresentationSnapshot& snapshot, int width) {
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
        rows.push_back(wrapped_text(turn.content.empty() ? " " : turn.content, width));
        rows.push_back(separatorEmpty());
    }
    return rows;
}

Elements activity_rows(const UiPresentationSnapshot& snapshot, int width) {
    Elements rows;
    if (snapshot.tools.empty()) {
        rows.push_back(paragraph("No tool calls yet. Arguments, results, duration, and failures appear here.") | color(kMuted));
        return rows;
    }
    for (const auto& tool : snapshot.tools) {
        std::string title = std::string(UiPresentationModel::state_name(tool.state)) + "  " + tool.tool_name;
        if (tool.duration_ms > 0) title += "  " + std::to_string(tool.duration_ms) + " ms";
        rows.push_back(text(title) | bold | color(state_color(tool.state)));
        if (!tool.arguments.empty()) rows.push_back(wrapped_text("args  " + truncate(tool.arguments.dump()), width) | color(Color::GrayLight));
        if (!tool.result.empty()) rows.push_back(wrapped_text("result  " + truncate(tool.result.dump()), width));
        rows.push_back(separatorEmpty());
    }
    return rows;
}

Elements operations_rows(const UiPresentationSnapshot& snapshot, int width) {
    Elements rows;
    if (!snapshot.has_operations) {
        rows.push_back(paragraph("No Phase 4 operations snapshot has been published.") | color(kMuted));
        return rows;
    }
    const auto& ops = snapshot.operations;
    rows.push_back(hbox({text(" " + std::string(Phase4OperationsProjection::status_name(ops.overall_status)) + " ") |
                            bold | color(operations_color(ops.overall_status)),
                        text(" task " + ops.task_id + " · run " + ops.run_id +
                             (ops.turn_id.empty()?"":" · turn "+ops.turn_id) +
                             " · plan r" + std::to_string(ops.plan_revision)) | color(kMuted)}));
    rows.push_back(wrapped_text(ops.summary, width));
    rows.push_back(hbox({text(" RESPONSE " + ops.response_delivery_state + " · PIPELINE " +
                             ops.pipeline_state + " · " +
                             (ops.task_completion_verified ? "VERIFIED " : "UNVERIFIED ")) |
                            bold | color(ops.task_completion_verified ? Color::GreenLight : Color::YellowLight),
                        text(" closure " + ops.task_closure_state + " · authority " +
                             ops.completion_authority + " · criteria " +
                             std::to_string(ops.criteria_closed) + "/" +
                             std::to_string(ops.criteria_total) + " · progress " +
                             std::to_string(ops.progress_delta) + " · stagnant " +
                             std::to_string(ops.stagnation_count)) | color(kMuted)}));
    if (!ops.blocker.empty()) rows.push_back(wrapped_text("BLOCKER  " + ops.blocker, width) | bold | color(Color::RedLight));
    if (!ops.residual_risk.empty()) rows.push_back(wrapped_text("RESIDUAL RISK  " + ops.residual_risk, width) | color(Color::YellowLight));
    if(!ops.task_actions.empty()) {
        std::string line="TASK COMMANDS";
        for(const auto& action:ops.task_actions)
            line += " · " + action.command + " " +
                (action.enabled ? "enabled" : "disabled") + "@r" +
                std::to_string(action.expected_task_revision);
        rows.push_back(wrapped_text(line, width) | color(kMuted));
    }
    rows.push_back(separator());
    rows.push_back(text("PLAN / EVIDENCE") | bold | color(kAccent));
    for (const auto& stage : ops.stages) {
        rows.push_back(hbox({text("[" + std::string(Phase4OperationsProjection::status_name(stage.status)) + "] ") |
                                bold | color(operations_color(stage.status)),
                            text(stage.label + "  r" + std::to_string(stage.revision)) | bold,
                            text("  " + stage.role) | color(kMuted)}));
        rows.push_back(wrapped_text("  " + stage.summary + " · evidence " + std::to_string(stage.evidence_ids.size()), width));
    }
    rows.push_back(separator());
    rows.push_back(text("MEMORY VIEW") | bold | color(kAccent));
    for (const auto& memory : ops.memory) {
        rows.push_back(wrapped_text(std::string(memory.selected ? "selected  " : "excluded  ") +
                                 memory.scope + " · " + memory.source + " · " + memory.authority +
                                 " · " + memory.freshness + " · " + memory.selection_reason, width) |
                       color(memory.selected ? Color::GreenLight : kMuted));
    }
    rows.push_back(separator());
    rows.push_back(text("LLM INVOCATIONS") | bold | color(kAccent));
    for (const auto& invocation : ops.invocations) {
        rows.push_back(wrapped_text(invocation.role + " · " + invocation.provider + "/" + invocation.model +
                                 " · " + invocation.prompt_version + " · " + invocation.view_id +
                                 " · " + std::to_string(invocation.latency_ms) + " ms · " +
                                 Phase4OperationsProjection::status_name(invocation.status), width));
    }
    rows.push_back(separator());
    rows.push_back(text("FIVE-LAYER ASSURANCE") | bold | color(kAccent));
    for (const auto& layer : ops.assurance)
        rows.push_back(hbox({text("[" + std::string(Phase4OperationsProjection::status_name(layer.status)) + "] ") |
                                color(operations_color(layer.status)), text(layer.label + " · " + layer.oracle + " · " + layer.verifier)}));
    for (const auto& request : ops.hitl)
        rows.push_back(wrapped_text("HITL " + request.kind + " · " + request.summary + " · actions " +
                                 std::to_string(request.allowed_actions.size()), width) | bold | color(Color::YellowLight));
    rows.push_back(separator());
    rows.push_back(text("INTERACTION GRAPH") | bold | color(kAccent));
    if(!snapshot.has_interactions) rows.push_back(text("No canonical interaction snapshot published.")|color(kMuted));
    else {
        rows.push_back(text("revision "+std::to_string(snapshot.interactions.revision)+" · "+std::to_string(snapshot.interactions.nodes.size())+" objects · "+std::to_string(snapshot.interactions.edges.size())+" relations")|color(kMuted));
        for(const auto& node:snapshot.interactions.nodes){
            const bool selected=node.node_id==snapshot.selected_interaction_id;
            auto line=wrapped_text(std::string(selected?"> ":"  ")+std::string(ui::name(node.kind))+" · "+node.label+" · "+std::string(ui::name(node.state)), width);
            if(selected)line=line|bold;rows.push_back(std::move(line));
            if(selected)rows.push_back(wrapped_text("    "+node.summary+" · source "+node.source.store+" r"+std::to_string(node.source.revision), width)|color(kMuted));
        }
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

Element session_rail(const ui::NativeWorkbenchSnapshot& workbench, int width) {
    Elements rows{
        text("AGENT WORKBENCH") | bold | color(kAccent),
        text("Sessions  " + std::to_string(workbench.sessions.size())) | color(kMuted),
        separatorEmpty(),
        text("+  /new [title]") | bold | color(Color::BlueLight),
        separator(),
    };
    for (const auto& session : workbench.sessions) {
        const bool selected = session.session_id == workbench.selected_session_id;
        std::string title = session.title.empty() ? session.session_id : session.title;
        auto item = vbox({
            wrapped_text(std::string(selected ? "> " : "  ") + title, std::max(8, width - 4)) |
                (selected ? bold : nothing),
            text("  " + session.state + " · r" + std::to_string(session.revision)) |
                color(selected ? Color::GreenLight : kMuted),
        });
        rows.push_back(std::move(item));
        rows.push_back(separatorEmpty());
    }
    rows.push_back(filler());
    rows.push_back(separator());
    rows.push_back(text(workbench.subject.principal_id) | bold);
    rows.push_back(text(workbench.subject.organization_id + " / " +
                        workbench.subject.project_id) | color(kMuted));
    rows.push_back(text("/profile  /settings") | color(Color::BlueLight));
    return vbox(std::move(rows));
}

Elements profile_rows(const ui::NativeWorkbenchSnapshot& workbench, int width) {
    const auto& s = workbench.subject;
    Elements rows{
        text("AUTHENTICATED RUNTIME IDENTITY") | bold | color(kAccent),
        text("Authorization r" + std::to_string(s.authorization_revision)) | color(Color::GreenLight),
        separator(),
    };
    for (const auto& [label, value] : std::vector<std::pair<std::string, std::string>>{
             {"Principal", s.principal_id}, {"Tenant", s.tenant_id},
             {"Organization", s.organization_id}, {"Project", s.project_id},
             {"Workspace", s.workspace_id}, {"Agent", s.agent_id},
             {"Session", s.session_id}}) {
        rows.push_back(text(label) | bold);
        rows.push_back(wrapped_text("  " + value, width) | color(kMuted));
    }
    return rows;
}

std::string setting_value(const nlohmann::json& value) {
    if (value.is_boolean()) return value.get<bool>() ? "enabled" : "disabled";
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "—";
    return value.dump();
}

Elements settings_rows(const ui::NativeWorkbenchSnapshot& workbench, int width) {
    Elements rows{
        text("SYSTEM SETTINGS") | bold | color(kAccent),
        text("Settings r" + std::to_string(workbench.settings_revision) +
             " · authorization r" + std::to_string(workbench.settings_authorization_revision)) |
            color(Color::GreenLight),
        separator(),
    };
    std::string category;
    for (const auto& field : workbench.settings_fields) {
        const auto next = field.value("category", "Other");
        if (next != category) {
            category = next;
            rows.push_back(text(category) | bold | color(Color::BlueLight));
        }
        const std::string mutability = field.value("mutability", "read_only");
        rows.push_back(wrapped_text(field.value("label", field.value("key", "")) + "  " +
            setting_value(field.value("value", nlohmann::json{})) + "  [" + mutability + "]",
            width));
    }
    return rows;
}

Elements context_rows(const UiPresentationSnapshot& snapshot,
                      ui::NativeWorkbenchView view, int width) {
    Elements rows;
    const auto& ops = snapshot.operations;
    const auto title = std::string(ui::NativeWorkbenchController::name(view));
    rows.push_back(text(title) | bold | color(kAccent));
    rows.push_back(separator());
    if (!snapshot.has_operations) {
        rows.push_back(text("No durable context is available.") | color(kMuted));
        return rows;
    }
    if (view == ui::NativeWorkbenchView::Understanding ||
        view == ui::NativeWorkbenchView::Plan) {
        for (const auto& stage : ops.stages) {
            rows.push_back(text("[" + std::string(Phase4OperationsProjection::status_name(stage.status)) +
                                "] " + stage.label + " · r" + std::to_string(stage.revision)) |
                           color(operations_color(stage.status)));
            rows.push_back(wrapped_text(stage.summary + " · " + stage.role, width));
        }
    } else if (view == ui::NativeWorkbenchView::Memory) {
        for (const auto& item : ops.memory)
            rows.push_back(wrapped_text(std::string(item.selected ? "selected · " : "excluded · ") +
                item.scope + " · " + item.source + " · " + item.selection_reason, width) |
                color(item.selected ? Color::GreenLight : kMuted));
    } else if (view == ui::NativeWorkbenchView::Approval) {
        if (ops.hitl.empty()) rows.push_back(text("No pending approval.") | color(kMuted));
        for (const auto& item : ops.hitl)
            rows.push_back(wrapped_text(item.kind + " · " + item.summary + " · " +
                std::to_string(item.allowed_actions.size()) + " allowed actions", width) |
                color(Color::YellowLight));
    } else if (view == ui::NativeWorkbenchView::Evidence) {
        for (const auto& layer : ops.assurance)
            rows.push_back(wrapped_text("[" + std::string(Phase4OperationsProjection::status_name(layer.status)) +
                "] " + layer.label + " · " + layer.oracle + " · " + layer.verifier, width) |
                color(operations_color(layer.status)));
    } else if (view == ui::NativeWorkbenchView::Files) {
        for (const auto& turn : snapshot.turns)
            for (const auto& attachment : turn.attachments)
                rows.push_back(wrapped_text(attachment.caption + " · " + attachment.path +
                    (attachment.sha256.empty() ? "" : " · " + attachment.sha256), width));
        if (rows.size() == 2U) rows.push_back(text("No artifacts attached.") | color(kMuted));
    }
    return rows;
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
    float operations_scroll{0.0F};
    Element composer;
    ui::NativeWorkbenchSnapshot workbench;
    bool has_workbench{false};
};

Element render_document(RenderState state) {
    const auto run_color = state.busy ? Color::Yellow : state_color(state.snapshot.run_state);
    const auto selected = state.has_workbench
        ? std::find_if(state.workbench.sessions.begin(), state.workbench.sessions.end(),
              [&](const auto& item) { return item.session_id == state.workbench.selected_session_id; })
        : state.workbench.sessions.end();
    const std::string session_title = selected != state.workbench.sessions.end()
        ? selected->title : state.snapshot.session_id;
    const std::uint64_t session_revision = selected != state.workbench.sessions.end()
        ? selected->revision : 0;
    auto header = hbox({
        text(state.has_workbench ? " AGENT WORKBENCH " : " SCIENTIFIC CONSOLE ") |
            bold | bgcolor(Color::Blue) | color(Color::White),
        text("  " + session_title) | bold,
        filler(),
        text(state.snapshot.model + "  ") | color(kMuted),
        text(UiPresentationModel::state_name(state.snapshot.run_state)) | bold | color(run_color),
        text("  " + state.snapshot.connection_label +
             (state.has_workbench ? "  Session r" + std::to_string(session_revision) : "") + " ") |
            color(kMuted),
    });

    auto run_strip = hbox({
        text(state.busy ? " ● RUNNING " : " ● " + std::string(UiPresentationModel::state_name(state.snapshot.run_state)) + " ") |
            bold | color(run_color),
        text(state.snapshot.has_operations
                 ? "Task " + state.snapshot.operations.task_id + " · Run " + state.snapshot.operations.run_id +
                       " · plan r" + std::to_string(state.snapshot.operations.plan_revision)
                 : "No active durable Run") | color(kMuted),
        filler(),
        text(state.snapshot.has_operations
                 ? std::to_string(state.snapshot.operations.criteria_closed) + "/" +
                       std::to_string(state.snapshot.operations.criteria_total) + " criteria "
                 : "") | color(kMuted),
    }) | border;

    Elements navigation;
    for (const auto view : {ui::NativeWorkbenchView::Conversation,
                            ui::NativeWorkbenchView::Understanding,
                            ui::NativeWorkbenchView::Plan,
                            ui::NativeWorkbenchView::Memory,
                            ui::NativeWorkbenchView::Files,
                            ui::NativeWorkbenchView::Approval,
                            ui::NativeWorkbenchView::Evidence}) {
        const bool active = state.has_workbench && state.workbench.view == view;
        navigation.push_back(text(" " + std::string(ui::NativeWorkbenchController::name(view)) + " ") |
                             (active ? bold : dim) | color(active ? Color::White : kMuted));
    }
    auto context_navigation = hbox(std::move(navigation)) | border;

    const int conversation_width = state.width >= 120 ? state.width - 70
        : state.width >= 100 ? state.width - 28 : state.width - 6;
    const int activity_width = state.width >= 120 ? 36 : state.width >= 100 ? state.width - 28 : state.width - 6;
    const int operations_width = state.width >= 100 ? state.width - 30 : state.width - 6;
    auto conversation = framed_panel("CONVERSATION", vbox(conversation_rows(state.snapshot, conversation_width)),
                                     state.conversation_scroll, state.active_tab == 1);
    auto activity = framed_panel("TOOL ACTIVITY", vbox(activity_rows(state.snapshot, activity_width)),
                                 state.activity_scroll, state.active_tab == 2);
    auto caps = framed_panel(state.has_workbench ? "SESSIONS" : "CAPABILITIES",
                             state.has_workbench
                                 ? session_rail(state.workbench, 26)
                                 : capabilities(state.snapshot, state.skills),
                             0.0F, state.active_tab == 0);
    auto operations = framed_panel("PHASE 4 OPERATIONS", vbox(operations_rows(state.snapshot, operations_width)),
                                   state.operations_scroll, state.active_tab == 3);

    Element primary = conversation;
    if (state.has_workbench) {
        if (state.workbench.view == ui::NativeWorkbenchView::Profile)
            primary = framed_panel("PROFILE", vbox(profile_rows(state.workbench, conversation_width)),
                                   state.operations_scroll, true);
        else if (state.workbench.view == ui::NativeWorkbenchView::Settings)
            primary = framed_panel("SETTINGS", vbox(settings_rows(state.workbench, conversation_width)),
                                   state.operations_scroll, true);
        else if (state.workbench.view != ui::NativeWorkbenchView::Conversation)
            primary = framed_panel(std::string(ui::NativeWorkbenchController::name(state.workbench.view)),
                                   vbox(context_rows(state.snapshot, state.workbench.view, conversation_width)),
                                   state.operations_scroll, true);
    }

    Element body;
    std::string layout;
    if (!state.has_workbench && state.active_tab == 3) {
        body = state.width >= 100
                   ? hbox({caps | size(WIDTH, EQUAL, 24), operations | flex})
                   : operations;
        layout = "operations · canonical control-plane snapshot";
    } else if (state.width >= 120) {
        body = hbox({caps | size(WIDTH, EQUAL, state.has_workbench ? 28 : 24), primary | flex,
                     activity | size(WIDTH, EQUAL, 40)});
        layout = "wide · three panels";
    } else if (state.width >= 100) {
        body = hbox({caps | size(WIDTH, EQUAL, state.has_workbench ? 26 : 22),
                     (state.active_tab == 2 ? activity : primary) | flex});
        layout = "medium · 1/2 conversation · 2/2 activity";
    } else {
        body = state.active_tab == 0 ? caps : state.active_tab == 2 ? activity : primary;
        layout = "compact · [1] capabilities  [2] conversation  [3] activity  [4] operations";
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
                               text(state.has_workbench
                                        ? "Enter send · /new /rename /trash /restore /purge · /profile /settings · /view <name>"
                                        : "Enter send · Esc cancel · Ctrl+C quit · 1/2/3/4 panes · PgUp/PgDn scroll") |
                                   color(kMuted),
                           })) |
                    size(HEIGHT, EQUAL, 5);
    auto footer = hbox({
        text(" " + std::to_string(state.snapshot.turns.size()) + " turns"),
        text("  " + std::to_string(state.snapshot.tools.size()) + " tools"),
        filler(),
        text(layout + " ") | color(kMuted),
    });
    Elements shell{std::move(header)};
    if (state.has_workbench) {
        shell.push_back(std::move(run_strip));
        shell.push_back(std::move(context_navigation));
    }
    shell.push_back(std::move(body) | flex);
    shell.push_back(std::move(composer));
    shell.push_back(std::move(footer));
    if (state.has_workbench && (!state.workbench.error.empty() || !state.workbench.status.empty()))
        shell.push_back(wrapped_text(!state.workbench.error.empty()
            ? "ERROR · " + state.workbench.error : state.workbench.status, state.width - 2) |
            color(!state.workbench.error.empty() ? Color::RedLight : Color::GreenLight));
    return vbox(std::move(shell)) |
           color(Color::GrayLight) |
           bgcolor(Color::RGB(11, 16, 22)) |
           size(HEIGHT, EQUAL, state.height);
}

}  // namespace

struct FtxuiConsoleView::Impl {
    Impl(SnapshotProvider snapshot_provider,
         SkillStatusProvider skill_status_provider,
         FtxuiConsoleCallbacks callbacks,
         std::shared_ptr<ui::NativeWorkbenchController> workbench)
        : snapshot_provider(std::move(snapshot_provider)),
          skill_status_provider(std::move(skill_status_provider)),
          callbacks(std::move(callbacks)),
          workbench(std::move(workbench)) {
        const char* initial = std::getenv("AGENT_UI_INITIAL_VIEW");
        if (initial && std::string_view(initial) == "operations") active_tab = 3;
    }

    SnapshotProvider snapshot_provider;
    SkillStatusProvider skill_status_provider;
    FtxuiConsoleCallbacks callbacks;
    std::shared_ptr<ui::NativeWorkbenchController> workbench;
    std::atomic<bool> busy{false};
    std::mutex app_mutex;
    App* app{nullptr};
    std::string composer;
    int active_tab{1};
    float conversation_scroll{1.0F};
    float activity_scroll{1.0F};
    float operations_scroll{0.0F};

    void scroll(float delta) {
        float& value = active_tab == 3 ? operations_scroll : active_tab == 2 ? activity_scroll : conversation_scroll;
        value = std::clamp(value + delta, 0.0F, 1.0F);
    }

    bool command(std::string_view line) {
        if (!workbench || line.empty() || line.front() != '/') return false;
        const auto previous_session = workbench->snapshot().selected_session_id;
        const auto split = line.find(' ');
        const auto verb = line.substr(0, split);
        const std::string argument = split == std::string_view::npos
            ? std::string{} : std::string(line.substr(split + 1));
        if (verb == "/new") workbench->create_session(argument);
        else if (verb == "/rename") workbench->rename_selected(argument);
        else if (verb == "/trash") workbench->trash_selected();
        else if (verb == "/restore") workbench->restore_selected();
        else if (verb == "/purge") {
            const auto current = workbench->snapshot();
            const auto selected = std::find_if(current.sessions.begin(), current.sessions.end(),
                [&](const auto& item) { return item.session_id == current.selected_session_id; });
            if (selected != current.sessions.end() && selected->state == "purge_pending")
                workbench->confirm_purge_selected();
            else workbench->request_purge_selected();
        } else if (verb == "/profile") workbench->set_view(ui::NativeWorkbenchView::Profile);
        else if (verb == "/settings") workbench->set_view(ui::NativeWorkbenchView::Settings);
        else if (verb == "/session") workbench->select_session(argument);
        else if (verb == "/view") {
            const std::vector<std::pair<std::string_view, ui::NativeWorkbenchView>> views{
                {"conversation", ui::NativeWorkbenchView::Conversation},
                {"understanding", ui::NativeWorkbenchView::Understanding},
                {"plan", ui::NativeWorkbenchView::Plan}, {"memory", ui::NativeWorkbenchView::Memory},
                {"files", ui::NativeWorkbenchView::Files}, {"approval", ui::NativeWorkbenchView::Approval},
                {"evidence", ui::NativeWorkbenchView::Evidence}};
            const auto found = std::find_if(views.begin(), views.end(),
                [&](const auto& item) { return item.first == argument; });
            if (found != views.end()) workbench->set_view(found->second);
        } else if (verb == "/set") {
            const auto value_split = argument.find(' ');
            if (value_split != std::string::npos) {
                const auto key = argument.substr(0, value_split);
                const auto raw = argument.substr(value_split + 1);
                nlohmann::json value = raw == "true" ? nlohmann::json(true)
                    : raw == "false" ? nlohmann::json(false) : nlohmann::json(raw);
                workbench->update_setting(key, value);
            }
        } else return false;
        const auto current = workbench->snapshot();
        if (current.selected_session_id != previous_session && callbacks.on_session_change)
            callbacks.on_session_change(current);
        {
            std::lock_guard<std::mutex> lock(app_mutex);
            if (app) app->PostEvent(ftxui::Event::Custom);
        }
        return true;
    }

    Element render(Element input) {
        const auto dimensions = Terminal::Size();
        auto state = RenderState{
            snapshot_provider ? snapshot_provider() : UiPresentationSnapshot{},
            skill_status_provider ? skill_status_provider() : FtxuiSkillStatus{},
            busy.load(),
            dimensions.dimx,
            dimensions.dimy,
            active_tab,
            conversation_scroll,
            activity_scroll,
            operations_scroll,
            std::move(input),
        };
        if (workbench) {
            state.workbench = workbench->snapshot();
            state.has_workbench = true;
        }
        return render_document(std::move(state));
    }
};

FtxuiConsoleView::FtxuiConsoleView(SnapshotProvider snapshot_provider,
                                   SkillStatusProvider skill_status_provider,
                                   FtxuiConsoleCallbacks callbacks,
                                   std::shared_ptr<ui::NativeWorkbenchController> workbench)
    : impl_(std::make_unique<Impl>(std::move(snapshot_provider),
                                  std::move(skill_status_provider),
                                  std::move(callbacks), std::move(workbench))) {}

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
        if (impl_->command(line)) return;
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
        if (event == ftxui::Event::Character('4') && impl_->composer.empty()) { impl_->active_tab = 3; return true; }
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
                                                 1.0F, 1.0F, 0.0F, text("test input")});
    Render(screen, document);
    return screen.ToString();
}

std::string FtxuiConsoleView::render_operations_for_test(const UiPresentationSnapshot& snapshot,
                                                         const FtxuiSkillStatus& skills,
                                                         int width,
                                                         int height) {
    Screen screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    auto document = render_document(RenderState{snapshot, skills, false, width, height, 3,
                                                 1.0F, 1.0F, 0.0F, text("test input")});
    Render(screen, document);
    return screen.ToString();
}

std::string FtxuiConsoleView::render_workbench_for_test(
    const UiPresentationSnapshot& snapshot, const FtxuiSkillStatus& skills,
    const ui::NativeWorkbenchSnapshot& workbench, int width, int height) {
    Screen screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    RenderState state{snapshot, skills, false, width, height, 1, 1.0F, 1.0F, 0.0F,
                      text("test input")};
    state.workbench = workbench;
    state.has_workbench = true;
    Render(screen, render_document(std::move(state)));
    return screen.ToString();
}

}  // namespace agent_framework::example

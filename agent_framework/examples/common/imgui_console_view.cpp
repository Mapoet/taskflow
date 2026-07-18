#include "imgui_console_view.hpp"

#include <agent/context_budget/context_budget.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace agent_framework::example {
namespace {

constexpr ImVec4 kAccent{0.22f, 0.77f, 0.84f, 1.0f};
constexpr ImVec4 kMuted{0.56f, 0.63f, 0.68f, 1.0f};
constexpr ImVec4 kSuccess{0.34f, 0.82f, 0.61f, 1.0f};
constexpr ImVec4 kDanger{0.94f, 0.47f, 0.47f, 1.0f};

const char* role_name(UiTurnRole role) {
    if (role == UiTurnRole::User) return "YOU";
    if (role == UiTurnRole::Assistant) return "AGENT";
    return "SYSTEM";
}

ImVec4 state_color(UiRunState state) {
    if (state == UiRunState::Completed) return kSuccess;
    if (state == UiRunState::Failed) return kDanger;
    if (state == UiRunState::Running) return kAccent;
    return kMuted;
}

std::string compact_json(const json& value, std::size_t cap = 420) {
    std::string out = value.dump(2);
    if (out.size() > cap) out = utf8_safe_truncate(out, cap) + "\n…";
    return out;
}

void render_header(const UiPresentationSnapshot& s) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.071f, 0.09f, 1.0f));
    ImGui::BeginChild("##header", ImVec2(0, 54), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPos(ImVec2(18, 18));
    ImGui::TextColored(kAccent, "SCIENTIFIC CONSOLE");
    ImGui::SameLine(238);
    ImGui::TextColored(kMuted, "Session:"); ImGui::SameLine(); ImGui::TextUnformatted(s.session_id.c_str());
    ImGui::SameLine(); ImGui::TextColored(kMuted, "  Model:"); ImGui::SameLine(); ImGui::TextUnformatted(s.model.empty() ? "provider default" : s.model.c_str());
    ImGui::SameLine(); ImGui::TextColored(kMuted, "  Provider:"); ImGui::SameLine(); ImGui::TextUnformatted(s.provider.empty() ? "OpenAI" : s.provider.c_str());
    const float label_w = ImGui::CalcTextSize(s.connection_label.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - label_w - 24.0f));
    ImGui::TextColored(state_color(s.run_state), "%s", s.connection_label.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void render_left_rail(const UiPresentationSnapshot& s, const ImGuiSkillStatus& skills, float width) {
    ImGui::BeginChild("##left", ImVec2(width, 0), true);
    ImGui::TextColored(kMuted, "SESSION");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.13f, 0.16f, 1.0f));
    ImGui::BeginChild("##session", ImVec2(0, 64), true);
    ImGui::TextUnformatted(s.session_id.c_str());
    ImGui::TextColored(kMuted, "Scientific workspace");
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(kMuted, "CAPABILITIES");
    const char* items[] = {"File system        FS", "Web research       WEB", "Expression engine  EXPR",
                           "Plot & draw        DRAW", "Skills             SKILL", "MCP services       MCP"};
    for (const char* item : items) { ImGui::Spacing(); ImGui::TextUnformatted(item); }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(kMuted, "SKILLS");
    if (skills.enabled) {
        ImGui::Text("Indexed %zu  Gen %llu", skills.count,
                    static_cast<unsigned long long>(skills.generation));
        ImGui::TextColored(skills.errors == 0 ? kMuted : kDanger,
                           "Diagnostics %zu  Errors %zu", skills.diagnostics, skills.errors);
        ImGui::TextWrapped("Root  %s", skills.root.empty() ? "-" : skills.root.c_str());
        ImGui::TextWrapped("Active  %s", skills.active.c_str());
    } else {
        ImGui::TextColored(kMuted, "Disabled");
    }
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 52.0f));
    ImGui::Separator(); ImGui::TextColored(kMuted, "Agent Framework\nFS jail enabled");
    ImGui::EndChild();
}

void render_conversation(const UiPresentationSnapshot& s, ImGuiConsoleAction& action,
                         char* input, std::size_t input_size, bool busy) {
    ImGui::BeginGroup();
    ImGui::BeginChild("##conversation", ImVec2(0, -132), true);
    if (s.turns.empty()) {
        ImGui::SetCursorPos(ImVec2(28, 34));
        ImGui::TextColored(kAccent, "READY");
        ImGui::SetCursorPosX(28); ImGui::Text("Start a verifiable research task");
        ImGui::SetCursorPosX(28); ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 42);
        ImGui::TextColored(kMuted, "Ask the agent to inspect files, search the web, evaluate an expression, or create a scientific artifact.");
        ImGui::PopTextWrapPos();
    }
    for (std::size_t i = 0; i < s.turns.size(); ++i) {
        const UiTurn& turn = s.turns[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextColored(turn.error ? kDanger : turn.role == UiTurnRole::Assistant ? kAccent : kMuted,
                           "%s", role_name(turn.role));
        ImGui::SameLine(); ImGui::TextColored(kMuted, turn.streaming ? "streaming" : "");
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 24);
        ImGui::TextWrapped("%s", turn.content.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::PopID();
    }
    if (busy) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.055f, 0.071f, 0.09f, 1.0f));
    ImGui::InputTextMultiline("##prompt", input, input_size, ImVec2(-1, 72));
    ImGui::PopStyleColor();
    ImGui::TextColored(kMuted, "Enter a task · Shift+Enter newline");
    ImGui::SameLine(ImGui::GetWindowWidth() - (busy ? 150.0f : 76.0f));
    if (busy) {
        if (ImGui::Button("Stop", ImVec2(66, 28))) action.cancel = true;
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(busy || input[0] == '\0');
    if (ImGui::Button("Send", ImVec2(66, 28))) {
        action.send = true; action.prompt = input; input[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

void render_activity(const UiPresentationSnapshot& s, float width) {
    ImGui::BeginChild("##activity", ImVec2(width, 0), true);
    ImGui::TextColored(kAccent, "EXECUTION");
    ImGui::Text("Tool activity");
    ImGui::SameLine(ImGui::GetWindowWidth() - 74);
    ImGui::TextColored(state_color(s.run_state), "%s", UiPresentationModel::state_name(s.run_state));
    ImGui::Separator();
    if (s.tools.empty()) {
        ImGui::Spacing(); ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 18);
        ImGui::TextColored(kMuted, "Tool calls appear here with arguments, results, and duration.");
        ImGui::PopTextWrapPos();
    }
    for (std::size_t i = 0; i < s.tools.size(); ++i) {
        const UiToolActivity& tool = s.tools[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Spacing();
        ImGui::TextColored(state_color(tool.state), "%s", UiPresentationModel::state_name(tool.state));
        ImGui::SameLine(); ImGui::TextWrapped("%s", tool.tool_name.c_str());
        if (tool.duration_ms > 0) { ImGui::SameLine(); ImGui::TextColored(kMuted, "%lld ms", static_cast<long long>(tool.duration_ms)); }
        if (ImGui::TreeNode("Arguments and result")) {
            const std::string args = compact_json(tool.arguments);
            ImGui::TextWrapped("Arguments\n%s", args.c_str());
            if (!tool.result.empty()) {
                const std::string result = compact_json(tool.result);
                ImGui::TextWrapped("Result\n%s", result.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::Separator(); ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace

void apply_scientific_console_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(8, 8); style.FramePadding = ImVec2(9, 7);
    style.ItemSpacing = ImVec2(8, 7); style.ItemInnerSpacing = ImVec2(7, 5);
    style.WindowRounding = 0; style.ChildRounding = 5; style.FrameRounding = 4;
    style.ScrollbarRounding = 4; style.GrabRounding = 3;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.055f, 0.068f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.060f, 0.075f, 0.092f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.09f, 0.11f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.17f, 0.21f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.115f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.16f, 0.19f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.19f, 0.22f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.12f, 0.48f, 0.53f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.66f, 0.72f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.58f, 0.64f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.10f, 0.30f, 0.34f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = kMuted;
}

ImGuiConsoleAction render_scientific_console(const UiPresentationSnapshot& s,
                                             const ImGuiSkillStatus& skills, char* input,
                                             std::size_t input_size, bool busy) {
    ImGuiConsoleAction action;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Scientific Console", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    render_header(s);
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool wide = avail >= 1020.0f;
    const float left = wide ? 218.0f : 0.0f;
    const float right = wide ? 318.0f : std::min(280.0f, avail * 0.33f);
    if (wide) { render_left_rail(s, skills, left); ImGui::SameLine(); }
    ImGui::BeginGroup();
    const float main_width = std::max(320.0f, avail - left - right - (wide ? 16.0f : 8.0f));
    ImGui::BeginChild("##main", ImVec2(main_width, 0), false);
    render_conversation(s, action, input, input_size, busy);
    ImGui::EndChild(); ImGui::EndGroup();
    ImGui::SameLine(); render_activity(s, right);
    ImGui::End();
    return action;
}

} // namespace agent_framework::example

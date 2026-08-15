#include "imgui_console_view.hpp"

#include <agent/context_budget/context_budget.hpp>
#include <agent/toolbus/fs_sandbox.hpp>
#include <imgui.h>
#include <GL/gl.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace agent_framework::example {
namespace {

constexpr ImVec4 kAccent{0.22f, 0.77f, 0.84f, 1.0f};
constexpr ImVec4 kMuted{0.56f, 0.63f, 0.68f, 1.0f};
constexpr ImVec4 kSuccess{0.34f, 0.82f, 0.61f, 1.0f};
constexpr ImVec4 kDanger{0.94f, 0.47f, 0.47f, 1.0f};

struct ArtifactTexture {
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

std::unordered_map<std::string, ArtifactTexture> g_artifact_textures;

const UiAttachment* find_attachment(const UiTurn& turn, const std::string& id) {
    const auto it = std::find_if(turn.attachments.begin(), turn.attachments.end(),
                                 [&](const UiAttachment& value) { return value.id == id; });
    return it == turn.attachments.end() ? nullptr : &*it;
}

const ArtifactTexture* load_artifact_texture(const UiAttachment& attachment) {
    if (attachment.mime != "image/png" && attachment.mime != "image/jpeg" &&
        attachment.mime != "image/webp" && attachment.mime != "image/gif") return nullptr;
    if (const auto found = g_artifact_textures.find(attachment.path);
        found != g_artifact_textures.end()) return &found->second;

    const char* root_env = std::getenv("AGENT_FS_ROOT");
    if (!root_env || !*root_env) return nullptr;
    json error;
    const auto resolved = fs_resolve_under_root(attachment.path, std::filesystem::path(root_env), error);
    if (!resolved || !std::filesystem::is_regular_file(*resolved) ||
        std::filesystem::file_size(*resolved) > 16U * 1024U * 1024U) return nullptr;

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(resolved->string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        stbi_image_free(pixels);
        return nullptr;
    }
    ArtifactTexture texture;
    texture.width = width;
    texture.height = height;
    glGenTextures(1, &texture.id);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    const auto [inserted, _] = g_artifact_textures.emplace(attachment.path, texture);
    return &inserted->second;
}

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

void render_content_block(const UiTurn& turn, const UiContentBlock& block) {
    switch (block.kind) {
        case UiContentBlockKind::Heading: {
            const float scale = block.heading_level <= 1 ? 1.30f : block.heading_level == 2 ? 1.16f : 1.06f;
            ImGui::SetWindowFontScale(scale);
            ImGui::TextColored(ImVec4(0.90f, 0.94f, 0.97f, 1.0f), "%s", block.text.c_str());
            ImGui::SetWindowFontScale(1.0f);
            if (block.heading_level <= 2) ImGui::Separator();
            break;
        }
        case UiContentBlockKind::List: {
            std::size_t begin = 0;
            while (begin <= block.text.size()) {
                const auto end = block.text.find('\n', begin);
                std::string item = block.text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                std::size_t content = item.find(' ');
                ImGui::BulletText("%s", content == std::string::npos ? item.c_str() : item.c_str() + content + 1);
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            break;
        }
        case UiContentBlockKind::Table: {
            std::size_t columns = 0;
            for (const auto& row : block.table_cells) columns = std::max(columns, row.size());
            if (columns && ImGui::BeginTable("##md-table", static_cast<int>(columns),
                                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                 ImGuiTableFlags_SizingStretchProp)) {
                for (std::size_t row = 0; row < block.table_cells.size(); ++row) {
                    ImGui::TableNextRow();
                    for (std::size_t col = 0; col < columns; ++col) {
                        ImGui::TableSetColumnIndex(static_cast<int>(col));
                        const char* value = col < block.table_cells[row].size()
                                                ? block.table_cells[row][col].c_str() : "";
                        if (row == 0) ImGui::TextColored(kAccent, "%s", value);
                        else ImGui::TextWrapped("%s", value);
                    }
                }
                ImGui::EndTable();
            }
            break;
        }
        case UiContentBlockKind::Code:
        case UiContentBlockKind::Mermaid:
        case UiContentBlockKind::MathBlock: {
            const char* label = block.kind == UiContentBlockKind::Mermaid ? "MERMAID" :
                                block.kind == UiContentBlockKind::MathBlock ? "MATH" :
                                block.info.empty() ? "CODE" : block.info.c_str();
            ImGui::TextColored(kMuted, "%s", label);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.032f, 0.043f, 0.055f, 1.0f));
            const float height = std::min(260.0f, std::max(54.0f, ImGui::CalcTextSize(block.text.c_str(), nullptr, false,
                                                                                     ImGui::GetContentRegionAvail().x - 18.0f).y + 24.0f));
            ImGui::BeginChild("##md-source", ImVec2(0, height), true);
            ImGui::TextWrapped("%s", block.text.c_str());
            ImGui::EndChild();
            ImGui::PopStyleColor();
            break;
        }
        case UiContentBlockKind::ThematicBreak:
            ImGui::Separator();
            break;
        case UiContentBlockKind::Image:
            ImGui::TextColored(kAccent, "ARTIFACT");
            ImGui::SameLine(); ImGui::TextWrapped("%s", block.text.empty() ? block.attachment_id.c_str() : block.text.c_str());
            if (const auto* attachment = find_attachment(turn, block.attachment_id)) {
                if (const auto* texture = load_artifact_texture(*attachment)) {
                    const float available = std::max(80.0f, ImGui::GetContentRegionAvail().x);
                    const float width_scale = available / static_cast<float>(texture->width);
                    const float height_scale = 320.0f / static_cast<float>(texture->height);
                    const float scale = std::min({1.0f, width_scale, height_scale});
                    ImGui::Image(static_cast<ImTextureID>(texture->id),
                                 ImVec2(texture->width * scale, texture->height * scale));
                } else {
                    ImGui::TextColored(kMuted, "Preview unavailable · %s", attachment->path.c_str());
                }
            }
            break;
        case UiContentBlockKind::Paragraph:
        case UiContentBlockKind::MathInline:
        case UiContentBlockKind::DraftTail:
            ImGui::TextWrapped("%s", block.text.c_str());
            break;
    }
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
        if (!turn.thinking_raw.empty() && ImGui::TreeNode("Reasoning summary")) {
            ImGui::TextColored(kMuted, "%s", turn.thinking_raw.c_str());
            ImGui::TreePop();
        }
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 24);
        if (turn.blocks.empty()) ImGui::TextWrapped("%s", turn.content.c_str());
        else for (const auto& block : turn.blocks) render_content_block(turn, block);
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

ImVec4 operations_color(OperationsStatus status) {
    switch (status) {
        case OperationsStatus::Passed: return kSuccess;
        case OperationsStatus::Running:
        case OperationsStatus::Warning: return ImVec4(0.91f, 0.70f, 0.36f, 1.0f);
        case OperationsStatus::Blocked:
        case OperationsStatus::Failed: return kDanger;
        case OperationsStatus::Pending: return kAccent;
        case OperationsStatus::Unknown: return kMuted;
    }
    return kMuted;
}

void render_operations(const UiPresentationSnapshot& s) {
    ImGui::BeginChild("##operations", ImVec2(0, 0), true);
    if (!s.has_operations) {
        ImGui::TextColored(kAccent, "CONTROL PLANE");
        ImGui::TextWrapped("No Phase 4 operations snapshot has been published.");
        ImGui::EndChild();
        return;
    }
    const auto& ops = s.operations;
    ImGui::TextColored(kAccent, "PHASE 4 OPERATIONS");
    ImGui::SameLine();
    ImGui::TextColored(operations_color(ops.overall_status), "%s",
                       Phase4OperationsProjection::status_name(ops.overall_status));
    ImGui::SameLine(); ImGui::TextColored(kMuted, "task %s · run %s · turn %s · plan r%llu · %s",
        ops.task_id.c_str(),ops.run_id.c_str(),ops.turn_id.empty()?"—":ops.turn_id.c_str(),
        static_cast<unsigned long long>(ops.plan_revision), ops.updated_at.c_str());
    ImGui::TextWrapped("%s", ops.summary.c_str());
    ImGui::TextColored(ops.task_completion_verified ? kSuccess : operations_color(OperationsStatus::Warning),
                       "%s", ops.task_completion_verified ? "VERIFIED" : "UNVERIFIED");
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "closure %s · authority %s · criteria %llu/%llu · progress %lld · stagnant %llu",
        ops.task_closure_state.c_str(), ops.completion_authority.c_str(),
        static_cast<unsigned long long>(ops.criteria_closed),
        static_cast<unsigned long long>(ops.criteria_total),
        static_cast<long long>(ops.progress_delta),
        static_cast<unsigned long long>(ops.stagnation_count));
    if (!ops.blocker.empty()) ImGui::TextColored(kDanger, "BLOCKER  %s", ops.blocker.c_str());
    if (!ops.residual_risk.empty()) ImGui::TextColored(operations_color(OperationsStatus::Warning),
                                                       "RESIDUAL RISK  %s", ops.residual_risk.c_str());
    ImGui::Separator();

    ImGui::TextColored(kAccent, "PLAN / EVIDENCE");
    if (ImGui::BeginTable("##ops-stages", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                               ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Stage"); ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Role / revision"); ImGui::TableSetupColumn("Displayable summary");
        ImGui::TableHeadersRow();
        for (const auto& stage : ops.stages) {
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(stage.label.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextColored(operations_color(stage.status), "%s", Phase4OperationsProjection::status_name(stage.status));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%s / r%llu", stage.role.c_str(), static_cast<unsigned long long>(stage.revision));
            ImGui::TableSetColumnIndex(3); ImGui::TextWrapped("%s", stage.summary.c_str());
        }
        ImGui::EndTable();
    }

    const float half = std::max(300.0f, (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f);
    ImGui::BeginChild("##ops-memory", ImVec2(half, 260), true);
    ImGui::TextColored(kAccent, "MEMORY VIEW · CONTENT HIDDEN");
    for (const auto& item : ops.memory) {
        ImGui::TextColored(item.selected ? kSuccess : kMuted, "%s  %s", item.selected ? "selected" : "excluded", item.scope.c_str());
        ImGui::SameLine(); ImGui::TextWrapped("%s · %s · %s", item.source.c_str(), item.authority.c_str(), item.freshness.c_str());
        ImGui::TextColored(kMuted, "%s", item.selection_reason.c_str()); ImGui::Separator();
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("##ops-assurance", ImVec2(0, 260), true);
    ImGui::TextColored(kAccent, "FIVE-LAYER ASSURANCE");
    for (const auto& layer : ops.assurance) {
        ImGui::TextColored(operations_color(layer.status), "%s", Phase4OperationsProjection::status_name(layer.status));
        ImGui::SameLine(); ImGui::TextWrapped("%s", layer.label.c_str());
        ImGui::TextColored(kMuted, "%s · %s", layer.oracle.c_str(), layer.verifier.c_str()); ImGui::Separator();
    }
    ImGui::EndChild();

    ImGui::TextColored(kAccent, "LLM INVOCATIONS · PROMPT BODY AND SECRETS EXCLUDED");
    if (ImGui::BeginTable("##ops-invocations", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                   ImGuiTableFlags_SizingStretchProp)) {
        for (const char* name : {"Role / model", "Prompt / view", "Tokens", "Cost", "Latency", "Status"}) ImGui::TableSetupColumn(name);
        ImGui::TableHeadersRow();
        for (const auto& item : ops.invocations) {
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextWrapped("%s\n%s / %s", item.role.c_str(), item.provider.c_str(), item.model.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s\n%s", item.prompt_version.c_str(), item.view_id.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%llu / %llu", static_cast<unsigned long long>(item.input_tokens), static_cast<unsigned long long>(item.output_tokens));
            ImGui::TableSetColumnIndex(3); ImGui::Text("$%.4f", item.cost_usd);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%lld ms", static_cast<long long>(item.latency_ms));
            ImGui::TableSetColumnIndex(5); ImGui::TextColored(operations_color(item.status), "%s", Phase4OperationsProjection::status_name(item.status));
        }
        ImGui::EndTable();
    }
    for (const auto& request : ops.hitl) {
        ImGui::Separator(); ImGui::TextColored(operations_color(request.status), "HITL · %s", request.kind.c_str());
        ImGui::TextWrapped("%s", request.summary.c_str());
        ImGui::TextColored(kMuted, "Requested by %s · due %s · %zu allowed actions", request.requested_by.c_str(), request.deadline.c_str(), request.allowed_actions.size());
    }
    ImGui::EndChild();
}

void render_interactions(const UiPresentationSnapshot& s) {
    ImGui::BeginChild("##interactions",ImVec2(0,0),true);
    ImGui::TextColored(kAccent,"CANONICAL INTERACTION GRAPH");
    if(!s.has_interactions){ImGui::TextWrapped("No canonical interaction snapshot has been published.");ImGui::EndChild();return;}
    ImGui::TextColored(kMuted,"revision %llu · %zu objects · %zu relations · source revisions %zu",
        static_cast<unsigned long long>(s.interactions.revision),s.interactions.nodes.size(),s.interactions.edges.size(),s.interactions.source_revisions.size());
    ImGui::Separator();
    if(ImGui::BeginTable("##interaction-objects",4,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)){
        for(const char* title:{"Kind / object","State","Summary","Canonical source"})ImGui::TableSetupColumn(title);ImGui::TableHeadersRow();
        for(const auto& n:s.interactions.nodes){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::Text("%s",std::string(ui::name(n.kind)).c_str());ImGui::TextColored(kMuted,"%s",n.node_id.c_str());
            ImGui::TableSetColumnIndex(1);ImGui::Text("%s",std::string(ui::name(n.state)).c_str());
            ImGui::TableSetColumnIndex(2);ImGui::TextWrapped("%s",n.label.c_str());ImGui::TextColored(kMuted,"%s",n.summary.c_str());
            ImGui::TableSetColumnIndex(3);ImGui::Text("%s r%llu",n.source.store.c_str(),static_cast<unsigned long long>(n.source.revision));ImGui::TextColored(kMuted,"%s",n.source.object_id.c_str());}
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace

void clear_imgui_artifact_textures() {
    for (auto& [_, texture] : g_artifact_textures) {
        if (texture.id != 0) glDeleteTextures(1, &texture.id);
    }
    g_artifact_textures.clear();
}

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
    static bool select_operations = [] {
        const char* value = std::getenv("AGENT_UI_INITIAL_VIEW");
        return value && std::string_view(value) == "operations";
    }();
    if (ImGui::BeginTabBar("##workspace-tabs")) {
    if (ImGui::BeginTabItem("Conversation")) {
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
    ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Phase 4 Operations", nullptr,
                            select_operations ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        select_operations = false;
        render_operations(s);
        ImGui::EndTabItem();
    }
    if(ImGui::BeginTabItem("Interaction Graph")){render_interactions(s);ImGui::EndTabItem();}
    ImGui::EndTabBar();
    }
    ImGui::End();
    return action;
}

} // namespace agent_framework::example

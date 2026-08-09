#ifndef AGENT_UI_PRESENTATION_MODEL_HPP
#define AGENT_UI_PRESENTATION_MODEL_HPP

#include <agent/core/types.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

enum class UiRunState { Idle, Running, Completed, Failed, Cancelled };
enum class UiTurnRole { User, Assistant, System };

enum class UiContentBlockKind {
    Paragraph,
    Heading,
    List,
    Code,
    Table,
    MathInline,
    MathBlock,
    Mermaid,
    Image,
    ThematicBreak,
    DraftTail,
};

struct UiAttachment {
    std::string id;
    std::string mime;
    std::string path;
    std::string caption;
    std::string tool_call_id;
    std::string sha256;
    std::string citation_id;
    std::string source_uri;
    std::size_t byte_size = 0;
};

struct UiContentBlock {
    UiContentBlockKind kind = UiContentBlockKind::Paragraph;
    std::string text;
    /** Fence info string (for example cpp or mermaid). */
    std::string info;
    int heading_level = 0;
    std::vector<std::vector<std::string>> table_cells;
    std::string attachment_id;
    bool stable = true;
};

struct UiTurn {
    UiTurnRole role = UiTurnRole::Assistant;
    /** Compatibility mirror of raw_markdown; remove after downstream migration. */
    std::string content;
    std::string raw_markdown;
    /** Displayable provider summary only; never raw internal chain-of-thought. */
    std::string thinking_raw;
    std::vector<UiContentBlock> blocks;
    std::vector<UiAttachment> attachments;
    std::int64_t timestamp_ms = 0;
    bool streaming = false;
    bool error = false;
};

struct UiToolActivity {
    std::string tool_call_id;
    std::string tool_name;
    json arguments = json::object();
    json result = json::object();
    UiRunState state = UiRunState::Running;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    std::int64_t duration_ms = 0;
};

struct UiPresentationSnapshot {
    UiRunState run_state = UiRunState::Idle;
    std::string session_id = "default";
    std::string model;
    std::string provider;
    std::string connection_label = "Ready";
    std::string last_error;
    std::vector<UiTurn> turns;
    std::vector<UiToolActivity> tools;
};

class UiPresentationModel {
public:
    explicit UiPresentationModel(std::size_t max_turns = 80, std::size_t max_tools = 120,
                                 std::size_t max_text_bytes = 256 * 1024);

    void set_runtime_metadata(std::string session_id, std::string provider, std::string model,
                              std::string connection_label);
    void begin_user_turn(std::string prompt);
    void append_stream_token(std::string_view token);
    void append_thinking_token(std::string_view token);
    bool observe_artifact(const json& payload);
    void complete(const json& result);
    void fail(std::string message);
    void cancel(std::string message = "Run cancelled");
    void add_system_notice(std::string message, bool error = false);
    void observe_tool(const ToolExecutionEvent& event);
    void reset();
    void load_demo_state();

    UiPresentationSnapshot snapshot() const;

    static const char* state_name(UiRunState state) noexcept;
    static bool result_is_error(const json& result);

private:
    static std::int64_t now_ms();
    static void append_utf8_capped(std::string& dst, std::string_view text, std::size_t cap);
    void ensure_assistant_turn_locked();
    void trim_locked();

    mutable std::mutex mutex_;
    UiPresentationSnapshot state_;
    std::size_t max_turns_;
    std::size_t max_tools_;
    std::size_t max_text_bytes_;
};

} // namespace agent_framework

#endif

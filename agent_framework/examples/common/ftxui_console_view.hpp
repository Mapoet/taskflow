#pragma once

#include <agent/ui/presentation_model.hpp>
#include <agent/ui/native_workbench.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework::example {

/**
 * Wrap UTF-8 text by terminal display cells, never by bytes.  The result is
 * safe for CJK text without spaces, emoji and combining sequences, and also
 * provides a deterministic hard-break fallback for URLs and filesystem paths.
 */
std::vector<std::string> wrap_terminal_text(std::string_view text,
                                            int display_columns,
                                            bool preserve_leading_space = false);

struct FtxuiSkillStatus {
    bool enabled{false};
    std::size_t count{0};
    std::uint64_t generation{0};
    std::size_t diagnostics{0};
    std::size_t errors{0};
    std::string root;
    std::string active{"-"};
};

struct FtxuiConsoleCallbacks {
    std::function<void(std::string)> on_submit;
    std::function<void()> on_cancel;
    std::function<void()> on_quit;
    std::function<void(const ui::NativeWorkbenchSnapshot&)> on_session_change;
};

class FtxuiConsoleView {
public:
    using SnapshotProvider = std::function<UiPresentationSnapshot()>;
    using SkillStatusProvider = std::function<FtxuiSkillStatus()>;

    FtxuiConsoleView(SnapshotProvider snapshot_provider,
                     SkillStatusProvider skill_status_provider,
                     FtxuiConsoleCallbacks callbacks,
                     std::shared_ptr<ui::NativeWorkbenchController> workbench = {});
    ~FtxuiConsoleView();

    FtxuiConsoleView(const FtxuiConsoleView&) = delete;
    FtxuiConsoleView& operator=(const FtxuiConsoleView&) = delete;

    int run();
    void set_busy(bool busy) noexcept;
    void request_refresh();

    static std::string render_for_test(const UiPresentationSnapshot& snapshot,
                                       const FtxuiSkillStatus& skills,
                                       bool busy,
                                       int width,
                                       int height);
    static std::string render_operations_for_test(const UiPresentationSnapshot& snapshot,
                                                  const FtxuiSkillStatus& skills,
                                                  int width,
                                                  int height);
    static std::string render_workbench_for_test(const UiPresentationSnapshot& snapshot,
                                                 const FtxuiSkillStatus& skills,
                                                 const ui::NativeWorkbenchSnapshot& workbench,
                                                 int width,
                                                 int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace agent_framework::example

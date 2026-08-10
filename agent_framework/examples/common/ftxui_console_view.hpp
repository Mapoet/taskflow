#pragma once

#include <agent/ui/presentation_model.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace agent_framework::example {

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
};

class FtxuiConsoleView {
public:
    using SnapshotProvider = std::function<UiPresentationSnapshot()>;
    using SkillStatusProvider = std::function<FtxuiSkillStatus()>;

    FtxuiConsoleView(SnapshotProvider snapshot_provider,
                     SkillStatusProvider skill_status_provider,
                     FtxuiConsoleCallbacks callbacks);
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace agent_framework::example

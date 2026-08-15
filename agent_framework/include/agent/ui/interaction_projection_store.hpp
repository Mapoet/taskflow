#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/ui/interaction_graph.hpp"

namespace agent_framework::ui {

enum class InteractionCommitStatus { Committed, AlreadyExists, RevisionConflict,
    Invalid, Corrupt, Error };
struct InteractionCommitResult {
    InteractionCommitStatus status{InteractionCommitStatus::Error};
    std::uint64_t revision{0}, head_sequence{0};
    std::string digest, error;
    bool ok() const noexcept { return status==InteractionCommitStatus::Committed ||
        status==InteractionCommitStatus::AlreadyExists; }
};
struct InteractionCommit {
    UiInteractionEvent event;
    std::vector<InteractionNode> nodes;
    std::vector<InteractionEdge> edges;
};

class InteractionProjectionStore {
public:
    virtual ~InteractionProjectionStore() = default;
    virtual InteractionCommitResult commit(const InteractionCommit&,std::uint64_t expected_revision)=0;
    virtual std::optional<InteractionSnapshot> snapshot(std::string_view tenant,
        std::string_view conversation,InteractionVisibility viewer)=0;
    virtual std::vector<UiInteractionEvent> events(std::string_view tenant,
        std::string_view conversation,std::uint64_t after,std::size_t limit,
        InteractionVisibility viewer)=0;
    virtual std::optional<InteractionNode> node(std::string_view tenant,
        std::string_view conversation,std::string_view node_id,InteractionVisibility viewer)=0;
};

class SQLiteInteractionProjectionStore final : public InteractionProjectionStore {
public:
    explicit SQLiteInteractionProjectionStore(std::string path);
    ~SQLiteInteractionProjectionStore() override;
    SQLiteInteractionProjectionStore(const SQLiteInteractionProjectionStore&)=delete;
    SQLiteInteractionProjectionStore& operator=(const SQLiteInteractionProjectionStore&)=delete;
    InteractionCommitResult commit(const InteractionCommit&,std::uint64_t) override;
    std::optional<InteractionSnapshot> snapshot(std::string_view,std::string_view,
        InteractionVisibility) override;
    std::vector<UiInteractionEvent> events(std::string_view,std::string_view,std::uint64_t,
        std::size_t,InteractionVisibility) override;
    std::optional<InteractionNode> node(std::string_view,std::string_view,std::string_view,
        InteractionVisibility) override;
private:
    void migrate();
    std::string path_; void* db_{nullptr}; std::mutex mutex_;
};

} // namespace agent_framework::ui

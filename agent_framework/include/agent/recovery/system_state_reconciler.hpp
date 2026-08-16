#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent/conversation/store.hpp"
#include "agent/harness/store.hpp"

namespace agent_framework::recovery {

enum class ReconciliationDisposition {
    Recoverable,
    FailTerminal,
    AwaitingExternal,
    ManualReview
};

struct ReconciliationScope {
    conversation::ConversationIdentity conversation;
    std::size_t limit{256};
};

struct ReconciliationFinding {
    std::string code;
    ReconciliationDisposition disposition{ReconciliationDisposition::ManualReview};
    std::string turn_id;
    std::string harness_id;
    std::uint64_t turn_revision{0};
    std::uint64_t harness_revision{0};
    std::string reason;
};

struct ReconciliationPlan {
    ReconciliationScope scope;
    std::vector<ReconciliationFinding> findings;
    std::string digest;
};

struct ReconciliationApplyResult {
    std::size_t inspected{0};
    std::size_t changed{0};
    std::size_t conflicts{0};
    std::size_t manual_review{0};
    std::vector<std::string> errors;
};

class SystemStateReconciler {
public:
    SystemStateReconciler(conversation::ConversationStore& conversations,
                          harness::HarnessStore& harnesses)
        : conversations_(conversations), harnesses_(harnesses) {}

    ReconciliationPlan scan(const ReconciliationScope& scope) const;
    ReconciliationApplyResult apply(const ReconciliationPlan& plan,
                                    bool dry_run = true) const;

private:
    conversation::ConversationStore& conversations_;
    harness::HarnessStore& harnesses_;
};

std::string_view name(ReconciliationDisposition disposition) noexcept;

} // namespace agent_framework::recovery

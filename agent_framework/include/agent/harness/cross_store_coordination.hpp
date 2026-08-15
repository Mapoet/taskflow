#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/harness/runtime.hpp"
#include "agent/harness/store.hpp"
#include "agent/run/store.hpp"
#include "agent/approval/store.hpp"
#include "agent/memory_v2/store.hpp"
#include "agent/assurance/professional_workflow.hpp"
#include "agent/conversation/task_registry.hpp"

namespace agent_framework::harness {

enum class CoordinationState { Prepared, Committing, Confirming, Confirmed,
                               Compensating, Compensated, ManualReview };

struct ParticipantPin {
    std::string participant_id;
    std::uint64_t revision{0};
    std::string digest;
    bool reversible{false};
};

struct CrossStoreOperation {
    std::string operation_id;
    std::string tenant_id;
    std::string run_id;
    std::string harness_id;
    std::string operation_kind;
    std::string idempotency_key;
    std::string policy_revision;
    std::map<std::string,std::string> expected_refs;
    std::vector<ParticipantPin> participants;
};

struct CoordinationReceipt {
    CrossStoreOperation operation;
    CoordinationState state{CoordinationState::Prepared};
    std::uint64_t transition_sequence{0};
    std::string previous_digest;
    std::string receipt_digest;
    std::string diagnostic;
};

class CrossStoreParticipant {
public:
    virtual ~CrossStoreParticipant() = default;
    virtual std::string id() const = 0;
    virtual std::string capability_manifest_digest() const = 0;
    virtual bool supports(const CrossStoreOperation&) const noexcept = 0;
    virtual std::optional<ParticipantPin> inspect(const CrossStoreOperation& operation,
                                                   std::string* error) = 0;
    virtual bool prepare(const CrossStoreOperation&, const ParticipantPin&,
                         std::string* error) = 0;
    virtual bool commit(const CrossStoreOperation&, const ParticipantPin&,
                        std::string* error) = 0;
    virtual bool confirm(const CrossStoreOperation&, const ParticipantPin&,
                         std::string* error) = 0;
    virtual bool compensate(const CrossStoreOperation&, const ParticipantPin&,
                            std::string* error) = 0;
};

class RunStoreCoordinationParticipant final : public CrossStoreParticipant {
public:
    explicit RunStoreCoordinationParticipant(run::RunStore& store,
                                              std::string revision);
    std::string id() const override { return "run_store"; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    bool supports(const CrossStoreOperation& value) const noexcept override {
        return !value.run_id.empty();
    }
    std::optional<ParticipantPin> inspect(const CrossStoreOperation&,std::string*) override;
    bool prepare(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool commit(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool confirm(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
private: run::RunStore& store_; std::string manifest_digest_;
};

class HarnessStoreCoordinationParticipant final : public CrossStoreParticipant {
public:
    explicit HarnessStoreCoordinationParticipant(HarnessStore& store,
                                                  std::string revision);
    std::string id() const override { return "harness_store"; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    bool supports(const CrossStoreOperation& value) const noexcept override {
        return !value.tenant_id.empty()&&!value.harness_id.empty();
    }
    std::optional<ParticipantPin> inspect(const CrossStoreOperation&,std::string*) override;
    bool prepare(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool commit(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool confirm(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
private: HarnessStore& store_; std::string manifest_digest_;
};

class ConversationTaskCoordinationParticipant final : public CrossStoreParticipant {
public:
    explicit ConversationTaskCoordinationParticipant(
        conversation::TaskRegistry& registry, std::string revision);
    std::string id() const override { return "conversation_task_registry"; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    bool supports(const CrossStoreOperation& value) const noexcept override;
    std::optional<ParticipantPin> inspect(const CrossStoreOperation&,
                                           std::string*) override;
    bool prepare(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool commit(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool confirm(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
private:
    conversation::TaskRegistry& registry_;
    std::string manifest_digest_;
};

class ApprovalStoreCoordinationParticipant final : public CrossStoreParticipant {
public:
    explicit ApprovalStoreCoordinationParticipant(approval::ApprovalStore& store,
                                                   std::string revision);
    std::string id() const override { return "approval_store"; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    bool supports(const CrossStoreOperation& value) const noexcept override;
    std::optional<ParticipantPin> inspect(const CrossStoreOperation&,std::string*) override;
    bool prepare(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool commit(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool confirm(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
private: approval::ApprovalStore& store_; std::string manifest_digest_;
};

class MemoryStoreCoordinationParticipant final : public CrossStoreParticipant {
public:
    explicit MemoryStoreCoordinationParticipant(memory_v2::MemoryStore& store,
                                                 std::string revision);
    std::string id() const override { return "memory_store"; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    bool supports(const CrossStoreOperation& value) const noexcept override;
    std::optional<ParticipantPin> inspect(const CrossStoreOperation&,std::string*) override;
    bool prepare(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool commit(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool confirm(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
private: memory_v2::MemoryStore& store_; std::string manifest_digest_;
};

class AssuranceStoreCoordinationParticipant final : public CrossStoreParticipant {
public:
    explicit AssuranceStoreCoordinationParticipant(assurance::AssuranceStore& store,
                                                    std::string revision);
    std::string id() const override { return "assurance_store"; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    bool supports(const CrossStoreOperation& value) const noexcept override;
    std::optional<ParticipantPin> inspect(const CrossStoreOperation&,std::string*) override;
    bool prepare(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool commit(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool confirm(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*) override;
private: assurance::AssuranceStore& store_; std::string manifest_digest_;
};

class SQLiteCoordinationJournal {
public:
    explicit SQLiteCoordinationJournal(std::string path);
    ~SQLiteCoordinationJournal();
    SQLiteCoordinationJournal(const SQLiteCoordinationJournal&) = delete;
    SQLiteCoordinationJournal& operator=(const SQLiteCoordinationJournal&) = delete;
    bool create(const CrossStoreOperation&, std::string* error = nullptr);
    bool transition(std::string_view operation_id, CoordinationState expected,
                    CoordinationState next, std::string_view diagnostic = {},
                    std::string* error = nullptr);
    std::optional<CoordinationReceipt> load(std::string_view operation_id);
    std::vector<CoordinationReceipt> unresolved(std::size_t limit);
private:
    void migrate();
    void* db_{nullptr};
    std::mutex mutex_;
};

class CrossStoreCoordinator final : public HarnessCheckpointObserver {
public:
    CrossStoreCoordinator(SQLiteCoordinationJournal& journal,
                          std::string policy_revision,
                          std::shared_ptr<HarnessCheckpointObserver> downstream = {});
    bool register_participant(std::shared_ptr<CrossStoreParticipant> participant);
    bool production_ready(std::vector<std::string>* issues = nullptr) const;
    std::string capability_manifest_digest() const;
    bool execute(CrossStoreOperation operation, std::string* error = nullptr);
    bool reconcile(std::string_view operation_id, std::string* error = nullptr);
    std::size_t reconcile_unresolved(std::size_t limit);
    bool committed(const HarnessCheckpoint&, std::string_view event_type,
                   std::string* error = nullptr) override;
private:
    bool drive(const CoordinationReceipt&, std::string* error);
    SQLiteCoordinationJournal& journal_;
    std::string policy_revision_;
    std::shared_ptr<HarnessCheckpointObserver> downstream_;
    mutable std::mutex mutex_;
    std::map<std::string,std::shared_ptr<CrossStoreParticipant>> participants_;
};

} // namespace agent_framework::harness

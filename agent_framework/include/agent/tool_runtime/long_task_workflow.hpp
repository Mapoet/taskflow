#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "agent/llm_runtime/runtime.hpp"
#include "agent/planning/plan_store.hpp"
#include "agent/tool_runtime/event_stream.hpp"
#include "agent/tool_runtime/execution_adapter.hpp"
#include "agent/run/store.hpp"

namespace agent_framework::tool_runtime
{
    enum class LongTaskState
    {
        Created,
        RunningReadyNodes,
        WaitingForEvents,
        CognitionRequired,
        Replanning,
        AwaitingApproval,
        CompletedCandidate,
        Failed,
        Cancelled,
        ManualReview
    };
    enum class PlanNodeState
    {
        Pending,
        Ready,
        Running,
        CompletedCandidate,
        EffectCommitted,
        Blocked,
        Failed,
        Cancelled
    };
    enum class ReplanTrigger
    {
        None,
        MeaningfulEvidence,
        Stall,
        Failure,
        BudgetDeviation,
        DependencyChange,
        ApprovalOrInput,
        IntegrityFailure,
        HumanInstruction
    };
    enum class LongTaskDecisionKind
    {
        ContinueWaiting,
        LaunchReadyNodes,
        RequestInput,
        RequestApproval,
        RevisePlan,
        CancelNode,
        ReconcileEffect,
        ManualReview
    };

    struct PlanNodeRuntime
    {
        std::string node_id;
        PlanNodeState state{PlanNodeState::Pending};
        std::uint64_t attempt{0};
        std::string invocation_id, result_digest, effect_receipt_digest;
    };
    struct InvocationWatch
    {
        std::string invocation_id;
        std::uint64_t cursor{0};
        std::string last_meaningful_digest;
        std::int64_t last_information_gain_ms{0};
    };
    struct LongTaskBudget
    {
        std::uint64_t wall_time_ms{0}, tool_calls{0}, llm_calls{0}, tokens{0};
        double cost_usd{0};
    };
    struct LongTaskCheckpoint
    {
        contracts::ContractMetadata metadata;
        std::string workflow_id, conversation_id, turn_id;
        std::uint64_t revision{1}, fencing_token{0}, plan_revision{0};
        LongTaskState state{LongTaskState::Created};
        std::string plan_digest, wake_reason;
        std::map<std::string, PlanNodeRuntime> nodes;
        std::map<std::string, InvocationWatch> watches;
        LongTaskBudget consumed, limit;
        std::vector<std::string> cognition_invocation_ids;
        std::string last_observation_digest, created_at, updated_at;
    };
    struct LongTaskEvent
    {
        std::string workflow_id, event_type;
        std::uint64_t sequence{0}, workflow_revision{0}, fencing_token{0};
        nlohmann::json payload = nlohmann::json::object();
        std::string previous_digest, event_digest, created_at;
    };
    struct ObservationBatch
    {
        std::vector<InvocationEvent> events;
        ReplanTrigger trigger{ReplanTrigger::None};
        bool information_gain{false};
        std::string digest;
    };
    struct LongTaskDecision
    {
        LongTaskDecisionKind kind{LongTaskDecisionKind::ContinueWaiting};
        nlohmann::json proposal = nlohmann::json::object();
        std::string rationale;
        llm_runtime::LLMInvocationManifest manifest;
    };
    struct LongTaskCommitResult
    {
        bool committed{false};
        std::uint64_t revision{0};
        std::string error;
        explicit operator bool() const noexcept { return committed; }
    };

    nlohmann::json encode(const LongTaskCheckpoint &);
    std::optional<LongTaskCheckpoint> decode_long_task(const nlohmann::json &);
    std::string_view name(LongTaskState);
    std::string_view name(PlanNodeState);
    std::string_view name(ReplanTrigger);

    class LongTaskStore
    {
    public:
        virtual ~LongTaskStore() = default;
        virtual LongTaskCommitResult create(const LongTaskCheckpoint &) = 0;
        virtual std::optional<LongTaskCheckpoint> load(std::string_view) = 0;
        virtual LongTaskCommitResult commit(const LongTaskCheckpoint &, std::uint64_t, const LongTaskEvent &) = 0;
        virtual std::vector<LongTaskEvent> events(std::string_view, std::uint64_t = 0) = 0;
        virtual std::vector<LongTaskCheckpoint> recoverable(std::size_t) = 0;
    };
    class SQLiteLongTaskStore final : public LongTaskStore
    {
    public:
        explicit SQLiteLongTaskStore(std::string path);
        ~SQLiteLongTaskStore();
        LongTaskCommitResult create(const LongTaskCheckpoint &) override;
        std::optional<LongTaskCheckpoint> load(std::string_view) override;
        LongTaskCommitResult commit(const LongTaskCheckpoint &, std::uint64_t, const LongTaskEvent &) override;
        std::vector<LongTaskEvent> events(std::string_view, std::uint64_t) override;
        std::vector<LongTaskCheckpoint> recoverable(std::size_t) override;

    private:
        void migrate();
        void *db_{nullptr};
        std::mutex mutex_;
    };

    struct ObservationPolicy
    {
        std::int64_t stall_after_ms{300000};
        double budget_warning_ratio{0.8};
    };
    class ObservationClassifier
    {
    public:
        explicit ObservationClassifier(ObservationPolicy p = {}) : policy_(p) {}
        ObservationBatch classify(const LongTaskCheckpoint &, std::vector<InvocationEvent>, std::int64_t now_ms) const;

    private:
        ObservationPolicy policy_;
    };
    struct DagSnapshot
    {
        bool valid{false};
        std::vector<std::string> ready, running, blocked, terminal;
        std::string error;
    };
    DagSnapshot schedule_dag(const planning::ExecutionPlan &, const LongTaskCheckpoint &);
    bool validate_revision(const planning::ExecutionPlan &, const planning::ExecutionPlan &,
                           const LongTaskCheckpoint &, std::string *);
    struct LongTaskStepResult;
    class LongTaskWorkflow;

    struct PlanNodeExecutionDescriptor
    {
        contracts::ContractMetadata metadata;
        std::string plan_digest, node_id, executor_id, executor_revision;
        nlohmann::json input = nlohmann::json::object();
        std::vector<std::string> granted_capabilities;
        std::string approval_decision_id, descriptor_digest;
        bool side_effecting{false};
    };
    class PlanNodeInputRepository
    {
    public:
        virtual ~PlanNodeInputRepository() = default;
        virtual std::optional<PlanNodeExecutionDescriptor> descriptor(
            const contracts::ContractIdentity &, std::string_view plan_digest,
            std::string_view node_id) = 0;
    };
    enum class PlanNodeExecutorOrigin { Production, Test, Scripted };
    struct PlanNodeExecutionResult
    {
        bool started{false};
        std::string invocation_id, external_operation_id, error;
    };
    class ProductionPlanNodeExecutor
    {
    public:
        virtual ~ProductionPlanNodeExecutor() = default;
        virtual std::string id() const = 0;
        virtual std::string revision() const = 0;
        virtual PlanNodeExecutorOrigin origin() const noexcept = 0;
        virtual PlanNodeExecutionResult start(const LongTaskCheckpoint &,
            const planning::PlanNode &, const PlanNodeExecutionDescriptor &) = 0;
    };
    class PlanNodeExecutorRegistry
    {
    public:
        explicit PlanNodeExecutorRegistry(bool production = true) : production_(production) {}
        bool register_executor(std::shared_ptr<ProductionPlanNodeExecutor>, std::string * = nullptr);
        std::shared_ptr<ProductionPlanNodeExecutor> find(std::string_view, std::string_view) const;
        bool production_ready() const;
    private:
        bool production_; mutable std::mutex mutex_;
        std::map<std::string, std::shared_ptr<ProductionPlanNodeExecutor>> executors_;
    };
    class AdapterPlanNodeExecutor final : public ProductionPlanNodeExecutor
    {
    public:
        AdapterPlanNodeExecutor(std::string id, std::string revision,
            std::shared_ptr<ExecutionAdapter> adapter, InvocationStore &store);
        std::string id() const override { return id_; }
        std::string revision() const override { return revision_; }
        PlanNodeExecutorOrigin origin() const noexcept override { return PlanNodeExecutorOrigin::Production; }
        PlanNodeExecutionResult start(const LongTaskCheckpoint &, const planning::PlanNode &,
                                      const PlanNodeExecutionDescriptor &) override;
    private:
        std::string id_, revision_; std::shared_ptr<ExecutionAdapter> adapter_; InvocationStore &store_;
    };
    class LongTaskDispatcher
    {
    public:
        LongTaskDispatcher(LongTaskStore &, planning::PlanStore &, PlanNodeInputRepository &,
                           PlanNodeExecutorRegistry &);
        LongTaskStepResult dispatch(const LongTaskStepResult &);
        LongTaskStepResult reconcile(std::string_view, InvocationStore &);
    private:
        LongTaskStore &store_; planning::PlanStore &plans_; PlanNodeInputRepository &inputs_;
        PlanNodeExecutorRegistry &executors_;
    };
    class LongTaskTimerWorker
    {
    public:
        LongTaskTimerWorker(run::RunStore &, LongTaskWorkflow &, LongTaskDispatcher &,
                            std::string owner, std::int64_t lease_ms);
        run::StoreResult schedule(const LongTaskCheckpoint &, std::int64_t due_ms,
                                  std::string reason);
        std::size_t run_due(std::int64_t now_ms, std::size_t limit = 64);
    private:
        run::RunStore &runs_; LongTaskWorkflow &workflow_; LongTaskDispatcher &dispatcher_;
        std::string owner_; std::int64_t lease_ms_;
    };

    class LongTaskCognitionModel
    {
    public:
        virtual ~LongTaskCognitionModel() = default;
        virtual LongTaskDecision invoke(const LongTaskCheckpoint &, const planning::ExecutionPlan &,
                                        const ObservationBatch &) = 0;
    };
    struct LongTaskRoleBinding
    {
        std::string profile_id, profile_revision, memory_snapshot_id, memory_view_digest;
        std::vector<std::string> capabilities;
    };
    class RoleRuntimeLongTaskModel final : public LongTaskCognitionModel
    {
    public:
        RoleRuntimeLongTaskModel(std::shared_ptr<llm_runtime::RoleRuntime>, LongTaskRoleBinding);
        LongTaskDecision invoke(const LongTaskCheckpoint &, const planning::ExecutionPlan &, const ObservationBatch &) override;

    private:
        std::shared_ptr<llm_runtime::RoleRuntime> runtime_;
        LongTaskRoleBinding binding_;
    };

    struct LongTaskStepResult
    {
        LongTaskCheckpoint checkpoint;
        std::vector<std::string> ready_nodes;
        bool llm_invoked{false};
        std::string error;
    };
    class LongTaskWorkflow
    {
    public:
        LongTaskWorkflow(LongTaskStore &, planning::PlanStore &, InvocationStore &, LongTaskCognitionModel &,
                         ObservationClassifier = ObservationClassifier(ObservationPolicy{}));
        LongTaskCommitResult start(LongTaskCheckpoint, const planning::ExecutionPlan &);
        LongTaskStepResult step(std::string_view, std::int64_t now_ms, std::size_t event_limit = 256);
        LongTaskStepResult wait_and_step(std::string_view, InvocationEventStreamHub &,
                                         std::chrono::milliseconds,
                                         std::int64_t now_ms, std::size_t event_limit = 256);

    private:
        LongTaskStore &store_;
        planning::PlanStore &plans_;
        InvocationStore &invocations_;
        LongTaskCognitionModel &model_;
        ObservationClassifier classifier_;
    };
}

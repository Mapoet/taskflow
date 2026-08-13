#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include "agent/agent_template/types.hpp"
#include "agent/toolbus/toolbus.hpp"
#include "agent/agent/child_task.hpp"
#include "agent/approval/store.hpp"
namespace agent_framework::agent_template {
enum class RunnerOrigin { Production, TestCallback };
struct RunnerRequest { AgentTemplateInvocation invocation; ActiveSkillSession session; SkillPlanNode node; nlohmann::json input=nlohmann::json::object(); std::string checkpoint_ref; std::shared_ptr<std::atomic_bool> cancel; };
struct RunnerResult { bool ok{false}; nlohmann::json output=nlohmann::json::object(); SkillRunnerReceipt receipt; std::vector<RunnerEvent> events; std::string error_code; std::string error_message; };
class SkillRunner { public: virtual ~SkillRunner()=default; virtual SkillRunnerKind kind()const noexcept=0;virtual RunnerOrigin origin()const noexcept=0; virtual RunnerResult run(const RunnerRequest&)=0; virtual RunnerResult attach(const RunnerRequest& r){return run(r);} virtual bool cancel(std::string_view)=0; virtual RunnerResult reconcile(const RunnerRequest&r){return attach(r);} };
using RunnerCallback=std::function<nlohmann::json(const RunnerRequest&)>;
class CallbackSkillRunner final:public SkillRunner { public: CallbackSkillRunner(SkillRunnerKind kind,RunnerCallback callback);SkillRunnerKind kind()const noexcept override{return kind_;}RunnerOrigin origin()const noexcept override{return RunnerOrigin::TestCallback;}RunnerResult run(const RunnerRequest&)override;bool cancel(std::string_view)override; private:SkillRunnerKind kind_;RunnerCallback callback_;std::mutex mutex_;std::map<std::string,std::shared_ptr<std::atomic_bool>>active_;};
class ToolBusSkillRunner final:public SkillRunner { public:ToolBusSkillRunner(SkillRunnerKind,std::shared_ptr<ToolBus>);SkillRunnerKind kind()const noexcept override{return kind_;}RunnerOrigin origin()const noexcept override{return RunnerOrigin::Production;}RunnerResult run(const RunnerRequest&)override;bool cancel(std::string_view)override;private:SkillRunnerKind kind_;std::shared_ptr<ToolBus>bus_;std::mutex mutex_;std::map<std::string,std::shared_ptr<std::atomic_bool>>active_;};
class NestedWorkflowPort { public:virtual ~NestedWorkflowPort()=default;virtual RunnerResult execute(const RunnerRequest&)=0;virtual bool cancel(std::string_view)=0;};
class ChildTaskSkillRunner final:public SkillRunner { public:explicit ChildTaskSkillRunner(std::shared_ptr<ChildTaskBackend>);SkillRunnerKind kind()const noexcept override{return SkillRunnerKind::ChildAgent;}RunnerOrigin origin()const noexcept override{return RunnerOrigin::Production;}RunnerResult run(const RunnerRequest&)override;bool cancel(std::string_view)override;private:std::shared_ptr<ChildTaskBackend>backend_;std::mutex mutex_;std::map<std::string,ChildTaskHandle*>active_;};
class NestedWorkflowSkillRunner final:public SkillRunner { public:explicit NestedWorkflowSkillRunner(std::shared_ptr<NestedWorkflowPort>);SkillRunnerKind kind()const noexcept override{return SkillRunnerKind::NestedWorkflow;}RunnerOrigin origin()const noexcept override{return RunnerOrigin::Production;}RunnerResult run(const RunnerRequest&r)override{return port_->execute(r);}bool cancel(std::string_view id)override{return port_->cancel(id);}private:std::shared_ptr<NestedWorkflowPort>port_;};
class ApprovalSkillRunner final:public SkillRunner { public:explicit ApprovalSkillRunner(approval::ApprovalStore&);SkillRunnerKind kind()const noexcept override{return SkillRunnerKind::HumanApproval;}RunnerOrigin origin()const noexcept override{return RunnerOrigin::Production;}RunnerResult run(const RunnerRequest&)override;bool cancel(std::string_view)override{return false;}private:approval::ApprovalStore&store_;};
class SkillRunnerRegistry { public:explicit SkillRunnerRegistry(bool production=false):production_(production){}bool register_runner(std::shared_ptr<SkillRunner>);std::shared_ptr<SkillRunner> resolve(SkillRunnerKind)const; private:bool production_;mutable std::mutex mutex_;std::map<SkillRunnerKind,std::shared_ptr<SkillRunner>>runners_;};
struct ProductionRunnerDependencies {std::shared_ptr<ToolBus>toolbus;std::shared_ptr<ChildTaskBackend>child_tasks;std::shared_ptr<NestedWorkflowPort>nested_workflows;approval::ApprovalStore*approvals{nullptr};};
std::shared_ptr<SkillRunnerRegistry> build_production_runners(ProductionRunnerDependencies);
std::shared_ptr<SkillRunnerRegistry> build_production_toolbus_runners(std::shared_ptr<ToolBus> bus);
}

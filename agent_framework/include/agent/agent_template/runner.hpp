#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include "agent/agent_template/types.hpp"
namespace agent_framework::agent_template {
struct RunnerRequest { AgentTemplateInvocation invocation; ActiveSkillSession session; SkillPlanNode node; nlohmann::json input=nlohmann::json::object(); std::string checkpoint_ref; std::shared_ptr<std::atomic_bool> cancel; };
struct RunnerResult { bool ok{false}; nlohmann::json output=nlohmann::json::object(); SkillRunnerReceipt receipt; std::vector<RunnerEvent> events; std::string error_code; std::string error_message; };
class SkillRunner { public: virtual ~SkillRunner()=default; virtual SkillRunnerKind kind()const noexcept=0; virtual RunnerResult run(const RunnerRequest&)=0; virtual RunnerResult attach(const RunnerRequest& r){return run(r);} virtual bool cancel(std::string_view)=0; virtual RunnerResult reconcile(const RunnerRequest&r){return attach(r);} };
using RunnerCallback=std::function<nlohmann::json(const RunnerRequest&)>;
class CallbackSkillRunner final:public SkillRunner { public: CallbackSkillRunner(SkillRunnerKind kind,RunnerCallback callback);SkillRunnerKind kind()const noexcept override{return kind_;}RunnerResult run(const RunnerRequest&)override;bool cancel(std::string_view)override; private:SkillRunnerKind kind_;RunnerCallback callback_;std::mutex mutex_;std::map<std::string,std::shared_ptr<std::atomic_bool>>active_;};
class SkillRunnerRegistry { public: bool register_runner(std::shared_ptr<SkillRunner>);std::shared_ptr<SkillRunner> resolve(SkillRunnerKind)const; private:mutable std::mutex mutex_;std::map<SkillRunnerKind,std::shared_ptr<SkillRunner>>runners_;};
}

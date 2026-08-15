#include "agent/agent_template/compiler.hpp"
#include "agent/agent_template/session.hpp"
#include "agent/toolbus/schema_validate.hpp"
#include <algorithm>
#include <future>
#include <queue>
#include <set>
#include <limits>
namespace agent_framework::agent_template
{
    namespace
    {
        nlohmann::json map_input(const SkillPlanNode &n, const nlohmann::json &root, const std::map<std::string, nlohmann::json> &outputs)
        {
            nlohmann::json out = nlohmann::json::object();
            if (n.input_mapping.empty())
                return root;
            for (const auto &[key, m] : n.input_mapping.items())
            {
                if (m.is_object() && m.contains("value"))
                {
                    if (m.contains("from") || m.contains("path"))
                        throw std::runtime_error("literal input mapping cannot also specify from/path: " + key);
                    out[key] = m.at("value");
                    continue;
                }
                const auto from = m.value("from", "$input"), path = m.value("path", "");
                const nlohmann::json *source = &root;
                if (from != "$input")
                {
                    auto i = outputs.find(from);
                    if (i == outputs.end())
                        throw std::runtime_error("mapped predecessor unavailable: " + from);
                    source = &i->second;
                }
                out[key] = path.empty() ? *source : source->at(nlohmann::json::json_pointer(path));
            }
            return out;
        }
        ActiveSkillSession narrow(const ActiveSkillSession&s,const SkillPlanNode&node) {
            auto out=s;PermissionEnvelope grant=s.effective_permissions;
            if(node.resolved_skill_id)for(const auto&pin:s.skills)if(pin.skill_id==*node.resolved_skill_id){grant=intersect_permissions(grant,pin.effective_permissions);break;}
            grant=intersect_permissions(grant,node.requested_permissions);out.effective_permissions=std::move(grant);return out;
        }
        std::string snapshot_digest(nlohmann::json value){value.erase("snapshot_digest");return contracts::canonical_digest(value).value_or("");}
        bool edge_enabled(const SkillPlanEdge&e,const nlohmann::json&value) {
            if(e.condition.empty()||e.condition=="true")return true;
            if(e.condition=="false")return false;
            if(e.condition.front()!='/')throw std::runtime_error("unsupported edge condition: "+e.condition);
            const auto&selected=value.at(nlohmann::json::json_pointer(e.condition));
            return selected.is_boolean()?selected.get<bool>():!selected.is_null()&&!selected.empty();
        }
    }
    CompiledExecutionResult SkillWorkflowCompiler::execute(const SkillCollaborationPlan &p, const AgentTemplateInvocation &i, const ActiveSkillSession &s, const nlohmann::json &input, std::shared_ptr<std::atomic_bool> cancel, const nlohmann::json &resume_snapshot) const
    {
        CompiledExecutionResult out;
        if (!runners_)
        {
            out.error_code = "runner_registry_missing";
            return out;
        }
        cancel = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
        std::map<std::string, SkillPlanNode> nodes;
        std::map<std::string, std::vector<std::string>> next;
        std::map<std::pair<std::string,std::string>,std::string> conditions;
        std::map<std::string, std::size_t> degree;
        std::size_t completed = 0;
        for (const auto &n : p.nodes)
        {
            nodes[n.node_id] = n;
            degree[n.node_id] = 0;
        }
        for (const auto &e : p.edges)
        {
            next[e.from].push_back(e.to);
            conditions[{e.from,e.to}]=e.condition;
            ++degree[e.to];
        }
        if(!resume_snapshot.empty()) {
            if(!resume_snapshot.is_object()||resume_snapshot.value("plan_digest",std::string{})!=i.plan_digest||
               resume_snapshot.value("invocation_id",std::string{})!=i.invocation_id||resume_snapshot.value("session_id",std::string{})!=s.session_id||
               resume_snapshot.value("snapshot_digest",std::string{})!=snapshot_digest(resume_snapshot)){
                out.error_code="resume_snapshot_integrity_failed";return out;}
        }
        if(resume_snapshot.is_object()&&resume_snapshot.contains("node_outputs")) {
            for(const auto&[id,value]:resume_snapshot.at("node_outputs").items())if(nodes.count(id))out.node_outputs[id]=value;
            for(const auto&[id,_]:out.node_outputs){++completed;for(const auto&target:next[id])if(degree[target])--degree[target];degree[id]=std::numeric_limits<std::size_t>::max();}
        }
        std::set<std::string> ready;
        for (const auto &[id, d] : degree)
            if (d == 0)
                ready.insert(id);
        while (!ready.empty())
        {
            if (cancel->load())
            {
                out.error_code = "cancelled";
                out.error_message = "execution cancelled";
                return out;
            }
            std::vector<std::string> batch;
            while (!ready.empty() && batch.size() < std::max<std::uint64_t>(1, p.budget.max_parallelism))
            {
                batch.push_back(*ready.begin());
                ready.erase(ready.begin());
            }
            std::vector<std::future<std::pair<std::string, RunnerResult>>> futures;
            for (const auto &id : batch)
            {
                const auto node = nodes.at(id);
                auto runner = runners_->resolve(node.runner);
                if (!runner)
                {
                    out.error_code = "runner_unavailable";
                    out.error_message = to_string(node.runner);
                    return out;
                }
                nlohmann::json arguments;
                try
                {
                    arguments = map_input(node, input, out.node_outputs);
                }
                catch (const std::exception &error)
                {
                    out.error_code = "input_mapping_failed";
                    out.error_message = error.what();
                    return out;
                }
                auto node_session=narrow(s,node);
                futures.push_back(std::async(std::launch::async, [=, &i]
                                             {RunnerRequest r{i,node_session,node,arguments,"",cancel};RunnerResult result;
                                              const auto attempts=std::max<std::uint64_t>(1,node.max_attempts);
                                              if(attempts>1&&node.effect!=EffectClass::ReadOnly&&node.idempotency_key.empty()){
                                                  result.error_code="idempotency_key_required";result.error_message="retrying effectful node requires idempotency_key";return std::make_pair(id,result);}
                                              for(std::uint64_t attempt=1;attempt<=attempts;++attempt){result=runner->run(r);if(result.ok||cancel->load())break;}
                                              return std::make_pair(id,result); }));
            }
            for (auto &future : futures)
            {
                auto [id, result] = future.get();
                out.events.insert(out.events.end(), result.events.begin(), result.events.end());
                out.receipts.push_back(result.receipt);
                if (!result.ok)
                {
                    if(result.receipt.terminal_state==RunnerLifecycleState::Waiting ||
                       result.receipt.terminal_state==RunnerLifecycleState::Checkpointed) {
                        out.suspended=true;out.checkpoint_ref=result.receipt.checkpoint_ref;
                        out.resume_snapshot={{"plan_digest",i.plan_digest},{"invocation_id",i.invocation_id},{"session_id",s.session_id},{"waiting_node",id},{"checkpoint_ref",out.checkpoint_ref},{"node_outputs",out.node_outputs},{"plan",encode(p)},{"session",encode(s)},{"input",input}};
                        out.resume_snapshot["snapshot_digest"]=snapshot_digest(out.resume_snapshot);
                        out.error_code=result.error_code.empty()?"execution_suspended":result.error_code;
                        out.error_message=result.error_message;
                        return out;
                    }
                    const auto&failed_node=nodes.at(id);
                    if(failed_node.failure_policy=="continue"&&!failed_node.required) {
                        out.node_outputs[id]={{"skipped",true},{"error_code",result.error_code}};
                        ++completed;
                        for(const auto&target:next[id])if(--degree[target]==0)ready.insert(target);
                        continue;
                    }
                    out.error_code = result.error_code;
                    out.error_message = result.error_message;
                    cancel->store(true);
                    return out;
                }
                const auto&completed_node=nodes.at(id);
                if(!completed_node.output.schema.empty()) {
                    nlohmann::json schema_error;
                    if(!validate_json_instance(completed_node.output.schema,result.output,schema_error)) {
                        out.error_code="output_contract_failed";out.error_message=schema_error.dump();cancel->store(true);return out;
                    }
                }
                if(completed_node.output.artifact_required&&result.receipt.artifacts.empty()) {out.error_code="artifact_required";cancel->store(true);return out;}
                if(completed_node.output.evidence_required&&result.receipt.evidence.empty()) {out.error_code="evidence_required";cancel->store(true);return out;}
                out.node_outputs[id] = std::move(result.output);
                ++completed;
                for (const auto &target : next[id]) {
                    SkillPlanEdge edge{id,target,conditions[{id,target}]};
                    if(!edge_enabled(edge,out.node_outputs[id])) {nodes[target].required=false;nodes[target].failure_policy="condition_skipped";}
                    if (--degree[target] == 0) {
                        if(nodes[target].failure_policy=="condition_skipped") {out.node_outputs[target]={{"skipped",true},{"reason","condition_false"}};++completed;for(const auto&child:next[target])if(--degree[child]==0)ready.insert(child);}
                        else ready.insert(target);
                    }
                }
            }
        }
        if (completed != nodes.size())
        {
            out.error_code = "plan_not_executable";
            return out;
        }
        out.ok = true;
        if (p.output_assembly.contains("from"))
        {
            auto found = out.node_outputs.find(p.output_assembly.at("from").get<std::string>());
            if (found != out.node_outputs.end())
                out.output = found->second;
        }
        else
        {
            for (const auto &[id, value] : out.node_outputs)
                out.output[id] = value;
        }
        return out;
    }
}

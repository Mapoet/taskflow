#include "agent/agent_template/compiler.hpp"
#include <algorithm>
#include <future>
#include <queue>
#include <set>
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
    }
    CompiledExecutionResult SkillWorkflowCompiler::execute(const SkillCollaborationPlan &p, const AgentTemplateInvocation &i, const ActiveSkillSession &s, const nlohmann::json &input, std::shared_ptr<std::atomic_bool> cancel) const
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
        std::map<std::string, std::size_t> degree;
        for (const auto &n : p.nodes)
        {
            nodes[n.node_id] = n;
            degree[n.node_id] = 0;
        }
        for (const auto &e : p.edges)
        {
            next[e.from].push_back(e.to);
            ++degree[e.to];
        }
        std::set<std::string> ready;
        for (const auto &[id, d] : degree)
            if (d == 0)
                ready.insert(id);
        std::size_t completed = 0;
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
                auto arguments = map_input(node, input, out.node_outputs);
                futures.push_back(std::async(std::launch::async, [=, &i, &s]
                                             {RunnerRequest r{i,s,node,arguments,"",cancel};return std::make_pair(id,runner->run(r)); }));
            }
            for (auto &future : futures)
            {
                auto [id, result] = future.get();
                out.events.insert(out.events.end(), result.events.begin(), result.events.end());
                out.receipts.push_back(result.receipt);
                if (!result.ok)
                {
                    out.error_code = result.error_code;
                    out.error_message = result.error_message;
                    cancel->store(true);
                    return out;
                }
                out.node_outputs[id] = std::move(result.output);
                ++completed;
                for (const auto &target : next[id])
                    if (--degree[target] == 0)
                        ready.insert(target);
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

#include "agent/tool_runtime/long_task_workflow.hpp"
#include "agent/internal/platform_io.hpp"
#include "agent/internal/sqlite_utils.hpp"
#include <algorithm>
#include <filesystem>
#include <queue>
#include <sqlite3.h>
namespace agent_framework::tool_runtime
{
    namespace s = internal::sqlite;
    using json = nlohmann::json;
    namespace
    {
        std::string digest(const json &j) { return contracts::embedded_digest(j).value_or(""); }
        bool terminal_node(PlanNodeState s) { return s == PlanNodeState::CompletedCandidate || s == PlanNodeState::EffectCommitted || s == PlanNodeState::Failed || s == PlanNodeState::Cancelled; }
        std::string decision_name(LongTaskDecisionKind k)
        {
            static const char *v[] = {"continue_waiting", "launch_ready_nodes", "request_input", "request_approval", "revise_plan", "cancel_node", "reconcile_effect", "manual_review"};
            return v[static_cast<int>(k)];
        }
        LongTaskDecisionKind decision_kind(std::string_view v)
        {
            for (int i = 0; i < 8; ++i)
                if (decision_name(static_cast<LongTaskDecisionKind>(i)) == v)
                    return static_cast<LongTaskDecisionKind>(i);
            return LongTaskDecisionKind::ManualReview;
        }
    }
    std::string_view name(LongTaskState v)
    {
        static const char *a[] = {"created", "running_ready_nodes", "waiting_for_events", "cognition_required", "replanning", "awaiting_approval", "completed_candidate", "failed", "cancelled", "manual_review"};
        return a[static_cast<int>(v)];
    }
    std::string_view name(PlanNodeState v)
    {
        static const char *a[] = {"pending", "ready", "running", "completed_candidate", "effect_committed", "blocked", "failed", "cancelled"};
        return a[static_cast<int>(v)];
    }
    std::string_view name(ReplanTrigger v)
    {
        static const char *a[] = {"none", "meaningful_evidence", "stall", "failure", "budget_deviation", "dependency_change", "approval_or_input", "integrity_failure", "human_instruction"};
        return a[static_cast<int>(v)];
    }
    json encode(const LongTaskCheckpoint &v)
    {
        json nodes = json::object(), watches = json::object();
        for (auto &[k, n] : v.nodes)
            nodes[k] = {{"node_id", n.node_id}, {"state", name(n.state)}, {"attempt", n.attempt}, {"invocation_id", n.invocation_id}, {"result_digest", n.result_digest}, {"effect_receipt_digest", n.effect_receipt_digest}};
        for (auto &[k, w] : v.watches)
            watches[k] = {{"invocation_id", w.invocation_id}, {"cursor", w.cursor}, {"last_meaningful_digest", w.last_meaningful_digest}, {"last_information_gain_ms", w.last_information_gain_ms}};
        return contracts::make_contract_json(v.metadata, {{"workflow_id", v.workflow_id}, {"conversation_id", v.conversation_id}, {"turn_id", v.turn_id}, {"revision", v.revision}, {"fencing_token", v.fencing_token}, {"plan_revision", v.plan_revision}, {"state", name(v.state)}, {"plan_digest", v.plan_digest}, {"wake_reason", v.wake_reason}, {"nodes", nodes}, {"watches", watches}, {"consumed", {{"wall_time_ms", v.consumed.wall_time_ms}, {"tool_calls", v.consumed.tool_calls}, {"llm_calls", v.consumed.llm_calls}, {"tokens", v.consumed.tokens}, {"cost_usd", v.consumed.cost_usd}}}, {"limit", {{"wall_time_ms", v.limit.wall_time_ms}, {"tool_calls", v.limit.tool_calls}, {"llm_calls", v.limit.llm_calls}, {"tokens", v.limit.tokens}, {"cost_usd", v.limit.cost_usd}}}, {"cognition_invocation_ids", v.cognition_invocation_ids}, {"last_observation_digest", v.last_observation_digest}, {"created_at", v.created_at}, {"updated_at", v.updated_at}});
    }
    std::optional<LongTaskCheckpoint> decode_long_task(const json &j)
    {
        try
        {
            LongTaskCheckpoint v;
            auto m = contracts::metadata_from_contract_json(j, {"workflow_id", "revision", "state", "nodes", "watches"}, {"workflow_id", "conversation_id", "turn_id", "revision", "fencing_token", "plan_revision", "state", "plan_digest", "wake_reason", "nodes", "watches", "consumed", "limit", "cognition_invocation_ids", "last_observation_digest", "created_at", "updated_at"}, {});
            if (!m)
                return {};
            v.metadata = *m;
            auto st = j.at("state").get<std::string>();
            for (int i = 0; i < 10; ++i)
                if (name(static_cast<LongTaskState>(i)) == st)
                    v.state = static_cast<LongTaskState>(i);
            v.workflow_id = j.at("workflow_id");
            v.conversation_id = j.value("conversation_id", "");
            v.turn_id = j.value("turn_id", "");
            v.revision = j.at("revision");
            v.fencing_token = j.value("fencing_token", 0ull);
            v.plan_revision = j.value("plan_revision", 0ull);
            v.plan_digest = j.value("plan_digest", "");
            v.wake_reason = j.value("wake_reason", "");
            for (auto &[k, x] : j.at("nodes").items())
            {
                PlanNodeRuntime n;
                n.node_id = x.at("node_id");
                auto ns = x.at("state").get<std::string>();
                for (int i = 0; i < 8; ++i)
                    if (name(static_cast<PlanNodeState>(i)) == ns)
                        n.state = static_cast<PlanNodeState>(i);
                n.attempt = x.value("attempt", 0ull);
                n.invocation_id = x.value("invocation_id", "");
                n.result_digest = x.value("result_digest", "");
                n.effect_receipt_digest = x.value("effect_receipt_digest", "");
                v.nodes[k] = n;
            }
            for (auto &[k, x] : j.at("watches").items())
                v.watches[k] = {x.at("invocation_id"), x.value("cursor", 0ull), x.value("last_meaningful_digest", ""), x.value("last_information_gain_ms", 0ll)};
            auto budget = [&](const char *k, LongTaskBudget &b)
            {if(!j.contains(k))return;auto&x=j.at(k);b={x.value("wall_time_ms",0ull),x.value("tool_calls",0ull),x.value("llm_calls",0ull),x.value("tokens",0ull),x.value("cost_usd",0.0)}; };
            budget("consumed", v.consumed);
            budget("limit", v.limit);
            v.cognition_invocation_ids = j.value("cognition_invocation_ids", std::vector<std::string>{});
            v.last_observation_digest = j.value("last_observation_digest", "");
            v.created_at = j.value("created_at", "");
            v.updated_at = j.value("updated_at", "");
            return v;
        }
        catch (...)
        {
            return {};
        }
    }
    SQLiteLongTaskStore::SQLiteLongTaskStore(std::string p)
    {
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error("open long task store failed");
        db_ = db;
        migrate();
#if !defined(_WIN32)
        std::error_code ec;
        std::filesystem::permissions(p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
        if (ec)
            throw std::runtime_error("unable to set private long task store permissions:" + ec.message());
#endif
    }
    SQLiteLongTaskStore::~SQLiteLongTaskStore() { sqlite3_close(s::database(db_)); }
    void SQLiteLongTaskStore::migrate()
    {
        auto d = s::database(db_);
        s::exec(d, "PRAGMA journal_mode=WAL");
        s::exec(d, "CREATE TABLE IF NOT EXISTS long_task_checkpoints(workflow_id TEXT PRIMARY KEY,revision INTEGER NOT NULL,state TEXT NOT NULL,document TEXT NOT NULL)");
        s::exec(d, "CREATE TABLE IF NOT EXISTS long_task_events(workflow_id TEXT,sequence INTEGER,workflow_revision INTEGER,fencing_token INTEGER,event_type TEXT,payload TEXT,previous_digest TEXT,event_digest TEXT,created_at TEXT,PRIMARY KEY(workflow_id,sequence))");
    }
    LongTaskCommitResult SQLiteLongTaskStore::create(const LongTaskCheckpoint &v)
    {
        std::lock_guard l(mutex_);
        auto d = s::database(db_);
        try
        {
            s::Statement q(d, "INSERT INTO long_task_checkpoints VALUES(?,?,?,?)");
            s::bind_text(q.get(), 1, v.workflow_id);
            s::bind_uint64(q.get(), 2, v.revision);
            s::bind_text(q.get(), 3, name(v.state));
            s::bind_text(q.get(), 4, encode(v).dump());
            if (s::step(q.get()) != SQLITE_DONE)
                return {false, 0, sqlite3_errmsg(d)};
            return {true, v.revision, {}};
        }
        catch (const std::exception &e)
        {
            return {false, 0, e.what()};
        }
    }
    std::optional<LongTaskCheckpoint> SQLiteLongTaskStore::load(std::string_view id)
    {
        std::lock_guard l(mutex_);
        s::Statement q(s::database(db_), "SELECT document FROM long_task_checkpoints WHERE workflow_id=?");
        s::bind_text(q.get(), 1, id);
        return s::step(q.get()) == SQLITE_ROW ? decode_long_task(json::parse(s::column_text(q.get(), 0))) : std::nullopt;
    }
    LongTaskCommitResult SQLiteLongTaskStore::commit(const LongTaskCheckpoint &v, std::uint64_t expected, const LongTaskEvent &e)
    {
        std::lock_guard l(mutex_);
        auto d = s::database(db_);
        try
        {
            s::Transaction tx(d);
            s::Statement current(d, "SELECT document FROM long_task_checkpoints WHERE workflow_id=? AND revision=?");
            s::bind_text(current.get(), 1, v.workflow_id);
            s::bind_uint64(current.get(), 2, expected);
            if (s::step(current.get()) != SQLITE_ROW)
                return {false, expected, "revision conflict"};
            const auto prior = decode_long_task(json::parse(s::column_text(current.get(), 0)));
            if (!prior || prior->fencing_token != e.fencing_token || v.fencing_token != e.fencing_token)
                return {false, expected, "fencing rejected"};
            s::Statement h(d, "SELECT COALESCE(MAX(sequence),0),COALESCE((SELECT event_digest FROM long_task_events WHERE workflow_id=? ORDER BY sequence DESC LIMIT 1),'') FROM long_task_events WHERE workflow_id=?");
            s::bind_text(h.get(), 1, v.workflow_id);
            s::bind_text(h.get(), 2, v.workflow_id);
            s::step(h.get());
            auto seq = s::column_uint64(h.get(), 0) + 1;
            auto prev = s::column_text(h.get(), 1);
            auto ed = digest({{"workflow_id", v.workflow_id}, {"sequence", seq}, {"revision", v.revision}, {"fencing_token", e.fencing_token}, {"event_type", e.event_type}, {"payload", e.payload}, {"previous_digest", prev}});
            s::Statement u(d, "UPDATE long_task_checkpoints SET revision=?,state=?,document=? WHERE workflow_id=? AND revision=?");
            s::bind_uint64(u.get(), 1, v.revision);
            s::bind_text(u.get(), 2, name(v.state));
            s::bind_text(u.get(), 3, encode(v).dump());
            s::bind_text(u.get(), 4, v.workflow_id);
            s::bind_uint64(u.get(), 5, expected);
            if (s::step(u.get()) != SQLITE_DONE || s::changes(d) != 1)
                return {false, expected, "revision conflict"};
            s::Statement i(d, "INSERT INTO long_task_events VALUES(?,?,?,?,?,?,?,?,?)");
            s::bind_text(i.get(), 1, v.workflow_id);
            s::bind_uint64(i.get(), 2, seq);
            s::bind_uint64(i.get(), 3, v.revision);
            s::bind_uint64(i.get(), 4, e.fencing_token);
            s::bind_text(i.get(), 5, e.event_type);
            s::bind_text(i.get(), 6, e.payload.dump());
            s::bind_text(i.get(), 7, prev);
            s::bind_text(i.get(), 8, ed);
            s::bind_text(i.get(), 9, e.created_at);
            if (s::step(i.get()) != SQLITE_DONE)
                throw std::runtime_error(sqlite3_errmsg(d));
            tx.commit();
            return {true, v.revision, {}};
        }
        catch (const std::exception &x)
        {
            return {false, expected, x.what()};
        }
    }
    std::vector<LongTaskEvent> SQLiteLongTaskStore::events(std::string_view id, std::uint64_t after)
    {
        std::lock_guard l(mutex_);
        s::Statement q(s::database(db_), "SELECT sequence,workflow_revision,fencing_token,event_type,payload,previous_digest,event_digest,created_at FROM long_task_events WHERE workflow_id=? AND sequence>? ORDER BY sequence");
        s::bind_text(q.get(), 1, id);
        s::bind_uint64(q.get(), 2, after);
        std::vector<LongTaskEvent> o;
        while (s::step(q.get()) == SQLITE_ROW)
        {
            LongTaskEvent e;
            e.workflow_id = id;
            e.sequence = s::column_uint64(q.get(), 0);
            e.workflow_revision = s::column_uint64(q.get(), 1);
            e.fencing_token = s::column_uint64(q.get(), 2);
            e.event_type = s::column_text(q.get(), 3);
            e.payload = json::parse(s::column_text(q.get(), 4));
            e.previous_digest = s::column_text(q.get(), 5);
            e.event_digest = s::column_text(q.get(), 6);
            e.created_at = s::column_text(q.get(), 7);
            o.push_back(std::move(e));
        }
        return o;
    }
    std::vector<LongTaskCheckpoint> SQLiteLongTaskStore::recoverable(std::size_t n)
    {
        std::lock_guard l(mutex_);
        s::Statement q(s::database(db_), "SELECT document FROM long_task_checkpoints WHERE state NOT IN('completed_candidate','failed','cancelled','manual_review') LIMIT ?");
        s::bind_uint64(q.get(), 1, n);
        std::vector<LongTaskCheckpoint> o;
        while (s::step(q.get()) == SQLITE_ROW)
            if (auto v = decode_long_task(json::parse(s::column_text(q.get(), 0))))
                o.push_back(*v);
        return o;
    }
    ObservationBatch ObservationClassifier::classify(const LongTaskCheckpoint &c, std::vector<InvocationEvent> events, std::int64_t now) const
    {
        ObservationBatch b;
        b.events = std::move(events);
        for (auto &e : b.events)
        {
            if (e.event_type.find("integrity") != std::string::npos)
            {
                b.trigger = ReplanTrigger::IntegrityFailure;
                break;
            }
            if (e.event_type.find("failed") != std::string::npos || e.event_type.find("unknown") != std::string::npos)
            {
                b.trigger = ReplanTrigger::Failure;
                break;
            }
            if (e.event_type.find("approval") != std::string::npos || e.event_type.find("input") != std::string::npos)
                b.trigger = ReplanTrigger::ApprovalOrInput;
            if (e.information_gain)
            {
                b.information_gain = true;
                if (b.trigger == ReplanTrigger::None)
                    b.trigger = ReplanTrigger::MeaningfulEvidence;
            }
        }
        if (b.trigger == ReplanTrigger::None)
            for (auto &[_, w] : c.watches)
                if (w.last_information_gain_ms && now - w.last_information_gain_ms >= policy_.stall_after_ms)
                {
                    b.trigger = ReplanTrigger::Stall;
                    break;
                }
        auto ratio = [](auto used, auto lim)
        { return lim ? double(used) / double(lim) : 0.; };
        if (b.trigger == ReplanTrigger::None && (ratio(c.consumed.wall_time_ms, c.limit.wall_time_ms) >= policy_.budget_warning_ratio || ratio(c.consumed.tool_calls, c.limit.tool_calls) >= policy_.budget_warning_ratio || ratio(c.consumed.llm_calls, c.limit.llm_calls) >= policy_.budget_warning_ratio))
            b.trigger = ReplanTrigger::BudgetDeviation;
        json ids = json::array();
        for (auto &e : b.events)
            ids.push_back({e.invocation_id, e.sequence, e.event_digest});
        b.digest = digest(ids);
        return b;
    }
    DagSnapshot schedule_dag(const planning::ExecutionPlan &p, const LongTaskCheckpoint &c)
    {
        DagSnapshot o;
        std::set<std::string> ids;
        for (auto &n : p.nodes)
            if (!ids.insert(n.node_id).second)
            {
                o.error = "duplicate node";
                return o;
            }
        std::map<std::string, int> deg;
        std::map<std::string, std::vector<std::string>> out;
        for (auto &n : p.nodes)
        {
            deg[n.node_id] = n.dependencies.size();
            for (auto &d : n.dependencies)
            {
                if (!ids.count(d))
                {
                    o.error = "missing dependency:" + d;
                    return o;
                }
                out[d].push_back(n.node_id);
            }
        }
        std::queue<std::string> q;
        for (auto &[k, d] : deg)
            if (!d)
                q.push(k);
        std::size_t seen = 0;
        while (!q.empty())
        {
            auto x = q.front();
            q.pop();
            ++seen;
            for (auto &y : out[x])
                if (!--deg[y])
                    q.push(y);
        }
        if (seen != ids.size())
        {
            o.error = "dependency cycle";
            return o;
        }
        for (auto &n : p.nodes)
        {
            auto it = c.nodes.find(n.node_id);
            auto state = it == c.nodes.end() ? PlanNodeState::Pending : it->second.state;
            if (terminal_node(state))
            {
                o.terminal.push_back(n.node_id);
                continue;
            }
            if (state == PlanNodeState::Running)
            {
                o.running.push_back(n.node_id);
                continue;
            }
            bool ready = true;
            for (auto &d : n.dependencies)
            {
                auto di = c.nodes.find(d);
                if (di == c.nodes.end() || (di->second.state != PlanNodeState::CompletedCandidate && di->second.state != PlanNodeState::EffectCommitted))
                {
                    ready = false;
                    break;
                }
            }
            (ready ? o.ready : o.blocked).push_back(n.node_id);
        }
        o.valid = true;
        return o;
    }
    bool validate_revision(const planning::ExecutionPlan &old, const planning::ExecutionPlan &next, const LongTaskCheckpoint &c, std::string *e)
    {
        if (next.plan_revision != old.plan_revision + 1 || next.parent_plan_digest != encode(old).at("canonical_digest"))
        {
            if (e)
                *e = "parent/revision mismatch";
            return false;
        }
        auto find = [&](std::string_view id) -> const planning::PlanNode *
        {for(auto&n:next.nodes)if(n.node_id==id)return &n;return nullptr; };
        for (auto &[id, r] : c.nodes)
            if (r.state == PlanNodeState::EffectCommitted || r.state == PlanNodeState::CompletedCandidate)
            {
                auto *n = find(id);
                auto *o = ([&]() -> const planning::PlanNode *
                           {for(auto&x:old.nodes)if(x.node_id==id)return &x;return nullptr; })();
                if (!n || !o || n->objective != o->objective || n->side_effects != o->side_effects)
                {
                    if (e)
                        *e = "committed/completed node changed:" + id;
                    return false;
                }
            }
        if (!schedule_dag(next, c).valid)
        {
            if (e)
                *e = "invalid revised DAG";
            return false;
        }
        if (next.budget.wall_time_ms > old.budget.wall_time_ms || next.budget.token_budget > old.budget.token_budget || next.budget.tool_calls > old.budget.tool_calls || next.budget.cost_limit > old.budget.cost_limit)
        {
            if (e)
                *e = "budget expansion denied";
            return false;
        }
        return true;
    }
    bool PlanNodeExecutorRegistry::register_executor(
        std::shared_ptr<ProductionPlanNodeExecutor> executor, std::string *error)
    {
        if (!executor || executor->id().empty() || executor->revision().empty() ||
            (production_ && executor->origin() != PlanNodeExecutorOrigin::Production))
        {
            if (error) *error = "production executor identity/origin invalid";
            return false;
        }
        std::lock_guard lock(mutex_);
        return executors_.emplace(executor->id() + "\x1f" + executor->revision(),
                                  std::move(executor)).second;
    }
    std::shared_ptr<ProductionPlanNodeExecutor> PlanNodeExecutorRegistry::find(
        std::string_view id, std::string_view revision) const
    {
        std::lock_guard lock(mutex_);
        const auto found = executors_.find(std::string(id) + "\x1f" + std::string(revision));
        return found == executors_.end() ? nullptr : found->second;
    }
    bool PlanNodeExecutorRegistry::production_ready() const
    {
        std::lock_guard lock(mutex_);
        return !executors_.empty() && std::all_of(executors_.begin(), executors_.end(),
            [](const auto &entry) { return entry.second->origin() == PlanNodeExecutorOrigin::Production; });
    }
    AdapterPlanNodeExecutor::AdapterPlanNodeExecutor(std::string id, std::string revision,
        std::shared_ptr<ExecutionAdapter> adapter, InvocationStore &store)
        : id_(std::move(id)), revision_(std::move(revision)),
          adapter_(std::move(adapter)), store_(store) {}
    PlanNodeExecutionResult AdapterPlanNodeExecutor::start(
        const LongTaskCheckpoint &workflow, const planning::PlanNode &node,
        const PlanNodeExecutionDescriptor &descriptor)
    {
        if (!adapter_ || adapter_->revision() != descriptor.executor_revision)
            return {false, {}, {}, "pinned execution adapter unavailable"};
        LongRunningToolInvocation invocation;
        invocation.metadata = workflow.metadata;
        invocation.invocation_id = workflow.workflow_id + ":" + node.node_id + ":" +
                                   std::to_string(workflow.plan_revision);
        invocation.conversation_id = workflow.conversation_id;
        invocation.turn_id = workflow.turn_id;
        invocation.tool_call_id = node.node_id;
        invocation.tool_name = adapter_->id();
        invocation.tool_contract_revision = descriptor.executor_revision;
        invocation.deployment_revision = adapter_->revision();
        invocation.tool_generation = adapter_->deployment_generation();
        invocation.input_digest = contracts::canonical_digest(descriptor.input).value_or("");
        invocation.lease.fencing_token = workflow.fencing_token;
        invocation.idempotent = !descriptor.side_effecting;
        invocation.created_at = invocation.updated_at = workflow.updated_at;
        const auto created = store_.create(invocation);
        if (!created && created.status != InvocationStoreStatus::AlreadyExists)
            return {false, {}, {}, created.error};
        ExecutionRequest request{invocation, descriptor.input,
            workflow.workflow_id + ":" + workflow.plan_digest + ":" + node.node_id,
            {}, workflow.fencing_token};
        std::string error;
        const auto handle = adapter_->start(request, &error);
        if (!handle) return {false, invocation.invocation_id, {}, error};
        return {true, invocation.invocation_id, handle->external_id, {}};
    }
    LongTaskDispatcher::LongTaskDispatcher(LongTaskStore &store, planning::PlanStore &plans,
        PlanNodeInputRepository &inputs, PlanNodeExecutorRegistry &executors)
        : store_(store), plans_(plans), inputs_(inputs), executors_(executors) {}
    LongTaskStepResult LongTaskDispatcher::dispatch(const LongTaskStepResult &step)
    {
        auto out = step;
        auto checkpoint = store_.load(step.checkpoint.workflow_id);
        if (!checkpoint) { out.error = "workflow not found"; return out; }
        auto plan = plans_.current(checkpoint->metadata.identity);
        if (!plan || planning::encode(*plan).at("canonical_digest") != checkpoint->plan_digest)
        { out.error = "pinned plan unavailable"; return out; }
        const auto prior = checkpoint->revision;
        for (const auto &node_id : step.ready_nodes)
        {
            auto runtime = checkpoint->nodes.find(node_id);
            const auto plan_node = std::find_if(plan->nodes.begin(), plan->nodes.end(),
                [&](const auto &node) { return node.node_id == node_id; });
            if (runtime == checkpoint->nodes.end() || plan_node == plan->nodes.end() ||
                runtime->second.state == PlanNodeState::EffectCommitted ||
                runtime->second.state == PlanNodeState::Running) continue;
            const auto descriptor = inputs_.descriptor(checkpoint->metadata.identity,
                                                        checkpoint->plan_digest, node_id);
            if (!descriptor || descriptor->descriptor_digest !=
                contracts::canonical_digest({{"plan_digest",descriptor->plan_digest},
                    {"node_id",descriptor->node_id},{"executor_id",descriptor->executor_id},
                    {"executor_revision",descriptor->executor_revision},{"input",descriptor->input},
                    {"granted_capabilities",descriptor->granted_capabilities},
                    {"approval_decision_id",descriptor->approval_decision_id},
                    {"side_effecting",descriptor->side_effecting}}).value_or(""))
            { runtime->second.state=PlanNodeState::Blocked; out.error="typed execution descriptor missing/invalid:"+node_id; continue; }
            if (descriptor->side_effecting && plan_node->approval_required &&
                descriptor->approval_decision_id.empty())
            { runtime->second.state=PlanNodeState::Blocked; out.error="approval missing:"+node_id; continue; }
            bool capabilities=true; for(const auto &required:plan_node->required_capabilities)
                if(std::find(descriptor->granted_capabilities.begin(),descriptor->granted_capabilities.end(),required)==descriptor->granted_capabilities.end())capabilities=false;
            auto executor=executors_.find(descriptor->executor_id,descriptor->executor_revision);
            if(!capabilities||!executor){runtime->second.state=PlanNodeState::Blocked;out.error=!capabilities?"capability denied:"+node_id:"executor unavailable:"+node_id;continue;}
            const auto launched=executor->start(*checkpoint,*plan_node,*descriptor);
            if(!launched.started){runtime->second.state=PlanNodeState::Failed;out.error=launched.error;continue;}
            runtime->second.state=PlanNodeState::Running;runtime->second.attempt++;
            runtime->second.invocation_id=launched.invocation_id;
            checkpoint->watches[launched.invocation_id]={launched.invocation_id,0,{},0};
        }
        checkpoint->revision++;
        const auto committed=store_.commit(*checkpoint,prior,{checkpoint->workflow_id,
            "long_task_nodes_dispatched",0,checkpoint->revision,checkpoint->fencing_token,
            {{"ready_nodes",step.ready_nodes}}, {},{},checkpoint->updated_at});
        if(!committed) out.error=committed.error;
        out.checkpoint=*checkpoint;
        return out;
    }
    LongTaskStepResult LongTaskDispatcher::reconcile(std::string_view id, InvocationStore &invocations)
    {
        LongTaskStepResult out;auto checkpoint=store_.load(id);if(!checkpoint){out.error="workflow not found";return out;}
        const auto prior=checkpoint->revision;
        for(auto &[_,node]:checkpoint->nodes){if(node.invocation_id.empty())continue;auto invocation=invocations.load(node.invocation_id);if(!invocation)continue;
            switch(invocation->state){case InvocationState::CompletedCandidate:node.state=PlanNodeState::CompletedCandidate;break;case InvocationState::EffectCommitted:case InvocationState::Verified:node.state=PlanNodeState::EffectCommitted;node.effect_receipt_digest="committed";break;case InvocationState::Failed:node.state=PlanNodeState::Failed;break;case InvocationState::Cancelled:node.state=PlanNodeState::Cancelled;break;case InvocationState::Reconciling:case InvocationState::Orphaned:case InvocationState::ManualReview:checkpoint->state=LongTaskState::ManualReview;break;default:break;}}
        auto plan=plans_.current(checkpoint->metadata.identity);if(plan){auto dag=schedule_dag(*plan,*checkpoint);out.ready_nodes=dag.ready;}
        checkpoint->revision++;const auto committed=store_.commit(*checkpoint,prior,{checkpoint->workflow_id,"long_task_nodes_reconciled",0,checkpoint->revision,checkpoint->fencing_token,{{"ready_nodes",out.ready_nodes}}, {},{},checkpoint->updated_at});if(!committed)out.error=committed.error;out.checkpoint=*checkpoint;return out;
    }
    LongTaskTimerWorker::LongTaskTimerWorker(run::RunStore &runs,LongTaskWorkflow &workflow,
        LongTaskDispatcher &dispatcher,std::string owner,std::int64_t lease_ms)
        :runs_(runs),workflow_(workflow),dispatcher_(dispatcher),owner_(std::move(owner)),lease_ms_(lease_ms){}
    run::StoreResult LongTaskTimerWorker::schedule(const LongTaskCheckpoint &c,std::int64_t due,std::string reason)
    {return runs_.schedule_timer({"long-task:"+c.workflow_id+":"+std::to_string(c.revision)+":"+std::to_string(due),c.metadata.identity.run_id,due,{{"kind","long_task_wake"},{"workflow_id",c.workflow_id},{"workflow_revision",c.revision},{"fencing_token",c.fencing_token},{"reason",std::move(reason)}},{},0,false});}
    std::size_t LongTaskTimerWorker::run_due(std::int64_t now,std::size_t limit)
    {std::size_t completed=0;for(const auto &timer:runs_.claim_due_timers(now,owner_,lease_ms_,limit)){if(timer.payload.value("kind","")!="long_task_wake")continue;auto step=workflow_.step(timer.payload.value("workflow_id",""),now);if(step.error.empty()&&step.checkpoint.revision==timer.payload.value("workflow_revision",0ull)+1&&step.checkpoint.fencing_token==timer.payload.value("fencing_token",0ull)){dispatcher_.dispatch(step);if(runs_.complete_timer(timer.timer_id,owner_))++completed;}}return completed;}
    RoleRuntimeLongTaskModel::RoleRuntimeLongTaskModel(std::shared_ptr<llm_runtime::RoleRuntime> r, LongTaskRoleBinding b) : runtime_(std::move(r)), binding_(std::move(b)) {}
    LongTaskDecision RoleRuntimeLongTaskModel::invoke(const LongTaskCheckpoint &c, const planning::ExecutionPlan &p, const ObservationBatch &b)
    {
        llm_runtime::RoleInvocationRequest r;
        r.metadata = c.metadata;
        r.invocation_id = c.workflow_id + "-cognition-" + std::to_string(c.revision);
        r.profile_id = binding_.profile_id;
        r.profile_revision = binding_.profile_revision;
        r.memory_view = {binding_.memory_snapshot_id, "replan", binding_.memory_view_digest};
        r.granted_capabilities = binding_.capabilities;
        r.prompt_variables = {{"workflow", encode(c).dump()}, {"plan", planning::encode(p).dump()}, {"observations", json{{"trigger", name(b.trigger)}, {"digest", b.digest}}.dump()}};
        auto x = runtime_->invoke(std::move(r));
        LongTaskDecision d;
        d.manifest = x.manifest;
        if (!x.ok || !x.structured_output)
        {
            d.kind = LongTaskDecisionKind::ManualReview;
            d.rationale = x.error_code;
            return d;
        }
        d.kind = decision_kind(x.structured_output->value("decision", "manual_review"));
        d.proposal = x.structured_output->value("proposal", json::object());
        d.rationale = x.structured_output->value("rationale", "");
        return d;
    }
    LongTaskWorkflow::LongTaskWorkflow(LongTaskStore &s, planning::PlanStore &p, InvocationStore &i, LongTaskCognitionModel &m, ObservationClassifier c) : store_(s), plans_(p), invocations_(i), model_(m), classifier_(c) {}
    LongTaskCommitResult LongTaskWorkflow::start(LongTaskCheckpoint c, const planning::ExecutionPlan &p)
    {
        auto dg = planning::encode(p).at("canonical_digest").get<std::string>();
        if (c.plan_digest != dg || c.plan_revision != p.plan_revision)
            return {false, 0, "plan pin mismatch"};
        for (auto &n : p.nodes)
        {
            PlanNodeRuntime r;
            r.node_id = n.node_id;
            c.nodes.try_emplace(n.node_id, std::move(r));
        }
        c.state = LongTaskState::RunningReadyNodes;
        return store_.create(c);
    }
    LongTaskStepResult LongTaskWorkflow::step(std::string_view id, std::int64_t now, std::size_t limit)
    {
        LongTaskStepResult o;
        auto c = store_.load(id);
        if (!c)
        {
            o.error = "workflow not found";
            return o;
        }
        auto p = plans_.current(c->metadata.identity);
        if (!p || planning::encode(*p).at("canonical_digest") != c->plan_digest)
        {
            c->state = LongTaskState::ManualReview;
            o.error = "pinned plan unavailable";
        }
        std::vector<InvocationEvent> all;
        for (auto &[wid, w] : c->watches)
        {
            if (w.cursor + 1 < invocations_.event_retention_floor(wid))
            {
                InvocationEvent integrity;
                integrity.invocation_id = wid;
                integrity.event_type = "cursor_integrity_failure";
                all.push_back(std::move(integrity));
                continue;
            }
            auto es = invocations_.events(wid, w.cursor, limit);
            for (auto &e : es)
            {
                w.cursor = std::max(w.cursor, e.sequence);
                if (e.information_gain)
                {
                    w.last_information_gain_ms = now;
                    w.last_meaningful_digest = e.event_digest;
                }
            }
            all.insert(all.end(), es.begin(), es.end());
        }
        std::sort(all.begin(), all.end(), [](auto &a, auto &b)
                  { return std::tie(a.invocation_id, a.sequence) < std::tie(b.invocation_id, b.sequence); });
        auto batch = classifier_.classify(*c, std::move(all), now);
        c->last_observation_digest = batch.digest;
        c->wake_reason = std::string(name(batch.trigger));
        auto dag = p ? schedule_dag(*p, *c) : DagSnapshot{};
        if (p && dag.valid)
            o.ready_nodes = dag.ready;
        if (batch.trigger != ReplanTrigger::None)
        {
            c->state = LongTaskState::CognitionRequired;
            auto d = model_.invoke(*c, *p, batch);
            o.llm_invoked = true;
            c->consumed.llm_calls++;
            if (!d.manifest.invocation_id.empty())
                c->cognition_invocation_ids.push_back(d.manifest.invocation_id);
            if (d.kind == LongTaskDecisionKind::RevisePlan)
            {
                const auto proposed = planning::decode_execution_plan(d.proposal);
                std::string revision_error;
                if (!proposed || !validate_revision(*p, *proposed, *c, &revision_error))
                {
                    c->state = LongTaskState::ManualReview;
                    o.error = proposed ? revision_error : "invalid plan proposal";
                }
                else
                {
                    const auto committed = plans_.compare_exchange(*proposed, p->plan_revision);
                    if (!committed)
                    {
                        c->state = LongTaskState::ManualReview;
                        o.error = committed.error;
                    }
                    else
                    {
                        p = *proposed;
                        c->plan_revision = p->plan_revision;
                        c->plan_digest = committed.digest;
                        c->state = LongTaskState::RunningReadyNodes;
                    }
                }
            }
            else if (d.kind == LongTaskDecisionKind::ManualReview)
                c->state = LongTaskState::ManualReview;
            else if (d.kind == LongTaskDecisionKind::RequestApproval)
                c->state = LongTaskState::AwaitingApproval;
            else
                c->state = o.ready_nodes.empty() ? LongTaskState::WaitingForEvents : LongTaskState::RunningReadyNodes;
        }
        else
            c->state = o.ready_nodes.empty() ? LongTaskState::WaitingForEvents : LongTaskState::RunningReadyNodes;
        auto expected = c->revision;
        c->revision++;
        auto cr = store_.commit(*c, expected, {c->workflow_id, "long_task_step", 0, c->revision, c->fencing_token, {{"observation_digest", batch.digest}, {"trigger", name(batch.trigger)}, {"llm_invoked", o.llm_invoked}, {"ready_nodes", o.ready_nodes}}, {}, {}, c->updated_at});
        if (!cr)
            o.error = cr.error;
        o.checkpoint = *c;
        return o;
    }
    LongTaskStepResult LongTaskWorkflow::wait_and_step(
        std::string_view id, InvocationEventStreamHub &hub,
        std::chrono::milliseconds timeout, std::int64_t now, std::size_t limit)
    {
        auto checkpoint = store_.load(id);
        if (!checkpoint)
            return {{}, {}, false, "workflow not found"};
        if (checkpoint->watches.empty())
            return step(id, now, limit);
        const auto &watch = checkpoint->watches.begin()->second;
        auto subscribed = hub.subscribe(watch.invocation_id, watch.cursor, limit);
        if (!subscribed.subscription)
            return {{}, {}, false, subscribed.error};
        InvocationEvent signal;
        const auto status = subscribed.subscription->next(signal, timeout);
        if (status == InvocationSubscriptionRead::CursorExpired ||
            status == InvocationSubscriptionRead::IntegrityFailure)
        {
            auto current = *checkpoint;
            const auto expected = current.revision;
            current.revision++;
            current.state = LongTaskState::ManualReview;
            current.wake_reason = status == InvocationSubscriptionRead::CursorExpired
                                      ? "cursor_expired" : "integrity_failure";
            const auto committed = store_.commit(current, expected,
                {current.workflow_id, "long_task_subscription_failure", 0,
                 current.revision, current.fencing_token,
                 {{"reason", current.wake_reason}}, {}, {}, current.updated_at});
            return {current, {}, false, committed ? std::string{} : committed.error};
        }
        // Subscription is only a wake signal. step() replays durable events from the
        // checkpoint cursor and commits observation plus cursor atomically.
        return step(id, now, limit);
    }
}

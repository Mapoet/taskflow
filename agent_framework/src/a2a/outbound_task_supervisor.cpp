/**
 * @file outbound_task_supervisor.cpp
 */
#include <agent/a2a/a2a_task_monitor.hpp>
#include <agent/a2a/outbound_task_supervisor.hpp>

#include <agent/a2a/jsonrpc_client.hpp>
#include <agent/context_budget/context_budget.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace agent_framework {
namespace a2a {

namespace {

std::string remote_key(const std::string& peer_id, const std::string& task_id) {
    return peer_id + std::string(1, '\x1f') + task_id;
}

const char* task_status_cstr(AgentTaskStatus s) {
    switch (s) {
    case AgentTaskStatus::PENDING:
        return "pending";
    case AgentTaskStatus::WORKING:
        return "working";
    case AgentTaskStatus::COMPLETED:
        return "completed";
    case AgentTaskStatus::FAILED:
        return "failed";
    case AgentTaskStatus::INPUT_REQUIRED:
        return "input_required";
    case AgentTaskStatus::CANCELLED:
        return "cancelled";
    }
    return "unknown";
}

std::string summarize_task_text(const AgentTask& t) {
    std::string s;
    for (const auto& m : t.messages) {
        for (const auto& p : m.parts) {
            if (p.type == AgentPart::Type::TEXT && p.text.has_value()) {
                s += *p.text;
            }
        }
    }
    return s;
}

bool env_subtask_log_enabled() {
    const char* e = std::getenv("AGENT_A2A_SUBTASK_LOG");
    if (!e || !e[0]) {
        return true;
    }
    std::string v(e);
    for (char& c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v != "none" && v != "0" && v != "off";
}

} // namespace

OutboundTaskSupervisor::OutboundTaskSupervisor(A2aPeerRegistry& registry,
                                               PeerSessionBook& session_book,
                                               OutboundSessionPolicy policy)
    : registry_(registry), session_book_(session_book), policy_(std::move(policy)) {
    if (policy_.max_parallel_a2a_submits <= 0) {
        policy_.max_parallel_a2a_submits = 1;
    }
    if (const char* e = std::getenv("AGENT_A2A_CANCEL_ON_NEW_TURN")) {
        if (e[0] == '1' || (e[0] == 't' || e[0] == 'T')) {
            policy_.cancel_all_on_new_user_turn = true;
        }
    }
}

OutboundTaskSupervisor::~OutboundTaskSupervisor() {
    std::vector<std::shared_ptr<TrackedEntry>> copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : by_handle_) {
            copy.push_back(p.second);
        }
    }
    for (const auto& e : copy) {
        e->force_stop = true;
        if (e->monitor_jt) {
            e->monitor_jt->request_stop();
        }
    }
    for (const auto& e : copy) {
        if (e->monitor_jt) {
            e->monitor_jt->join();
            e->monitor_jt.reset();
        }
    }
}

void OutboundTaskSupervisor::set_remote_log(
    std::function<void(std::string_view peer_id, std::string_view line)> fn) {
    remote_log_ = std::move(fn);
}

void OutboundTaskSupervisor::set_structured_log(std::function<void(const json&)> fn) {
    structured_log_ = std::move(fn);
}

void OutboundTaskSupervisor::push_event_locked(std::unique_lock<std::mutex>& /*lk*/, json ev) {
    const std::uint64_t s = ++event_seq_;
    ev["seq"] = s;
    ev["ts_ms"] = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    event_ring_.push_back(std::move(ev));
    while (event_ring_.size() > ring_cap_) {
        event_ring_.pop_front();
    }
}

void OutboundTaskSupervisor::emit_subtask_line(const std::string& peer_id,
                                               const std::string& local_handle,
                                               const std::string& remote_task_id,
                                               const std::string& status,
                                               const std::string& outcome) {
    if (!env_subtask_log_enabled()) {
        return;
    }
    json j;
    j["component"] = "subtask_event";
    j["peer_id"] = peer_id;
    j["local_handle"] = local_handle;
    j["task_id"] = remote_task_id;
    j["status"] = status;
    j["outcome"] = outcome;
    if (structured_log_) {
        structured_log_(j);
    } else {
        std::clog << "[a2a_subtask] " << j.dump() << '\n';
    }
}

std::shared_ptr<OutboundTaskSupervisor::TrackedEntry> OutboundTaskSupervisor::find_by_handle(
    const std::string& h) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_handle_.find(h);
    if (it == by_handle_.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<OutboundTaskSupervisor::TrackedEntry> OutboundTaskSupervisor::find_by_remote(
    const std::string& peer_id,
    const std::string& task_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_remote_.find(remote_key(peer_id, task_id));
    if (it == by_remote_.end()) {
        return nullptr;
    }
    return it->second.lock();
}

json OutboundTaskSupervisor::tool_submit(const json& args, const A2aToolRegistrationOptions& reg_opts) {
    const std::string peer_id = args.at("peer_id").get<std::string>();
    const std::string user_text = args.at("user_text").get<std::string>();
    const bool continue_session = args.value("continue_session", true);
    const bool do_monitor = args.value("monitor", true);
    json meta_in = json::object();
    if (args.contains("metadata") && args["metadata"].is_object()) {
        meta_in = args["metadata"];
    }
    int timeout_ms = reg_opts.default_timeout_ms > 0 ? reg_opts.default_timeout_ms
                                                     : registry_.default_timeout_ms_for(peer_id);
    if (args.contains("timeout_ms") && args["timeout_ms"].is_number_integer()) {
        timeout_ms = args["timeout_ms"].get<int>();
    }

    if (do_monitor && active_monitors_.load() >= policy_.max_concurrent_monitors) {
        return json{{"ok", false},
                    {"error", "max concurrent monitors (see OutboundSessionPolicy.max_concurrent_monitors)"}};
    }

    std::optional<std::string> sid;
    if (continue_session) {
        sid = session_book_.get(peer_id);
    }

    AgentMessage msg;
    msg.role = AgentMessage::Role::USER;
    AgentPart part;
    part.type = AgentPart::Type::TEXT;
    part.text = user_text;
    msg.parts.push_back(std::move(part));

    AgentTask t0;
    AgentClient& cli = registry_.client(peer_id);
    try {
        t0 = cli.send_task("", msg, sid, meta_in).get();
    } catch (const A2aRpcException& e) {
        json err;
        err["ok"] = false;
        err["peer_id"] = peer_id;
        err["error"] = e.what();
        err["a2a_error_code"] = e.code();
        err["local_handle"] = nullptr;
        err["remote_task_id"] = nullptr;
        return err;
    } catch (const std::exception& e) {
        json err;
        err["ok"] = false;
        err["peer_id"] = peer_id;
        err["error"] = e.what();
        err["local_handle"] = nullptr;
        err["remote_task_id"] = nullptr;
        return err;
    }

    const std::string handle = std::string("o") + std::to_string(next_handle_.fetch_add(1));
    auto entry = std::make_shared<TrackedEntry>();
    entry->local_handle = handle;
    entry->peer_id = peer_id;
    entry->remote_task_id = t0.task_id;
    {
        std::lock_guard<std::mutex> el(entry->mu);
        entry->snapshot = t0;
        entry->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    }

    {
        std::unique_lock<std::mutex> lk(mu_);
        by_handle_[handle] = entry;
        by_remote_[remote_key(peer_id, t0.task_id)] = entry;
        json ev;
        ev["type"] = "submitted";
        ev["peer_id"] = peer_id;
        ev["local_handle"] = handle;
        ev["remote_task_id"] = t0.task_id;
        push_event_locked(lk, std::move(ev));
    }
    emit_subtask_line(peer_id, handle, t0.task_id, task_status_cstr(t0.status), "submitted");

    if (do_monitor) {
        active_monitors_.fetch_add(1);
        OutboundTaskSupervisor* self = this;
        entry->monitor_jt.emplace([self, entry](std::stop_token st) {
            struct DecMon {
                std::atomic<int>& n;
                ~DecMon() { n.fetch_sub(1); }
            } dec{self->active_monitors_};

            AgentClient& cli_inner = self->registry_.client(entry->peer_id);
            const AgentCard& card_inner = self->registry_.card(entry->peer_id);

            A2aRemoteTaskOptions mopts;
            mopts.timeout_ms = 86400000;
            mopts.use_sse_if_capable = true;
            mopts.on_remote_log = self->remote_log_;

            auto deadline_supplier = [entry, st]() -> std::chrono::steady_clock::time_point {
                if (st.stop_requested()) {
                    return std::chrono::steady_clock::now();
                }
                std::lock_guard<std::mutex> lk(entry->mu);
                if (entry->force_stop.load()) {
                    return std::chrono::steady_clock::now();
                }
                return entry->deadline;
            };

            AgentTask initial = [&] {
                std::lock_guard<std::mutex> lk(entry->mu);
                return entry->snapshot;
            }();

            A2aTaskMonitorOutcome outcome = monitor_remote_task_until_deadline(
                entry->peer_id, cli_inner, card_inner, initial, mopts, deadline_supplier);

            {
                std::lock_guard<std::mutex> lk(entry->mu);
                if (st.stop_requested() || entry->force_stop.load()) {
                    if (!a2a_task_is_terminal(outcome.latest.status)) {
                        outcome.latest.status = AgentTaskStatus::CANCELLED;
                    }
                }
                entry->snapshot = outcome.latest;
            }

            if (outcome.latest.session_id.has_value()) {
                self->session_book_.set(entry->peer_id, *outcome.latest.session_id);
            }

            const std::string st_str = task_status_cstr(outcome.latest.status);
            self->emit_subtask_line(entry->peer_id, entry->local_handle, entry->remote_task_id, st_str,
                                    outcome.poll_error.has_value() ? "poll_error" : "terminal");

            std::unique_lock<std::mutex> lk(self->mu_);
            json ev;
            ev["type"] = "terminal";
            ev["peer_id"] = entry->peer_id;
            ev["local_handle"] = entry->local_handle;
            ev["remote_task_id"] = entry->remote_task_id;
            ev["status"] = st_str;
            if (outcome.poll_error) {
                ev["error"] = *outcome.poll_error;
            }
            self->push_event_locked(lk, std::move(ev));
        });
    }

    int remain_ms = timeout_ms;
    json ok;
    ok["ok"] = true;
    ok["local_handle"] = handle;
    ok["peer_id"] = peer_id;
    ok["remote_task_id"] = t0.task_id;
    if (t0.session_id.has_value()) {
        ok["context_id"] = *t0.session_id;
    } else {
        ok["context_id"] = nullptr;
    }
    ok["deadline_ms_from_now"] = remain_ms;
    return ok;
}

json OutboundTaskSupervisor::tool_get_status(const json& args) {
    std::shared_ptr<TrackedEntry> entry;
    if (args.contains("local_handle")) {
        entry = find_by_handle(args.at("local_handle").get<std::string>());
    } else {
        entry =
            find_by_remote(args.at("peer_id").get<std::string>(), args.at("remote_task_id").get<std::string>());
    }
    if (!entry) {
        return json{{"ok", false}, {"error", "unknown task"}};
    }
    std::lock_guard<std::mutex> lk(entry->mu);
    const AgentTask& t = entry->snapshot;
    json o;
    o["ok"] = true;
    o["phase"] = a2a_task_is_terminal(t.status) ? "terminal" : "monitoring";
    o["status"] = task_status_cstr(t.status);
    o["summary_text"] = summarize_task_text(t);
    o["peer_id"] = entry->peer_id;
    o["local_handle"] = entry->local_handle;
    o["remote_task_id"] = entry->remote_task_id;
    o["error"] = nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (now < entry->deadline) {
        o["deadline_remaining_ms"] = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(entry->deadline - now).count());
    } else {
        o["deadline_remaining_ms"] = 0;
    }
    return o;
}

json OutboundTaskSupervisor::tool_wait_tasks(const json& args) {
    const int wait_cap_ms = args.value("timeout_ms", 120000);
    const std::string mode = args.value("mode", "all");
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_cap_ms);

    std::vector<std::shared_ptr<TrackedEntry>> entries;
    if (args.contains("handles") && args["handles"].is_array()) {
        for (const auto& h : args["handles"]) {
            auto e = find_by_handle(h.get<std::string>());
            if (e) {
                entries.push_back(std::move(e));
            }
        }
    } else if (args.contains("peer_task_pairs") && args["peer_task_pairs"].is_array()) {
        for (const auto& p : args["peer_task_pairs"]) {
            auto e = find_by_remote(p.at("peer_id").get<std::string>(),
                                    p.at("remote_task_id").get<std::string>());
            if (e) {
                entries.push_back(std::move(e));
            }
        }
    }

    if (entries.empty()) {
        return json{{"ok", false}, {"error", "no tasks"}, {"results", json::array()}};
    }

    while (std::chrono::steady_clock::now() < until) {
        bool all_term = true;
        bool any_term = false;
        for (const auto& e : entries) {
            std::lock_guard<std::mutex> lk(e->mu);
            if (a2a_task_is_terminal(e->snapshot.status)) {
                any_term = true;
            } else {
                all_term = false;
            }
        }
        if (mode == "all" && all_term) {
            break;
        }
        if (mode == "any" && any_term) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    json results = json::array();
    bool partial = false;
    for (const auto& e : entries) {
        std::lock_guard<std::mutex> lk(e->mu);
        const AgentTask& t = e->snapshot;
        const bool tok = (t.status == AgentTaskStatus::COMPLETED);
        if (!tok && a2a_task_is_terminal(t.status)) {
            partial = true;
        }
        if (!a2a_task_is_terminal(t.status)) {
            partial = true;
        }
        json one;
        one["local_handle"] = e->local_handle;
        one["peer_id"] = e->peer_id;
        one["remote_task_id"] = e->remote_task_id;
        one["ok"] = tok;
        one["status"] = task_status_cstr(t.status);
        one["summary_text"] = summarize_task_text(t);
        one["error"] = nullptr;
        results.push_back(std::move(one));
    }

    if (mode == "all" && std::chrono::steady_clock::now() >= until) {
        partial = true;
    }

    return json{{"ok", true}, {"results", std::move(results)}, {"partial", partial}};
}

json OutboundTaskSupervisor::tool_cancel(const json& args) {
    std::shared_ptr<TrackedEntry> entry;
    if (args.contains("local_handle")) {
        entry = find_by_handle(args.at("local_handle").get<std::string>());
    } else {
        entry =
            find_by_remote(args.at("peer_id").get<std::string>(), args.at("remote_task_id").get<std::string>());
    }
    if (!entry) {
        return json{{"ok", false}, {"error", "unknown task"}};
    }

    AgentTaskStatus prev = AgentTaskStatus::PENDING;
    bool remote_ack = false;
    {
        std::lock_guard<std::mutex> lk(entry->mu);
        prev = entry->snapshot.status;
        entry->force_stop = true;
    }
    if (entry->monitor_jt.has_value()) {
        entry->monitor_jt->request_stop();
    }
    try {
        remote_ack = registry_.client(entry->peer_id).cancel_task("", entry->remote_task_id).get();
    } catch (...) {
        remote_ack = false;
    }
    {
        std::lock_guard<std::mutex> lk(entry->mu);
        if (!a2a_task_is_terminal(entry->snapshot.status)) {
            entry->snapshot.status = AgentTaskStatus::CANCELLED;
        }
    }
    emit_subtask_line(entry->peer_id, entry->local_handle, entry->remote_task_id, "cancelled", "cancel_tool");

    return json{{"ok", true},
                {"prev_status", task_status_cstr(prev)},
                {"remote_ack", remote_ack}};
}

json OutboundTaskSupervisor::tool_extend(const json& args) {
    const std::string handle = args.at("local_handle").get<std::string>();
    const int extra_ms = args.at("extra_ms").get<int>();
    if (extra_ms <= 0) {
        return json{{"ok", false}, {"error", "extra_ms must be positive"}};
    }
    auto entry = find_by_handle(handle);
    if (!entry) {
        return json{{"ok", false}, {"error", "unknown task"}};
    }
    {
        std::lock_guard<std::mutex> lk(entry->mu);
        if (a2a_task_is_terminal(entry->snapshot.status)) {
            return json{{"ok", false}, {"error", "task already terminal"}};
        }
        entry->deadline += std::chrono::milliseconds(extra_ms);
        const auto now = std::chrono::steady_clock::now();
        const int rem =
            now < entry->deadline
                ? static_cast<int>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(entry->deadline - now).count())
                : 0;
        return json{{"ok", true}, {"new_deadline_ms_from_now", rem}};
    }
}

json OutboundTaskSupervisor::tool_list_subtasks(const json& args) {
    const std::uint64_t since = args.value("since_seq", 0);
    const std::string peer_f = args.value("peer_id", "");
    const int limit = args.value("limit", 20);
    json arr = json::array();
    std::uint64_t next_seq = since;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& ev : event_ring_) {
            if (!ev.contains("seq")) {
                continue;
            }
            const std::uint64_t sq = ev["seq"].get<std::uint64_t>();
            if (sq <= since) {
                continue;
            }
            if (!peer_f.empty() && ev.value("peer_id", "") != peer_f) {
                continue;
            }
            if (static_cast<int>(arr.size()) >= limit) {
                break;
            }
            arr.push_back(ev);
            next_seq = std::max(next_seq, sq);
        }
    }
    return json{{"ok", true}, {"events", std::move(arr)}, {"next_seq", next_seq}};
}

void OutboundTaskSupervisor::on_user_turn_barrier() {
    if (!policy_.cancel_all_on_new_user_turn) {
        return;
    }
    std::vector<std::shared_ptr<TrackedEntry>> copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : by_handle_) {
            copy.push_back(p.second);
        }
    }
    for (const auto& e : copy) {
        json j;
        j["local_handle"] = e->local_handle;
        (void)tool_cancel(j);
    }
}

std::string OutboundTaskSupervisor::format_digest_for_llm(std::size_t max_events,
                                                          std::size_t max_bytes) const {
    std::ostringstream o;
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (auto it = event_ring_.rbegin(); it != event_ring_.rend() && n < max_events; ++it, ++n) {
        o << (*it).dump() << '\n';
        if (o.str().size() >= max_bytes) {
            break;
        }
    }
    std::string s = o.str();
    if (s.size() > max_bytes) {
        s = utf8_safe_truncate(s, max_bytes);
        s += "\n…";
    }
    return s;
}

json OutboundTaskSupervisor::debug_subtasks_json() const {
    json arr = json::array();
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : by_handle_) {
        std::lock_guard<std::mutex> el(p.second->mu);
        json one;
        one["local_handle"] = p.second->local_handle;
        one["peer_id"] = p.second->peer_id;
        one["remote_task_id"] = p.second->remote_task_id;
        one["status"] = task_status_cstr(p.second->snapshot.status);
        arr.push_back(std::move(one));
    }
    return arr;
}

} // namespace a2a
} // namespace agent_framework

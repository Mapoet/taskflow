/**
 * @file orchestration.cpp
 * @brief WP2.agent2agent orchestration + ToolBus bridge
 */
#include <agent/a2a/orchestration.hpp>
#include <agent/a2a/a2a_task_monitor.hpp>
#include <agent/a2a/jsonrpc_client.hpp>
#include <agent/a2a/outbound_task_supervisor.hpp>
#include <agent/toolbus.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace agent_framework {
namespace a2a {

std::optional<std::string> PeerSessionBook::get(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = by_peer_.find(peer_id);
    if (it == by_peer_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void PeerSessionBook::set(const std::string& peer_id, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    by_peer_[peer_id] = session_id;
}

void PeerSessionBook::clear(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    by_peer_.erase(peer_id);
}

namespace {

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

json task_to_tool_json(const std::string& peer_id, const AgentTask& t, bool ok) {
    json out;
    out["ok"] = ok;
    out["peer_id"] = peer_id;
    out["task_id"] = t.task_id;
    out["status"] = task_status_cstr(t.status);
    out["summary_text"] = summarize_task_text(t);
    if (t.session_id.has_value()) {
        out["context_id"] = *t.session_id;
    } else {
        out["context_id"] = nullptr;
    }
    return out;
}

} // namespace

bool agent_card_has_streaming(const AgentCard& card) {
    for (const auto& c : card.capabilities) {
        for (char ch : c) {
            (void)ch;
        }
        if (c == "streaming") {
            return true;
        }
    }
    return false;
}

json run_remote_task_and_wait(
    const std::string& peer_id,
    AgentClient& rpc_client,
    const AgentCard& card,
    const AgentMessage& message,
    PeerSessionBook* session_book,
    bool continue_session,
    const json& metadata,
    const A2aRemoteTaskOptions& opts) {

    std::optional<std::string> sid;
    if (continue_session && session_book != nullptr) {
        sid = session_book->get(peer_id);
    }

    AgentTask t0;
    try {
        t0 = rpc_client.send_task("", message, sid, metadata).get();
    } catch (const A2aRpcException& e) {
        json err;
        err["ok"] = false;
        err["peer_id"] = peer_id;
        err["error"] = e.what();
        err["a2a_error_code"] = e.code();
        err["task_id"] = nullptr;
        err["status"] = nullptr;
        err["summary_text"] = "";
        err["context_id"] = nullptr;
        return err;
    } catch (const std::exception& e) {
        json err;
        err["ok"] = false;
        err["peer_id"] = peer_id;
        err["error"] = e.what();
        err["a2a_error_code"] = nullptr;
        err["task_id"] = nullptr;
        err["status"] = nullptr;
        err["summary_text"] = "";
        err["context_id"] = nullptr;
        return err;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
    A2aTaskMonitorOutcome mon = monitor_remote_task_until_deadline(
        peer_id, rpc_client, card, t0, opts, deadline);

    if (mon.poll_error.has_value()) {
        json err = task_to_tool_json(peer_id, mon.latest, false);
        err["ok"] = false;
        err["error"] = *mon.poll_error;
        return err;
    }

    AgentTask& latest = mon.latest;

    if (session_book != nullptr && latest.session_id.has_value()) {
        session_book->set(peer_id, *latest.session_id);
    }

    const bool ok = latest.status == AgentTaskStatus::COMPLETED;
    json out = task_to_tool_json(peer_id, latest, ok);
    if (!ok && !out.contains("error")) {
        out["error"] = std::string("terminal status: ") + task_status_cstr(latest.status);
    }
    return out;
}

void register_a2a_orchestrator_tools(
    ToolBus& bus,
    A2aPeerRegistry& registry,
    PeerSessionBook& session_book,
    const A2aToolRegistrationOptions& opts) {

    ToolMeta meta;
    meta.name = kA2aOrchestratorToolSendMessage;
    meta.description =
        "Send a user message to a remote A2A agent (JSON-RPC SendMessage). Returns task summary and context_id.";
    meta.side_effect = ToolSideEffect::Write;
    meta.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "peer_id": { "type": "string", "description": "Peer id from peers.json" },
            "user_text": { "type": "string" },
            "continue_session": { "type": "boolean", "default": true },
            "metadata": { "type": "object" }
        },
        "required": ["peer_id", "user_text"]
    })");

    bus.register_local_tool(
        kA2aOrchestratorToolSendMessage,
        [&registry, &session_book, opts](const json& args) -> json {
            const std::string peer_id = args.at("peer_id").get<std::string>();
            const std::string user_text = args.at("user_text").get<std::string>();
            const bool continue_session = args.value("continue_session", true);
            json meta_in = json::object();
            if (args.contains("metadata") && args["metadata"].is_object()) {
                meta_in = args["metadata"];
            }
            AgentMessage msg;
            msg.role = AgentMessage::Role::USER;
            AgentPart part;
            part.type = AgentPart::Type::TEXT;
            part.text = user_text;
            msg.parts.push_back(std::move(part));

            A2aRemoteTaskOptions rw;
            rw.timeout_ms = opts.default_timeout_ms > 0 ? opts.default_timeout_ms
                                                      : registry.default_timeout_ms_for(peer_id);
            rw.use_sse_if_capable = true;
            rw.on_remote_log = opts.on_remote_log;

            AgentClient& cli = registry.client(peer_id);
            const AgentCard& card = registry.card(peer_id);
            return run_remote_task_and_wait(peer_id, cli, card, msg, &session_book, continue_session,
                                            meta_in, rw);
        },
        meta);

    if (!opts.register_per_peer_aliases) {
        return;
    }

    for (const std::string& pid : registry.peer_ids()) {
        const std::string tool_name = std::string("a2a_send_message__") + pid;
        ToolMeta m2;
        m2.name = tool_name;
        m2.description = "Send user_text to peer " + pid + " (fixed peer).";
        m2.side_effect = ToolSideEffect::Write;
        m2.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "user_text": { "type": "string" },
                "continue_session": { "type": "boolean", "default": true },
                "metadata": { "type": "object" }
            },
            "required": ["user_text"]
        })");

        bus.register_local_tool(
            tool_name,
            [&registry, &session_book, opts, pid](const json& args) -> json {
                const std::string user_text = args.at("user_text").get<std::string>();
                const bool continue_session = args.value("continue_session", true);
                json meta_in = json::object();
                if (args.contains("metadata") && args["metadata"].is_object()) {
                    meta_in = args["metadata"];
                }
                AgentMessage msg;
                msg.role = AgentMessage::Role::USER;
                AgentPart part;
                part.type = AgentPart::Type::TEXT;
                part.text = user_text;
                msg.parts.push_back(std::move(part));
                A2aRemoteTaskOptions rw;
                rw.timeout_ms = opts.default_timeout_ms > 0 ? opts.default_timeout_ms
                                                          : registry.default_timeout_ms_for(pid);
                rw.use_sse_if_capable = true;
                rw.on_remote_log = opts.on_remote_log;
                AgentClient& cli = registry.client(pid);
                const AgentCard& card = registry.card(pid);
                return run_remote_task_and_wait(pid, cli, card, msg, &session_book, continue_session,
                                                meta_in, rw);
            },
            m2);
    }
}

void register_a2a_orchestrator_tools(
    ToolBus& bus,
    A2aPeerRegistry& registry,
    PeerSessionBook& session_book,
    const std::shared_ptr<OutboundTaskSupervisor>& supervisor,
    const A2aToolRegistrationOptions& opts) {

    register_a2a_orchestrator_tools(bus, registry, session_book, opts);
    if (!supervisor) {
        return;
    }

    ToolMeta m_submit;
    m_submit.name = kA2aToolSubmitTask;
    m_submit.description =
        "Submit a remote A2A task (SendMessage) and return immediately with local_handle; "
        "background monitor until terminal unless monitor=false.";
    m_submit.side_effect = ToolSideEffect::Write;
    m_submit.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "peer_id": { "type": "string" },
            "user_text": { "type": "string" },
            "continue_session": { "type": "boolean", "default": true },
            "metadata": { "type": "object" },
            "timeout_ms": { "type": "integer" },
            "monitor": { "type": "boolean", "default": true }
        },
        "required": ["peer_id", "user_text"]
    })");
    bus.register_local_tool(
        kA2aToolSubmitTask,
        [supervisor, opts](const json& args) -> json { return supervisor->tool_submit(args, opts); },
        m_submit);

    ToolMeta m_gs;
    m_gs.name = kA2aToolGetTaskStatus;
    m_gs.description = "Read outbound subtask status by local_handle or peer_id+remote_task_id.";
    m_gs.side_effect = ToolSideEffect::ReadOnly;
    m_gs.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "local_handle": { "type": "string" },
            "peer_id": { "type": "string" },
            "remote_task_id": { "type": "string" }
        }
    })");
    bus.register_local_tool(
        kA2aToolGetTaskStatus,
        [supervisor](const json& args) -> json { return supervisor->tool_get_status(args); },
        m_gs);

    ToolMeta m_wait;
    m_wait.name = kA2aToolWaitTasks;
    m_wait.description = "Wait for subtasks: handles[] or peer_task_pairs[], mode=all|any.";
    m_wait.side_effect = ToolSideEffect::Write;
    m_wait.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "handles": { "type": "array", "items": { "type": "string" } },
            "peer_task_pairs": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "peer_id": { "type": "string" },
                        "remote_task_id": { "type": "string" }
                    },
                    "required": ["peer_id", "remote_task_id"]
                }
            },
            "timeout_ms": { "type": "integer" },
            "mode": { "type": "string", "enum": ["all", "any"], "default": "all" }
        }
    })");
    bus.register_local_tool(
        kA2aToolWaitTasks,
        [supervisor](const json& args) -> json { return supervisor->tool_wait_tasks(args); },
        m_wait);

    ToolMeta m_can;
    m_can.name = kA2aToolCancelTask;
    m_can.description = "Cancel outbound subtask locally and request CancelTask on peer.";
    m_can.side_effect = ToolSideEffect::Write;
    m_can.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "local_handle": { "type": "string" },
            "peer_id": { "type": "string" },
            "remote_task_id": { "type": "string" }
        }
    })");
    bus.register_local_tool(
        kA2aToolCancelTask,
        [supervisor](const json& args) -> json { return supervisor->tool_cancel(args); },
        m_can);

    ToolMeta m_ext;
    m_ext.name = kA2aToolExtendTimeout;
    m_ext.description = "Extend local wait deadline for a non-terminal subtask by extra_ms.";
    m_ext.side_effect = ToolSideEffect::Write;
    m_ext.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "local_handle": { "type": "string" },
            "extra_ms": { "type": "integer" }
        },
        "required": ["local_handle", "extra_ms"]
    })");
    bus.register_local_tool(
        kA2aToolExtendTimeout,
        [supervisor](const json& args) -> json { return supervisor->tool_extend(args); },
        m_ext);

    ToolMeta m_list;
    m_list.name = kA2aToolListSubtasks;
    m_list.description = "List recent subtask events since_seq (ring buffer).";
    m_list.side_effect = ToolSideEffect::ReadOnly;
    m_list.schema = json::parse(R"({
        "type": "object",
        "properties": {
            "since_seq": { "type": "integer" },
            "peer_id": { "type": "string" },
            "limit": { "type": "integer" }
        }
    })");
    bus.register_local_tool(
        kA2aToolListSubtasks,
        [supervisor](const json& args) -> json { return supervisor->tool_list_subtasks(args); },
        m_list);
}

} // namespace a2a
} // namespace agent_framework

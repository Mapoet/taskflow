/**
 * @file orchestration.cpp
 * @brief WP2.agent2agent orchestration + ToolBus bridge
 */
#include <agent/a2a/orchestration.hpp>
#include <agent/a2a/jsonrpc_client.hpp>
#include <agent/toolbus.hpp>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

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

bool is_terminal_status(AgentTaskStatus s) {
    return s == AgentTaskStatus::COMPLETED || s == AgentTaskStatus::FAILED ||
           s == AgentTaskStatus::CANCELLED || s == AgentTaskStatus::INPUT_REQUIRED;
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

    const bool use_sse = opts.use_sse_if_capable && agent_card_has_streaming(card);
    AgentTask latest = t0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);

    if (use_sse) {
        std::mutex mu;
        std::condition_variable cv;
        bool terminal = is_terminal_status(latest.status);

        auto on_status = [&](const AgentTask& t) {
            {
                std::lock_guard<std::mutex> lk(mu);
                latest = t;
            }
            if (opts.on_remote_log) {
                std::ostringstream line;
                line << "task_id=" << t.task_id << " status=" << task_status_cstr(t.status);
                opts.on_remote_log(peer_id, line.str());
            }
            if (is_terminal_status(t.status)) {
                terminal = true;
                cv.notify_all();
            }
        };
        rpc_client.subscribe_task_updates("", t0.task_id, on_status, [](const AgentArtifact&) {});

        std::unique_lock<std::mutex> ul(mu);
        while (!terminal && std::chrono::steady_clock::now() < deadline) {
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                break;
            }
            cv.wait_for(ul, left, [&] { return terminal; });
        }
        ul.unlock();

        if (!terminal) {
            try {
                latest = rpc_client.get_task("", t0.task_id).get();
            } catch (...) {
                // keep latest
            }
        }
    } else {
        while (!is_terminal_status(latest.status) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            try {
                latest = rpc_client.get_task("", t0.task_id).get();
            } catch (const std::exception& e) {
                json err = task_to_tool_json(peer_id, latest, false);
                err["ok"] = false;
                err["error"] = e.what();
                return err;
            }
            if (opts.on_remote_log) {
                std::ostringstream line;
                line << "task_id=" << latest.task_id << " status=" << task_status_cstr(latest.status);
                opts.on_remote_log(peer_id, line.str());
            }
        }
    }

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

} // namespace a2a
} // namespace agent_framework

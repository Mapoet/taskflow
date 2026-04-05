/**
 * @file agent_server.cpp
 * @brief Agent 服务器实现（A2A / WP2.2）
 * @author Mapoet
 * @version 0.2
 * @date 2026-04-05
 */
#include <agent/agent_server.hpp>
#include <agent/a2a/dispatch_table.hpp>
#include <agent/a2a/jsonrpc.hpp>
#include <agent/a2a/sse_framing.hpp>
#include <agent/a2a/wire_card.hpp>
#include <agent/a2a/wire_mapping.hpp>
#include <agent/context_budget.hpp>
#include <agent/internal/sse_server_channel.hpp>
#include <agent/internal/task_dispatch_queue.hpp>
#include <agent/task_state_machine.hpp>
#include <agent/types.hpp>
#include <workflow/nodeflow.hpp>

#include <taskflow/taskflow.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>

#if __has_include(<httplib/httplib.hpp>)
    #include <httplib/httplib.hpp>
#elif __has_include(<httplib.hpp>)
    #include <httplib.hpp>
#elif __has_include(<httplib.h>)
    #include <httplib.h>
#else
    #error "httplib not found."
#endif

namespace agent_framework {

namespace {

using HttplibHolder = std::unique_ptr<httplib::Server>;

HttplibHolder* as_holder(void* p) {
    return static_cast<HttplibHolder*>(p);
}

httplib::Server* server_ptr(void* p) {
    if (!p) {
        return nullptr;
    }
    return as_holder(p)->get();
}

std::size_t env_size(const char* var, std::size_t default_val) {
    const char* v = std::getenv(var);
    if (!v || !v[0]) {
        return default_val;
    }
    char* end = nullptr;
    unsigned long n = std::strtoul(v, &end, 10);
    if (end == v || n == 0) {
        return default_val;
    }
    return static_cast<std::size_t>(std::min(n, static_cast<unsigned long>(1000000)));
}

unsigned env_u32(const char* var, unsigned default_val) {
    return static_cast<unsigned>(std::min(env_size(var, default_val), std::size_t{4096}));
}

std::string resolve_jsonrpc_path(const AgentCard& card) {
    const char* o = std::getenv("AGENT_SERVER_JSON_RPC_PATH");
    if (o && o[0]) {
        std::string p(o);
        if (p[0] != '/') {
            p.insert(p.begin(), '/');
        }
        return p;
    }
    const std::string& ep = card.api_endpoint;
    if (ep.empty()) {
        return "/";
    }
    if (ep[0] == '/') {
        return ep;
    }
    static const char http[] = "http://";
    static const char https[] = "https://";
    std::size_t start = 0;
    if (ep.rfind(http, 0) == 0) {
        start = sizeof(http) - 1;
    } else if (ep.rfind(https, 0) == 0) {
        start = sizeof(https) - 1;
    } else {
        return "/";
    }
    std::size_t slash = ep.find('/', start);
    if (slash == std::string::npos) {
        return "/";
    }
    return ep.substr(slash);
}

bool strict_a2a() {
    const char* v = std::getenv("AGENT_A2A_STRICT");
    if (!v || !v[0]) {
        return true;
    }
    return v[0] != '0';
}

bool legacy_rest() {
    const char* v = std::getenv("AGENT_SERVER_LEGACY_REST");
    return v && v[0] == '1';
}

std::string sse_ping_interval_sec() {
    const char* v = std::getenv("AGENT_SERVER_SSE_PING_SEC");
    if (!v || !v[0]) {
        return "30";
    }
    return std::string(v);
}

bool task_status_is_terminal(AgentTaskStatus s) {
    return s == AgentTaskStatus::COMPLETED || s == AgentTaskStatus::FAILED ||
           s == AgentTaskStatus::CANCELLED;
}

long env_long(const char* var, long default_val) {
    const char* v = std::getenv(var);
    if (!v || !v[0]) {
        return default_val;
    }
    char* end = nullptr;
    long x = std::strtol(v, &end, 10);
    if (end == v) {
        return default_val;
    }
    return x;
}

int compute_effective_task_timeout_sec(const json& metadata) {
    long max_s = env_long("AGENT_TASK_MAX_TIMEOUT_SEC", 86400);
    if (max_s < 1) {
        max_s = 1;
    }
    if (max_s > 86400 * 366) {
        max_s = 86400 * 366;
    }
    long def_sec = env_long("AGENT_TASK_DEFAULT_TIMEOUT_SEC", 0);
    if (def_sec < 0) {
        def_sec = 0;
    }
    if (def_sec > max_s) {
        std::clog << "[AgentServer] AGENT_TASK_DEFAULT_TIMEOUT_SEC clamped to max\n";
        def_sec = max_s;
    }

    int from_meta = -1;
    if (metadata.contains("timeout_sec") && metadata["timeout_sec"].is_number_integer()) {
        from_meta = static_cast<int>(metadata["timeout_sec"].get<int>());
    } else if (metadata.contains("timeout_sec") && metadata["timeout_sec"].is_number_unsigned()) {
        from_meta = static_cast<int>(metadata["timeout_sec"].get<std::uint64_t>());
    }

    int base = (from_meta >= 0) ? from_meta : static_cast<int>(def_sec);
    if (base < 0) {
        base = 0;
    }
    if (base > max_s) {
        std::clog << "[AgentServer] metadata timeout_sec clamped to AGENT_TASK_MAX_TIMEOUT_SEC\n";
        base = static_cast<int>(max_s);
    }
    return base;
}

} // namespace

AgentServer::AgentServer(int port) : port_(port) {}

AgentServer::~AgentServer() {
    stop();
    if (http_server_) {
        delete as_holder(http_server_);
        http_server_ = nullptr;
    }
}

void AgentServer::ensure_runtime() {
    if (!http_server_) {
        http_server_ = new HttplibHolder(std::make_unique<httplib::Server>());
    }
    if (!task_queue_) {
        task_queue_ = std::make_unique<internal::TaskDispatchQueue>(env_size("AGENT_SERVER_MAX_QUEUED_TASKS", 64));
    }
    if (!process_executor_) {
        unsigned nt = env_u32("AGENT_SERVER_EXECUTOR_THREADS", std::max(1u, std::thread::hardware_concurrency()));
        process_executor_ = std::make_shared<tf::Executor>(static_cast<int>(nt));
    }
    if (!rpc_dispatch_) {
        rpc_dispatch_ = std::make_unique<a2a::DispatchTable>();
        register_jsonrpc_methods();
    }
}

void AgentServer::register_jsonrpc_methods() {
    rpc_dispatch_->register_method("SendMessage", [this](const json& p) { return jsonrpc_send_message(p); });
    rpc_dispatch_->register_method("GetTask", [this](const json& p) { return jsonrpc_get_task(p); });
    rpc_dispatch_->register_method("CancelTask", [this](const json& p) { return jsonrpc_cancel_task(p); });
    rpc_dispatch_->register_method("ListTasks", [this](const json& p) { return jsonrpc_list_tasks(p); });
    rpc_dispatch_->register_method("SendStreamingMessage", [](const json&) -> json {
        throw a2a::JsonRpcInvokeError(a2a::JsonRpcErrorCode::method_not_found, "SendStreamingMessage not implemented");
    });
    rpc_dispatch_->register_method("SubscribeToTask", [](const json&) -> json {
        throw a2a::JsonRpcInvokeError(
            a2a::JsonRpcErrorCode::method_not_found,
            "SubscribeToTask JSON-RPC not implemented; use GET /tasks/sendSubscribe");
    });
}

void AgentServer::start_dispatch_workers() {
    if (workers_started_.exchange(true)) {
        return;
    }
    std::size_t nw = env_size("AGENT_SERVER_WORKER_THREADS", 0);
    if (nw == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        nw = std::max<std::size_t>(2, hc == 0 ? 2 : hc / 2);
    }
    for (std::size_t i = 0; i < nw; ++i) {
        dispatch_workers_.emplace_back([this] { dispatch_worker_loop(); });
    }
}

void AgentServer::dispatch_worker_loop() {
    while (task_queue_) {
        std::function<void()> job;
        if (!task_queue_->wait_pop(job)) {
            break;
        }
        if (job) {
            job();
        }
    }
}

void AgentServer::run_agent_task_on_executor(const std::string& task_id,
                                             AgentTask task_snapshot,
                                             std::shared_ptr<workflow::GraphBuilder> builder,
                                             std::shared_ptr<TaskControl> control) {
    (void)process_executor_;
    (void)task_snapshot;
    // WP2.0: replace with GraphExecutor::execute using process_executor_
    if (!task_handler_ || !control) {
        return;
    }
    try {
        AgentTask snap;
        {
            std::lock_guard<std::mutex> lk(tasks_mutex_);
            auto it = active_tasks_.find(task_id);
            if (it == active_tasks_.end()) {
                task_controls_.erase(task_id);
                return;
            }
            if (task_status_is_terminal(it->second.status)) {
                task_controls_.erase(task_id);
                return;
            }
            if (it->second.status == AgentTaskStatus::PENDING) {
                if (control->is_cancel_requested()) {
                    std::string terr;
                    try_transition(it->second, AgentTaskStatus::CANCELLED, &terr);
                    push_task_status_update(task_id, it->second);
                    task_controls_.erase(task_id);
                    return;
                }
                std::string terr;
                if (!try_transition(it->second, AgentTaskStatus::WORKING, &terr)) {
                    return;
                }
                control->arm_working_deadline(control->effective_timeout_sec());
            }
            snap = it->second;
        }

        std::future<AgentTask> fut = task_handler_(snap, builder, control);
        for (;;) {
            if (fut.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
                break;
            }
            control->check_deadline_now();
        }
        AgentTask done = fut.get();
        control->check_deadline_now();

        std::lock_guard<std::mutex> lk(tasks_mutex_);
        auto it = active_tasks_.find(task_id);
        if (it == active_tasks_.end()) {
            task_controls_.erase(task_id);
            return;
        }

        if (task_status_is_terminal(it->second.status)) {
            task_controls_.erase(task_id);
            return;
        }

        if (it->second.status != AgentTaskStatus::WORKING) {
            return;
        }

        if (control->is_cancel_requested()) {
            std::string terr;
            try_transition(it->second, AgentTaskStatus::CANCELLED, &terr);
            push_task_status_update(task_id, it->second);
            task_controls_.erase(task_id);
            return;
        }
        if (control->is_deadline_exceeded()) {
            std::string terr;
            try_transition(it->second, AgentTaskStatus::FAILED, &terr);
            it->second.metadata["a2a_failure_reason"] = "timeout";
            push_task_status_update(task_id, it->second);
            task_controls_.erase(task_id);
            return;
        }

        it->second.messages = std::move(done.messages);
        it->second.artifacts = std::move(done.artifacts);
        it->second.metadata = std::move(done.metadata);
        it->second.session_id = std::move(done.session_id);

        AgentTaskStatus target = done.status;
        if (target == AgentTaskStatus::WORKING) {
            target = AgentTaskStatus::COMPLETED;
        }
        std::string terr;
        if (!try_transition(it->second, target, &terr)) {
            try_transition(it->second, AgentTaskStatus::FAILED, nullptr);
        }
        push_task_status_update(task_id, it->second);
        if (task_status_is_terminal(it->second.status)) {
            task_controls_.erase(task_id);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lk(tasks_mutex_);
        auto it = active_tasks_.find(task_id);
        if (it != active_tasks_.end() && it->second.status == AgentTaskStatus::WORKING) {
            std::string terr;
            try_transition(it->second, AgentTaskStatus::FAILED, &terr);
            it->second.updated_at = std::chrono::system_clock::now();
            push_task_status_update(task_id, it->second);
        }
        task_controls_.erase(task_id);
    }
}

void AgentServer::start() {
    if (http_server_) {
        httplib::Server* old = server_ptr(http_server_);
        if (old && old->is_running()) {
            return;
        }
        delete as_holder(http_server_);
        http_server_ = nullptr;
    }
    routes_ready_ = false;

    ensure_runtime();
    start_dispatch_workers();

    httplib::Server* srv = server_ptr(http_server_);
    if (!srv) {
        return;
    }

    if (!routes_ready_.exchange(true)) {
        setup_routes();
    }

    const char* bind_host = std::getenv("AGENT_SERVER_BIND");
    if (!bind_host || !bind_host[0]) {
        bind_host = "0.0.0.0";
    }
    int listen_port = port_;
    const char* pe = std::getenv("AGENT_SERVER_PORT");
    if (pe && pe[0]) {
        char* end = nullptr;
        long p = std::strtol(pe, &end, 10);
        if (end != pe && p >= 0 && p <= 65535) {
            listen_port = static_cast<int>(p);
        }
    }

    if (listen_port == 0) {
        int p = srv->bind_to_any_port(bind_host);
        if (p < 0) {
            return;
        }
        bound_port_ = p;
        srv->listen_after_bind();
    } else {
        bound_port_ = listen_port;
        srv->listen(bind_host, listen_port);
    }
}

void AgentServer::stop() {
    if (http_server_) {
        if (httplib::Server* s = server_ptr(http_server_)) {
            s->stop();
        }
    }

    {
        std::lock_guard<std::mutex> lk(sse_mutex_);
        for (auto& kv : sse_subscribers_) {
            for (auto& ch : kv.second) {
                if (ch) {
                    ch->close();
                }
            }
        }
        sse_subscribers_.clear();
    }

    if (task_queue_) {
        task_queue_->shutdown();
    }
    for (auto& t : dispatch_workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    dispatch_workers_.clear();
    workers_started_ = false;
    task_queue_.reset();
    {
        std::lock_guard<std::mutex> lk(tasks_mutex_);
        task_controls_.clear();
    }
    routes_ready_ = false;
}

void AgentServer::register_agent_card(const AgentCard& card) {
    agent_card_ = card;
}

void AgentServer::set_task_handler(
    std::function<std::future<AgentTask>(
        const AgentTask&,
        std::shared_ptr<workflow::GraphBuilder>,
        std::shared_ptr<TaskControl>)> handler
) {
    task_handler_ = std::move(handler);
}

void AgentServer::set_authentication_validator(
    std::function<bool(const std::map<std::string, std::string>& headers)> validator
) {
    auth_validator_ = std::move(validator);
}

std::map<std::string, std::string> AgentServer::lower_headers(const httplib::Request& req) {
    std::map<std::string, std::string> out;
    for (const auto& h : req.headers) {
        std::string key = h.first;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        out[std::move(key)] = h.second;
    }
    return out;
}

bool AgentServer::validate_authentication(const httplib::Request& req) {
    if (!auth_validator_) {
        return true;
    }
    return auth_validator_(lower_headers(req));
}

void AgentServer::remove_sse_channel(const std::string& task_id,
                                     const std::shared_ptr<internal::SseServerChannel>& ch) {
    std::lock_guard<std::mutex> lk(sse_mutex_);
    auto it = sse_subscribers_.find(task_id);
    if (it == sse_subscribers_.end()) {
        return;
    }
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), ch), vec.end());
    if (vec.empty()) {
        sse_subscribers_.erase(it);
    }
}

void AgentServer::push_task_status_update(const std::string& task_id, const AgentTask& task) {
    json payload = a2a::stream_response_status_update(task);
    apply_wire_payload_cap(payload, ContextBudgetLimits{}.max_wire_message_bytes, nullptr);
    std::string framed;
    a2a::append_sse_event(framed, "", payload.dump());

    std::lock_guard<std::mutex> lk(sse_mutex_);
    auto it = sse_subscribers_.find(task_id);
    if (it == sse_subscribers_.end()) {
        return;
    }
    for (auto& ch : it->second) {
        if (ch) {
            ch->push_framed(framed);
        }
    }
}

void AgentServer::push_artifact_update(const std::string& task_id, const AgentArtifact& artifact) {
    std::optional<std::string> ctx;
    {
        std::lock_guard<std::mutex> lk(tasks_mutex_);
        auto it = active_tasks_.find(task_id);
        if (it != active_tasks_.end() && it->second.session_id) {
            ctx = it->second.session_id;
        }
    }
    json payload = a2a::stream_response_artifact_update(artifact, task_id, ctx);
    apply_wire_payload_cap(payload, ContextBudgetLimits{}.max_wire_message_bytes, nullptr);
    std::string framed;
    a2a::append_sse_event(framed, "", payload.dump());

    std::lock_guard<std::mutex> lk(sse_mutex_);
    auto it = sse_subscribers_.find(task_id);
    if (it == sse_subscribers_.end()) {
        return;
    }
    for (auto& ch : it->second) {
        if (ch) {
            ch->push_framed(framed);
        }
    }
}

void AgentServer::notify_task_update_via_webhook(const std::string& task_id, const AgentTask& task) {
    std::lock_guard<std::mutex> lk(tasks_mutex_);
    auto it = webhook_urls_.find(task_id);
    if (it != webhook_urls_.end()) {
        (void)task;
        (void)it;
    }
}

void AgentServer::setup_routes() {
    httplib::Server* srv = server_ptr(http_server_);
    if (!srv) {
        return;
    }

    srv->Get("/.well-known/agent-card.json", [this](const httplib::Request&, httplib::Response& res) {
        handle_well_known_agent_card(res);
    });

    srv->Get("/health", [this](const httplib::Request&, httplib::Response& res) { handle_health(res); });

    const std::string jpath = resolve_jsonrpc_path(agent_card_);
    srv->Post(jpath.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
        handle_jsonrpc_post(req, res);
    });

    srv->Get("/tasks/sendSubscribe", [this](const httplib::Request& req, httplib::Response& res) {
        handle_tasks_send_subscribe(req, res);
    });

    if (legacy_rest() && !strict_a2a()) {
        srv->Post("/tasks/send", [this](const httplib::Request& req, httplib::Response& res) {
            handle_tasks_send(req, res);
        });
        srv->Get("/tasks/get", [this](const httplib::Request& req, httplib::Response& res) {
            handle_tasks_get(req, res);
        });
        srv->Post("/tasks/cancel", [this](const httplib::Request& req, httplib::Response& res) {
            handle_tasks_cancel(req, res);
        });
        srv->Post("/tasks/update", [this](const httplib::Request& req, httplib::Response& res) {
            handle_tasks_update(req, res);
        });
        srv->Post("/tasks/resubscribe", [this](const httplib::Request& req, httplib::Response& res) {
            handle_tasks_resubscribe(req, res);
        });
        srv->Post("/tasks/pushNotification/set", [this](const httplib::Request& req, httplib::Response& res) {
            handle_push_notification_set(req, res);
        });
        srv->Get("/tasks/pushNotification/get", [this](const httplib::Request& req, httplib::Response& res) {
            handle_push_notification_get(req, res);
        });
    }
}

void AgentServer::handle_well_known_agent_card(httplib::Response& res) {
    res.status = 200;
    res.set_content(a2a::agent_card_discovery_json_string(agent_card_), "application/json");
}

void AgentServer::handle_health(httplib::Response& res) {
    res.status = 200;
    res.set_content(std::string(R"({"ok":true})"), "application/json");
}

void AgentServer::handle_jsonrpc_post(const httplib::Request& req, httplib::Response& res) {
    res.status = 200;
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(R"({"error":"Unauthorized"})", "application/json");
        return;
    }

    auto parsed = a2a::parse_jsonrpc_request(req.body);
    if (std::holds_alternative<json>(parsed)) {
        res.set_content(std::get<json>(parsed).dump(), "application/json");
        return;
    }

    const auto& jr = std::get<a2a::JsonRpcRequest>(parsed);
    json id = jr.id;

    try {
        json params = jr.params.value_or(json::object());
        json result = rpc_dispatch_->invoke(jr.method, params);
        res.set_content(a2a::make_success_response(id, result).dump(), "application/json");
    } catch (const a2a::JsonRpcInvokeError& e) {
        res.set_content(a2a::make_error_response(id, e.code, e.what(), e.data).dump(), "application/json");
    } catch (const std::exception& e) {
        res.set_content(
            a2a::make_error_response(id, a2a::JsonRpcErrorCode::internal_error, "Internal error",
                                     json{{"detail", e.what()}})
                .dump(),
            "application/json");
    } catch (...) {
        res.set_content(
            a2a::make_error_response(id, a2a::JsonRpcErrorCode::internal_error, "Internal error").dump(),
            "application/json");
    }
}

AgentTask AgentServer::create_task_from_message_wire(const AgentMessage& initial_message,
                                                     const std::optional<std::string>& session_id,
                                                     const json& metadata) {
    AgentTask task;
    task.task_id = generate_task_id();
    task.session_id = session_id;
    task.status = AgentTaskStatus::PENDING;
    task.messages.push_back(initial_message);
    task.metadata = metadata;
    task.created_at = std::chrono::system_clock::now();
    task.updated_at = task.created_at;
    return task;
}

json AgentServer::jsonrpc_send_message(const json& params) {
    a2a::validate_send_message_params_for_dispatch(params);
    AgentMessage msg = a2a::message_from_a2a_wire(params["message"]);
    std::optional<std::string> session_id;
    json metadata = params.value("metadata", json::object());
    if (metadata.contains("contextId") && metadata["contextId"].is_string()) {
        session_id = metadata["contextId"].get<std::string>();
    }

    AgentTask task = create_task_from_message_wire(msg, session_id, metadata);

    auto control = std::make_shared<TaskControl>();
    control->set_effective_timeout_sec(compute_effective_task_timeout_sec(metadata));

    {
        std::lock_guard<std::mutex> lk(tasks_mutex_);
        active_tasks_[task.task_id] = task;
        task_controls_[task.task_id] = control;
    }

    auto builder = std::make_shared<workflow::GraphBuilder>("AgentTask_" + task.task_id);

    if (task_handler_ && task_queue_) {
        const std::string tid = task.task_id;
        AgentTask snap = task;
        if (!task_queue_->try_push([this, tid, snap, builder, control]() {
                run_agent_task_on_executor(tid, snap, builder, control);
            })) {
            std::lock_guard<std::mutex> lk(tasks_mutex_);
            active_tasks_.erase(tid);
            task_controls_.erase(tid);
            throw a2a::JsonRpcInvokeError(
                -32001, "queue_full",
                json{{"max_queued", static_cast<int>(task_queue_->max_queued())}});
        }
    }

    std::lock_guard<std::mutex> lk(tasks_mutex_);
    return json{{"task", a2a::task_to_a2a_wire(active_tasks_.at(task.task_id))}};
}

json AgentServer::jsonrpc_get_task(const json& params) {
    if (!params.contains("id") || !params["id"].is_string()) {
        throw a2a::JsonRpcInvokeError(a2a::JsonRpcErrorCode::invalid_params, "Invalid params: id");
    }
    const std::string id = params["id"].get<std::string>();
    std::lock_guard<std::mutex> lk(tasks_mutex_);
    auto it = active_tasks_.find(id);
    if (it == active_tasks_.end()) {
        throw a2a::JsonRpcInvokeError(a2a::JsonRpcErrorCode::invalid_params, "Invalid params: unknown task id");
    }
    return a2a::task_to_a2a_wire(it->second);
}

json AgentServer::jsonrpc_cancel_task(const json& params) {
    if (!params.contains("id") || !params["id"].is_string()) {
        throw a2a::JsonRpcInvokeError(a2a::JsonRpcErrorCode::invalid_params, "Invalid params: id");
    }
    const std::string id = params["id"].get<std::string>();
    std::lock_guard<std::mutex> lk(tasks_mutex_);
    auto it = active_tasks_.find(id);
    if (it == active_tasks_.end()) {
        throw a2a::JsonRpcInvokeError(a2a::JsonRpcErrorCode::invalid_params, "Invalid params: unknown task id");
    }
    if (task_status_is_terminal(it->second.status)) {
        return a2a::task_to_a2a_wire(it->second);
    }
    std::shared_ptr<TaskControl> ctrl;
    auto tc = task_controls_.find(id);
    if (tc != task_controls_.end()) {
        ctrl = tc->second;
    } else {
        ctrl = std::make_shared<TaskControl>();
        task_controls_[id] = ctrl;
    }
    ctrl->request_cancel();

    if (it->second.status == AgentTaskStatus::PENDING) {
        std::string terr;
        if (try_transition(it->second, AgentTaskStatus::CANCELLED, &terr)) {
            push_task_status_update(id, it->second);
            task_controls_.erase(id);
        }
    } else if (it->second.status == AgentTaskStatus::INPUT_REQUIRED) {
        std::string terr;
        if (try_transition(it->second, AgentTaskStatus::CANCELLED, &terr)) {
            push_task_status_update(id, it->second);
            task_controls_.erase(id);
        }
    }
    return a2a::task_to_a2a_wire(it->second);
}

json AgentServer::jsonrpc_list_tasks(const json& params) {
    (void)params;
    return json{{"tasks", json::array()}, {"nextPageToken", ""}, {"pageSize", 0}, {"totalSize", 0}};
}

void AgentServer::handle_tasks_send(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }

    try {
        json request = json::parse(req.body);
        AgentMessage initial_message = AgentMessage::from_json(request["message"]);
        std::optional<std::string> session_id;
        if (request.contains("session_id")) {
            session_id = request["session_id"].get<std::string>();
        }
        json metadata = request.value("metadata", json::object());

        AgentTask task;
        task.task_id = generate_task_id();
        task.session_id = session_id;
        task.status = AgentTaskStatus::PENDING;
        task.messages.push_back(initial_message);
        task.metadata = metadata;
        task.created_at = std::chrono::system_clock::now();
        task.updated_at = task.created_at;

        auto control = std::make_shared<TaskControl>();
        control->set_effective_timeout_sec(compute_effective_task_timeout_sec(metadata));

        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            active_tasks_[task.task_id] = task;
            task_controls_[task.task_id] = control;
        }

        auto builder = std::make_shared<workflow::GraphBuilder>("AgentTask_" + task.task_id);
        if (task_handler_ && task_queue_) {
            const std::string tid = task.task_id;
            AgentTask snap = task;
            if (!task_queue_->try_push([this, tid, snap, builder, control]() {
                    run_agent_task_on_executor(tid, snap, builder, control);
                })) {
                std::lock_guard<std::mutex> lk(tasks_mutex_);
                active_tasks_.erase(tid);
                task_controls_.erase(tid);
                res.status = 503;
                res.set_content(json{{"error", "queue_full"}}.dump(), "application/json");
                return;
            }
        }

        res.set_content(json{{"task", task.to_json()}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_tasks_get(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }

    std::string task_id = req.get_param_value("task_id");

    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = active_tasks_.find(task_id);

    if (it == active_tasks_.end()) {
        res.status = 404;
        res.set_content(json{{"error", "Task not found"}}.dump(), "application/json");
        return;
    }

    res.set_content(json{{"task", it->second.to_json()}}.dump(), "application/json");
}

void AgentServer::handle_tasks_cancel(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }

    try {
        json request = json::parse(req.body);
        std::string task_id = request["task_id"].get<std::string>();

        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = active_tasks_.find(task_id);

        if (it == active_tasks_.end()) {
            res.status = 404;
            res.set_content(json{{"error", "Task not found"}}.dump(), "application/json");
            return;
        }
        if (task_status_is_terminal(it->second.status)) {
            res.set_content(json{{"success", true}}.dump(), "application/json");
            return;
        }
        std::shared_ptr<TaskControl> ctrl;
        auto tc = task_controls_.find(task_id);
        if (tc != task_controls_.end()) {
            ctrl = tc->second;
        } else {
            ctrl = std::make_shared<TaskControl>();
            task_controls_[task_id] = ctrl;
        }
        ctrl->request_cancel();
        if (it->second.status == AgentTaskStatus::PENDING ||
            it->second.status == AgentTaskStatus::INPUT_REQUIRED) {
            std::string terr;
            if (try_transition(it->second, AgentTaskStatus::CANCELLED, &terr)) {
                push_task_status_update(task_id, it->second);
                task_controls_.erase(task_id);
            }
        }
        res.set_content(json{{"success", true}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_tasks_update(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
        return;
    }

    try {
        json request = json::parse(req.body);
        std::string task_id = request["task_id"].get<std::string>();
        AgentMessage additional = AgentMessage::from_json(request["message"]);

        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = active_tasks_.find(task_id);
        if (it == active_tasks_.end()) {
            res.status = 404;
            res.set_content(json{{"error", "Task not found"}}.dump(), "application/json");
            return;
        }

        it->second.messages.push_back(additional);
        if (it->second.status == AgentTaskStatus::INPUT_REQUIRED) {
            std::string terr;
            try_transition(it->second, AgentTaskStatus::WORKING, &terr);
        }
        it->second.updated_at = std::chrono::system_clock::now();
        res.set_content(json{{"task", it->second.to_json()}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_tasks_send_subscribe(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }

    std::string task_id = req.get_param_value("task_id");
    {
        std::lock_guard<std::mutex> lk(tasks_mutex_);
        if (active_tasks_.find(task_id) == active_tasks_.end()) {
            res.status = 404;
            res.set_content(R"({"error":"Task not found"})", "application/json");
            return;
        }
    }

    auto channel = std::make_shared<internal::SseServerChannel>();
    {
        std::lock_guard<std::mutex> lk(sse_mutex_);
        sse_subscribers_[task_id].push_back(channel);
    }

    res.status = 200;
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    const long ping_sec = std::strtol(sse_ping_interval_sec().c_str(), nullptr, 10);
    const auto ping_interval =
        ping_sec > 0 ? std::chrono::seconds(ping_sec) : std::chrono::seconds::max();

    auto weak_ch = std::weak_ptr<internal::SseServerChannel>(channel);
    res.set_chunked_content_provider(
        "text/event-stream",
        [weak_ch, ping_interval](std::size_t /*offset*/, httplib::DataSink& sink) {
            auto ch = weak_ch.lock();
            if (!ch) {
                return false;
            }
            static thread_local std::chrono::steady_clock::time_point last_ping =
                std::chrono::steady_clock::now();
            std::string chunk;
            using PR = internal::SseServerChannel::PopResult;
            PR r = ch->pop_or_wait(chunk, std::chrono::milliseconds(500));
            if (r == PR::chunk) {
                sink.write(chunk.data(), chunk.size());
                last_ping = std::chrono::steady_clock::now();
                return true;
            }
            if (r == PR::closed) {
                return false;
            }
            if (ping_interval != std::chrono::seconds::max()) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_ping >= ping_interval) {
                    const char ping[] = ":\n\n";
                    sink.write(ping, sizeof(ping) - 1);
                    last_ping = now;
                }
            }
            return true;
        },
        [this, task_id, channel]() { remove_sse_channel(task_id, channel); });
}

void AgentServer::handle_tasks_resubscribe(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }

    try {
        json request = json::parse(req.body);
        (void)request;
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_push_notification_set(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }

    try {
        json request = json::parse(req.body);
        std::string task_id = request["task_id"].get<std::string>();
        std::string webhook_url = request["webhook_url"].get<std::string>();

        std::lock_guard<std::mutex> lock(tasks_mutex_);
        webhook_urls_[task_id] = webhook_url;

        res.set_content(json{{"success", true}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void AgentServer::handle_push_notification_get(const httplib::Request& req, httplib::Response& res) {
    if (!validate_authentication(req)) {
        res.status = 401;
        return;
    }

    std::string task_id = req.get_param_value("task_id");

    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = webhook_urls_.find(task_id);

    if (it != webhook_urls_.end()) {
        res.set_content(json{{"webhook_url", it->second}}.dump(), "application/json");
    } else {
        res.status = 404;
        res.set_content(json{{"error", "Webhook not found"}}.dump(), "application/json");
    }
}

std::string AgentServer::generate_task_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);

    std::ostringstream oss;
    oss << "task_";
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << dis(gen);
    }
    return oss.str();
}

} // namespace agent_framework

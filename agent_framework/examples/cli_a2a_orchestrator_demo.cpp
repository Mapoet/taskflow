/**
 * @file cli_a2a_orchestrator_demo.cpp
 * @brief CLI demo: local ReAct loop + `a2a.send_message` to peers from peers.json (WP2.agent2agent)
 */
#include "CLI11.hpp"
#include "cli_multiline_tty.hpp"

#include <agent/a2a/orchestration.hpp>
#include <agent/a2a/outbound_task_supervisor.hpp>
#include <agent/a2a/peer_registry.hpp>
#include <agent/graph_executor.hpp>
#include <agent/internal/agent_thread_state.hpp>
#include <agent/llm_client.hpp>
#include <agent/prompt_renderer.hpp>
#include <agent/toolbus.hpp>
#include <agent/types.hpp>
#include <agent/ui_manager.hpp>

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <stdlib.h>
#define ISATTY(_fd) _isatty(_fd)
#else
#include <unistd.h>
#define ISATTY(_fd) isatty(_fd)
#endif

namespace {

using json = nlohmann::json;
using namespace agent_framework;
using namespace agent_framework::a2a;

std::atomic<bool> g_shutdown_requested{false};

void on_sigint_handler(int /*sig*/) {
    g_shutdown_requested.store(true);
}

std::string env_or(const char* key, const char* default_val) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(default_val);
}

void set_env_if_absent(const char* key, const char* val) {
    if (std::getenv(key) != nullptr) {
        return;
    }
#if defined(_WIN32)
    (void)_putenv_s(key, val);
#else
    (void)::setenv(key, val, 0);
#endif
}

void apply_live_llm_env_defaults() {
    if (std::getenv("OPENAI_API_KEY") == nullptr) {
        const char* dk = std::getenv("DEEPSEEK_API_KEY");
        if (dk && *dk) {
#if defined(_WIN32)
            (void)_putenv_s("OPENAI_API_KEY", dk);
#else
            (void)::setenv("OPENAI_API_KEY", dk, 1);
#endif
        }
    }
    set_env_if_absent("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1");
    set_env_if_absent("AGENT_LLM_PROVIDER", "openai");
    set_env_if_absent("AGENT_HTTP_TIMEOUT_SEC", "120");
    set_env_if_absent("AGENT_LLM_MAX_RETRIES", "1");
    if (std::getenv("AGENT_LLM_MODEL") == nullptr) {
        const std::string m = env_or("DEEPSEEK_MODEL", "deepseek-chat");
#if defined(_WIN32)
        (void)_putenv_s("AGENT_LLM_MODEL", m.c_str());
#else
        (void)::setenv("AGENT_LLM_MODEL", m.c_str(), 1);
#endif
    }
}

std::string join_peer_ids(const std::vector<std::string>& ids) {
    std::ostringstream o;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) {
            o << ", ";
        }
        o << ids[i];
    }
    return o.str();
}

int run_graph_once(tf::Executor& executor,
                   const AgentConfig& cfg,
                   const AgentWorkflowDeps& deps,
                   const std::shared_ptr<internal::AgentThreadState>& state,
                   CLIHandler& cli) {
    GraphExecutor gx;
    ReactCliRunRequest req;
    req.config = cfg;
    req.deps = deps;
    req.session = state;
    req.options.sink.sink_node_name = "CliSink";
    req.options.sink.on_final_json = [&cli](const json& j) { cli.handle_final_result(j); };
    req.options.graph_options.stream_callback = [&cli](std::string_view tok) {
        if (!g_shutdown_requested.load()) {
            cli.handle_stream_token(tok);
        }
    };
    try {
        WorkflowResult wr = gx.run_react_cli_sync(executor, req);
        if (!wr.success) {
            cli.handle_error(wr.error_message.value_or("run_react_cli_sync failed"));
            return wr.exit_code != 0 ? wr.exit_code : 1;
        }
    } catch (const std::exception& e) {
        cli.handle_error(std::string("run_react_cli_sync: ") + e.what());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app("cli_a2a_orchestrator_demo — ReAct + A2A peers (peers.json)\n"
                 "Requires remote agent_server_demo (or compatible) per peer origin.");
    app.get_formatter()->column_width(32);

    std::string peers_file;
    std::string prompt_arg;
    std::string provider_arg;
    int max_iterations = -1;
    bool verbose = false;
    bool merge_streams = false;
    bool register_peer_aliases = false;
    app.add_option("--peers-file", peers_file, "Path to peers.json")->required();
    app.add_flag("-v,--verbose", verbose, "Set AGENT_LOG_LEVEL=debug for this process");
    app.add_option("-p,--prompt", prompt_arg, "Single user message; then exit");
    app.add_option("--provider", provider_arg, "Override AGENT_LLM_PROVIDER");
    app.add_option("--max-iterations", max_iterations, "Override AgentConfig::max_iterations");
    app.add_flag("--merge-streams", merge_streams,
                 "Route remote A2A status lines into the same stream handler as local LLM tokens");
    app.add_flag("--register-peer-aliases", register_peer_aliases,
                 "Also register a2a.send_message__<peer_id> per peer");
    app.set_help_flag("-h,--help", "Print help");

    CLI11_PARSE(app, argc, argv);

    if (verbose) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_LOG_LEVEL", "debug");
#else
        (void)::setenv("AGENT_LOG_LEVEL", "debug", 1);
#endif
    }
    if (!provider_arg.empty()) {
#if defined(_WIN32)
        (void)_putenv_s("AGENT_LLM_PROVIDER", provider_arg.c_str());
#else
        (void)::setenv("AGENT_LLM_PROVIDER", provider_arg.c_str(), 1);
#endif
    }

    std::signal(SIGINT, on_sigint_handler);
    apply_live_llm_env_defaults();

    A2aPeerRegistry registry;
    try {
        registry.load_from_json_file(peers_file);
        registry.discover_all();
    } catch (const std::exception& e) {
        std::cerr << "[error] peers / discover: " << e.what() << '\n';
        return 1;
    }

    CLIHandler cli;
    std::shared_ptr<LLMClient> llm;
    try {
        llm = std::make_shared<LLMClient>(LLMClient::from_env());
    } catch (const std::exception& e) {
        std::cerr << "[error] LLM init: " << e.what() << "\n";
        return 1;
    }
    auto renderer = std::make_shared<PromptRenderer>();
    llm->set_prompt_renderer(renderer);

    auto bus = std::make_shared<ToolBus>();
    PeerSessionBook session_book;
    A2aToolRegistrationOptions orch_opts;
    orch_opts.register_per_peer_aliases = register_peer_aliases;
    orch_opts.default_timeout_ms = 0;
    if (merge_streams) {
        orch_opts.on_remote_log = [&cli](std::string_view peer_id, std::string_view line) {
            std::string s;
            s.reserve(peer_id.size() + line.size() + 16);
            s.append("[a2a:");
            s.append(peer_id);
            s.append("] ");
            s.append(line);
            s.push_back('\n');
            cli.handle_stream_token(s);
        };
    } else {
        orch_opts.on_remote_log = [](std::string_view peer_id, std::string_view line) {
            std::clog << "[a2a:" << peer_id << "] " << line << '\n';
        };
    }

    OutboundSessionPolicy sup_pol;
    auto supervisor = std::make_shared<OutboundTaskSupervisor>(registry, session_book, sup_pol);
    supervisor->set_remote_log(orch_opts.on_remote_log);

    try {
        register_a2a_orchestrator_tools(*bus, registry, session_book, supervisor, orch_opts);
    } catch (const std::exception& e) {
        std::cerr << "[error] register A2A tools: " << e.what() << '\n';
        return 1;
    }

    const std::vector<std::string> pids = registry.peer_ids();
    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;
    deps.skills = nullptr;

    AgentConfig cfg;
    cfg.name = "cli_a2a_orchestrator_demo";
    cfg.system_prompt =
        "You are an orchestrator with access to remote agents via A2A.\n"
        "Peers loaded: " +
        join_peer_ids(pids) +
        ".\n"
        "Fine-grained tools: `" +
        std::string(kA2aToolSubmitTask) +
        "` (peer_id, user_text; optional timeout_ms, monitor, continue_session, metadata), `" +
        std::string(kA2aToolGetTaskStatus) +
        "` (local_handle or peer_id+remote_task_id), `" + std::string(kA2aToolWaitTasks) +
        "` (handles[] or peer_task_pairs[], mode=all|any, timeout_ms), `" +
        std::string(kA2aToolCancelTask) + "`, `" + std::string(kA2aToolExtendTimeout) +
        "` (local_handle, extra_ms), `" + std::string(kA2aToolListSubtasks) +
        "` (since_seq, peer_id?, limit).\n"
        "Sync convenience: `" +
        std::string(kA2aOrchestratorToolSendMessage) +
        "` waits until the remote task completes in one step.\n"
        "Prefer batching: multiple `" +
        std::string(kA2aToolSubmitTask) +
        "` then one `" + std::string(kA2aToolWaitTasks) + "`.\n"
        "Reply concisely; pick the correct peer_id.\n";

    if (const char* m = std::getenv("AGENT_LLM_MODEL")) {
        cfg.model_config.model_name = m;
    }
    if (const char* s = std::getenv("AGENT_LLM_STREAM")) {
        cfg.model_config.stream = std::string(s) != "0";
    } else {
        cfg.model_config.stream = false; // default to non-streaming for reliability
    }
    if (max_iterations > 0) {
        cfg.max_iterations = max_iterations;
    }

    // Apply model config to LLM client
    const char* provider = std::getenv("AGENT_LLM_PROVIDER");
    if (!provider || strlen(provider) == 0) provider = "openai";
    llm->configure(provider, cfg.model_config);

    tf::Executor executor;
    auto state = std::make_shared<internal::AgentThreadState>();
    state->outbound_supervisor = supervisor;

    auto exec_line = [&](const std::string& line) -> int {
        if (g_shutdown_requested.load()) {
            return 130;
        }
        if (state->outbound_supervisor) {
            state->outbound_supervisor->on_user_turn_barrier();
        }
        state->skill_prompt_cache.reset();
        state->active_skill_id.reset();
        state->initial_user_prompt = line;
        std::clog << "[cli_a2a_orchestrator_demo] running agent loop...\n" << std::flush;
        return run_graph_once(executor, cfg, deps, state, cli);
    };

    if (!prompt_arg.empty()) {
        return exec_line(prompt_arg);
    }

    if (ISATTY(STDIN_FILENO)) {
        std::clog << "[cli_a2a_orchestrator_demo] REPL (peers=" << pids.size() << ")\n" << std::flush;
        std::cout << "Enter=newline, Ctrl+Enter or Ctrl+O=submit, :quit / :q, EOF.\n" << std::flush;
        std::string line;
        while (!g_shutdown_requested.load()) {
            std::cout << "> " << std::flush;
            bool got = cli_multiline_tty::read_multiline_repl_input(line, &g_shutdown_requested);
            if (!got) {
                if (g_shutdown_requested.load()) {
                    std::cout << "\n[interrupt]\n";
                    break;
                }
                if (!std::getline(std::cin, line)) {
                    break;
                }
            }
            if (g_shutdown_requested.load()) {
                std::cout << "\n[interrupt]\n";
                break;
            }
            if (line == ":quit" || line == ":q") {
                break;
            }
            if (line.empty()) {
                continue;
            }
            const int rc = exec_line(line);
            if (rc != 0) {
                return rc;
            }
            std::cout << std::flush;
        }
        return g_shutdown_requested.load() ? 130 : 0;
    }

    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "[error] no input (pipe mode needs one line)\n";
        return 1;
    }
    return exec_line(line);
}

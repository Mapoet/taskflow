/**
 * @file agent_server_demo.cpp
 * @brief Production-oriented Live A2A AgentServer demo.
 */
#include "CLI11.hpp"
#include "common/agent_example_bootstrap.hpp"

#include <agent/a2a/auth_gate.hpp>
#include <agent/agent_server/agent_server.hpp>
#include <agent/session/session_store.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace agent_framework;

void set_env(const char* key, const std::string& value) {
    example::set_environment_override(key, value);
}

void set_env_if_missing(const char* key, const std::string& value) {
    example::set_env_if_absent(key, value);
}

std::string normalized_path(std::string path) {
    if(path.empty()) path = "/rpc";
    if(path.front() != '/') path.insert(path.begin(), '/');
    return path;
}

AgentCard build_card(const std::string& public_base, const std::string& rpc_path,
                     const example::LiveRuntime& runtime) {
    AgentCard card;
    card.name = "agent-server-live";
    card.description = "Live AgentServer with LLM, MCP, Skills, tools, verifier, and A2A streaming";
    card.provider = "taskflow";
    std::string base = public_base;
    while(!base.empty() && base.back() == '/') base.pop_back();
    card.api_endpoint = base + rpc_path;
    card.capabilities = {"streaming", "pushNotifications"};
    AgentSkill skill;
    skill.name = "agent.execute";
    skill.description = "Run the configured live Agent workflow";
    skill.input_schema = json::object();
    skill.output_schema = json::object();
    skill.required_capabilities = {"streaming"};
    card.skills.push_back(std::move(skill));
    for(const auto& service : runtime.bootstrap.registered_mcp_services)
        card.capabilities.push_back("mcp:" + service);
    if(runtime.skills) card.capabilities.push_back("skills");
    return card;
}
} // namespace

int main(int argc, char** argv) {
    CLI::App app("agent_server_demo — Live A2A AgentServer");
    app.get_formatter()->column_width(32);
    int port = 8080;
    std::string bind = "127.0.0.1";
    std::string card_public_base;
    std::string rpc_path = "/rpc";
    std::string fs_root;
    std::string skills_root;
    std::string skill_authoring_root;
    std::string cursor_mcp_json;
    std::vector<std::string> skip_mcp_services;
    std::string provider;
    std::string model;
    int max_iterations = -1;
    bool no_skills = false;
    bool no_cursor_mcp = false;
    bool no_tier_b = false;
    std::string verifier = "";
    std::string session = "memory";
    std::string session_db;
    std::string auth_token;
    bool verbose = false;
    app.add_option("--port", port, "Listen port");
    app.add_option("--bind", bind, "Bind address (default 127.0.0.1)");
    app.add_option("--card-public-base", card_public_base, "Public base URL used in Agent Card");
    app.add_option("--json-rpc-path", rpc_path, "JSON-RPC path (default /rpc)");
    app.add_option("--fs-root", fs_root, "Filesystem jail root (AGENT_FS_ROOT)");
    app.add_option("--skills-root", skills_root, "Installed/read-only Skills root");
    app.add_option("--skill-authoring-root", skill_authoring_root, "Writable Skill authoring root");
    app.add_flag("--no-skills", no_skills, "Disable Skill services");
    app.add_option("--cursor-mcp-json", cursor_mcp_json, "Cursor mcp.json path");
    app.add_flag("--no-cursor-mcp", no_cursor_mcp, "Disable Cursor MCP import");
    app.add_option("--skip-mcp-service", skip_mcp_services, "Skip one Cursor MCP service");
    app.add_option("--max-iterations", max_iterations, "Override Agent loop iteration limit");
    app.add_option("--provider", provider, "Override AGENT_LLM_PROVIDER");
    app.add_option("--model", model, "Override AGENT_LLM_MODEL");
    app.add_flag("--no-tier-b", no_tier_b, "Disable Tier-B input policy");
    app.add_option("--verifier", verifier, "Verifier mode: on, off, or sample");
    app.add_option("--session", session, "Session store: memory or sqlite");
    app.add_option("--session-db", session_db, "SQLite session database path");
    app.add_option("--auth-token", auth_token, "Bearer token (otherwise AGENT_SERVER_AUTH_TOKEN)");
    app.add_flag("-v,--verbose", verbose, "Enable debug diagnostics");
    app.set_help_flag("-h,--help", "Show this help");
    CLI11_PARSE(app, argc, argv);
    if(port < 1 || port > 65535) throw CLI::ValidationError("--port", "must be 1..65535");
    if(session != "memory" && session != "sqlite")
        throw CLI::ValidationError("--session", "must be memory or sqlite");
    if(!verifier.empty() && verifier != "on" && verifier != "off" && verifier != "sample")
        throw CLI::ValidationError("--verifier", "must be on, off, or sample");

    set_env("AGENT_SERVER_BIND", bind);
    set_env("AGENT_SERVER_PORT", std::to_string(port));
    set_env("AGENT_SERVER_JSON_RPC_PATH", normalized_path(rpc_path));
    set_env("AGENT_SERVER_LEGACY_REST", "0");
    set_env("AGENT_A2A_STRICT", "1");
    set_env_if_missing("AGENT_SERVER_SSE_PING_SEC", "0");
    set_env_if_missing("AGENT_SERVER_WORKER_THREADS", "4");
    set_env_if_missing("AGENT_SERVER_EXECUTOR_THREADS", "2");
    set_env_if_missing("AGENT_WEB_ENABLE", "1");
    set_env_if_missing("AGENT_EXPR_ENABLE", "1");
    set_env_if_missing("AGENT_DRAW_ENABLE", "1");
    set_env_if_missing("AGENT_VERIFIER", "on");
    if(!fs_root.empty()) set_env("AGENT_FS_ROOT", fs_root);
    if(!skill_authoring_root.empty()) set_env("AGENT_SKILL_AUTHORING_DIR", skill_authoring_root);
    if(!verifier.empty()) set_env("AGENT_VERIFIER", verifier);
    if(!auth_token.empty()) set_env("AGENT_SERVER_AUTH_TOKEN", auth_token);
    if(verbose) set_env("AGENT_LOG_LEVEL", "debug");

    example::apply_skill_cli_options(skills_root, skill_authoring_root, no_skills);
    example::LiveRuntimeOptions options;
    options.agent_name = "agent-server-live";
    options.skills_root = skills_root;
    options.cursor_mcp_config = cursor_mcp_json;
    options.skip_mcp_services = skip_mcp_services;
    options.provider = provider;
    options.model = model;
    options.max_iterations = max_iterations;
    options.use_cursor_skill_roots = true;
    options.import_cursor_mcp = !no_cursor_mcp;
    options.enable_skills = !no_skills;
    options.enable_tier_b = !no_tier_b;
    options.verbose = verbose;

    example::LiveRuntime runtime;
    try {
        runtime = example::build_live_runtime(options);
    } catch(const std::exception& error) {
        std::cerr << "agent_server_demo: Live LLM initialization failed: " << error.what() << "\n"
                  << "Set AGENT_LLM_PROVIDER and its API key (or DEEPSEEK_API_KEY), or use --help.\n";
        return 1;
    }
    for(const auto& diagnostic : runtime.bootstrap.diagnostics)
        std::clog << "[agent-server bootstrap] " << diagnostic << '\n';

    const std::string jpath = normalized_path(rpc_path);
    if(card_public_base.empty()) card_public_base = "http://" + bind + ":" + std::to_string(port);
    AgentServer server(port);
    const AgentCard card = build_card(card_public_base, jpath, runtime);
    server.register_agent_card(card);
    server.set_execution_profile(example::to_execution_profile(runtime));
    server.set_input_preprocess_toolbus(runtime.toolbus);
    if(session == "sqlite") {
        if(session_db.empty()) {
            const char* configured = std::getenv("AGENT_SESSION_DB");
            session_db = configured && *configured ? configured : "agent-server-sessions.sqlite";
        }
        server.set_session_store(std::make_shared<SQLiteSessionStore>(session_db));
    } else {
        server.set_session_store(std::make_shared<InMemorySessionStore>());
    }
    if(const char* token = std::getenv("AGENT_SERVER_AUTH_TOKEN"); token && *token) {
        const std::string expected = std::string("Bearer ") + token;
        server.set_authentication_validator([expected](const a2a::AuthContext& ctx) {
            const auto it = ctx.headers_lower.find("authorization");
            return it != ctx.headers_lower.end() && it->second == expected;
        });
    }
    const char* verifier_mode = std::getenv("AGENT_VERIFIER");
    std::cerr << "agent_server_demo: listening live card.url=" << card.api_endpoint
              << " tier_b=" << (options.enable_tier_b ? "on" : "off")
              << " verifier=" << (verifier_mode && *verifier_mode ? verifier_mode : "off")
              << " session=" << session << '\n';
    server.start();
    return 0;
}

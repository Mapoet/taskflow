/**
 * @file test_a2a_live_multi_agent.cpp
 * @brief Tier D: two base URLs, worker → reviewer handoff (agent_server_demo roles)
 */
#include <agent/agent_client.hpp>
#include <agent/types.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using agent_framework::AgentCard;
using agent_framework::AgentClient;
using agent_framework::AgentClientOptions;
using agent_framework::AgentMessage;
using agent_framework::AgentPart;
using agent_framework::AgentTask;
using agent_framework::AgentTaskStatus;

void fail(const char* m) {
    std::cerr << "test_a2a_live_multi_agent: " << m << "\n";
    std::exit(1);
}

void split_api_endpoint(const std::string& api_ep, std::string& out_base, std::string& out_path) {
    const std::size_t scheme = api_ep.find("://");
    if (scheme == std::string::npos) {
        fail("split_api_endpoint");
    }
    const std::size_t path_start = api_ep.find('/', scheme + 3);
    if (path_start == std::string::npos) {
        out_base = api_ep;
        out_path = "/";
        return;
    }
    out_base = api_ep.substr(0, path_start);
    out_path = api_ep.substr(path_start);
    if (out_path.empty()) {
        out_path = "/";
    }
}

AgentClient make_rpc_client_for_card(const AgentCard& card) {
    std::string rpc_base;
    std::string rpc_path;
    split_api_endpoint(card.api_endpoint, rpc_base, rpc_path);
    AgentClientOptions rpc_opts;
    rpc_opts.use_legacy_rest = false;
    rpc_opts.json_rpc_path = rpc_path;
    return AgentClient(rpc_base, rpc_opts);
}

void apply_token(AgentClient& c) {
    const char* tok = std::getenv("AGENT_A2A_LIVE_TOKEN");
    if (tok != nullptr && tok[0] != '\0') {
        json auth;
        auth["type"] = "bearer";
        auth["token"] = std::string(tok);
        c.set_authentication(auth);
    }
}

AgentTask wait_terminal(AgentClient& cli, const std::string& tid) {
    AgentTask t;
    for (int i = 0; i < 400; ++i) {
        t = cli.get_task("", tid).get();
        if (t.status == AgentTaskStatus::COMPLETED || t.status == AgentTaskStatus::FAILED ||
            t.status == AgentTaskStatus::CANCELLED) {
            return t;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    fail("timeout waiting task");
    return t;
}

std::string extract_blob(const AgentTask& t) {
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

void run_once(const std::string& origin_a, const std::string& origin_b) {
    AgentClientOptions dopts;
    dopts.use_legacy_rest = false;
    dopts.json_rpc_path = "/rpc";

    AgentClient da(origin_a, dopts);
    AgentClient db(origin_b, dopts);
    apply_token(da);
    apply_token(db);

    AgentCard ca = da.discover_agent("/.well-known/agent-card.json").get();
    AgentCard cb = db.discover_agent("/.well-known/agent-card.json").get();
    if (ca.name == cb.name) {
        fail("card names must differ (Tier D)");
    }
    if (ca.skills.empty() || cb.skills.empty()) {
        fail("skills required on both cards");
    }
    if (ca.skills[0].name == cb.skills[0].name) {
        fail("skills must differ");
    }

    AgentClient ra = make_rpc_client_for_card(ca);
    AgentClient rb = make_rpc_client_for_card(cb);
    apply_token(ra);
    apply_token(rb);

    AgentMessage ma;
    ma.role = AgentMessage::Role::USER;
    AgentPart pa;
    pa.type = AgentPart::Type::TEXT;
    pa.text = std::string("produce output");
    ma.parts.push_back(std::move(pa));
    AgentTask ta0 = ra.send_task("", ma, std::nullopt, json::object()).get();
    AgentTask ta = wait_terminal(ra, ta0.task_id);
    if (ta.status != AgentTaskStatus::COMPLETED) {
        fail("worker did not complete");
    }
    const std::string sig_blob = extract_blob(ta);
    if (sig_blob.find("SIG_TIER_D_A") == std::string::npos) {
        fail("worker output missing signature");
    }

    AgentMessage mb;
    mb.role = AgentMessage::Role::USER;
    AgentPart pb;
    pb.type = AgentPart::Type::TEXT;
    pb.text = std::string("review: ") + sig_blob;
    mb.parts.push_back(std::move(pb));
    AgentTask tb0 = rb.send_task("", mb, std::nullopt, json::object()).get();
    AgentTask tb = wait_terminal(rb, tb0.task_id);
    if (tb.status != AgentTaskStatus::COMPLETED) {
        fail("reviewer should pass when signature present");
    }
}

} // namespace

int main() {
    const char* m = std::getenv("AGENT_A2A_MULTI_LIVE");
    const char* a = std::getenv("AGENT_A2A_LIVE_AGENT_A_URL");
    const char* b = std::getenv("AGENT_A2A_LIVE_AGENT_B_URL");
    if (m == nullptr || m[0] == '\0' || m[0] == '0') {
        std::cout << "test_a2a_live_multi_agent: SKIP (AGENT_A2A_MULTI_LIVE=1 required)\n";
        return 0;
    }
    if (a == nullptr || b == nullptr || !a[0] || !b[0]) {
        std::cout << "test_a2a_live_multi_agent: SKIP (set AGENT_A2A_LIVE_AGENT_A_URL and _B_URL)\n";
        return 0;
    }
    std::string oa = a;
    std::string ob = b;
    while (!oa.empty() && oa.back() == '/') {
        oa.pop_back();
    }
    while (!ob.empty() && ob.back() == '/') {
        ob.pop_back();
    }

    try {
        for (int rep = 0; rep < 3; ++rep) {
            (void)rep;
            run_once(oa, ob);
        }
    } catch (const std::exception& e) {
        std::cerr << "test_a2a_live_multi_agent: " << e.what() << "\n";
        return 1;
    }
    std::cout << "test_a2a_live_multi_agent: ok\n";
    return 0;
}

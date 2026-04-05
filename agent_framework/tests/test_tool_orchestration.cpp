/**
 * @file test_tool_orchestration.cpp
 * @brief WP2.1b tool orchestration unit tests (O-1 .. O-6)
 */

#include <agent/toolbus.hpp>
#include <agent/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

ToolMeta meta_named(const std::string& name, ToolSideEffect se) {
    ToolMeta m;
    m.name = name;
    m.description = name;
    m.schema = json{{"type", "object"}, {"properties", json::object()}};
    m.side_effect = se;
    return m;
}

struct EnvRestore {
    const char* key;
    bool had_value;
    std::string value;
    explicit EnvRestore(const char* k) : key(k) {
        const char* v = std::getenv(k);
        had_value = (v != nullptr);
        if (had_value) {
            value = v;
        }
    }
    ~EnvRestore() {
        if (had_value) {
            ::setenv(key, value.c_str(), 1);
        } else {
            ::unsetenv(key);
        }
    }
};

void clear_tool_env() {
    ::unsetenv("AGENT_TOOL_ALLOWLIST");
    ::unsetenv("AGENT_TOOL_PARALLEL_READS");
    ::unsetenv("AGENT_TOOL_MAX_PARALLEL");
}

// O-1: parallel off, three ReadOnly -> strict call order
void test_o1() {
    clear_tool_env();
    auto bus = std::make_shared<ToolBus>();
    std::vector<std::string> order;
    std::mutex mu;
    for (const char* n : {"a", "b", "c"}) {
        const std::string nm(n);
        bus->register_local_tool(
            nm,
            [&, nm](const json& /*j*/) {
                {
                    std::lock_guard<std::mutex> lock(mu);
                    order.push_back(nm);
                }
                return json{{"n", nm}};
            },
            meta_named(nm, ToolSideEffect::ReadOnly));
    }
    std::vector<CallSpec> calls;
    for (const char* n : {"a", "b", "c"}) {
        CallSpec c;
        c.name = n;
        c.arguments = json::object();
        calls.push_back(std::move(c));
    }
    ToolOrchestrationOptions opts;
    opts.enable_parallel_reads = false;
    opts.max_parallel_reads = 4;
    auto classify = [&](std::string_view name) -> ToolSideEffect {
        return bus->get_tool_meta(std::string(name)).side_effect;
    };
    std::vector<json> results = execute_tool_calls_sequenced(bus, calls, opts, classify);
    if (order.size() != 3U) {
        std::cerr << "O-1: order size " << order.size() << "\n";
        std::abort();
    }
    if (order[0] != "a" || order[1] != "b" || order[2] != "c") {
        std::cerr << "O-1: bad order\n";
        std::abort();
    }
    if (results.size() != 3U) {
        std::cerr << "O-1: results size\n";
        std::abort();
    }
}

// O-2: parallel on, max_parallel=2, four 50ms ReadOnly -> wall < 150ms
void test_o2() {
    clear_tool_env();
    auto bus = std::make_shared<ToolBus>();
    for (int i = 0; i < 4; ++i) {
        const std::string nm = "s" + std::to_string(i);
        bus->register_local_tool(
            nm,
            [](const json& /*j*/) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return json{{"ok", true}};
            },
            meta_named(nm, ToolSideEffect::ReadOnly));
    }
    std::vector<CallSpec> calls;
    for (int i = 0; i < 4; ++i) {
        CallSpec c;
        c.name = "s" + std::to_string(i);
        c.arguments = json::object();
        calls.push_back(std::move(c));
    }
    ToolOrchestrationOptions opts;
    opts.enable_parallel_reads = true;
    opts.max_parallel_reads = 2;
    auto classify = [&](std::string_view name) -> ToolSideEffect {
        return bus->get_tool_meta(std::string(name)).side_effect;
    };
    const auto t0 = std::chrono::steady_clock::now();
    (void)execute_tool_calls_sequenced(bus, calls, opts, classify);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    if (ms >= 150) {
        std::cerr << "O-2: wall too high ms=" << ms << " (expect < 150)\n";
        std::abort();
    }
}

// O-3: R,R,W,R with parallel on — write after both reads complete
void test_o3() {
    clear_tool_env();
    auto bus = std::make_shared<ToolBus>();
    std::atomic<int> reads_in_flight{0};
    std::atomic<int> max_reads_in_flight{0};
    std::vector<std::string> events;
    std::mutex ev_mu;

    bus->register_local_tool(
        "rA",
        [&](const json& /*j*/) {
            int v = ++reads_in_flight;
            int cur = max_reads_in_flight.load();
            while (cur < v && !max_reads_in_flight.compare_exchange_weak(cur, v)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            --reads_in_flight;
            {
                std::lock_guard<std::mutex> lock(ev_mu);
                events.push_back("rA_done");
            }
            return json{{"t", "rA"}};
        },
        meta_named("rA", ToolSideEffect::ReadOnly));
    bus->register_local_tool(
        "rB",
        [&](const json& /*j*/) {
            int v = ++reads_in_flight;
            int cur = max_reads_in_flight.load();
            while (cur < v && !max_reads_in_flight.compare_exchange_weak(cur, v)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            --reads_in_flight;
            {
                std::lock_guard<std::mutex> lock(ev_mu);
                events.push_back("rB_done");
            }
            return json{{"t", "rB"}};
        },
        meta_named("rB", ToolSideEffect::ReadOnly));
    bus->register_local_tool(
        "w1",
        [&](const json& /*j*/) {
            {
                std::lock_guard<std::mutex> lock(ev_mu);
                events.push_back("w_start");
            }
            if (reads_in_flight.load() != 0) {
                std::cerr << "O-3: write started while reads in flight\n";
                std::abort();
            }
            return json{{"t", "w"}};
        },
        meta_named("w1", ToolSideEffect::Write));
    bus->register_local_tool(
        "rC",
        [&](const json& /*j*/) {
            {
                std::lock_guard<std::mutex> lock(ev_mu);
                events.push_back("rC_done");
            }
            return json{{"t", "rC"}};
        },
        meta_named("rC", ToolSideEffect::ReadOnly));

    std::vector<CallSpec> calls;
    for (const char* n : {"rA", "rB", "w1", "rC"}) {
        CallSpec c;
        c.name = n;
        c.arguments = json::object();
        calls.push_back(std::move(c));
    }
    ToolOrchestrationOptions opts;
    opts.enable_parallel_reads = true;
    opts.max_parallel_reads = 4;
    auto classify = [&](std::string_view name) -> ToolSideEffect {
        return bus->get_tool_meta(std::string(name)).side_effect;
    };
    std::vector<json> results = execute_tool_calls_sequenced(bus, calls, opts, classify);
    if (results.size() != 4U) {
        std::cerr << "O-3: results size\n";
        std::abort();
    }
    // w_start must occur only after both read dones
    std::size_t wpos = 0;
    bool found = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i] == "w_start") {
            wpos = i;
            found = true;
            break;
        }
    }
    if (!found) {
        std::cerr << "O-3: no w_start\n";
        std::abort();
    }
    bool ra = false, rb = false;
    for (std::size_t i = 0; i < wpos; ++i) {
        if (events[i] == "rA_done") {
            ra = true;
        }
        if (events[i] == "rB_done") {
            rb = true;
        }
    }
    if (!ra || !rb) {
        std::cerr << "O-3: write before both reads done\n";
        std::abort();
    }
}

// O-4: two Write tools — never two concurrent
void test_o4() {
    clear_tool_env();
    auto bus = std::make_shared<ToolBus>();
    std::atomic<int> active{0};
    auto make_w = [&](const char* name) {
        return std::function<json(const json&)>([&, name](const json& /*j*/) {
            const int a = ++active;
            if (a != 1) {
                std::cerr << "O-4: concurrent write detected\n";
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            --active;
            return json{{"n", name}};
        });
    };
    bus->register_local_tool("wA", make_w("wA"), meta_named("wA", ToolSideEffect::Write));
    bus->register_local_tool("wB", make_w("wB"), meta_named("wB", ToolSideEffect::Write));
    std::vector<CallSpec> calls;
    for (const char* n : {"wA", "wB"}) {
        CallSpec c;
        c.name = n;
        c.arguments = json::object();
        calls.push_back(std::move(c));
    }
    ToolOrchestrationOptions opts;
    opts.enable_parallel_reads = true;
    opts.max_parallel_reads = 4;
    auto classify = [&](std::string_view name) -> ToolSideEffect {
        return bus->get_tool_meta(std::string(name)).side_effect;
    };
    (void)execute_tool_calls_sequenced(bus, calls, opts, classify);
}

// O-5: env AGENT_TOOL_PARALLEL_READS=0 overrides config on
void test_o5() {
    clear_tool_env();
    EnvRestore r1("AGENT_TOOL_PARALLEL_READS");
    ::setenv("AGENT_TOOL_PARALLEL_READS", "0", 1);
    AgentConfig cfg;
    cfg.enable_parallel_read_tools = true;
    cfg.max_parallel_read_tools = 4;
    ToolOrchestrationOptions opts = resolve_tool_orchestration_options(cfg);
    if (opts.enable_parallel_reads) {
        std::cerr << "O-5: env should force parallel off\n";
        std::abort();
    }
}

// O-6: Unknown between ReadOnly — segments; unknown serial
void test_o6() {
    clear_tool_env();
    auto bus = std::make_shared<ToolBus>();
    std::vector<std::string> order;
    std::mutex mu;
    bus->register_local_tool(
        "r1",
        [&](const json& /*j*/) {
            {
                std::lock_guard<std::mutex> lock(mu);
                order.push_back("r1");
            }
            return json{};
        },
        meta_named("r1", ToolSideEffect::ReadOnly));
    bus->register_local_tool(
        "u1",
        [&](const json& /*j*/) {
            {
                std::lock_guard<std::mutex> lock(mu);
                order.push_back("u1");
            }
            return json{};
        },
        meta_named("u1", ToolSideEffect::Unknown));
    bus->register_local_tool(
        "r2",
        [&](const json& /*j*/) {
            {
                std::lock_guard<std::mutex> lock(mu);
                order.push_back("r2");
            }
            return json{};
        },
        meta_named("r2", ToolSideEffect::ReadOnly));

    std::vector<CallSpec> calls;
    for (const char* n : {"r1", "u1", "r2"}) {
        CallSpec c;
        c.name = n;
        c.arguments = json::object();
        calls.push_back(std::move(c));
    }
    ToolOrchestrationOptions opts;
    opts.enable_parallel_reads = true;
    opts.max_parallel_reads = 4;
    auto classify = [&](std::string_view name) -> ToolSideEffect {
        return bus->get_tool_meta(std::string(name)).side_effect;
    };
    (void)execute_tool_calls_sequenced(bus, calls, opts, classify);
    if (order.size() != 3U || order[0] != "r1" || order[1] != "u1" || order[2] != "r2") {
        std::cerr << "O-6: order mismatch\n";
        std::abort();
    }
}

} // namespace

int main() {
    clear_tool_env();
    test_o1();
    test_o2();
    test_o3();
    test_o4();
    test_o5();
    test_o6();
    std::cout << "test_tool_orchestration: all passed\n";
    return 0;
}

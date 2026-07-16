/**
 * @file test_agent_loop_wp5.cpp
 * @brief WP1.5 Agent 循环综合集成测试：Live LLM + **Cursor MCP 工具** + 自然语言行程问题
 *
 * **运行**：默认跳过；`AGENT_TEST_LIVE=1` 时执行（需 API Key，约定同 `test_llm_client_wp1.cpp`）。
 *
 * **行为**：
 * - 默认从 Cursor MCP 配置加载全部已配置 server 的工具（`register_mcp_from_cursor_config`）。
 *   - 路径：`AGENT_TEST_CURSOR_MCP_JSON`；若未设置则传空路径，由 ToolBus 使用
 *     `AGENT_MCP_CONFIG_PATH` 或默认 `~/.cursor/mcp.json`。
 * - 用户问题为开放式中文：**从现在出发，北京→西安，何时能到**（高铁/火车等，需结合工具查信息）。
 * - 构图使用 **`build_cli_agent_graph_with_terminal_sink`**（与 WP1.6 终稿 Sink 一致）；`on_final_json` 取终稿字符串，
 *   `on_final_state` 读取最终 `AgentThreadState::history`（Loop 内会更迭状态指针，勿仅用初始 `agent_state`）。
 *
 * **环境变量**：
 * - `AGENT_TEST_AGENT_LOOP_DEBUG=1`：打印 MCP 注册结果、导出工具数、history 尾部。
 * - `AGENT_TEST_SKIP_CURSOR_MCP=1`：不导入 MCP（仅测纯模型回复；断言会放宽）。
 * - `AGENT_TEST_WP5_RELAX=1`：不要求 history 中出现 `tool` 消息（模型可能未调工具）。
 * - `AGENT_HTTP_TIMEOUT_SEC` / `AGENT_MCP_REQUEST_TIMEOUT_MS`：可按网络调大（MCP 多轮较慢）。
 */

#include <agent/graph_executor/graph_executor.hpp>
#include <agent/llm_client/llm_client.hpp>
#include <agent/prompt_renderer/prompt_renderer.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <agent/core/types.hpp>
#include <agent/internal/agent_thread_state.hpp>

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <taskflow/taskflow.hpp>
#include <workflow/nodeflow.hpp>

namespace {

using json = nlohmann::json;
using namespace agent_framework;

std::string env_or(const char* key, const char* default_val) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(default_val);
}

bool env_truthy(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !*v) {
        return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

bool is_debug_enabled() {
    const char* v = std::getenv("AGENT_TEST_AGENT_LOOP_DEBUG");
    return v && std::string(v) != "0";
}

void apply_live_llm_env_defaults() {
    if (std::getenv("OPENAI_API_KEY") == nullptr) {
        const char* dk = std::getenv("DEEPSEEK_API_KEY");
        if (dk && *dk) {
            (void)::setenv("OPENAI_API_KEY", dk, 0);
        }
    }
    if (std::getenv("AGENT_OPENAI_BASE_URL") == nullptr) {
        (void)::setenv("AGENT_OPENAI_BASE_URL", "https://api.deepseek.com/v1", 0);
    }
    if (std::getenv("AGENT_LLM_PROVIDER") == nullptr) {
        (void)::setenv("AGENT_LLM_PROVIDER", "openai", 0);
    }
    if (std::getenv("AGENT_HTTP_TIMEOUT_SEC") == nullptr) {
        (void)::setenv("AGENT_HTTP_TIMEOUT_SEC", "120", 0);
    }
    if (std::getenv("AGENT_LLM_MAX_RETRIES") == nullptr) {
        (void)::setenv("AGENT_LLM_MAX_RETRIES", "1", 0);
    }
    if (std::getenv("AGENT_LLM_MODEL") == nullptr) {
        const std::string m = env_or("DEEPSEEK_MODEL", "deepseek-chat");
        (void)::setenv("AGENT_LLM_MODEL", m.c_str(), 0);
    }
    if (std::getenv("AGENT_MCP_REQUEST_TIMEOUT_MS") == nullptr) {
        (void)::setenv("AGENT_MCP_REQUEST_TIMEOUT_MS", "20000", 0);
    }
}

static std::string trim_copy(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

int count_history_tool_messages(const std::vector<Message>& history) {
    int n = 0;
    for (const auto& m : history) {
        if (m.role == "tool") {
            ++n;
        }
    }
    return n;
}

bool final_mentions_route(const std::string& answer) {
    return answer.find("北京") != std::string::npos || answer.find("西安") != std::string::npos ||
           answer.find("Xi'an") != std::string::npos || answer.find("Xian") != std::string::npos;
}

std::shared_ptr<ToolBus> build_toolbus_cursor_mcp(bool skip_mcp, bool dbg, std::size_t* out_exported_tools) {
    auto bus = std::make_shared<ToolBus>();
    *out_exported_tools = 0;

    if (skip_mcp) {
        if (dbg) {
            std::cout << "cursor_mcp: skipped (AGENT_TEST_SKIP_CURSOR_MCP=1)\n";
        }
        return bus;
    }

    const std::string config_path = env_or("AGENT_TEST_CURSOR_MCP_JSON", "");
    ToolBus::CursorMcpImportResult r =
        bus->register_mcp_from_cursor_config(config_path, true);

    auto tools = bus->export_as_llm_tools();
    *out_exported_tools = tools.size();

    if (dbg) {
        std::cout << "cursor_mcp config_path=\"" << (config_path.empty() ? "<default>" : config_path) << "\"\n";
        std::cout << "  registered_services=" << r.registered_services.size()
                  << " failures=" << r.failures.size() << "\n";
        for (const auto& name : r.registered_services) {
            std::cout << "    ok: " << name << "\n";
        }
        for (const auto& f : r.failures) {
            std::cout << "    fail: " << f.service_name << " — " << f.reason << "\n";
        }
        std::cout << "toolbus export_as_llm_tools count=" << tools.size() << "\n";
    }

    return bus;
}

void test_agent_loop_live_travel_mcp() {
    const bool dbg = is_debug_enabled();
    const bool skip_mcp = env_truthy("AGENT_TEST_SKIP_CURSOR_MCP");
    const bool relax = env_truthy("AGENT_TEST_WP5_RELAX");

    if (dbg) {
        std::cout << "== WP1.5 agent loop: Cursor MCP + 北京→西安 自然语言问题 ==\n";
    }

    apply_live_llm_env_defaults();

    std::size_t exported_tools = 0;
    auto bus = build_toolbus_cursor_mcp(skip_mcp, dbg, &exported_tools);

    if (!skip_mcp && exported_tools == 0) {
        throw std::runtime_error(
            "Cursor MCP 导入后 export_as_llm_tools 为空。请检查 mcp.json 路径、各 server 是否可连、"
            "或暂时设置 AGENT_TEST_SKIP_CURSOR_MCP=1 / AGENT_TEST_WP5_RELAX=1 做排查。"
            " 可用 AGENT_TEST_AGENT_LOOP_DEBUG=1 查看失败原因。");
    }

    auto llm = std::make_shared<LLMClient>(LLMClient::from_env());
    {
        ModelConfig mc;
        mc.model_name = env_or("AGENT_LLM_MODEL", "deepseek-chat");
        mc.stream = false;
        mc.http_timeout_sec = std::atoi(env_or("AGENT_HTTP_TIMEOUT_SEC", "120").c_str());
        if (mc.http_timeout_sec <= 0) {
            mc.http_timeout_sec = 120;
        }
        mc.max_retries = std::atoi(env_or("AGENT_LLM_MAX_RETRIES", "1").c_str());
        if (mc.max_retries < 0) {
            mc.max_retries = 1;
        }
        llm->configure("openai", mc);
    }

    auto renderer = std::make_shared<PromptRenderer>();
    llm->set_prompt_renderer(renderer);

    AgentConfig cfg;
    cfg.name = "wp5_travel_mcp";
    if (!skip_mcp && exported_tools > 0) {
        cfg.system_prompt =
            "你是具备外部工具能力的智能助手。用户会询问国内出行与时间估算等问题。\n"
            "若有与问题直接相关的工具，必须优先调用它们获取可核对的信息；"
            "若没有完全对口的工具，仍应尽可能调用当前已注册列表中已有的工具（检索、网页、地图、代码与计算等）"
            "辅助推理与取证，再结合常识补全结论，不要空转不用工具。\n"
            "不要编造具体车次号、精确到分钟的到达时刻或实时余票；工具若给出时间，请写明预计出发/到达的大致日期与时刻，并说明依据。"
            "回答使用简体中文，结构清晰；信息不足时说明假设并给出合理区间。\n";
    } else {
        cfg.system_prompt =
            "你是出行规划助手。当前未挂载 MCP 工具，请根据常识与公开典型情况回答，并明确标注为估算，"
            "不要伪造精确时刻表。使用简体中文。\n";
    }
    cfg.model_config.model_name = env_or("AGENT_LLM_MODEL", "deepseek-chat");
    cfg.max_iterations = 12;
    cfg.max_tool_calls_per_iteration = 8;

    tf::Executor ex;
    workflow::GraphBuilder b("wp5_travel_mcp");

    auto init_state = std::make_shared<internal::AgentThreadState>();
    init_state->initial_user_prompt =
        "从现在这一刻算起，如果我从北京出发前往西安，打算不吃不喝连续步行去，"
        "请帮我估算或查询：我大概什么时候能到西安？请给出预计到达的大致日期和时间（说明你是依据工具结果还是常识推断）。";

    if (dbg) {
        std::cout << "model=" << cfg.model_config.model_name << " max_iterations=" << cfg.max_iterations
                  << " exported_tools=" << exported_tools << " skip_mcp=" << (skip_mcp ? 1 : 0)
                  << " relax=" << (relax ? 1 : 0) << "\n";
    }

    AgentWorkflowDeps deps;
    deps.llm = llm;
    deps.toolbus = bus;

    std::string final_answer;
    CliAgentTerminalSinkOptions sink_opts;
    sink_opts.sink_node_name = "Sink";
    sink_opts.on_final_json = [&final_answer](const json& j) {
        final_answer = j.at("final_answer").get<std::string>();
    };
    sink_opts.on_final_state = [&final_answer, dbg, relax, exported_tools, skip_mcp](
        const std::shared_ptr<internal::AgentThreadState>& st) {
        assert(st);
        assert(st->iteration >= 1);

        const int n_tools = count_history_tool_messages(st->history);
        if (!relax && !skip_mcp && exported_tools > 0 && n_tools < 1) {
            throw std::runtime_error(
                "期望至少调用 1 次 MCP 工具（history 中无 role=tool）。"
                "若模型未选工具，可设 AGENT_TEST_WP5_RELAX=1 重试。");
        }

        if (dbg) {
            std::cout << "== loop exited ==\n";
            std::cout << "final_answer=\"" << final_answer << "\"\n";
            std::cout << "iteration=" << st->iteration << " history_size=" << st->history.size()
                      << " tool_messages=" << n_tools << "\n";
            const std::size_t n = st->history.size();
            const std::size_t start = (n > 12) ? (n - 12) : 0;
            for (std::size_t i = start; i < n; ++i) {
                const auto& m = st->history[i];
                std::cout << "  hist[" << i << "] role=" << m.role;
                if (m.tool_name) {
                    std::cout << " tool=" << *m.tool_name;
                }
                if (m.tool_result) {
                    const std::string tr = m.tool_result->dump();
                    const std::size_t cap = 500;
                    if (tr.size() > cap) {
                        std::cout << " -> " << tr.substr(0, cap) << "...";
                    } else {
                        std::cout << " -> " << tr;
                    }
                }
                std::cout << "\n";
            }
        }
    };
    build_cli_agent_graph_with_terminal_sink(b, cfg, deps, init_state, sink_opts);

    auto f = b.run_async(ex);
    f.wait();

    const std::string trimmed = trim_copy(final_answer);
    if (trimmed.size() < 40) {
        throw std::runtime_error("final_answer 过短（期望至少约 40 字的有内容回复），got len=" +
                                 std::to_string(trimmed.size()));
    }
    if (!final_mentions_route(trimmed)) {
        throw std::runtime_error(
            "final_answer 应提及北京或西安（或 Xi'an）等与路线相关的字眼，便于确认答非所问未发生。");
    }

    if (!dbg) {
        std::cout << "--- final_answer ---\n" << final_answer << "\n--- end ---\n";
    }
    if (dbg) {
        std::cout << "PASS travel_mcp\n";
    }
    std::cout << "test_agent_loop_wp5: travel+MCP OK\n";
}

} // namespace

int main() {
    if (!env_truthy("AGENT_TEST_LIVE")) {
        std::cout << "test_agent_loop_wp5: skipped (set AGENT_TEST_LIVE=1: Cursor MCP + 北京→西安 问题)\n";
        return 0;
    }
    test_agent_loop_live_travel_mcp();
    return 0;
}

/**
 * @file test_draw_tools.cpp
 * @brief 内建 draw_render / draw_export（canvas_ity + stb）；门闩、allowlist 分进程用例。
 */
#include <agent/draw_tools.hpp>
#include <agent/toolbus.hpp>

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <filesystem>
#endif

namespace {

using json = nlohmann::json;

std::vector<unsigned char> base64_decode(const std::string& in) {
    static signed char T[256];
    static bool init = false;
    if (!init) {
        init = true;
        for (int i = 0; i < 256; ++i) {
            T[i] = -1;
        }
        const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            T[static_cast<unsigned char>(b64[i])] = static_cast<signed char>(i);
        }
    }
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        s.push_back(c);
    }
    if (s.size() % 4 != 0) {
        return {};
    }
    std::vector<unsigned char> out;
    out.reserve(s.size() / 4 * 3);
    int val = 0;
    int valb = -8;
    for (unsigned char c : s) {
        if (c == '=') {
            break;
        }
        const signed char d = T[c];
        if (d < 0) {
            return {};
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

unsigned read_be32(const unsigned char* p) {
    return (static_cast<unsigned>(p[0]) << 24) | (static_cast<unsigned>(p[1]) << 16) |
           (static_cast<unsigned>(p[2]) << 8) | static_cast<unsigned>(p[3]);
}

void assert_png_ihdr(const std::vector<unsigned char>& png, int expect_w, int expect_h) {
    assert(png.size() >= 24U);
    assert(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    const unsigned len = read_be32(png.data() + 8);
    assert(len == 13U);
    assert(png[12] == 'I' && png[13] == 'H' && png[14] == 'D' && png[15] == 'R');
    const unsigned w = read_be32(png.data() + 16);
    const unsigned h = read_be32(png.data() + 20);
    assert(static_cast<int>(w) == expect_w);
    assert(static_cast<int>(h) == expect_h);
}

void allowlist_partial_mode() {
    agent_framework::ToolBus bus;
    (void)::setenv("AGENT_DRAW_ENABLE", "1", 1);
    try {
        agent_framework::register_builtin_draw_tools_if_configured(bus);
        std::cerr << "test_draw_tools: expected invalid_argument from partial allowlist\n";
        std::exit(1);
    } catch (const std::invalid_argument& e) {
        const std::string m = e.what();
        if (m.find("AGENT_TOOL_ALLOWLIST") == std::string::npos) {
            std::cerr << "test_draw_tools: unexpected message: " << m << '\n';
            std::exit(1);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--allowlist-partial") {
        allowlist_partial_mode();
        return 0;
    }

    using namespace agent_framework;

    (void)::setenv("AGENT_TOOL_ALLOWLIST", "", 1);
    (void)::unsetenv("AGENT_DRAW_ENABLE");

    {
        (void)::setenv("AGENT_DRAW_ENABLE", "0", 1);
        ToolBus bus_off;
        register_builtin_draw_tools_if_configured(bus_off);
        assert(!bus_off.get_tool_info("draw_render").has_value());
        (void)::unsetenv("AGENT_DRAW_ENABLE");
    }

    ToolBus bus;
    register_builtin_draw_tools_if_configured(bus);

    if (!bus.get_tool_info("draw_render").has_value()) {
        return 0;
    }

    register_builtin_draw_tools_if_configured(bus);
    assert(bus.get_tool_info("draw_render").has_value());
    assert(bus.get_tool_info("draw_export").has_value());

    json r =
        bus
            .call_tool("draw_render",
                       json{{"template_id", "line_series_uniform_x"},
                            {"width", 64},
                            {"height", 48},
                            {"template_params",
                             json{{"series",
                                   json::array(
                                       {json{{"y_values", json::array({0.0, 1.0, 0.5})}}})}}}})
            .get();
    assert(!r.contains("error"));
    assert(r.at("width").get<int>() == 64);
    assert(r.at("height").get<int>() == 48);
    const std::string b64 = r.at("png_base64").get<std::string>();
    const std::vector<unsigned char> png = base64_decode(b64);
    assert(!png.empty());
    assert_png_ihdr(png, 64, 48);

    {
        json r2 =
            bus
                .call_tool("draw_render",
                           json{{"template_id", "line_series_uniform_x"},
                                {"width", 100000},
                                {"height", 48},
                                {"template_params",
                                 json{{"series",
                                       json::array(
                                           {json{{"y_values", json::array({0.0, 1.0})}}})}}}})
                .get();
        assert(r2.contains("error"));
        assert(r2["error"]["code"] == "dimension_limit");
    }

    {
        (void)::setenv("AGENT_DRAW_MAX_COMMANDS", "5", 1);
        ToolBus bus2;
        register_builtin_draw_tools_if_configured(bus2);
        json r3 =
            bus2
                .call_tool(
                    "draw_render",
                    json{{"template_id", "sparkline"},
                         {"template_params", json{{"y_values", json::array({0.0, 1.0})}}}})
                .get();
        (void)::unsetenv("AGENT_DRAW_MAX_COMMANDS");
        assert(r3.contains("error"));
        assert(r3["error"]["code"] == "command_limit");
    }

#if defined(_WIN32)
    (void)bus;
    return 0;
#else
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "agent_draw_tools_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    assert(fs::create_directories(root));
    const std::string root_s = root.string();
    (void)::setenv("AGENT_FS_ROOT", root_s.c_str(), 1);

    json re =
        bus
            .call_tool("draw_export",
                       json{{"template_id", "line_series_uniform_x"},
                            {"width", 32},
                            {"height", 24},
                            {"padding",
                             json{{"top", 2.0}, {"right", 2.0}, {"bottom", 2.0}, {"left", 2.0}}},
                            {"template_params",
                             json{{"series",
                                   json::array({json{{"y_values", json::array({1.0, 2.0})}}})}}},
                            {"relative_path", "out.png"},
                            {"confirm_overwrite", true}})
            .get();
    assert(!re.contains("error"));
    {
        std::ifstream f(root / "out.png", std::ios::binary);
        assert(f.good());
        const std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
        assert(buf.size() >= 24U);
        assert(buf[0] == 0x89 && buf[1] == 'P');
    }

    (void)::unsetenv("AGENT_FS_ROOT");
    fs::remove_all(root, ec);
    return 0;
#endif
}

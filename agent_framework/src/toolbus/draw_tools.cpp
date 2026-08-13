/**
 * @file draw_tools.cpp
 * @brief 内建 draw_render / draw_export（canvas_ity + stb_image_write；AGENT_DRAW_ENABLE）
 *
 * AGENT_DRAW_MAX_COMMANDS 折算（预检，防 OOM）：
 * - 背景 clear：1（fill_rectangle）
 * - 坐标轴：2（可选两条 line）
 * - 折线（n 点）：1 move_to + (n-1) line_to + 1 stroke = n + 1
 * - 面积（n 点）：折线计数 (n+1) + 2 line_to（到基线）+ close_path + fill = n + 5；再上描边折线 + (n+1)
 * - 柱图 n 柱：n 次 fill_rectangle
 * - 散点 n 点：n 次 fill_rectangle（方形标记）
 */
#include <agent/toolbus/draw_tools.hpp>
#include <agent/toolbus/fs_sandbox.hpp>
#include <agent/core/types.hpp>
#include <agent/internal/platform_io.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(AGENT_HAVE_CANVAS_ITY) && AGENT_HAVE_CANVAS_ITY && __has_include("stb_image_write.h")

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"

namespace agent_framework {
namespace {

namespace fs = std::filesystem;

using json = nlohmann::json;

bool env_falsy_off(const char* v) {
    if (!v || !*v) {
        return false;
    }
    std::string s(v);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s == "0" || s == "false" || s == "no" || s == "off";
}

bool draw_register_enabled() {
    const char* v = std::getenv("AGENT_DRAW_ENABLE");
    if (!v || !*v) {
        return true;
    }
    return !env_falsy_off(v);
}

std::size_t env_size_t(const char* name, std::size_t default_v) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return default_v;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v || *end != '\0' || n == 0ULL) {
        return default_v;
    }
    return static_cast<std::size_t>(std::min(n, static_cast<unsigned long long>(SIZE_MAX)));
}

struct DrawConfig {
    std::size_t max_width = 4096;
    std::size_t max_height = 4096;
    std::size_t max_pixels = 16777216;
    std::size_t max_commands = 10000;
    std::size_t max_output_bytes = 20971520;
};

DrawConfig load_draw_config_from_env() {
    DrawConfig c;
    c.max_width = env_size_t("AGENT_DRAW_MAX_WIDTH", 4096);
    c.max_height = env_size_t("AGENT_DRAW_MAX_HEIGHT", 4096);
    c.max_pixels = env_size_t("AGENT_DRAW_MAX_PIXELS", 16777216);
    c.max_commands = env_size_t("AGENT_DRAW_MAX_COMMANDS", 10000);
    c.max_output_bytes = env_size_t("AGENT_DRAW_MAX_OUTPUT_BYTES", 20971520);
    return c;
}

json make_draw_error(const std::string& code, const std::string& message) {
    return json{{"error", json{{"code", code}, {"message", message}}}};
}

std::string base64_encode(const unsigned char* data, std::size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const unsigned n = len - i;
        const unsigned b0 = data[i];
        const unsigned b1 = n > 1 ? data[i + 1] : 0;
        const unsigned b2 = n > 2 ? data[i + 2] : 0;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(tbl[(triple >> 18) & 63]);
        out.push_back(tbl[(triple >> 12) & 63]);
        out.push_back(n > 1 ? tbl[(triple >> 6) & 63] : '=');
        out.push_back(n > 2 ? tbl[triple & 63] : '=');
    }
    return out;
}

struct Padding {
    float top = 20.0f;
    float right = 20.0f;
    float bottom = 20.0f;
    float left = 20.0f;
};

bool parse_padding(const json& j, Padding& out, json& err) {
    if (!j.is_object()) {
        err = make_draw_error("invalid_arguments", "padding must be object");
        return false;
    }
    static const char* keys[] = {"top", "right", "bottom", "left"};
    float* ptrs[] = {&out.top, &out.right, &out.bottom, &out.left};
    for (int k = 0; k < 4; ++k) {
        if (j.contains(keys[k])) {
            if (!j[keys[k]].is_number()) {
                err = make_draw_error("invalid_arguments", std::string("padding.") + keys[k] + " must be number");
                return false;
            }
            *ptrs[k] = static_cast<float>(j[keys[k]].get<double>());
            if (!std::isfinite(*ptrs[k]) || *ptrs[k] < 0.0f) {
                err = make_draw_error("invalid_arguments", "padding values must be finite and non-negative");
                return false;
            }
        }
    }
    return true;
}

float clamp01(float x) {
    if (!std::isfinite(x)) {
        return 0.0f;
    }
    return std::max(0.0f, std::min(1.0f, x));
}

void set_stroke_rgba(canvas_ity::canvas& ctx, float r, float g, float b, float a) {
    ctx.set_color(canvas_ity::stroke_style, clamp01(r), clamp01(g), clamp01(b), clamp01(a));
}

void set_fill_rgba(canvas_ity::canvas& ctx, float r, float g, float b, float a) {
    ctx.set_color(canvas_ity::fill_style, clamp01(r), clamp01(g), clamp01(b), clamp01(a));
}

float map_x_index(std::size_t i, std::size_t n, float px0, float pw) {
    if (n <= 1) {
        return px0 + pw * 0.5f;
    }
    return px0 + static_cast<float>(i) / static_cast<float>(n - 1) * pw;
}

float map_y_value(double y, double ymin, double ymax, float py0, float ph) {
    if (!(std::isfinite(y) && std::isfinite(ymin) && std::isfinite(ymax))) {
        return py0 + ph * 0.5f;
    }
    if (ymax <= ymin) {
        return py0 + ph * 0.5f;
    }
    const double t = (y - ymin) / (ymax - ymin);
    return py0 + static_cast<float>((1.0 - t) * static_cast<double>(ph));
}

float map_x_value(double x, double xmin, double xmax, float px0, float pw) {
    if (!(std::isfinite(x) && std::isfinite(xmin) && std::isfinite(xmax))) {
        return px0 + pw * 0.5f;
    }
    if (xmax <= xmin) {
        return px0 + pw * 0.5f;
    }
    const double t = (x - xmin) / (xmax - xmin);
    return px0 + static_cast<float>(t * static_cast<double>(pw));
}

struct RasterResult {
    std::vector<unsigned char> png;
};

RasterResult rgba_to_png(const unsigned char* rgba, int w, int h, std::size_t max_out, json& err) {
    RasterResult r;
    int len = 0;
    unsigned char* png =
        stbi_write_png_to_mem(rgba, w * 4, w, h, 4, &len);
    if (!png || len <= 0) {
        err = make_draw_error("png_encode_failed", "stbi_write_png_to_mem failed");
        return r;
    }
    if (static_cast<std::size_t>(len) > max_out) {
        STBIW_FREE(png);
        err = make_draw_error("png_too_large", "PNG exceeds AGENT_DRAW_MAX_OUTPUT_BYTES");
        return r;
    }
    r.png.assign(png, png + static_cast<std::size_t>(len));
    STBIW_FREE(png);
    return r;
}

std::size_t line_command_cost(std::size_t n) {
    if (n < 2) {
        return 0;
    }
    return n + 1;
}

std::size_t area_command_cost(std::size_t n) {
    if (n < 2) {
        return 0;
    }
    return (n + 1) + 4 + (n + 1);
}

std::optional<json> draw_validate_dims(int w, int h, const DrawConfig& cfg, std::size_t command_budget) {
    if (w <= 0 || h <= 0) {
        return make_draw_error("invalid_arguments", "width and height must be positive");
    }
    if (static_cast<std::size_t>(w) > cfg.max_width || static_cast<std::size_t>(h) > cfg.max_height) {
        return make_draw_error("dimension_limit", "width/height exceed AGENT_DRAW_MAX_*");
    }
    if (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) > cfg.max_pixels) {
        return make_draw_error("pixel_limit", "width*height exceeds AGENT_DRAW_MAX_PIXELS");
    }
    if (command_budget > cfg.max_commands) {
        return make_draw_error("command_limit", "estimated draw commands exceed AGENT_DRAW_MAX_COMMANDS");
    }
    return std::nullopt;
}

json render_template(const std::string& template_id,
                     int width,
                     int height,
                     const Padding& pad,
                     const json& params,
                     std::vector<unsigned char>& rgba_out,
                     json& warnings) {
    json err;
    rgba_out.assign(static_cast<std::size_t>(width * height * 4), 0);
    canvas_ity::canvas ctx(width, height);

    const float px0 = pad.left;
    const float py0 = pad.top;
    const float pw = static_cast<float>(width) - pad.left - pad.right;
    const float ph = static_cast<float>(height) - pad.top - pad.bottom;
    if (pw <= 1.0f || ph <= 1.0f) {
        err = make_draw_error("invalid_arguments", "padding leaves no plot area");
        return err;
    }

    ctx.set_shadow_color(0.0f, 0.0f, 0.0f, 0.0f);
    set_fill_rgba(ctx, 1.0f, 1.0f, 1.0f, 1.0f);
    ctx.fill_rectangle(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));

    set_stroke_rgba(ctx, 0.7f, 0.7f, 0.7f, 1.0f);
    ctx.set_line_width(1.0f);
    ctx.move_to(px0, py0 + ph);
    ctx.line_to(px0 + pw, py0 + ph);
    ctx.stroke();
    ctx.move_to(px0, py0);
    ctx.line_to(px0, py0 + ph);
    ctx.stroke();

    if (template_id == "line_series_uniform_x") {
        if (!params.contains("series") || !params["series"].is_array() || params["series"].empty()) {
            err = make_draw_error("invalid_arguments", "template_params.series must be non-empty array");
            return err;
        }
        std::size_t n_ref = 0;
        for (const auto& ser : params["series"]) {
            if (!ser.is_object() || !ser.contains("y_values") || !ser["y_values"].is_array()) {
                err = make_draw_error("invalid_arguments", "each series needs y_values array");
                return err;
            }
            const auto& yv = ser["y_values"];
            if (yv.size() < 2) {
                err = make_draw_error("invalid_arguments", "y_values length must be >= 2");
                return err;
            }
            if (n_ref == 0) {
                n_ref = yv.size();
            } else if (yv.size() != n_ref) {
                err = make_draw_error("invalid_arguments", "all series must have same y_values length");
                return err;
            }
        }
        double ymin = std::numeric_limits<double>::infinity();
        double ymax = -std::numeric_limits<double>::infinity();
        for (const auto& ser : params["series"]) {
            for (const auto& y : ser["y_values"]) {
                if (!y.is_number()) {
                    err = make_draw_error("invalid_arguments", "y_values must be numbers");
                    return err;
                }
                const double v = y.get<double>();
                if (!std::isfinite(v)) {
                    err = make_draw_error("invalid_arguments", "non-finite y value");
                    return err;
                }
                ymin = std::min(ymin, v);
                ymax = std::max(ymax, v);
            }
        }
        if (ymin == ymax) {
            ymin -= 1.0;
            ymax += 1.0;
            warnings.push_back("y_flat_padded");
        }
        for (const auto& ser : params["series"]) {
            const auto& yv = ser["y_values"];
            float lw = 2.0f;
            if (ser.contains("line_width") && ser["line_width"].is_number()) {
                lw = static_cast<float>(ser["line_width"].get<double>());
            }
            float r = 0.1f, g = 0.4f, b = 0.9f, a = 1.0f;
            if (ser.contains("stroke") && ser["stroke"].is_array() && ser["stroke"].size() >= 4) {
                r = static_cast<float>(ser["stroke"][0].get<double>());
                g = static_cast<float>(ser["stroke"][1].get<double>());
                b = static_cast<float>(ser["stroke"][2].get<double>());
                a = static_cast<float>(ser["stroke"][3].get<double>());
            }
            ctx.set_line_width(std::max(0.5f, lw));
            set_stroke_rgba(ctx, r, g, b, a);
            const std::size_t n = yv.size();
            ctx.move_to(map_x_index(0, n, px0, pw),
                        map_y_value(yv[0].get<double>(), ymin, ymax, py0, ph));
            for (std::size_t i = 1; i < n; ++i) {
                ctx.line_to(map_x_index(i, n, px0, pw),
                            map_y_value(yv[i].get<double>(), ymin, ymax, py0, ph));
            }
            ctx.stroke();
        }
    } else if (template_id == "line_series_dual_batch") {
        if (!params.contains("series") || !params["series"].is_array() || params["series"].empty()) {
            err = make_draw_error("invalid_arguments", "template_params.series required");
            return err;
        }
        const auto& ser = params["series"][0];
        if (!ser.contains("x_values") || !ser.contains("y_values")) {
            err = make_draw_error("invalid_arguments", "series[0] needs x_values and y_values");
            return err;
        }
        const auto& xv = ser["x_values"];
        const auto& yv = ser["y_values"];
        if (!xv.is_array() || !yv.is_array() || xv.size() != yv.size() || xv.size() < 2) {
            err = make_draw_error("invalid_arguments", "x_values and y_values must be same length >= 2");
            return err;
        }
        double xmin = std::numeric_limits<double>::infinity();
        double xmax = -std::numeric_limits<double>::infinity();
        double ymin = std::numeric_limits<double>::infinity();
        double ymax = -std::numeric_limits<double>::infinity();
        const std::size_t n = xv.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (!xv[i].is_number() || !yv[i].is_number()) {
                err = make_draw_error("invalid_arguments", "x_values/y_values must be numbers");
                return err;
            }
            const double x = xv[i].get<double>();
            const double y = yv[i].get<double>();
            if (!std::isfinite(x) || !std::isfinite(y)) {
                err = make_draw_error("invalid_arguments", "non-finite point");
                return err;
            }
            xmin = std::min(xmin, x);
            xmax = std::max(xmax, x);
            ymin = std::min(ymin, y);
            ymax = std::max(ymax, y);
        }
        if (xmin == xmax) {
            xmin -= 1.0;
            xmax += 1.0;
        }
        if (ymin == ymax) {
            ymin -= 1.0;
            ymax += 1.0;
        }
        float lw = 2.0f;
        if (ser.contains("line_width") && ser["line_width"].is_number()) {
            lw = static_cast<float>(ser["line_width"].get<double>());
        }
        float r = 0.2f, g = 0.6f, b = 0.3f, a = 1.0f;
        if (ser.contains("stroke") && ser["stroke"].is_array() && ser["stroke"].size() >= 4) {
            r = static_cast<float>(ser["stroke"][0].get<double>());
            g = static_cast<float>(ser["stroke"][1].get<double>());
            b = static_cast<float>(ser["stroke"][2].get<double>());
            a = static_cast<float>(ser["stroke"][3].get<double>());
        }
        ctx.set_line_width(std::max(0.5f, lw));
        set_stroke_rgba(ctx, r, g, b, a);
        ctx.move_to(map_x_value(xv[0].get<double>(), xmin, xmax, px0, pw),
                    map_y_value(yv[0].get<double>(), ymin, ymax, py0, ph));
        for (std::size_t i = 1; i < n; ++i) {
            ctx.line_to(map_x_value(xv[i].get<double>(), xmin, xmax, px0, pw),
                        map_y_value(yv[i].get<double>(), ymin, ymax, py0, ph));
        }
        ctx.stroke();
    } else if (template_id == "bar_chart") {
        if (!params.contains("heights") || !params["heights"].is_array() || params["heights"].size() < 1) {
            err = make_draw_error("invalid_arguments", "heights must be non-empty array");
            return err;
        }
        const auto& hv = params["heights"];
        double hmin = 0.0;
        double hmax = 0.0;
        for (const auto& v : hv) {
            if (!v.is_number()) {
                err = make_draw_error("invalid_arguments", "heights must be numbers");
                return err;
            }
            const double t = v.get<double>();
            if (!std::isfinite(t)) {
                err = make_draw_error("invalid_arguments", "non-finite height");
                return err;
            }
            hmin = std::min(hmin, t);
            hmax = std::max(hmax, t);
        }
        if (hmin < 0.0) {
            err = make_draw_error("invalid_arguments", "bar_chart heights must be non-negative");
            return err;
        }
        if (hmax <= 0.0) {
            hmax = 1.0;
        }
        double gap = 0.1;
        if (params.contains("gap") && params["gap"].is_number()) {
            gap = params["gap"].get<double>();
            gap = std::max(0.0, std::min(0.95, gap));
        }
        const std::size_t nb = hv.size();
        const float slot_w = pw / static_cast<float>(nb);
        const float bar_w = slot_w * static_cast<float>(1.0 - gap);
        const float offset = (slot_w - bar_w) * 0.5f;
        set_fill_rgba(ctx, 0.3f, 0.5f, 0.85f, 1.0f);
        for (std::size_t i = 0; i < nb; ++i) {
            const double ht = hv[i].get<double>();
            const float x = px0 + static_cast<float>(i) * slot_w + offset;
            const float bar_h = static_cast<float>(ht / hmax * static_cast<double>(ph));
            const float y = py0 + ph - bar_h;
            ctx.fill_rectangle(x, y, bar_w, std::max(1.0f, bar_h));
        }
    } else if (template_id == "area_under_line") {
        if (!params.contains("series") || !params["series"].is_array() || params["series"].empty()) {
            err = make_draw_error("invalid_arguments", "series required");
            return err;
        }
        const auto& ser = params["series"][0];
        if (!ser.contains("y_values") || !ser["y_values"].is_array() || ser["y_values"].size() < 2) {
            err = make_draw_error("invalid_arguments", "y_values length >= 2");
            return err;
        }
        if (!params.contains("y_base") || !params["y_base"].is_number()) {
            err = make_draw_error("invalid_arguments", "y_base number required");
            return err;
        }
        const double y_base = params["y_base"].get<double>();
        const auto& yv = ser["y_values"];
        const std::size_t n = yv.size();
        double ymin = y_base;
        double ymax = y_base;
        for (const auto& y : yv) {
            if (!y.is_number()) {
                err = make_draw_error("invalid_arguments", "y_values must be numbers");
                return err;
            }
            const double v = y.get<double>();
            if (!std::isfinite(v)) {
                err = make_draw_error("invalid_arguments", "non-finite y");
                return err;
            }
            ymin = std::min(ymin, v);
            ymax = std::max(ymax, v);
        }
        ymin = std::min(ymin, y_base);
        ymax = std::max(ymax, y_base);
        if (ymin == ymax) {
            ymin -= 1.0;
            ymax += 1.0;
        }
        set_fill_rgba(ctx, 0.4f, 0.65f, 0.95f, 0.45f);
        ctx.move_to(map_x_index(0, n, px0, pw), map_y_value(yv[0].get<double>(), ymin, ymax, py0, ph));
        for (std::size_t i = 1; i < n; ++i) {
            ctx.line_to(map_x_index(i, n, px0, pw), map_y_value(yv[i].get<double>(), ymin, ymax, py0, ph));
        }
        ctx.line_to(map_x_index(n - 1, n, px0, pw), map_y_value(y_base, ymin, ymax, py0, ph));
        ctx.line_to(map_x_index(0, n, px0, pw), map_y_value(y_base, ymin, ymax, py0, ph));
        ctx.close_path();
        ctx.fill();

        float lw = 2.0f;
        if (ser.contains("line_width") && ser["line_width"].is_number()) {
            lw = static_cast<float>(ser["line_width"].get<double>());
        }
        ctx.set_line_width(std::max(0.5f, lw));
        set_stroke_rgba(ctx, 0.1f, 0.35f, 0.8f, 1.0f);
        ctx.move_to(map_x_index(0, n, px0, pw), map_y_value(yv[0].get<double>(), ymin, ymax, py0, ph));
        for (std::size_t i = 1; i < n; ++i) {
            ctx.line_to(map_x_index(i, n, px0, pw), map_y_value(yv[i].get<double>(), ymin, ymax, py0, ph));
        }
        ctx.stroke();
    } else if (template_id == "sparkline") {
        if (!params.contains("y_values") || !params["y_values"].is_array() || params["y_values"].size() < 2) {
            err = make_draw_error("invalid_arguments", "y_values length >= 2");
            return err;
        }
        const auto& yv = params["y_values"];
        const std::size_t n = yv.size();
        double ymin = std::numeric_limits<double>::infinity();
        double ymax = -std::numeric_limits<double>::infinity();
        for (const auto& y : yv) {
            if (!y.is_number()) {
                err = make_draw_error("invalid_arguments", "y_values must be numbers");
                return err;
            }
            const double v = y.get<double>();
            if (!std::isfinite(v)) {
                err = make_draw_error("invalid_arguments", "non-finite y");
                return err;
            }
            ymin = std::min(ymin, v);
            ymax = std::max(ymax, v);
        }
        if (ymin == ymax) {
            ymin -= 1.0;
            ymax += 1.0;
        }
        ctx.set_line_width(1.5f);
        set_stroke_rgba(ctx, 0.15f, 0.55f, 0.28f, 1.0f);
        ctx.move_to(map_x_index(0, n, px0, pw), map_y_value(yv[0].get<double>(), ymin, ymax, py0, ph));
        for (std::size_t i = 1; i < n; ++i) {
            ctx.line_to(map_x_index(i, n, px0, pw), map_y_value(yv[i].get<double>(), ymin, ymax, py0, ph));
        }
        ctx.stroke();
    } else if (template_id == "scatter_rows") {
        if (!params.contains("points") || !params["points"].is_array() || params["points"].empty()) {
            err = make_draw_error("invalid_arguments", "points must be non-empty array");
            return err;
        }
        const auto& pts = params["points"];
        double xmin = std::numeric_limits<double>::infinity();
        double xmax = -std::numeric_limits<double>::infinity();
        double ymin = std::numeric_limits<double>::infinity();
        double ymax = -std::numeric_limits<double>::infinity();
        for (const auto& p : pts) {
            if (!p.is_object() || !p.contains("x") || !p.contains("y")) {
                err = make_draw_error("invalid_arguments", "each point needs x,y");
                return err;
            }
            if (!p["x"].is_number() || !p["y"].is_number()) {
                err = make_draw_error("invalid_arguments", "point x,y must be numbers");
                return err;
            }
            const double x = p["x"].get<double>();
            const double y = p["y"].get<double>();
            if (!std::isfinite(x) || !std::isfinite(y)) {
                err = make_draw_error("invalid_arguments", "non-finite point");
                return err;
            }
            xmin = std::min(xmin, x);
            xmax = std::max(xmax, x);
            ymin = std::min(ymin, y);
            ymax = std::max(ymax, y);
        }
        if (xmin == xmax) {
            xmin -= 1.0;
            xmax += 1.0;
        }
        if (ymin == ymax) {
            ymin -= 1.0;
            ymax += 1.0;
        }
        float ms = 3.0f;
        if (params.contains("marker_size") && params["marker_size"].is_number()) {
            ms = static_cast<float>(std::max(1.0, params["marker_size"].get<double>()));
        }
        set_fill_rgba(ctx, 0.85f, 0.2f, 0.25f, 1.0f);
        for (const auto& p : pts) {
            const float cx = map_x_value(p["x"].get<double>(), xmin, xmax, px0, pw);
            const float cy = map_y_value(p["y"].get<double>(), ymin, ymax, py0, ph);
            ctx.fill_rectangle(cx - ms, cy - ms, ms * 2.0f, ms * 2.0f);
        }
    } else {
        err = make_draw_error("unknown_template", "template_id not recognized");
        return err;
    }

    ctx.get_image_data(rgba_out.data(), width, height, width * 4, 0, 0);
    return json::object();
}

std::size_t estimate_command_budget(const std::string& template_id, const json& params) {
    const std::size_t base = 1 + 2;
    if (template_id == "line_series_uniform_x") {
        std::size_t sum = 0;
        if (params.contains("series") && params["series"].is_array()) {
            for (const auto& ser : params["series"]) {
                if (ser.contains("y_values") && ser["y_values"].is_array()) {
                    sum += line_command_cost(ser["y_values"].size());
                }
            }
        }
        return base + sum;
    }
    if (template_id == "line_series_dual_batch") {
        if (params.contains("series") && params["series"].is_array() && !params["series"].empty()) {
            const auto& ser = params["series"][0];
            if (ser.contains("x_values") && ser["x_values"].is_array()) {
                return base + line_command_cost(ser["x_values"].size());
            }
        }
        return base;
    }
    if (template_id == "bar_chart") {
        if (params.contains("heights") && params["heights"].is_array()) {
            return base + params["heights"].size();
        }
        return base;
    }
    if (template_id == "area_under_line") {
        if (params.contains("series") && params["series"].is_array() && !params["series"].empty()) {
            const auto& ser = params["series"][0];
            if (ser.contains("y_values") && ser["y_values"].is_array()) {
                return base + area_command_cost(ser["y_values"].size());
            }
        }
        return base;
    }
    if (template_id == "sparkline") {
        if (params.contains("y_values") && params["y_values"].is_array()) {
            return base + line_command_cost(params["y_values"].size());
        }
        return base;
    }
    if (template_id == "scatter_rows") {
        if (params.contains("points") && params["points"].is_array()) {
            return base + params["points"].size();
        }
        return base;
    }
    return base;
}

struct PreparedPng {
    json err = json::object();
    std::vector<unsigned char> png;
    int width = 0;
    int height = 0;
    json warnings = json::array();
};

PreparedPng prepare_draw_png(const json& j, const DrawConfig& cfg) {
    PreparedPng out;
    if (!j.contains("template_id") || !j["template_id"].is_string()) {
        out.err = make_draw_error("invalid_arguments", "template_id string required");
        return out;
    }
    const std::string template_id = j["template_id"].get<std::string>();
    int width = 0;
    int height = 0;
    if (template_id == "sparkline") {
        width = j.contains("width") && j["width"].is_number() ? static_cast<int>(j["width"].get<double>()) : 120;
        height =
            j.contains("height") && j["height"].is_number() ? static_cast<int>(j["height"].get<double>()) : 32;
    } else {
        if (!j.contains("width") || !j.contains("height") || !j["width"].is_number() ||
            !j["height"].is_number()) {
            out.err = make_draw_error("invalid_arguments", "width and height numbers required");
            return out;
        }
        width = static_cast<int>(j["width"].get<double>());
        height = static_cast<int>(j["height"].get<double>());
    }
    Padding pad;
    json perr;
    if (j.contains("padding")) {
        if (!parse_padding(j["padding"], pad, perr)) {
            out.err = std::move(perr);
            return out;
        }
    }
    if (!j.contains("template_params") || !j["template_params"].is_object()) {
        out.err = make_draw_error("invalid_arguments", "template_params object required");
        return out;
    }
    const json& params = j["template_params"];
    const std::size_t budget = estimate_command_budget(template_id, params);
    if (const auto derr = draw_validate_dims(width, height, cfg, budget)) {
        out.err = *derr;
        return out;
    }

    std::vector<unsigned char> rgba;
    json rerr = render_template(template_id, width, height, pad, params, rgba, out.warnings);
    if (rerr.contains("error")) {
        out.err = std::move(rerr);
        return out;
    }

    json enc_err;
    RasterResult png = rgba_to_png(rgba.data(), width, height, cfg.max_output_bytes, enc_err);
    if (enc_err.contains("error")) {
        out.err = std::move(enc_err);
        return out;
    }
    out.png = std::move(png.png);
    out.width = width;
    out.height = height;
    return out;
}

json invoke_draw_render(const json& j, const DrawConfig& cfg) {
    PreparedPng p = prepare_draw_png(j, cfg);
    if (p.err.contains("error")) {
        return p.err;
    }
    json out{{"png_base64", base64_encode(p.png.data(), p.png.size())},
             {"width", p.width},
             {"height", p.height}};
    if (!p.warnings.empty()) {
        out["warnings"] = std::move(p.warnings);
    }
    return out;
}

json invoke_draw_export(const json& j, const DrawConfig& cfg) {
    if (!j.contains("relative_path") || !j["relative_path"].is_string()) {
        return make_draw_error("invalid_arguments", "relative_path string required");
    }
    const bool confirm =
        j.contains("confirm_overwrite") && j["confirm_overwrite"].is_boolean() && j["confirm_overwrite"].get<bool>();
    std::optional<FsSandboxConfig> fs_cfg = load_fs_sandbox_config_from_env();
    if (!fs_cfg.has_value()) {
        return make_draw_error("fs_root_required", "AGENT_FS_ROOT must be set for draw_export");
    }
    json inner = j;
    inner.erase("relative_path");
    inner.erase("confirm_overwrite");
    PreparedPng p = prepare_draw_png(inner, cfg);
    if (p.err.contains("error")) {
        return p.err;
    }
    if (p.png.size() > fs_cfg->max_write_bytes) {
        return make_draw_error("content_too_large", "PNG exceeds AGENT_FS_MAX_WRITE_BYTES");
    }
    json path_err = json::object();
    const std::string rel = j["relative_path"].get<std::string>();
    std::optional<fs::path> path = fs_resolve_under_root(rel, fs_cfg->root, path_err);
    if (!path) {
        return path_err;
    }
    std::error_code ec;
    if (fs::exists(*path, ec) && fs::is_regular_file(*path, ec)) {
        if (!confirm) {
            return make_draw_error("confirm_required",
                                   "file exists; set confirm_overwrite true to replace");
        }
    }
    if (fs::exists(*path, ec) && fs::is_directory(*path, ec)) {
        return make_draw_error("is_directory", "path is a directory");
    }
    fs::path parent = path->parent_path();
    if (!fs::exists(parent, ec)) {
        return make_draw_error("parent_missing", "parent directory does not exist");
    }
    const std::string tmp =
        path->string() + ".tmp." + std::to_string(internal::current_process_id());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_draw_error("open_failed", "cannot open temp file for write");
        }
        out.write(reinterpret_cast<const char*>(p.png.data()), static_cast<std::streamsize>(p.png.size()));
        if (!out) {
            fs::remove(tmp, ec);
            return make_draw_error("write_failed", "short write");
        }
    }
    fs::rename(tmp, *path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return make_draw_error("rename_failed", ec.message());
    }
    return json{{"written", true},
                {"relative_path", rel},
                {"bytes_written", p.png.size()},
                {"width", p.width},
                {"height", p.height}};
}

/**
 * JSON Schema for draw_render (and base for draw_export).
 *
 * Must stay aligned with docs/guides/builtin-draw-tools.md section
 * 「draw_* 参数形状（建议，与模板联动）」: top-level template_id, width, height,
 * optional padding, template_params.series[].y_values / stroke / line_width.
 * y_values are commonly filled from expr_batch_eval.values (same length as rows).
 */
json make_draw_render_tool_schema() {
    return json::parse(R"({
        "type": "object",
        "description": "Rasterize a whitelisted template. Top-level: template_id (enum). width and height are integers; sparkline may omit both (defaults 120x32). Optional padding {top,right,bottom,left} as numbers (implementation default 20 per side if omitted—use smaller values for small canvases). template_params shape depends on template_id: line_series_uniform_x: series[] with y_values (number[], len>=2 per series), optional stroke [r,g,b,a], line_width. line_series_dual_batch: series[0] with x_values and y_values (same length>=2), optional stroke, line_width. bar_chart: heights (number[], non-negative), optional gap (0..0.95). area_under_line: series[0].y_values (len>=2) and y_base (number). sparkline: y_values (len>=2) only. scatter_rows: points [{x,y},...], optional marker_size. y_values often come from expr_batch_eval.values. Canonical full example: see schema.examples[0] (matches builtin-draw-tools.md).",
        "examples": [
            {
                "template_id": "line_series_uniform_x",
                "width": 800,
                "height": 400,
                "padding": { "top": 20, "right": 20, "bottom": 40, "left": 50 },
                "template_params": {
                    "series": [
                        {
                            "y_values": [0.12, 0.48, 0.33],
                            "stroke": [0.1, 0.4, 0.9, 1.0],
                            "line_width": 2.0
                        }
                    ]
                }
            }
        ],
        "properties": {
            "template_id": {
                "type": "string",
                "enum": [
                    "line_series_uniform_x",
                    "line_series_dual_batch",
                    "bar_chart",
                    "area_under_line",
                    "sparkline",
                    "scatter_rows"
                ]
            },
            "width": {"type": "integer"},
            "height": {"type": "integer"},
            "padding": {
                "type": "object",
                "properties": {
                    "top": {"type": "number"},
                    "right": {"type": "number"},
                    "bottom": {"type": "number"},
                    "left": {"type": "number"}
                }
            },
            "template_params": {
                "type": "object",
                "description": "Per template_id; keys below are validated when present. line_series_uniform_x: series[].y_values (required per series, len>=2), optional stroke [r,g,b,a], line_width — same nesting as builtin-draw-tools.md listing. line_series_dual_batch: series[0].x_values + y_values. bar_chart: heights, gap. area_under_line: series[0].y_values + y_base. sparkline: y_values. scatter_rows: points, marker_size. Extra keys allowed (additionalProperties).",
                "properties": {
                    "series": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "y_values": {"type": "array", "items": {"type": "number"}},
                                "x_values": {"type": "array", "items": {"type": "number"}},
                                "stroke": {"type": "array"},
                                "line_width": {"type": "number"}
                            },
                            "additionalProperties": true
                        }
                    },
                    "heights": {"type": "array", "items": {"type": "number"}},
                    "y_values": {"type": "array", "items": {"type": "number"}},
                    "y_base": {"type": "number"},
                    "points": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "x": {"type": "number"},
                                "y": {"type": "number"}
                            },
                            "additionalProperties": true
                        }
                    },
                    "gap": {"type": "number"},
                    "marker_size": {"type": "number"}
                },
                "additionalProperties": true
            }
        },
        "required": ["template_id", "template_params"]
    })");
}

void register_draw_tools_impl(ToolBus& bus) {
    const DrawConfig cfg = load_draw_config_from_env();
    static const json schema_draw_render = make_draw_render_tool_schema();

    {
        ToolMeta meta;
        meta.name = "RenderChart";
        meta.description =
            "Rasterize a whitelisted template_id with canvas_ity, return PNG as png_base64. "
            "Argument shape matches docs/guides/builtin-draw-tools.md (section draw_* 参数形状); "
            "Tool JSON Schema includes the same canonical example as schema.examples[0]. "
            "Combine expr_batch_eval.values with template_params.series[].y_values. "
            "Templates: line_series_uniform_x, line_series_dual_batch, bar_chart, area_under_line, "
            "sparkline (default 120x32 if width/height omitted), scatter_rows.";
        meta.schema = schema_draw_render;
        meta.side_effect = ToolSideEffect::ReadOnly;
        bus.register_local_tool(
            "RenderChart", [cfg](const json& args) { return invoke_draw_render(args, cfg); }, meta);
    }
    {
        ToolMeta meta;
        meta.name = "ExportChart";
        meta.description =
            "Same as RenderChart but writes PNG under AGENT_FS_ROOT. Requires relative_path and "
            "confirm_overwrite when replacing an existing file.";
        json schema_export = schema_draw_render;
        schema_export["properties"]["relative_path"] = json{{"type", "string"}};
        schema_export["properties"]["confirm_overwrite"] = json{{"type", "boolean"}};
        {
            std::string desc = schema_export["description"].get<std::string>();
            schema_export["description"] =
                std::move(desc) +
                " draw_export: set relative_path (relative to AGENT_FS_ROOT) and confirm_overwrite (bool).";
        }
        json req = schema_export["required"];
        req.push_back("relative_path");
        req.push_back("confirm_overwrite");
        schema_export["required"] = std::move(req);
        meta.schema = std::move(schema_export);
        meta.side_effect = ToolSideEffect::Write;
        bus.register_local_tool(
            "ExportChart", [cfg](const json& args) { return invoke_draw_export(args, cfg); }, meta);
    }
}

} // namespace

void register_builtin_draw_tools_if_configured(ToolBus& bus) {
    if (bus.get_tool_info("RenderChart").has_value()) {
        return;
    }
    if (!draw_register_enabled()) {
        return;
    }
    register_draw_tools_impl(bus);
    bus.register_tool_alias("draw_render", "RenderChart");
    bus.register_tool_alias("draw_export", "ExportChart");
}

} // namespace agent_framework

#else

namespace agent_framework {

void register_builtin_draw_tools_if_configured(ToolBus& bus) {
    (void)bus;
}

} // namespace agent_framework

#endif

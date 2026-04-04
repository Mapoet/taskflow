/**
 * @file test_canvas_ity_smoke.cpp
 * @brief Complex canvas_ity scene + image output (PNG via stb_image_write, or TGA).
 *
 * Output path: argv[1], or env @c AGENT_TEST_CANVAS_OUT, or default
 * @c canvas_ity_smoke.png (with stb) / @c canvas_ity_smoke.tga (without).
 * Use @c .png or @c .tga extension to force format when stb is available.
 */
#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"

#ifdef AGENT_HAVE_STB_IMAGE_WRITE
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int k_width = 256;
constexpr int k_height = 256;

std::size_t rgba_index(int x, int y, int w) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u;
}

bool path_ends_with_ignore_case(std::string const& path, char const* ext) {
    std::size_t const el = std::strlen(ext);
    if (path.size() < el) {
        return false;
    }
    for (std::size_t i = 0; i < el; ++i) {
        unsigned char const u = static_cast<unsigned char>(path[path.size() - el + i]);
        unsigned char const v = static_cast<unsigned char>(ext[i]);
        if (std::tolower(u) != std::tolower(v)) {
            return false;
        }
    }
    return true;
}

bool write_tga(char const* path, int width, int height, unsigned char const* rgba) {
    std::vector<unsigned char> row(static_cast<std::size_t>(width) * 4u);
    unsigned char header[18] = {
        0,
        0,
        2,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        static_cast<unsigned char>(width & 255),
        static_cast<unsigned char>((width >> 8) & 255),
        static_cast<unsigned char>(height & 255),
        static_cast<unsigned char>((height >> 8) & 255),
        32,
        40,
    };
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<char const*>(header), sizeof(header));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::size_t const i = rgba_index(x, y, width);
            row[static_cast<std::size_t>(x) * 4u + 0] = rgba[i + 2]; // B
            row[static_cast<std::size_t>(x) * 4u + 1] = rgba[i + 1]; // G
            row[static_cast<std::size_t>(x) * 4u + 2] = rgba[i + 0]; // R
            row[static_cast<std::size_t>(x) * 4u + 3] = rgba[i + 3]; // A
        }
        stream.write(reinterpret_cast<char const*>(row.data()),
                     static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(stream);
}

#ifdef AGENT_HAVE_STB_IMAGE_WRITE
bool write_png(char const* path, int width, int height, unsigned char const* rgba) {
    int const stride = width * 4;
    return stbi_write_png(path, width, height, 4, rgba, stride) != 0;
}
#endif

enum class ImageFormat { k_tga, k_png };

ImageFormat output_format_for_path(std::string const& path) {
    if (path_ends_with_ignore_case(path, ".tga")) {
        return ImageFormat::k_tga;
    }
#ifdef AGENT_HAVE_STB_IMAGE_WRITE
    if (path_ends_with_ignore_case(path, ".png")) {
        return ImageFormat::k_png;
    }
    return ImageFormat::k_png;
#else
    return ImageFormat::k_tga;
#endif
}

bool write_image(char const* path, int width, int height, unsigned char const* rgba, ImageFormat fmt) {
    switch (fmt) {
    case ImageFormat::k_tga:
        return write_tga(path, width, height, rgba);
#ifdef AGENT_HAVE_STB_IMAGE_WRITE
    case ImageFormat::k_png:
        return write_png(path, width, height, rgba);
#else
    case ImageFormat::k_png:
        return false;
#endif
    }
    return false;
}

bool verify_output_file(std::string const& path, ImageFormat fmt) {
    std::ifstream check(path.c_str(), std::ios::binary | std::ios::ate);
    if (!check) {
        return false;
    }
    auto const size = check.tellg();
    if (fmt == ImageFormat::k_tga) {
        return size == static_cast<std::streamoff>(18 + k_width * k_height * 4);
    }
    // PNG compressed size varies; require plausible minimum and signature.
    if (size < 67) {
        return false;
    }
    check.seekg(0);
    unsigned char sig[8];
    check.read(reinterpret_cast<char*>(sig), 8);
    static unsigned char const png_magic[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    for (int i = 0; i < 8; ++i) {
        if (sig[i] != png_magic[i]) {
            return false;
        }
    }
    return true;
}

void draw_scene(canvas_ity::canvas& ctx) {
    ctx.set_shadow_color(0.0f, 0.0f, 0.0f, 0.0f);
    ctx.set_color(canvas_ity::fill_style, 0.92f, 0.94f, 0.98f, 1.0f);
    ctx.fill_rectangle(0.0f, 0.0f, static_cast<float>(k_width), static_cast<float>(k_height));

    ctx.move_to(128.0f, 28.0f);
    ctx.line_to(157.0f, 87.0f);
    ctx.line_to(223.0f, 97.0f);
    ctx.line_to(175.0f, 143.0f);
    ctx.line_to(186.0f, 208.0f);
    ctx.line_to(128.0f, 178.0f);
    ctx.line_to(69.0f, 208.0f);
    ctx.line_to(80.0f, 143.0f);
    ctx.line_to(32.0f, 97.0f);
    ctx.line_to(98.0f, 87.0f);
    ctx.close_path();

    ctx.set_shadow_blur(8.0f);
    ctx.shadow_offset_y = 4.0f;
    ctx.set_shadow_color(0.0f, 0.0f, 0.0f, 0.5f);

    ctx.set_color(canvas_ity::fill_style, 1.0f, 0.9f, 0.2f, 1.0f);
    ctx.fill();

    ctx.line_join = canvas_ity::rounded;
    ctx.set_line_width(12.0f);
    ctx.set_color(canvas_ity::stroke_style, 0.9f, 0.0f, 0.5f, 1.0f);
    ctx.stroke();

    float dash_segments[] = {21.0f, 9.0f, 1.0f, 9.0f, 7.0f, 9.0f, 1.0f, 9.0f};
    ctx.set_line_dash(dash_segments, 8);
    ctx.line_dash_offset = 10.0f;
    ctx.line_cap = canvas_ity::circle;
    ctx.set_line_width(6.0f);
    ctx.set_color(canvas_ity::stroke_style, 0.95f, 0.65f, 0.15f, 1.0f);
    ctx.stroke();

    ctx.set_shadow_color(0.0f, 0.0f, 0.0f, 0.0f);

    ctx.set_linear_gradient(canvas_ity::fill_style, 64.0f, 0.0f, 192.0f, 256.0f);
    ctx.add_color_stop(canvas_ity::fill_style, 0.30f, 1.0f, 1.0f, 1.0f, 0.0f);
    ctx.add_color_stop(canvas_ity::fill_style, 0.35f, 1.0f, 1.0f, 1.0f, 0.8f);
    ctx.add_color_stop(canvas_ity::fill_style, 0.45f, 1.0f, 1.0f, 1.0f, 0.8f);
    ctx.add_color_stop(canvas_ity::fill_style, 0.50f, 1.0f, 1.0f, 1.0f, 0.0f);
    ctx.global_composite_operation = canvas_ity::source_atop;
    ctx.fill_rectangle(0.0f, 0.0f, static_cast<float>(k_width), static_cast<float>(k_height));
    ctx.global_composite_operation = canvas_ity::source_over;

    ctx.set_line_dash(static_cast<float const*>(nullptr), 0);
    ctx.line_dash_offset = 0.0f;
    ctx.line_cap = canvas_ity::butt;
    ctx.set_line_width(4.0f);
    ctx.move_to(16.0f, 230.0f);
    ctx.quadratic_curve_to(52.0f, 188.0f, 108.0f, 232.0f);
    ctx.set_color(canvas_ity::stroke_style, 0.12f, 0.38f, 0.82f, 0.95f);
    ctx.stroke();

    ctx.set_line_width(3.0f);
    ctx.move_to(208.0f, 36.0f);
    ctx.arc(228.0f, 56.0f, 22.0f, 0.0f, 1.85f * 3.14159265f, false);
    ctx.set_color(canvas_ity::stroke_style, 0.15f, 0.55f, 0.28f, 1.0f);
    ctx.stroke();
}

char const* default_output_filename() {
#ifdef AGENT_HAVE_STB_IMAGE_WRITE
    return "canvas_ity_smoke.png";
#else
    return "canvas_ity_smoke.tga";
#endif
}

std::string output_path(int argc, char** argv) {
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        return std::string(argv[1]);
    }
    if (char const* env = std::getenv("AGENT_TEST_CANVAS_OUT")) {
        if (env[0] != '\0') {
            return std::string(env);
        }
    }
    return std::string(default_output_filename());
}

} // namespace

int main(int argc, char** argv) {
    canvas_ity::canvas context(k_width, k_height);
    draw_scene(context);

    std::vector<unsigned char> image(static_cast<std::size_t>(k_width * k_height * 4));
    context.get_image_data(image.data(), k_width, k_height, k_width * 4, 0, 0);

    std::size_t const bg = rgba_index(4, 4, k_width);
    if (static_cast<int>(image[bg + 0]) < 210 || static_cast<int>(image[bg + 1]) < 210
        || static_cast<int>(image[bg + 2]) < 220) {
        return 1;
    }

    std::size_t const star = rgba_index(128, 125, k_width);
    if (static_cast<int>(image[star + 0]) + static_cast<int>(image[star + 1]) < 280) {
        return 2;
    }
    if (static_cast<int>(image[star + 2]) > 140) {
        return 3;
    }

    std::string const path = output_path(argc, argv);
    ImageFormat const fmt = output_format_for_path(path);

    if (!write_image(path.c_str(), k_width, k_height, image.data(), fmt)) {
        return 4;
    }

    if (!verify_output_file(path, fmt)) {
        return 6;
    }

    return 0;
}

#ifndef AGENT_INTERNAL_HTML_TEXT_HPP
#define AGENT_INTERNAL_HTML_TEXT_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace agent_framework::internal {

struct HtmlTextLimits {
    std::size_t max_input_bytes{262144};
    std::size_t max_output_bytes{131072};
    std::size_t max_tag_bytes{16384};
    std::size_t max_operations{2097152};
};

struct HtmlTextResult {
    std::string text;
    bool truncated{false};
    std::string error_code;
    std::size_t scanned_bytes{0};
};

/** Linear, bounded best-effort HTML-to-text extraction for untrusted pages. */
HtmlTextResult extract_html_text(std::string_view html, const HtmlTextLimits& limits = {});

/** Linear tag stripping for already bounded fragments such as search titles. */
std::string strip_html_tags_bounded(std::string_view html, std::size_t max_output_bytes = 16384);

} // namespace agent_framework::internal

#endif

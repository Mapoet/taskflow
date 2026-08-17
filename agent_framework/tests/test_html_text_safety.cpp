#include <agent/internal/html_text.hpp>

#include <cassert>
#include <future>
#include <string>
#include <vector>

int main() {
    using agent_framework::internal::HtmlTextLimits;
    using agent_framework::internal::extract_html_text;
    HtmlTextLimits limits;
    limits.max_input_bytes = 262144;
    limits.max_output_bytes = 4096;
    limits.max_tag_bytes = 1024;
    limits.max_operations = 1048576;

    const auto normal = extract_html_text(
        "<html><style>hidden</style><script>bad()</script><body>A &amp; B</body></html>", limits);
    assert(normal.error_code.empty() && normal.text == "A & B");

    const auto huge_tag = extract_html_text("<div " + std::string(4096, 'x') + ">tail", limits);
    assert(huge_tag.error_code == "html_tag_too_large" && huge_tag.truncated);

    const auto unclosed_script = extract_html_text("prefix<script>" + std::string(200000, '<'), limits);
    assert(unclosed_script.error_code.empty() && unclosed_script.truncated);
    assert(unclosed_script.text == "prefix");

    const auto unclosed_comment = extract_html_text("visible<!--" + std::string(200000, '-'), limits);
    assert(unclosed_comment.error_code.empty() && unclosed_comment.text == "visible");

    const auto output_bound = extract_html_text(std::string(200000, 'a'), limits);
    assert(output_bound.text.size() == limits.max_output_bytes && output_bound.truncated);

    std::vector<std::future<void>> work;
    for (int worker = 0; worker < 8; ++worker) {
        work.push_back(std::async(std::launch::async, [limits]() {
            for (int iteration = 0; iteration < 100; ++iteration) {
                const auto result = extract_html_text(
                    "<main>worker</main><script>" + std::string(32768, '<') + "</script>", limits);
                assert(result.error_code.empty() && result.text == "worker");
            }
        }));
    }
    for (auto& task : work) task.get();
    return 0;
}

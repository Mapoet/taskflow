#include <agent/ui/streaming_markdown.hpp>

#include <cassert>
#include <iostream>

using namespace agent_framework;

int main() {
    StreamingMarkdownAssembler assembler;
    const auto blocks = assembler.parse(
        "# Result\n\n- first\n- second\n\n| x | y |\n|---|---|\n| 1 | 2 |\n\n"
        "$$\nx^2\n$$\n\n```mermaid\ngraph LR\nA-->B\n```\n", true);
    assert(blocks.size() == 5);
    assert(blocks[0].kind == UiContentBlockKind::Heading && blocks[0].heading_level == 1);
    assert(blocks[1].kind == UiContentBlockKind::List);
    assert(blocks[2].kind == UiContentBlockKind::Table && blocks[2].table_cells.size() == 2);
    assert(blocks[3].kind == UiContentBlockKind::MathBlock && blocks[3].text == "x^2");
    assert(blocks[4].kind == UiContentBlockKind::Mermaid && blocks[4].stable);

    const auto partial_fence = assembler.parse("```mermaid\ngraph LR\nA-->");
    assert(partial_fence.size() == 1);
    assert(partial_fence[0].kind == UiContentBlockKind::DraftTail);
    assert(!partial_fence[0].stable);

    const auto partial_text = assembler.parse("answer still streaming");
    assert(partial_text.size() == 1);
    assert(partial_text[0].kind == UiContentBlockKind::DraftTail);
    assert(!partial_text[0].stable);

    std::cout << "test_streaming_markdown: ok\n";
    return 0;
}

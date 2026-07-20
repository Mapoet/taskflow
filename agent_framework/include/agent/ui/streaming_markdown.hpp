#ifndef AGENT_UI_STREAMING_MARKDOWN_HPP
#define AGENT_UI_STREAMING_MARKDOWN_HPP

#include <agent/ui/presentation_model.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

/**
 * Deterministic, dependency-free block assembler shared by native UIs.
 *
 * It deliberately recognizes only the structural subset needed by the
 * presentation contract. Browser-side markdown-it remains the authoritative
 * GFM renderer. During streaming, the final incomplete construct is emitted as
 * DraftTail and is never sent to Mermaid/KaTeX rasterizers.
 */
class StreamingMarkdownAssembler {
public:
    std::vector<UiContentBlock> parse(std::string_view markdown,
                                      bool finalized = false) const;
};

} // namespace agent_framework

#endif

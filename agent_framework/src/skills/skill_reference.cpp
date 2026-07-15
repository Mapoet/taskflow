#include <agent/skill_reference.hpp>

#include <algorithm>
#include <fstream>
#include <limits>

namespace agent_framework {
namespace {

nlohmann::json failure(const char* code, const std::string& message) {
    return {{"code", code}, {"message", message}};
}

SkillReferenceResult failed(const char* code, const std::string& message) {
    return {false, failure(code, message), std::nullopt};
}

bool textual(const std::string& media_type) {
    return media_type.rfind("text/", 0) == 0 || media_type == "application/json" ||
           media_type == "application/yaml" || media_type == "application/x-yaml" ||
           media_type == "application/xml" || media_type == "application/markdown";
}

bool continuation(unsigned char value) { return (value & 0xC0U) == 0x80U; }

std::optional<std::size_t> codepoint_size(const std::string& bytes,
                                         std::size_t offset) {
    const auto first = static_cast<unsigned char>(bytes[offset]);
    std::size_t size = 0;
    if(first <= 0x7FU) size = 1;
    else if(first >= 0xC2U && first <= 0xDFU) size = 2;
    else if(first >= 0xE0U && first <= 0xEFU) size = 3;
    else if(first >= 0xF0U && first <= 0xF4U) size = 4;
    else return std::nullopt;
    if(offset + size > bytes.size()) return std::nullopt;
    for(std::size_t i = 1; i < size; ++i)
        if(!continuation(static_cast<unsigned char>(bytes[offset + i])))
            return std::nullopt;
    if(size == 3) {
        const auto second = static_cast<unsigned char>(bytes[offset + 1]);
        if((first == 0xE0U && second < 0xA0U) ||
           (first == 0xEDU && second >= 0xA0U)) return std::nullopt;
    }
    if(size == 4) {
        const auto second = static_cast<unsigned char>(bytes[offset + 1]);
        if((first == 0xF0U && second < 0x90U) ||
           (first == 0xF4U && second > 0x8FU)) return std::nullopt;
    }
    return size;
}

SkillCitation citation_for(const SkillResourceHandle& handle,
                           std::uint64_t start, std::uint64_t end) {
    SkillCitation citation;
    citation.resource_id = handle.descriptor.id;
    citation.resource_digest = handle.resource_digest;
    citation.package_digest = handle.package_digest;
    citation.source_uri = handle.descriptor.source_uri;
    citation.license = handle.descriptor.license;
    citation.byte_start = start;
    citation.byte_end = end;
    if(handle.descriptor.citation) {
        citation.title = handle.descriptor.citation->title;
        citation.authors = handle.descriptor.citation->authors;
        citation.published = handle.descriptor.citation->published;
        citation.url = handle.descriptor.citation->url;
        citation.locator = handle.descriptor.citation->locator;
    }
    return citation;
}

} // namespace

std::uint64_t SkillReferenceService::bytes_read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_read_;
}

SkillReferenceResult SkillReferenceService::read_page(
    const SkillResourceHandle& handle, std::uint64_t offset,
    std::size_t max_bytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(handle.descriptor.kind != SkillResourceType::Reference ||
       !textual(handle.descriptor.media_type))
        return failed("skill_reference_binary_refused",
                      "reference paging requires a textual Reference resource");
    if(max_bytes == 0 || max_bytes > limits_.max_page_bytes)
        return failed("skill_reference_page_limit",
                      "requested page size exceeds the page limit");
    if(max_bytes > std::numeric_limits<std::uint64_t>::max() - 3U ||
       offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return failed("skill_reference_page_limit", "reference page range is not addressable");
    if(offset > handle.size)
        return failed("skill_reference_range_invalid", "reference offset exceeds resource size");
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(handle.path, ec);
    if(ec || std::filesystem::is_symlink(status) ||
       !std::filesystem::is_regular_file(status))
        return failed("skill_reference_source_changed",
                      "reference is no longer a regular snapshot file");
    const auto actual_size = std::filesystem::file_size(handle.path, ec);
    if(ec || actual_size != handle.size)
        return failed("skill_reference_source_changed",
                      "reference size differs from its snapshot");

    if(offset == handle.size) {
        SkillReferencePage page;
        page.next_offset = offset;
        page.eof = true;
        page.citation = citation_for(handle, offset, offset);
        return {true, nlohmann::json::object(), std::move(page)};
    }
    std::ifstream input(handle.path, std::ios::binary);
    if(!input) return failed("skill_reference_read_failed", "reference cannot be opened");
    input.seekg(static_cast<std::streamoff>(offset));
    if(!input) return failed("skill_reference_read_failed", "reference seek failed");
    const auto remaining = handle.size - offset;
    const std::uint64_t lookahead = std::min<std::uint64_t>(remaining,
        static_cast<std::uint64_t>(max_bytes) + 3U);
    if(lookahead > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return failed("skill_reference_page_limit", "reference page is not addressable");
    std::string bytes(static_cast<std::size_t>(lookahead), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if(input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return failed("skill_reference_source_changed", "reference changed during paging");
    if(continuation(static_cast<unsigned char>(bytes.front())))
        return failed("skill_reference_cursor_invalid",
                      "reference cursor is inside a UTF-8 code point");

    std::size_t page_size = 0;
    std::size_t cursor = 0;
    while(cursor < bytes.size() && cursor < max_bytes) {
        const auto size = codepoint_size(bytes, cursor);
        if(!size)
            return failed("skill_reference_utf8_invalid", "reference contains invalid UTF-8");
        if(cursor + *size > max_bytes) break;
        cursor += *size;
        page_size = cursor;
    }
    if(page_size == 0)
        return failed("skill_reference_page_limit",
                      "page size is too small for the next UTF-8 code point");
    if(bytes_read_ > limits_.max_total_bytes ||
       page_size > limits_.max_total_bytes - bytes_read_)
        return failed("skill_reference_budget_exceeded",
                      "reference session byte budget is exhausted");
    const std::uint64_t end = offset + page_size;
    SkillReferencePage page;
    page.text.assign(bytes.data(), page_size);
    page.next_offset = end;
    page.eof = end == handle.size;
    page.citation = citation_for(handle, offset, end);
    bytes_read_ += page_size;
    return {true, nlohmann::json::object(), std::move(page)};
}

} // namespace agent_framework

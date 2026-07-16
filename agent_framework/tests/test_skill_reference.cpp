#include <agent/skills/skill_lifecycle.hpp>
#include <agent/skills/skill_reference.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

SkillResourceHandle reference_handle(const fs::path& path, const std::string& content) {
    write_file(path, content);
    std::string error;
    const auto digest = skill_sha256_file(path, &error);
    assert(digest);
    SkillResourceHandle handle;
    handle.path = path;
    handle.size = content.size();
    handle.view_size = content.size();
    handle.resource_digest = *digest;
    handle.package_digest = "package-digest";
    handle.descriptor.id = "guide";
    handle.descriptor.kind = SkillResourceType::Reference;
    handle.descriptor.media_type = "text/plain";
    handle.descriptor.license = "CC-BY-4.0";
    handle.descriptor.source_uri = "https://example.test/guide";
    SkillCitationMetadata citation;
    citation.title = "Guide";
    citation.authors = {"A. Author"};
    citation.published = "2026";
    citation.url = "https://example.test/paper";
    citation.locator = "section-1";
    handle.descriptor.citation = std::move(citation);
    return handle;
}

bool code_is(const nlohmann::json& error, const char* code) {
    return error.is_object() && error.value("code", "") == code;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_reference_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const std::string content = "A中B文C";
    const auto handle = reference_handle(base / "guide.txt", content);

    SkillReferenceLimits limits;
    limits.max_page_bytes = 4;
    limits.max_total_bytes = 16;
    SkillReferenceService service(limits);
    const auto first = service.read_page(handle, 0, 4);
    assert(first.ok && first.page);
    assert(first.page->text == "A中");
    assert(first.page->next_offset == 4U && !first.page->eof);
    assert(first.page->citation.resource_id == "guide");
    assert(first.page->citation.resource_digest == handle.resource_digest);
    assert(first.page->citation.package_digest == "package-digest");
    assert(first.page->citation.source_uri == "https://example.test/guide");
    assert(first.page->citation.license == "CC-BY-4.0");
    assert(first.page->citation.title == "Guide");
    assert(first.page->citation.byte_start == 0U);
    assert(first.page->citation.byte_end == 4U);

    const auto second = service.read_page(handle, first.page->next_offset, 4);
    assert(second.ok && second.page && second.page->text == "B文");
    assert(second.page->next_offset == 8U && !second.page->eof);
    const auto last = service.read_page(handle, 8, 4);
    assert(last.ok && last.page && last.page->text == "C" && last.page->eof);
    assert(last.page->next_offset == content.size());

    const auto invalid_offset = service.read_page(handle, 2, 4);
    assert(!invalid_offset.ok &&
           code_is(invalid_offset.error, "skill_reference_cursor_invalid"));
    const auto beyond = service.read_page(handle, 99, 4);
    assert(!beyond.ok && code_is(beyond.error, "skill_reference_range_invalid"));

    const auto empty = service.read_page(handle, content.size(), 4);
    assert(empty.ok && empty.page && empty.page->text.empty() && empty.page->eof);

    auto binary = handle;
    binary.descriptor.media_type = "application/octet-stream";
    const auto refused = service.read_page(binary, 0, 4);
    assert(!refused.ok && code_is(refused.error, "skill_reference_binary_refused"));

    const std::string invalid_bytes("\xC3\x28", 2);
    const auto invalid_handle = reference_handle(base / "invalid.txt", invalid_bytes);
    SkillReferenceService invalid_service(limits);
    const auto invalid_utf8 = invalid_service.read_page(invalid_handle, 0, 4);
    assert(!invalid_utf8.ok && code_is(invalid_utf8.error, "skill_reference_utf8_invalid"));

    SkillReferenceLimits tight;
    tight.max_page_bytes = 4;
    tight.max_total_bytes = 8;
    SkillReferenceService bounded(tight);
    assert(bounded.read_page(handle, 0, 4).ok);
    assert(bounded.read_page(handle, 4, 4).ok);
    const auto exhausted = bounded.read_page(handle, 8, 4);
    assert(!exhausted.ok && code_is(exhausted.error, "skill_reference_budget_exceeded"));

    const std::string searchable =
        "Alpha beta.\n"
        "alpha alpha beta.\n"
        "中文天气。\n"
        "天气中文。\n"
        "tie only.\n"
        "tie only.\n";
    const auto search_handle = reference_handle(base / "search.txt", searchable);
    SkillReferenceLimits search_limits;
    search_limits.max_index_source_bytes = 4096;
    search_limits.max_derived_index_bytes = 65536;
    search_limits.max_search_hits = 8;
    search_limits.snippet_bytes = 128;
    SkillReferenceService search_service(search_limits, base / "derived");

    const auto latin = search_service.search(search_handle, "ALPHA", 4);
    assert(latin.ok && latin.hits.size() == 2U);
    assert(latin.hits[0].score > latin.hits[1].score);
    assert(latin.hits[0].snippet == "alpha alpha beta.");
    assert(latin.hits[0].citation.resource_digest == search_handle.resource_digest);
    assert(latin.hits[0].citation.byte_start == latin.hits[0].snippet_start);
    assert(latin.hits[0].citation.byte_end == latin.hits[0].snippet_end);
    assert(fs::is_regular_file(latin.index_path));

    const auto cjk = search_service.search(search_handle, "天气", 4);
    assert(cjk.ok && cjk.hits.size() >= 2U);
    assert(cjk.hits[0].snippet.find("天气") != std::string::npos);

    const auto tied = search_service.search(search_handle, "tie", 4);
    assert(tied.ok && tied.hits.size() == 2U);
    assert(tied.hits[0].score == tied.hits[1].score);
    assert(tied.hits[0].match_start < tied.hits[1].match_start);

    auto changed_digest = search_handle;
    changed_digest.resource_digest = std::string(64, 'a');
    const auto invalidated = search_service.search(changed_digest, "alpha", 1);
    assert(invalidated.ok && invalidated.index_path != latin.index_path);
    auto invalid_digest = search_handle;
    invalid_digest.resource_digest = "../not-a-digest";
    const auto rejected_digest = search_service.search(invalid_digest, "alpha", 1);
    assert(!rejected_digest.ok &&
           code_is(rejected_digest.error, "skill_reference_digest_invalid"));

    SkillReferenceLimits tiny_index = search_limits;
    tiny_index.max_derived_index_bytes = 32;
    SkillReferenceService quota_service(tiny_index, base / "tiny-derived");
    const auto quota = quota_service.search(search_handle, "alpha", 1);
    assert(!quota.ok && code_is(quota.error, "skill_reference_index_quota_exceeded"));

    fs::remove_all(base, ec);
    std::cout << "test_skill_reference: ok\n";
    return 0;
}

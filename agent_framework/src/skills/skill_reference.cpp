#include <agent/skill_reference.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>

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

std::uint32_t codepoint_value(const std::string& bytes, std::size_t offset,
                              std::size_t size) {
    const auto first = static_cast<unsigned char>(bytes[offset]);
    if(size == 1) return first;
    std::uint32_t value = first & (size == 2 ? 0x1FU : size == 3 ? 0x0FU : 0x07U);
    for(std::size_t i = 1; i < size; ++i)
        value = (value << 6U) | (static_cast<unsigned char>(bytes[offset + i]) & 0x3FU);
    return value;
}

bool cjk(std::uint32_t value) {
    return (value >= 0x3400U && value <= 0x4DBFU) ||
           (value >= 0x4E00U && value <= 0x9FFFU) ||
           (value >= 0xF900U && value <= 0xFAFFU) ||
           (value >= 0x20000U && value <= 0x2FA1FU);
}

bool sha256_digest(const std::string& value) {
    return value.size() == 64U &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct TermOccurrence {
    std::string term;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

struct SearchDocument {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::string text;
    std::vector<TermOccurrence> terms;
};

bool tokenize(const std::string& text, std::uint64_t base,
              std::vector<TermOccurrence>* terms) {
    std::size_t cursor = 0;
    std::optional<TermOccurrence> previous_cjk;
    while(cursor < text.size()) {
        const auto size = codepoint_size(text, cursor);
        if(!size) return false;
        const auto value = codepoint_value(text, cursor, *size);
        if(*size == 1 && ((value >= 'A' && value <= 'Z') ||
                          (value >= 'a' && value <= 'z') ||
                          (value >= '0' && value <= '9') || value == '_')) {
            const std::size_t start = cursor;
            std::string word;
            while(cursor < text.size()) {
                const unsigned char ch = static_cast<unsigned char>(text[cursor]);
                if(!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9') || ch == '_')) break;
                word.push_back(static_cast<char>(std::tolower(ch)));
                ++cursor;
            }
            terms->push_back({std::move(word), base + start, base + cursor});
            previous_cjk.reset();
            continue;
        }
        if(cjk(value)) {
            TermOccurrence current{text.substr(cursor, *size), base + cursor,
                                   base + cursor + *size};
            terms->push_back(current);
            if(previous_cjk) {
                terms->push_back({previous_cjk->term + current.term,
                                  previous_cjk->start, current.end});
            }
            previous_cjk = current;
        } else {
            previous_cjk.reset();
        }
        cursor += *size;
    }
    return true;
}

std::optional<std::uint64_t> derived_usage(const std::filesystem::path& root,
                                           const std::filesystem::path& exclude) {
    std::uint64_t total = 0;
    std::error_code ec;
    if(!std::filesystem::exists(root, ec)) return ec ? std::nullopt : std::optional(total);
    const auto root_status = std::filesystem::symlink_status(root, ec);
    if(ec || std::filesystem::is_symlink(root_status) ||
       !std::filesystem::is_directory(root_status)) return std::nullopt;
    for(std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::none, ec), end;
        !ec && it != end; it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if(ec || std::filesystem::is_symlink(status)) return std::nullopt;
        if(std::filesystem::is_regular_file(status) && it->path() != exclude) {
            const auto size = std::filesystem::file_size(it->path(), ec);
            if(ec || size > std::numeric_limits<std::uint64_t>::max() - total)
                return std::nullopt;
            total += size;
        }
    }
    return ec ? std::nullopt : std::optional(total);
}

std::string utf8_prefix(const std::string& text, std::size_t limit) {
    std::size_t cursor = 0;
    std::size_t end = 0;
    while(cursor < text.size() && cursor < limit) {
        const auto size = codepoint_size(text, cursor);
        if(!size || cursor + *size > limit) break;
        cursor += *size;
        end = cursor;
    }
    return text.substr(0, end);
}

inline constexpr const char* kLexicalConfigDigest =
    "e5e3254be010baf08d5f52e5937f815fa7c691fc69c8415212fbb1ec5637f28f";

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

SkillReferenceSearchResult SkillReferenceService::search(
    const SkillResourceHandle& handle, const std::string& query,
    std::size_t max_hits) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [](const char* code, const std::string& message) {
        return SkillReferenceSearchResult{false, failure(code, message), {}, {}};
    };
    if(handle.descriptor.kind != SkillResourceType::Reference ||
       !textual(handle.descriptor.media_type))
        return fail("skill_reference_binary_refused",
                    "reference search requires a textual Reference resource");
    if(query.empty()) return fail("skill_reference_query_invalid", "search query is empty");
    if(max_hits == 0 || max_hits > limits_.max_search_hits)
        return fail("skill_reference_search_limit", "search hit limit is invalid");
    if(handle.size > limits_.max_index_source_bytes ||
       handle.size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return fail("skill_reference_index_source_too_large",
                    "reference exceeds the lexical index source limit");
    if(derived_root_.empty())
        return fail("skill_reference_index_root_missing", "derived cache root is unavailable");
    if(!sha256_digest(handle.resource_digest))
        return fail("skill_reference_digest_invalid",
                    "reference snapshot digest must be SHA-256");
    const std::string source_digest = lowercase(handle.resource_digest);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(handle.path, ec);
    if(ec || std::filesystem::is_symlink(status) ||
       !std::filesystem::is_regular_file(status) ||
       std::filesystem::file_size(handle.path, ec) != handle.size || ec)
        return fail("skill_reference_source_changed", "reference snapshot is unavailable");
    std::ifstream input(handle.path, std::ios::binary);
    if(!input) return fail("skill_reference_read_failed", "reference cannot be opened");
    std::string content(static_cast<std::size_t>(handle.size), '\0');
    if(!content.empty()) input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if(input.gcount() != static_cast<std::streamsize>(content.size()))
        return fail("skill_reference_source_changed", "reference changed during indexing");

    std::vector<SearchDocument> documents;
    std::size_t line_start = 0;
    while(line_start <= content.size()) {
        const auto newline = content.find('\n', line_start);
        const std::size_t line_end = newline == std::string::npos ? content.size() : newline;
        if(line_end > line_start) {
            SearchDocument document;
            document.start = line_start;
            document.end = line_end;
            document.text = content.substr(line_start, line_end - line_start);
            if(!tokenize(document.text, document.start, &document.terms))
                return fail("skill_reference_utf8_invalid",
                            "reference contains invalid UTF-8");
            if(!document.terms.empty()) documents.push_back(std::move(document));
        }
        if(newline == std::string::npos) break;
        line_start = newline + 1U;
    }
    std::vector<TermOccurrence> query_occurrences;
    if(!tokenize(query, 0, &query_occurrences))
        return fail("skill_reference_query_invalid", "search query is invalid UTF-8");
    std::set<std::string> query_terms;
    for(const auto& occurrence : query_occurrences) query_terms.insert(occurrence.term);
    if(query_terms.empty())
        return fail("skill_reference_query_invalid", "search query has no lexical terms");

    nlohmann::json index_documents = nlohmann::json::array();
    for(const auto& document : documents) {
        std::map<std::string, std::vector<nlohmann::json>> positions;
        for(const auto& term : document.terms)
            positions[term.term].push_back({term.start, term.end});
        index_documents.push_back({{"start", document.start}, {"end", document.end},
                                   {"text", document.text}, {"terms", positions}});
    }
    const nlohmann::json index = {{"schemaVersion", 1}, {"kind", "lexical-v1"},
        {"sourceDigest", source_digest},
        {"configDigest", kLexicalConfigDigest}, {"documents", index_documents}};
    const std::string serialized = index.dump();
    const auto index_directory = derived_root_ / "lexical-v1" /
        source_digest / kLexicalConfigDigest;
    const auto index_path = index_directory / "index.json";
    const auto usage = derived_usage(derived_root_, index_path);
    if(!usage || serialized.size() > limits_.max_derived_index_bytes ||
       *usage > limits_.max_derived_index_bytes - serialized.size())
        return fail("skill_reference_index_quota_exceeded",
                    "derived lexical index exceeds its cache quota");
    std::filesystem::create_directories(index_directory, ec);
    if(ec) return fail("skill_reference_index_write_failed", "index directory cannot be created");
    for(const auto& directory : {derived_root_, derived_root_ / "lexical-v1",
                                  derived_root_ / "lexical-v1" / source_digest,
                                  index_directory}) {
        const auto directory_status = std::filesystem::symlink_status(directory, ec);
        if(ec || std::filesystem::is_symlink(directory_status) ||
           !std::filesystem::is_directory(directory_status))
            return fail("skill_reference_index_write_failed",
                        "derived cache path is not trusted");
    }
    static std::atomic<std::uint64_t> sequence{0};
    const auto temporary = index_directory /
        ("index.tmp-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if(!output) return fail("skill_reference_index_write_failed", "index cannot be opened");
        output << serialized << '\n';
        output.flush();
        if(!output) {
            std::filesystem::remove(temporary, ec);
            return fail("skill_reference_index_write_failed", "index cannot be written");
        }
    }
    std::filesystem::rename(temporary, index_path, ec);
    if(ec) {
        std::filesystem::remove(temporary, ec);
        return fail("skill_reference_index_write_failed", "index cannot be published");
    }

    const double document_count = static_cast<double>(documents.size());
    double average_length = 0.0;
    for(const auto& document : documents)
        average_length += static_cast<double>(document.terms.size());
    if(document_count > 0.0) average_length /= document_count;
    std::map<std::string, std::size_t> document_frequency;
    for(const auto& term : query_terms) {
        for(const auto& document : documents) {
            if(std::any_of(document.terms.begin(), document.terms.end(),
                           [&](const auto& occurrence) { return occurrence.term == term; }))
                ++document_frequency[term];
        }
    }
    std::vector<SkillReferenceSearchHit> hits;
    for(const auto& document : documents) {
        double score = 0.0;
        std::optional<TermOccurrence> first_match;
        for(const auto& term : query_terms) {
            std::size_t frequency = 0;
            for(const auto& occurrence : document.terms) {
                if(occurrence.term == term) {
                    ++frequency;
                    if(!first_match || occurrence.start < first_match->start)
                        first_match = occurrence;
                }
            }
            if(frequency == 0) continue;
            const double df = static_cast<double>(document_frequency[term]);
            const double idf = std::log(1.0 + (document_count - df + 0.5) / (df + 0.5));
            const double tf = static_cast<double>(frequency);
            const double length = static_cast<double>(document.terms.size());
            const double normalization = average_length > 0.0 ? length / average_length : 1.0;
            score += idf * (tf * 2.2) / (tf + 1.2 * (0.25 + 0.75 * normalization));
        }
        if(score <= 0.0 || !first_match) continue;
        SkillReferenceSearchHit hit;
        hit.score = score;
        hit.match_start = first_match->start;
        hit.match_end = first_match->end;
        hit.snippet_start = document.start;
        hit.snippet = utf8_prefix(document.text, limits_.snippet_bytes);
        hit.snippet_end = hit.snippet_start + hit.snippet.size();
        hit.citation = citation_for(handle, hit.snippet_start, hit.snippet_end);
        hits.push_back(std::move(hit));
    }
    std::sort(hits.begin(), hits.end(), [](const auto& lhs, const auto& rhs) {
        if(lhs.score != rhs.score) return lhs.score > rhs.score;
        if(lhs.match_start != rhs.match_start) return lhs.match_start < rhs.match_start;
        return lhs.match_end < rhs.match_end;
    });
    if(hits.size() > max_hits) hits.resize(max_hits);
    return {true, nlohmann::json::object(), index_path, std::move(hits)};
}

} // namespace agent_framework

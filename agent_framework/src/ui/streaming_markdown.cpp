#include <agent/ui/streaming_markdown.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace agent_framework {
namespace {

std::string trim(std::string_view value) {
    auto first = value.begin();
    auto last = value.end();
    while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;
    return {first, last};
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> lines_of(std::string_view markdown) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= markdown.size()) {
        const auto end = markdown.find('\n', begin);
        if (end == std::string_view::npos) {
            lines.emplace_back(markdown.substr(begin));
            break;
        }
        lines.emplace_back(markdown.substr(begin, end - begin));
        begin = end + 1;
    }
    return lines;
}

std::vector<std::string> table_row(std::string_view line) {
    std::vector<std::string> cells;
    auto value = trim(line);
    if (!value.empty() && value.front() == '|') value.erase(value.begin());
    if (!value.empty() && value.back() == '|') value.pop_back();
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('|', begin);
        cells.push_back(trim(std::string_view(value).substr(
            begin, end == std::string::npos ? std::string::npos : end - begin)));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return cells;
}

bool is_table_separator(std::string_view line) {
    const auto cells = table_row(line);
    if (cells.empty()) return false;
    return std::all_of(cells.begin(), cells.end(), [](const std::string& raw) {
        auto cell = raw;
        if (!cell.empty() && cell.front() == ':') cell.erase(cell.begin());
        if (!cell.empty() && cell.back() == ':') cell.pop_back();
        return cell.size() >= 3 &&
               std::all_of(cell.begin(), cell.end(), [](char c) { return c == '-'; });
    });
}

bool is_list_item(std::string_view line) {
    const auto value = trim(line);
    if (starts_with(value, "- ") || starts_with(value, "* ") || starts_with(value, "+ "))
        return true;
    std::size_t i = 0;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) ++i;
    return i > 0 && i + 1 < value.size() && value[i] == '.' && value[i + 1] == ' ';
}

void append_line(std::string& dst, const std::string& line) {
    if (!dst.empty()) dst.push_back('\n');
    dst += line;
}

} // namespace

std::vector<UiContentBlock> StreamingMarkdownAssembler::parse(std::string_view markdown,
                                                               bool finalized) const {
    std::vector<UiContentBlock> blocks;
    const auto lines = lines_of(markdown);
    std::size_t i = 0;
    while (i < lines.size()) {
        const auto line = trim(lines[i]);
        if (line.empty()) { ++i; continue; }

        if (starts_with(line, "```")) {
            const auto language = trim(std::string_view(line).substr(3));
            std::string body;
            std::size_t end = i + 1;
            while (end < lines.size() && !starts_with(trim(lines[end]), "```")) {
                append_line(body, lines[end]);
                ++end;
            }
            const bool closed = end < lines.size();
            UiContentBlock block;
            block.kind = closed ? (language == "mermaid" ? UiContentBlockKind::Mermaid
                                                          : UiContentBlockKind::Code)
                                : UiContentBlockKind::DraftTail;
            block.text = std::move(body);
            block.info = language;
            block.stable = closed || finalized;
            blocks.push_back(std::move(block));
            i = closed ? end + 1 : lines.size();
            continue;
        }

        if (line == "$$") {
            std::string body;
            std::size_t end = i + 1;
            while (end < lines.size() && trim(lines[end]) != "$$") {
                append_line(body, lines[end]);
                ++end;
            }
            const bool closed = end < lines.size();
            UiContentBlock block;
            block.kind = closed ? UiContentBlockKind::MathBlock : UiContentBlockKind::DraftTail;
            block.text = std::move(body);
            block.stable = closed || finalized;
            blocks.push_back(std::move(block));
            i = closed ? end + 1 : lines.size();
            continue;
        }

        std::size_t heading = 0;
        while (heading < line.size() && heading < 6 && line[heading] == '#') ++heading;
        if (heading > 0 && heading < line.size() && line[heading] == ' ') {
            UiContentBlock block;
            block.kind = UiContentBlockKind::Heading;
            block.heading_level = static_cast<int>(heading);
            block.text = trim(std::string_view(line).substr(heading + 1));
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        if (line == "---" || line == "***" || line == "___") {
            UiContentBlock block;
            block.kind = UiContentBlockKind::ThematicBreak;
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        if (i + 1 < lines.size() && line.find('|') != std::string::npos &&
            is_table_separator(lines[i + 1])) {
            UiContentBlock block;
            block.kind = UiContentBlockKind::Table;
            block.table_cells.push_back(table_row(lines[i]));
            i += 2;
            while (i < lines.size() && trim(lines[i]).find('|') != std::string::npos &&
                   !trim(lines[i]).empty()) {
                block.table_cells.push_back(table_row(lines[i]));
                ++i;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        if (is_list_item(line)) {
            UiContentBlock block;
            block.kind = UiContentBlockKind::List;
            while (i < lines.size() && is_list_item(lines[i])) {
                append_line(block.text, trim(lines[i]));
                ++i;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        UiContentBlock paragraph;
        paragraph.kind = UiContentBlockKind::Paragraph;
        while (i < lines.size()) {
            const auto value = trim(lines[i]);
            if (value.empty()) break;
            if (!paragraph.text.empty()) paragraph.text.push_back('\n');
            paragraph.text += lines[i];
            ++i;
        }
        const bool has_terminal_newline = !markdown.empty() && markdown.back() == '\n';
        if (!finalized && i == lines.size() && !has_terminal_newline) {
            paragraph.kind = UiContentBlockKind::DraftTail;
            paragraph.stable = false;
        }
        blocks.push_back(std::move(paragraph));
    }
    return blocks;
}

} // namespace agent_framework

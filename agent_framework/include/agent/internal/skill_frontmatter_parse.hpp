/**
 * @file skill_frontmatter_parse.hpp
 * @brief WP1.8：受限 YAML frontmatter 子集（无 yaml-cpp）
 */
#ifndef __AGENT_INTERNAL_SKILL_FRONTMATTER_PARSE_HPP__
#define __AGENT_INTERNAL_SKILL_FRONTMATTER_PARSE_HPP__

#include <agent/skills/skill_types.hpp>

#include <optional>
#include <string>

namespace agent_framework {
namespace internal {

/**
 * @brief 解析首个 `---` … `---` 块为 SkillIndexEntry 字段（Cursor：`name`、`description:>-`、
 *        `disable-model-invocation`；legacy `id` 写入 `yaml_id`）。
 * @return YAML 体在 trim 后为空则 nullopt；否则返回部分填充的条目（canonical 由 SkillRegistry 合并）。
 */
std::optional<SkillIndexEntry> parse_skill_frontmatter_yaml(const std::string& yaml_block,
                                                            std::string* error_out);

/**
 * @brief 从整文件内容切出 frontmatter 与正文（正文 = 第二个 `---` 行之后全文）
 */
struct SplitFrontmatterResult {
    std::string yaml_inner;
    std::string body;
    bool ok = false;
};

SplitFrontmatterResult split_skill_file_content(std::string_view file_content);

} // namespace internal
} // namespace agent_framework

#endif // __AGENT_INTERNAL_SKILL_FRONTMATTER_PARSE_HPP__

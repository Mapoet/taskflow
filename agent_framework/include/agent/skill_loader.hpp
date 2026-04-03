/**
 * @file skill_loader.hpp
 * @brief WP1.8 L2：技能正文加载（去 frontmatter + 字符预算）
 */
#ifndef __AGENT_SKILL_LOADER_H__
#define __AGENT_SKILL_LOADER_H__

#include "skill_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace agent_framework {

class SkillLoader {
public:
    explicit SkillLoader(const SkillRegistry& registry);

    /**
     * @brief 读取文件、去掉首块 frontmatter，返回正文；按 `max_chars` 截断 UTF-8 安全按字节截断即可。
     */
    std::optional<std::string> load_instructions(const std::string& skill_id,
                                                   std::size_t max_chars) const;

    std::filesystem::path skill_directory(const std::string& skill_id) const;

private:
    const SkillRegistry& registry_;
    mutable std::unordered_map<std::string, std::pair<std::string, std::uintmax_t>> cache_;
};

} // namespace agent_framework

#endif // __AGENT_SKILL_LOADER_H__

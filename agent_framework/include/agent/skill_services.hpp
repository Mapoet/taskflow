/**
 * @file skill_services.hpp
 * @brief WP1.8：Registry + Loader 聚合；可选 `from_env`
 */
#ifndef __AGENT_SKILL_SERVICES_H__
#define __AGENT_SKILL_SERVICES_H__

#include "skill_loader.hpp"
#include "skill_registry.hpp"

#include <memory>

namespace agent_framework {

struct SkillServices {
    std::shared_ptr<SkillRegistry> registry;
    std::shared_ptr<SkillLoader> loader;

    /** `AGENT_SKILLS_DIR` 未设置或为空则返回 nullptr */
    static std::shared_ptr<SkillServices> from_env();

    /**
     * @brief 合并扫描 Cursor 默认技能目录（存在的才会加入）：
     *        `$HOME/.cursor/skills`、`$HOME/.cursor/skills-cursor`（Windows：`%USERPROFILE%`）。
     *        若二者均不存在则返回 nullptr。
     */
    static std::shared_ptr<SkillServices> from_cursor_default_skill_roots();
};

/** 读取 `AGENT_SKILL_CONTEXT_MAX_CHARS`，默认 8000 */
std::size_t skill_context_max_chars_from_env();

} // namespace agent_framework

#endif // __AGENT_SKILL_SERVICES_H__

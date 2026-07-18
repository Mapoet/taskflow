/**
 * @file skill_services.hpp
 * @brief WP1.8：Registry + Loader 聚合；可选 `from_env`
 */
#ifndef __AGENT_SKILL_SERVICES_H__
#define __AGENT_SKILL_SERVICES_H__

#include <agent/skills/skill_loader.hpp>
#include <agent/skills/skill_registry.hpp>
#include <agent/skills/skill_resource_access.hpp>
#include <agent/skills/skill_resource_cache.hpp>
#include <agent/skills/skill_reference.hpp>
#include <agent/skills/skill_model.hpp>
#include <agent/skills/skill_manager.hpp>
#include <agent/resources/session_resource_context.hpp>

#include <memory>

namespace agent_framework {

class SkillRuntime;

struct SkillServices {
    std::shared_ptr<SkillRegistry> registry;
    std::shared_ptr<SkillLoader> loader;
    std::shared_ptr<SkillRuntime> runtime;
    std::shared_ptr<SkillResourceAccess> resource_access;
    std::shared_ptr<SkillResourceCache> resource_cache;
    std::shared_ptr<SkillReferenceService> references;
    std::shared_ptr<SkillModelService> models;
    std::shared_ptr<SkillManager> manager;
    std::shared_ptr<SessionResourceContext> resources;

    /** Create a request-pinned context using the latest atomically published Registry snapshot. */
    std::shared_ptr<SessionResourceContext> pin_resource_context() const;

    /** `AGENT_SKILLS_DIR` 未设置或为空则返回 nullptr */
    static std::shared_ptr<SkillServices> from_env();

    /**
     * @brief 合并扫描 Cursor 默认技能目录（存在的才会加入）：
     *        `$HOME/.cursor/skills`、`$HOME/.cursor/skills-cursor`（Windows：`%USERPROFILE%`）。
     *        每根下技能形态为 **`<skill-folder>/SKILL.md`**。
     *        若二者均不存在则返回 nullptr。
     */
    static std::shared_ptr<SkillServices> from_cursor_default_skill_roots();
};

/** 读取 `AGENT_SKILL_CONTEXT_MAX_CHARS`，默认 8000 */
std::size_t skill_context_max_chars_from_env();

/**
 * @brief 将已索引技能的短目录（canonical + 截断 description）拼成一段，用于注入 system。
 */
std::string format_skill_catalog_l1(const SkillRegistry& registry, std::size_t max_chars = 2048);

} // namespace agent_framework

#endif // __AGENT_SKILL_SERVICES_H__

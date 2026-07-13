/**
 * @file skill_types.hpp
 * @brief WP1.8 Skills：L1 索引条目（与 phase-1-wp8 §4.1 对齐）
 */
#ifndef __AGENT_SKILL_TYPES_H__
#define __AGENT_SKILL_TYPES_H__

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

struct SkillIndexEntry {
    /**
     * @brief Canonical skill key（对外唯一）：Cursor `name`、legacy `id` 或目录名合并结果；用于 get、match、run_skill_script。
     */
    std::string id;
    /** @brief Frontmatter 中的展示名（通常与 Cursor `name` 一致；可与 id 相同） */
    std::string name;
    std::string description;
    std::string version;
    std::string license;
    std::vector<std::string> trigger_keywords;
    std::vector<std::string> tags;
    std::filesystem::path file_path;
    /**
     * @brief `run_skill_script` 的 jail 根目录（含 `SKILL.md` 的技能包目录，即 `<root>/<skill-folder>/`）。
     *        若为 nullopt，工具回退为 `SkillRegistry::root()/id`。
     */
    std::optional<std::filesystem::path> script_jail;
    /** @brief Cursor：`disable-model-invocation: true` 时不参与关键词路由 match（仍可供 L3） */
    bool disable_model_invocation = false;
    /** 解析用：Frontmatter 中显式的 legacy `id`（与 canonical 分离，供冲突告警） */
    std::string yaml_id;
    /** 可选；首版可为空对象或含 `raw` 字符串 */
    nlohmann::json resources = nlohmann::json::object();
    std::vector<std::string> scripts;
    std::vector<std::string> references;
    std::vector<std::string> cli_programs;
    std::vector<std::string> allowed_tools;
};

enum class SkillDiagnosticSeverity { Warning, Error };

struct SkillDiagnostic {
    SkillDiagnosticSeverity severity = SkillDiagnosticSeverity::Warning;
    std::string code;
    std::filesystem::path path;
    std::string message;
};

enum class SkillResourceKind { Script, Reference, Cli, AnyDeclared };

} // namespace agent_framework

#endif // __AGENT_SKILL_TYPES_H__

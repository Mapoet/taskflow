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
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> trigger_keywords;
    std::vector<std::string> tags;
    std::filesystem::path file_path;
    /**
     * @brief `run_skill_script` 的 jail 根目录（通常为扫描根目录下的 `<id>/`）。
     *        若扫描时该目录不存在则为 nullopt，工具回退为 `SkillRegistry::root()/id`。
     */
    std::optional<std::filesystem::path> script_jail;
    /** 可选；首版可为空对象或含 `raw` 字符串 */
    nlohmann::json resources = nlohmann::json::object();
};

} // namespace agent_framework

#endif // __AGENT_SKILL_TYPES_H__

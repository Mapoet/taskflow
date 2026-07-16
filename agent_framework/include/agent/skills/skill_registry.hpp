/**
 * @file skill_registry.hpp
 * @brief WP1.8 L1：技能目录扫描与关键词路由
 *
 * **布局**：每个技能为 **`<root>/<skill-folder>/SKILL.md`**（`skill-folder` 为一级子目录名）。
 * **重复 id**：后扫描到的包 **不覆盖**已有条目，写入 stderr 警告后跳过。
 */
#ifndef __AGENT_SKILL_REGISTRY_H__
#define __AGENT_SKILL_REGISTRY_H__

#include <agent/skills/skill_types.hpp>
#include <agent/skills/skill_manifest.hpp>

#include <filesystem>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace agent_framework {

struct SkillRegistryState {
    std::vector<SkillIndexEntry> entries;
    std::vector<SkillDiagnostic> diagnostics;
    std::uint64_t generation = 0;
};

class SkillRegistrySnapshot {
public:
    SkillRegistrySnapshot() = default;
    explicit SkillRegistrySnapshot(std::shared_ptr<const SkillRegistryState> state)
        : state_(std::move(state)) {}

    const std::vector<SkillIndexEntry>& entries() const;
    const std::vector<SkillDiagnostic>& diagnostics() const;
    std::uint64_t generation() const noexcept;
    bool valid() const;
    std::optional<SkillIndexEntry> get(std::string_view skill_id) const;
    std::shared_ptr<const SkillManifest> get_manifest(std::string_view skill_id) const;
    std::optional<std::string> match(std::string_view user_text) const;

private:
    std::shared_ptr<const SkillRegistryState> state_;
};

class SkillRegistry {
public:
    explicit SkillRegistry(std::filesystem::path root_directory);

    /**
     * @brief 多根目录扫描（顺序：先出现的根优先；重复 id 仍遵循「后扫到跳过」）。
     */
    explicit SkillRegistry(std::vector<std::filesystem::path> root_directories);

    /** 扫描各根下一层子目录中的 **`SKILL.md`**；可重复调用以 reload */
    void scan_or_reload();

    std::vector<SkillIndexEntry> entries() const { return snapshot().entries(); }
    std::vector<SkillDiagnostic> diagnostics() const { return snapshot().diagnostics(); }
    bool valid() const;
    SkillRegistrySnapshot snapshot() const;

    /** Atomically publish a fully validated lifecycle-managed generation. */
    void publish(std::vector<SkillIndexEntry> entries,
                 std::vector<SkillDiagnostic> diagnostics = {});

    /** 单根时为该根；多根时为 **第一个** 根目录（仅作兼容；L3 请优先用 `SkillIndexEntry::script_jail`） */
    const std::filesystem::path& root() const {
        return primary_root_;
    }

    const std::vector<std::filesystem::path>& roots() const {
        return roots_;
    }

    std::optional<SkillIndexEntry> get(std::string_view skill_id) const;
    std::shared_ptr<const SkillManifest> get_manifest(std::string_view skill_id) const;

    /**
     * @brief 关键词/标签子串计分；多命中取最大分，平手取 id 字典序最小。
     *        `AGENT_SKILL_ROUTER=off` 时始终返回 nullopt。
     */
    std::optional<std::string> match(std::string_view user_text) const;

private:
    std::filesystem::path primary_root_;
    std::vector<std::filesystem::path> roots_;
    std::shared_ptr<const SkillRegistryState> state_;
    mutable std::mutex reload_mutex_;

    void scan_one_root(const std::filesystem::path& scan_root,
                       std::unordered_set<std::string>& seen_ids,
                       std::vector<SkillIndexEntry>& entries,
                       std::vector<SkillDiagnostic>& diagnostics) const;
};

} // namespace agent_framework

#endif // __AGENT_SKILL_REGISTRY_H__

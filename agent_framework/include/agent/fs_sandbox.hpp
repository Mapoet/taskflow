/**
 * @file fs_sandbox.hpp
 * @brief 内建 fs_* 工具：AGENT_FS_ROOT 下的路径解析与配额配置
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-03
 */
#ifndef __AGENT_FS_SANDBOX_H__
#define __AGENT_FS_SANDBOX_H__

#include "types.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace agent_framework {

/**
 * @brief 从环境变量加载的 fs 沙箱参数；root 必须已规范化为现有目录。
 */
struct FsSandboxConfig {
    std::filesystem::path root;
    std::size_t max_read_bytes = 1048576;
    std::size_t max_write_bytes = 1048576;
    std::size_t max_list_depth = 8;
    std::size_t max_list_entries = 5000;
    std::size_t max_grep_files = 200;
    std::size_t max_grep_matches = 500;
    std::size_t max_line_length = 8192;
    std::size_t search_max_results = 500;
};

/**
 * @brief 从 getenv 填充配置；root 不存在或非目录时返回 nullopt。
 */
std::optional<FsSandboxConfig> load_fs_sandbox_config_from_env();

/**
 * @brief 将用户 path（相对 AGENT_FS_ROOT；也允许已是根内绝对路径）解析为 canonical 路径。
 * @param rel_or_abs 相对路径（首选）或落在 root 下的绝对路径
 * @param root 已 canonical 的根目录
 * @param err 失败时写入 {"error":{"code","message"}}
 * @return 成功则返回目标路径
 */
std::optional<std::filesystem::path> fs_resolve_under_root(const std::string& rel_or_abs,
                                                           const std::filesystem::path& root,
                                                           json& err);

/**
 * @brief 判断绝对路径 target 是否落在 root 下（含相等）。
 */
bool fs_is_path_inside_root(const std::filesystem::path& target,
                            const std::filesystem::path& root_canon);

json fs_tool_error(const std::string& code, const std::string& message);

} // namespace agent_framework

#endif // __AGENT_FS_SANDBOX_H__

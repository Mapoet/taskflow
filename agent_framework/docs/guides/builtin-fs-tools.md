# 内建 fs_* 本地工具（AGENT_FS_ROOT）

在 **`build_cli_agent_graph`** 构图时，若 **`AGENT_FS_ROOT`** 指向**已存在的目录**，则会向 **ToolBus** 注册一组 **`fs_*` 本地工具**（非 MCP 进程）。所有路径参数均解析为该根目录下的 **`weakly_canonical` 绝对路径**；任一路径在规范后越出根（含 `..` 逃逸）则返回错误 **`path_outside_root`**。

**安全说明**：此为**路径监禁**，不是操作系统级沙箱；被许可的路径上仍可进行读写删。勿对不可信代码或不可信模型在无人工监督下放开过大根目录。

## 与 Node `filesystem` MCP 的关系

推荐 **二选一**：

- 仅使用内建 `fs_*`：设置 `AGENT_FS_ROOT`，并在 Cursor `mcp.json` 中**移除** `@modelcontextprotocol/server-filesystem`（或等价配置），避免两套根目录策略让模型混淆。
- 若 **并存**：务必在系统提示或运维文档中写清 **MCP 允许的根** 与 **`AGENT_FS_ROOT`** 的差异。

## 环境变量

| 变量 | 含义 | 未设置时默认 |
|------|------|----------------|
| `AGENT_FS_ROOT` | 唯一允许访问的目录；必须存在且为目录 | 未设置或无效 → **不注册**任何 `fs_*` |
| `AGENT_FS_MAX_READ_BYTES` | `fs_read` 等单次读上限 | `1048576` |
| `AGENT_FS_MAX_WRITE_BYTES` | `fs_write` / `fs_replace` 写入内容上限 | 与读相同（若未单独设） |
| `AGENT_FS_MAX_LIST_DEPTH` | `fs_list_dir` 的 depth 硬顶 | `8` |
| `AGENT_FS_MAX_LIST_ENTRIES` | `fs_list_dir` 单次返回条数上限 | `5000` |
| `AGENT_FS_MAX_GREP_FILES` | `fs_grep` 打开文件数上限 | `200` |
| `AGENT_FS_MAX_GREP_MATCHES` | `fs_grep` 匹配条数上限 | `500` |
| `AGENT_FS_MAX_LINE_LENGTH` | `fs_grep` 单行截断长度 | `8192` |
| `AGENT_FS_SEARCH_MAX_RESULTS` | `fs_search` 路径结果上限 | `500` |

## AGENT_TOOL_ALLOWLIST

若设置 **`AGENT_TOOL_ALLOWLIST`**（逗号分隔），则注册阶段会拒绝不在列表中的工具。使用内建 `fs_*` 时，请把所需名称一并列入，例如：

`fs_read,fs_write,fs_list_dir,fs_mkdir,fs_delete,fs_search,fs_grep,fs_replace`（外加你仍需要的 `add`、`run_skill_script` 等）。

## 工具一览

| 名称 | 作用 |
|------|------|
| `fs_read` | 读文件；`mode`：`utf8`（默认，非法 UTF-8 报错）或 `binary_preview`（十六进制预览） |
| `fs_write` | 原子写文件；已存在文件须 **`confirm_overwrite: true`** |
| `fs_list_dir` | 列目录；`depth`、`include_dotfiles`；结果可 `truncated` |
| `fs_mkdir` | 建目录；`parents: true` 等价 `mkdir -p` |
| `fs_delete` | 删文件或**仅空目录**；**`confirm` 必须为 `true`** |
| `fs_search` | 相对根的路径 glob（支持 `*`、`?`、段内 `**`）；可选 `exclude_glob` |
| `fs_grep` | 目录下 UTF-8 文本按正则搜索；非法 UTF-8 行计入 `skipped_binary_lines` |
| `fs_replace` | 单文件字符串替换；**默认 `dry_run: true`**；真实写入须 `dry_run: false` 且 **`confirm_write: true`** |

## 常见错误码（`error.code`）

| code | 说明 |
|------|------|
| `path_outside_root` | 路径逃出 `AGENT_FS_ROOT` |
| `file_too_large` | 超过读/写配额 |
| `confirm_required` | 缺 `fs_delete` 的 `confirm` 或 `fs_write`/`fs_replace` 的确认项 |
| `directory_not_empty` | `fs_delete` 暂不支持删非空目录 |
| `invalid_utf8` | `fs_read`（utf8 模式）或 `fs_replace` 要求 UTF-8 文本 |
| `no_match` | `fs_replace` 未找到 `old_string` |

## 测试

`BUILD_TESTING=ON` 时构建 **`test_fs_tools`**，运行：`ctest -R fs_tools` 或直接执行该二进制（POSIX 路径语义）。

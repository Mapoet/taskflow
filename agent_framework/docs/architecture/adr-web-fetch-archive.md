# ADR: `web_fetch_archive` 依赖与解压策略

**状态**：已采纳（Milestone 2）  
**日期**：2026-04-04  
**上下文**：仓库需内建 **`web_fetch_archive`**，在受控环境下自 URL 下载归档并解压，同时缓解 Zip Slip 与解压炸弹。

## 决策

1. **格式范围（首版）**：仅支持 **ZIP**（含 **deflate** 与 **stored**）。**不**在首版纳入 `.tar.gz`（需额外依赖或更多解析面）。
2. **解压依赖**：若系统/构建提供 **zlib**，则链接 **`ZLIB::ZLIB`** 并定义 **`AGENT_HAVE_ZLIB`**，以支持 **deflate**（`compression method 8`）。若无 zlib，仍支持 **stored（method 0）**；deflate 请求返回明确错误码（见 `builtin-web-tools.md`）。
3. **落盘策略**：解压**仅**允许写入 **`AGENT_FS_ROOT`** 下的相对目录：默认子路径为 **`AGENT_WEB_EXTRACT_SUBDIR`**（默认 `web_extract`）再加一次性作业子目录。无有效 **`AGENT_FS_ROOT`** 时工具返回 **`fs_root_required`**，与 `fs_*` 沙箱配置一致。
4. **解析实现**：进程内 **EOCD + Central Directory** 扫描（非 libzip/libarchive），便于审计边界；条目路径经 **`fs_is_path_inside_root`** 与 Zip Slip 规则校验。

## 后果

- **优点**：依赖面小（可选 zlib 为常见系统包）；行为与 **`AGENT_FS_ROOT`** 统一，运维边界清晰。
- **缺点**：ZIP 解析子集，不支持分卷/Zip64 等复杂形态；`.tar.gz` 需后续 ADR 追加依赖（如 libarchive 或独立 tar 解析）。

## 未纳入（明确拒绝或延期）

- 默认解锁任意磁盘路径。
- 在未配置 FS 根时静默写入临时目录并泄露路径给模型（与当前 **`fs_root_required`** 契约冲突）。

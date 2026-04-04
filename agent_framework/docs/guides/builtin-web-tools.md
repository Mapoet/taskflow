# 内建 web_* 网络工具（设计 / 实现指南）

在 **`build_cli_agent_graph`**（或等价构图入口）中，若 **`AGENT_WEB_ENABLE`** 为真（或下文约定的其它**显式开关**全部满足），则向 **ToolBus** 注册一组 **`web_*` 本地工具**（非 MCP 进程），用于：**网络检索**、**网络资源拉取与结构化解析**、**压缩包下载与安全解压**、**RSS/Atom 订阅解析**。

本文档与 [builtin-fs-tools.md](./builtin-fs-tools.md) 并列：**`fs_*` 管本地根目录监禁，`web_*` 管受控出站 HTTP(S)**。二者可配合使用（例如将 `web_fetch` 解压结果落盘到 `AGENT_FS_ROOT` 下时再交给 `fs_read`），但须在运维文档中写清 **出站策略** 与 **本地根** 的边界。

**安全说明**：`web_*` 不是浏览器沙箱；默认应实现 **SSRF 缓解**（禁止或限制访问私网/元数据地址）、**响应体与解压后总大小上限**、**重定向次数上限**、**单 host 速率限制**。勿对不可信模型在无监督环境下放开 **任意 URL** 与 **任意解压路径**。

## 与外部「搜索 / 抓取」MCP 的关系

推荐策略与 `fs_*` 文档类似：

- **仅内建 `web_*`**：在 `mcp.json` 中移除功能重叠的搜索/抓取 MCP，避免模型在「同一意图」下混用两套工具、两套配额与两套 URL 策略。
- **若并存**：在系统提示或运维文档中说明：**MCP 工具的权限范围**（白名单站点、API Key 归属）与 **`AGENT_WEB_*` 环境变量** 的差异；优先约定「默认用一种」。

## 1. 网络资料搜索（`web_search`）

本指南 **`web_search` 仅实现一种后端**：**[DuckDuckGo HTML](https://html.duckduckgo.com/html/)**（HTTPS、`/html/?q=`、解析结果页 HTML）。**不要求搜索 API Key**；合规、速率限制与页面结构稳定性由运维与实现共同承担（见 **§1.1**、**§1.4**）。

### 1.1 职责边界

- **只做检索摘要**：返回 `title`、`url`、`snippet`（及必要时的 `rank`），**不**默认串联抓取每个结果的全文（避免流量与版权风险不可控）。
- **固定出站目标**：仅连接 **`html.duckduckgo.com:443`**（及重定向链中 DuckDuckGo 可控跳转，仍须限次数与体积）。用户输入只进入 **query 字符串**（URL 编码后放入 `q=`），**不得**作为任意 host/path 拼接（防 SSRF）。

**风险说明**：与完全任意的「网页 SERP 抓取」相比，固定单一主机可将攻击面缩小，但该集成仍依赖 **第三方 HTML 形态**，存在 **反爬/限流/DOM 改版** 导致解析失效或零结果；须在运维文档中说明使用场景，并配合 **§1.4** 的限流与监控。

### 1.2 建议参数（JSON Schema 级别描述）

| 参数 | 类型 | 说明 |
|------|------|------|
| `query` | string | 必填；搜索查询字符串（可直接写 `site:example.com 关键词` 等 DuckDuckGo 支持的查询语法） |
| `max_results` | int | 可选；默认如 `10`，硬顶如 `25`（可配置） |
| `market` / `locale` | string | 可选；若 HTML 端点不单独支持，实现可 **忽略** 或 **合并进 `query`** |
| `site_filter` | string | 可选；若未并入 `query`，实现可拼接为 `site:... ` 前缀 |

### 1.3 建议返回

```json
{
  "provider": "duckduckgo",
  "query": "...",
  "results": [
    {"title": "...", "url": "https://...", "snippet": "..."}
  ],
  "truncated": false
}
```

返回体中 **`provider` 固定为** `duckduckgo`（与实现常量一致即可）。失败时返回 `error.code`（见下文「常见错误码」），**勿**将完整上游 HTML 原文写入对模型可见的字段（可记录内部日志并截断）。

### 1.4 DuckDuckGo HTML（工程落地要点）

以下约定与 **cpp-httplib** + **nlohmann::json** 的嵌入方式一致，作为 **`web_search` 的唯一实现**；接口形态与 **§1.2 / §1.3** 对齐。

**请求**

| 项 | 约定 |
|----|------|
| 主机 | **固定** `html.duckduckgo.com`，端口 `443`；构造路径 `/html/?q=` + URL 编码后的 `query`。禁止将用户输入用于任意 host/path（防 SSRF）。 |
| 客户端 | `httplib::SSLClient`；`User-Agent` 使用 `AGENT_WEB_USER_AGENT`（含项目标识）；`Accept: text/html` |
| 超时 / 重定向 | `connection_timeout` / `read_timeout` 使用 `AGENT_WEB_TIMEOUT_MS` 或专用 `AGENT_WEB_SEARCH_TIMEOUT_MS`；`set_follow_location(true)` 与 **全局 `max_redirects` 上限** 一致 |
| 响应体上限 | 独立上限推荐 **`AGENT_WEB_DDG_MAX_BODY_BYTES`**（例如 `1048576`）；超过则 **`body_too_large`**，不解析 |

**解析（轻量、无 DOM 库）**

- 使用正则或手动扫描提取结果块（示例模式：含 `class="result__a"` 的链接、`result__snippet` 摘要等）。**页面改版会导致零结果或错解析**，应监控并预留 **`search_html_parse_error` 或降级空结果** 策略。
- 对 `title` / `snippet` 做 **HTML entity 解码**（`&amp;`、`&quot;` 等）与 **去标签**后再写入 JSON。
- 结果条数截断至 **`max_results`** 与配置硬顶（如 `25`）。

**结果 URL（可选增强）**

- DuckDuckGo 有时返回 `https://duckduckgo.com/l/?uddg=...` 形式；可对 `uddg` 参数做 **URL 解码** 得到目标链接再返回，便于下游 `web_fetch` 直接使用（仍须遵守 `web_fetch` 的 SSRF 策略）。

**限流（强烈建议）**

- 同一进程内对 **`html.duckduckgo.com`** 做 **最小调用间隔**（如 **`AGENT_WEB_SEARCH_MIN_INTERVAL_MS`**，默认不少于 `1000` ms），避免被限流或封禁；Agent 循环高频搜索时尤需依赖此项。

**推荐实现骨架（逻辑流）**

```text
validate_query → 仅连接固定 host → GET /html/?q=... → 检查状态码与 body 大小
→ 轻量 HTML 提取 →（可选）解 uddg → 拼 results[] → 返回 JSON
```

### 1.5 `web_search`（DuckDuckGo）环境变量

| 变量 | 含义 | 未设置时默认 |
|------|------|----------------|
| `AGENT_WEB_SEARCH_TIMEOUT_MS` | 仅 `web_search` 阶段的超时；可覆盖 `AGENT_WEB_TIMEOUT_MS` | 与 `AGENT_WEB_TIMEOUT_MS` 相同 |
| `AGENT_WEB_SEARCH_MIN_INTERVAL_MS` | 两次 `web_search` 最小间隔（同进程，对 `html.duckduckgo.com`） | `1000` |
| `AGENT_WEB_DDG_MAX_BODY_BYTES` | DuckDuckGo HTML 响应体解析前上限 | `1048576` |
| `HTTPS_PROXY` / `https_proxy` / `HTTP_PROXY` / `http_proxy` | 按序取第一个非空；经 **HTTP CONNECT** 访问 DuckDuckGo（与 curl 常见用法一致） | 未设置则直连 |
| `AGENT_WEB_DDG_PAUSE_ON_CHALLENGE` | 检测到人机验证页时：在 **stderr** 打印说明与链接，**阻塞**读 stdin 一行，再按当前环境重试本次搜索一次 | 关闭 |
| `AGENT_WEB_DDG_COOKIE` | 请求携带的 `Cookie` 头；可在浏览器完成验证后从开发者工具复制，配合上一项在按 Enter 前 `export` | 无 |

当响应被识别为 DuckDuckGo **人机验证 / anomaly** 页且解析结果为空时，成功返回的 JSON 会包含 **`ddg_challenge`**：`detected`、`open_in_browser`（建议在浏览器打开的搜索 URL）。若启用 `AGENT_WEB_DDG_PAUSE_ON_CHALLENGE`，重试后仍可能为空，此时 `interactive_retry` 等字段见返回体。

---

## 2. 网络文件处理（`web_fetch` / 多文件与压缩包）

### 2.1 基础能力：`web_fetch`

对 **单个 URL** 执行 **GET**（或 HEAD+GET），适用于 `text/plain`、`text/markdown`、`application/json`、`text/html`（可仅返回截断正文或后续再由可选步骤提取正文）等。

| 参数 | 类型 | 说明 |
|------|------|------|
| `url` | string | 必填；仅允许 `https://`（默认）；若允许 `http://` 须单独环境变量开启 |
| `max_bytes` | int | 可选；覆盖全局默认；超过则截断并标记 `truncated` 或返回 `body_too_large`（策略二选一，须在实现中固定） |
| `follow_redirects` | bool | 默认 `true`；与 `max_redirects` 联用 |
| `accept` | string | 可选；`Accept` 头，影响部分站点的内容协商 |
| `headers` | object | 可选；string 键值附加请求头；**不得**包含 `User-Agent` |

**内容类型分支（多格式解析）**：

| `Content-Type`（前缀匹配即可） | 建议行为 |
|--------------------------------|----------|
| `text/plain` | 按 UTF-8 解码；非法 UTF-8 → `invalid_encoding` 或 `replacement` 策略（需文档化） |
| `text/markdown` | 同 plain；可选保留原始 Markdown 字符串交模型解析 |
| `application/json` | 解析为 JSON 对象/数组返回；解析失败 → `invalid_json` |
| `text/html` | 默认返回**原始 HTML 截断**或**可选** `extract_mode: main_text`（若实现正文提取，须注明启发式、非 100% 可靠） |
| 其它 | `binary_preview`（十六进制预览，上限字节）或 `unsupported_media_type` |

### 2.2 多文件场景（单次响应内含多段逻辑资源）

常见形态：

1. **JSON 数组/对象** 内嵌多个 `url` 或 Base64 片段：`web_fetch` 一次即可；由 **Agent 多步** 再对每个 URL 调用 `web_fetch`（推荐：单工具职责单一）。
2. **HTML 索引页** 含多个链接：可提供可选工具 **`web_fetch_links`**（或 `web_fetch` 的 `mode: extract_links`）返回规范化 URL 列表（限条数、同 host 策略），**禁止**默认全站递归。
3. **`multipart` 响应**：实现侧若暂不支持，应返回 `unsupported_media_type` 并建议在系统提示中改写为分文件 URL。

### 2.3 压缩包：`web_fetch_archive`（或 `web_fetch` + `archive: true`）

支持从 URL 下载 **`application/zip`**、**`application/gzip`**（单文件 `.gz`）、**`application/x-tar`**、**`application/x-gzip` 与 tar 组合（`.tar.gz` / `.tgz`）** 等（具体以实现声明为准）。

**硬要求（安全）**：

1. **解压炸弹**：限制 **压缩流读取字节数**、**解压后总文件数**、**解压后总字节数**、**单文件最大字节**。
2. **路径穿越（Zip Slip）**：每个成员路径规范化后**不得**以 `..` 逃出目标目录；拒绝绝对路径、盘符、`:` 等资源定位符。
3. **落盘位置**：
   - **推荐**：仅解压到 **`AGENT_FS_ROOT` 下指定子目录**（例如 `AGENT_WEB_EXTRACT_SUBDIR`，相对根），与 `fs_*` 的监禁一致；或
   - 解压到 **进程临时目录** 并返回**内存内文件表**（小规模）；大规模须落盘并配合 `fs_*`。
4. **多文件输出**：返回 **清单**（见下），每条含 `path`（相对监禁根或沙箱子目录）、`size`、`sha256`（可选）、`media_type`（若可嗅探）。

建议返回形态：

```json
{
  "url": "https://example.com/data.tgz",
  "files": [
    {"path": "manifest.json", "size": 1024, "preview_truncated": false},
    {"path": "readme.md", "size": 2048, "preview_truncated": false}
  ],
  "total_files": 12,
  "total_uncompressed_bytes": 1048576,
  "truncated_manifest": false
}
```

对 **`.txt` / `.md` / `.json`** 成员：可提供 **`include_text_preview`**（每文件预览字符上限）或要求 Agent 再用 `fs_read` 读取，以避免单次响应过大。

### 2.4 环境变量（建议）

| 变量 | 含义 | 未设置时默认 |
|------|------|----------------|
| `AGENT_WEB_ENABLE` | 非空且为真 → 注册 `web_*` | 未设置 → **不注册** |
| `AGENT_WEB_MAX_RESPONSE_BYTES` | 单次 HTTP 体上限 | `2097152`（示例） |
| `AGENT_WEB_MAX_REDIRECTS` | 重定向上限 | `10` |
| `AGENT_WEB_TIMEOUT_MS` | 连接+读超时 | `30000` |
| `AGENT_WEB_USER_AGENT` | User-Agent | 含项目名与联系用途的固定串 |
| `AGENT_WEB_ALLOW_HOSTS` | 逗号分隔 host 白名单；空表示**不启用白名单**（依赖 SSRF 黑名单实现） | 空 |
| `AGENT_WEB_DENY_NETWORKS` | 可选；CIDR 黑名单（私网、链路本地等） | 建议默认拒绝 RFC1918 等 |
| `AGENT_WEB_MAX_ARCHIVE_FILES` | 归档内文件数上限 | `1000` |
| `AGENT_WEB_MAX_ARCHIVE_UNCOMPRESSED_BYTES` | 解压后总字节上限 | `52428800`（示例） |
| `AGENT_WEB_MAX_ARCHIVE_SINGLE_FILE_BYTES` | 单成员上限 | `10485760`（示例） |

（**`web_search`** 专用变量见 **§1.5**；`web_fetch` 等其它出站能力仍使用上表及 SSRF 相关变量。）

---

## 3. RSS / Atom 订阅解析（`web_rss_feed`）

本节对齐 **`news_fetcher.py` 中 `NewsFetcher.fetch_rss` 的语义**（参考：`news/scripts/news_fetcher.py`：**feedparser 拉取订阅 → 遍历 `feed.entries` → 抽取 title/link/summary/时间 → 可选关键词过滤 → 输出结构化列表**）。内建工具不必复刻该文件中的 **API 源** 与 **BeautifulSoup 爬取**；RSS 部分应保持字段与过滤概念一致，便于 Agent 替换或补全该脚本中的 RSS 分支。

### 3.1 输入参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `feed_url` | string | RSS 或 Atom 的 **HTTPS URL**（与 `web_fetch` 共用 URL 校验与 SSRF 策略） |
| `max_entries` | int | 类似脚本中 `for entry in feed.entries[:15]`；硬顶可配置 |
| `max_age_hours` | int | 仅保留「发布时间」不早于现在减该小时数的条目；与脚本中 **72 小时**窗口同理（脚本：`timedelta(hours=72)`） |
| `keywords` | string[] | 可选；**大小写不敏感子串匹配**；匹配 `title + " " + summary` 的拼接文本；若为空 → **不做关键词过滤** |
| `skip_keyword_filter` | bool | 若为 `true`，忽略 `keywords`（与脚本 `skip_keyword_filter` 一致） |

### 3.2 输出条目字段（与脚本 `article` dict 对齐）

每条建议至少包含：

| 字段 | 说明 |
|------|------|
| `title` | `entry.get('title','')` |
| `url` | `entry.get('link','')` |
| `summary` | `entry.get('summary','')`（或 `description`，以解析库为准） |
| `published` | ISO8601 字符串；优先 `published_parsed`，否则 `updated_parsed`，再否则**省略或**由实现标记 `published_inferred` |
| `source_type` | 固定 `"rss"` |
| `source` | 可选；由调用方传入 `source_name`，或从 `feed.feed.title` 推断 |

可选扩展（若实现分类器）：`category` 字段可参考脚本 `categorize_article` 的规则（**gnss / meteorology / launch / …**），**不作为**互操作性硬要求。

### 3.3 错误与降级

- 拉取失败：HTTP 错误 → `http_error`；超时 → `timeout`。
- 解析失败：非 XML、畸形 Feed → `rss_parse_error`。
- 条目为空（过滤后）：返回 `entries: []` 且 **非错误**，与脚本打印后返回空列表行为一致。

### 3.4 合规与礼仪

- 对同一 `feed_url` 实施 **最小间隔**（例如每 host 每 N 秒一次），避免 Agent 循环高频刷源。
- 固定 **User-Agent**；尊重站点 `robots.txt` 仅当实现浏览器式爬虫时强相关；**仅 GET Feed URL** 时仍建议在运维文档中说明使用场景。

---

## AGENT_TOOL_ALLOWLIST

若设置 **`AGENT_TOOL_ALLOWLIST`**（逗号分隔），注册阶段须将所需 `web_*` 一并列入，例如：

`web_search,web_fetch,web_fetch_archive,web_rss_feed,web_configured_source`（外加仍需要的 `fs_*`、`run_skill_script` 等）。

---

## 工具一览（汇总）

| 名称 | 作用 |
|------|------|
| `web_search` | **DuckDuckGo HTML** 检索；返回结果列表（title/url/snippet），不默认抓全文 |
| `web_fetch` | 单 URL GET；按类型处理 plain / markdown / json / html（及二进制预览策略） |
| `web_fetch_archive` | 自 URL 下载 zip / tar.gz 等并在沙箱内解压；返回文件清单；防 Zip Slip 与解压炸弹 |
| `web_rss_feed` | 拉取并解析 RSS/Atom；支持 `max_entries`、时间窗、`keywords` 与 `skip_keyword_filter`（对齐 `news_fetcher.fetch_rss` 语义） |
| `web_configured_source` | 按 **`AGENT_NEWS_SOURCES_JSON`** 信源目录，用 **`kind` + `source_id`** 调用 `rss` / `api` / `scrape` 条目（内部映射 `web_rss_feed` / `web_fetch`） |

（若实现中将归档与单文件合并为带 `mode` 的单一工具，须在文档与 Tool 名称中**二选一并全局一致**，避免模型混名。）

---

## 常见错误码（`error.code`）

| code | 说明 |
|------|------|
| `url_disallowed` | scheme/host 不许可（含白名单/黑名单/SSRF） |
| `invalid_url` | URL 解析失败 |
| `timeout` | 连接或读超时 |
| `too_many_redirects` | 超过重定向上限 |
| `http_error` | 4xx/5xx（可附 `status`） |
| `body_too_large` | 超过 `max_bytes` / 全局响应上限 |
| `unsupported_media_type` | 当前工具未实现的 Content-Type 或归档格式 |
| `invalid_json` | `application/json` 解析失败 |
| `invalid_encoding` | 文本解码失败 |
| `archive_too_many_files` | 解压文件数超额 |
| `archive_uncompressed_too_large` | 解压总体积超额 |
| `archive_path_escape` | 疑似 Zip Slip / 非法成员路径 |
| `fs_root_required` | `web_fetch_archive` 需要有效 **`AGENT_FS_ROOT`** 沙箱（与 `fs_*` 同配置） |
| `fs_root_invalid` | `AGENT_FS_ROOT` 无法规范化为可用目录 |
| `rss_parse_error` | RSS/Atom 解析失败 |
| `search_provider_error` | DuckDuckGo 请求失败或无可交付结果（HTTP 错误、超时、上游不可用等） |
| `search_html_parse_error` | （可选）结果页 HTML 解析异常或与当前选择器不匹配 |
| `invalid_arguments` | 参数非法（如 `web_fetch.headers` 含 **`User-Agent`**，或非 string 头值） |
| `news_sources_invalid` | 信源目录 JSON 校验失败，或 `api` URL 拼接错误 |
| `unknown_source` | `web_configured_source` 中 **`kind` + `source_id`** 在目录中不存在 |
| `source_disabled` | 目录条目的 **`enabled`** 为 `false` |
| `news_sources_not_configured` | （仅理论）目录句柄缺失；正常未设置 **`AGENT_NEWS_SOURCES_JSON`** 时 **不注册** 本工具 |

---

## 测试

- 构建 **`test_web_tools`**（或并入现有测试目标）：对 **Mock HTTP 服务**（本机端口）验证：`web_fetch` 各类型、`web_rss_feed` 固定 XML fixture、`web_fetch_archive` Zip Slip 用例与体积上限。
- 构建 **`test_news_sources`**：信源目录 v1 解析与 **`web_configured_source`** + mock HTTP（`ctest -R news_sources`）。
- CI 默认 **无外网**；`web_search` 使用 **录制 HTML fixture**（黄金样例 + 正则/抽取断言）或标记 **`network` / manual**；避免 CI 依赖 DuckDuckGo 在线稳定性。
- 运行示例：`ctest -R web_tools`（具体名称以 `CMakeLists.txt` 为准）。

---

## 与 `news_fetcher.py` 的对应关系（摘要）

| `NewsFetcher` 能力 | 建议 `web_*` 映射 |
|--------------------|-------------------|
| `fetch_rss` | `web_rss_feed` |
| `fetch_api`（JSON HTTP） | 可用 `web_fetch` + JSON 分支；API Key 来自环境变量，**勿**硬编码脚本中的占位 Key |
| `fetch_scrape`（BeautifulSoup） | **不**纳入内建默认（复杂度高、合规敏感）；若需要，应独立 MCP 或独立服务 |

以上约定便于 Agent Framework 在**显式配置**的前提下安全暴露网络能力，并与现有 **`fs_*`** 文档风格一致，供实现与运维共同遵守。

---

## 外部信源目录（标准 JSON）

本节定义一份与旧版 `news_fetcher` **兼容意图**的**静态目录**：用 JSON 描述多个 RSS、REST 类 API、以及仅含入口 URL 的「抓取」站点（**不**承诺内建 HTML 结构化抽取）。

**运行时（v1）**：在 **`AGENT_WEB_ENABLE`**、OpenSSL 与 **`AGENT_NEWS_SOURCES_JSON`**（指向可读 JSON 文件）均满足时，`build_cli_agent_graph` 会注册 **`web_configured_source`**：模型传入 **`kind`**（`rss` / `api` / `scrape`）与 **`source_id`**（与文件中该节键名一致），框架将条目解析为 `feed_url` 或带 query/header 的 GET URL，再调用 **`web_rss_feed` / `web_fetch`**。未设置路径或加载失败时 **不注册** 该工具（`AGENT_LOG_LEVEL=debug` 可打日志）。

**`web_fetch` 补充**：可选参数 **`headers`**（对象为 string 键值；**禁止**传 `User-Agent`，与内置 UA 冲突）；与 **`accept`** 一并进入受控 GET 请求头。

### 示例文件路径

仓库内提供可复制的示例（**无密钥**）：

- **`agent_framework/data/news_sources.example.json`**

复制为私有文件（例如 `~/config/news_sources.json`）后再改 `enabled`、增删条目或在外层注入 HTTP 头。

### 顶层结构

| 字段 | 类型 | 说明 |
|------|------|------|
| `version` | number | 目录格式版本，便于以后迁移 |
| `description` | string | 可选；给人读的说明 |
| `rss` | object | 键为 **source_id**（字符串），值为条目对象 |
| `api` | object | 同上；REST/JSON HTTP 源 |
| `scrape` | object | 同上；仅声明列表页/根 URL，**语义见下** |

### `rss` 条目

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `url` | string | 是 | RSS 或 Atom 的 HTTPS（推荐）feed URL；对齐 `web_rss_feed.feed_url` |
| `enabled` | boolean | 否 | 默认 `true`；`false` 时加载方应跳过 |

**用法**：由 **`web_configured_source`** 代为解析；或直接调用 `web_rss_feed` 并传入解析后的 `feed_url`。

### `api` 条目

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `url` | string | 是 | JSON API 的基准 URL（无 query 或可带固定 path） |
| `enabled` | boolean | 否 | 默认 `true` |
| `headers` | object | 否 | 字符串键值 → 作为 `web_fetch` 无法直接表达的头；实现层需合并进 HTTP 客户端或扩展工具。**勿**在检入文件中写真实 API Key |
| `params` | object | 否 | 查询参数（字符串/数字等 JSON 类型）；实现层负责 `?key=value` 编码并与 `url` 合并 |

**用法**：**`web_configured_source`** 将 `params` 按键 **字典序** 拼为 query（`url` 已有 `?` 时用 `&` 追加）；`headers` 经 **`web_fetch`** 发出。密钥请放在 **私有 JSON** 或运维注入，勿提交仓库。

### `scrape` 条目

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `url` | string | 是 | 列表或栏目入口页 URL |
| `base_url` | string | 否 | 相对链接拼接用基址（与旧版字段对齐） |
| `enabled` | boolean | 否 | 默认 `true` |

**语义**：与内建能力对齐时，仅表示「允许对该 URL 做 **`web_fetch` 拉回 HTML（可截断）**」；**不**表示自带 DOM 解析或正文抽取。复杂抓取仍走 MCP / 外部服务。

### 运维与安全

- **密钥**：示例 JSON 中 `api.*.headers` 可为空对象；生产环境在私有副本或加载代码中写入头，并从 **`AGENT_*` / 密钥管理** 读取。
- **出站与 SSRF**：所有实际请求仍受 **`AGENT_WEB_*`**、SSRF 规则与 **`AGENT_TOOL_ALLOWLIST`** 约束。
- **合并策略（v1）**：仅 **`AGENT_NEWS_SOURCES_JSON` 单一文件**；多文件 / 与内置默认合并留待后续版本约定。

将本目录交给 LLM 时，可只注入 **source_id** 与简短说明，整条 JSON 不必进入 system prompt，避免上下文膨胀与误泄露。

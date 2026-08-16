当前实现里，模型**看不到完整资源表**，也**不会在需要时自动读文件**。不完善的 Codex skill 只能靠 `SKILL.md` 正文里的相对路径去猜，再调用工具；猜错或未声明就会失败。模型权重是另一条通道，本来就不会进对话上下文。

## 模型现在能看见什么

三层，一层比一层窄：

**L1 目录**（`AGENT_SKILL_INJECT_CATALOG=1`）只注入 `id + 截断 description`，没有路径、没有 kind、没有资源数明细。

```161:189:agent_framework/src/skills/skill_services.cpp
std::string format_skill_catalog_l1(...) {
    oss << "\n\n## Indexed skills ...\n";
    // "- cesium-skill: Build polished..."
}
```

**L2 正文**：用户话匹配到 skill 后，`load_instructions` 把 `SKILL.md` **去掉 frontmatter 的 Markdown** 塞进 `skill_prompt_cache`。Cesium 这类 skill 的「资源清单」其实就是正文里的句子，例如「用 `assets/templates/vite-starter/`」。

**L3 工具**：`Skill` 工具的 `list`/`search` 每条只给 `resources: <个数>`，个数来自 `resource_digests.size()`。未声明则为 **0**。`load` 会返回完整 `manifest` JSON；不完善 skill 的 `resources` 仍是空对象。`read_resource` 只认 **manifest 里的 `resource_id`**，不认磁盘路径。

因此：不完善 skill 被「加载成功」= 模型读到了说明书，**不是**读到了包内文件树。

## 「需要时自动读进去」——现在没有

没有「正文提到路径就打开」的挂钩。循环里匹配成功只做一件事：注入 `SKILL.md` 正文。`references/*.md`、`assets/templates/**` 都要模型自己再调：

- `Skill(action=read_resource, resource_id=...)` — 必须是已声明 id  
- `read_skill_resource(skill_id, relative_path, kind)` — 必须已声明且 kind 对得上  

未声明 → `resource is not declared`。目录 → 不是 regular file。`fs_*` 进不了 `~/.codex/skills`。

所以 Cesium 场景是：模型按说明书去读 `assets/templates/vite-starter/`，三条通道全拒，它只能说 sandbox 里没有。

## 不完善 skill 下，模型怎样才「正确理解有哪些资源」

现状只能靠说明书文字，不可靠。要在**不把 skill 根并进 `fs_*`** 的前提下补齐，建议把披露和读取都收在 skill 通道里：

| 阶段 | 给模型什么 | 谁触发 |
|---|---|---|
| 匹配成功（已有） | `SKILL.md` 正文 | 自动 |
| 匹配成功（建议补） | **包内资源索引**：path、推断/声明 kind、size，不含文件内容 | 自动，跟正文一起或紧随其后 |
| 模型要某一类 | `list_skill_resources(skill_id, kind=template)` | 模型调工具 |
| 模型要某个文件 | `read_skill_resource(..., kind=...)` | 模型调工具；kind 必须与推断/声明一致 |

「自动读进去」不要做成「把整个 `vite-starter` 塞进 context」。自动的应是 **索引**；内容仍按需、按 kind、按 `max_bytes`。否则一个模板树就会撑爆窗口。

索引怎么来（清单不全时）：

1. 声明优先（v1 `resources.*`）  
2. 包内扫描常规目录：`references/`、`assets/templates/`、`assets/`、`scripts/`、`models/` …  
3. 每条带 `source: declared | inferred`  
4. 扫描结果**只用于披露和只读**，不写回 manifest、不算 package digest  

这样不完善 skill 也能让模型看见「有哪些文件、各是什么类型」，再按类型去读。

## 最核心：模型文件现在怎么管、怎么加载

这里的 **Model** 不是对话用的 LLM，而是 skill 包里的 **权重/推理工件**（ONNX 等），类型为 `SkillResourceType::Model`。

**管理（登记，不自动进 prompt）**

v1 必须在 manifest 里声明，缺一项扫描会失败：

- `path`（包内相对路径）  
- `sha256` + 精确 `size`  
- `license` / `source`  
- `runtime`（如 `onnxruntime`）  
- `requirements.devices` / `precisions` / 可选内存  

索引里记 `resource_digests[id]`。`skill doctor` 查文件是否存在、主机是否具备该 runtime。`read_skill_resource` **故意不把 Model/Asset 放进文本 cache**，避免把权重当字符串喂给 LLM。

**加载（按需、只读、先准入）**

不是 AgentLoop 自动 mmap，而是 `SkillModelService`：

1. `open_snapshot(skill_id, resource_id)` — 必须已声明  
2. `check(host)` — runtime / device / precision / 内存 / 不可执行 / 不超过 `max_readonly_bytes`（默认 256MB）  
3. 通过后 `cache_->acquire_policy`：按 digest 进内容寻址缓存，可 pin  
4. 返回只读 handle（路径或 mmap），给 **推理 runtime**，不给 chat 上下文  

CLI 是 `skillctl model check`，`automaticExecution: false`。对话模型即使用 `Skill(read_resource)` 去读一个 Model，也只是截断字节，不能当「加载模型」。

不完善 skill **不会**因为磁盘上有 `models/*.onnx` 就被登记。没有 `resources.models` 就等于没有 Model 资源，doctor / `SkillModelService` 都看不见。

和对话 LLM 的关系：skill 匹配只影响 **提示词**；本地 ONNX 不会替换 `LLMClient`。要跑 skill 自带模型，需要单独的 runtime 绑定（当前是服务 + CLI，不是 ReAct 自动步）。

## 对照（Cesium vs 带模型的 v1 skill）

| | Codex 式 Cesium | 完善的 v1（含 Model） |
|---|---|---|
| 模型知道 skill 存在 | L1 目录 + 匹配后 L2 正文 | 同左 |
| 知道有哪些文件 | 仅正文里的路径字符串 | `load` 的 manifest + digest 列表 |
| 自动读 template/reference | 无 | 无（仍要工具） |
| 读未声明文件 | 失败 | 失败 |
| 模型权重 | 不管（也不是这类包） | 声明 → doctor → `SkillModelService.open` → cache/mmap |
| 权重进 LLM context | 不应发生 | 被禁止缓存为文本 |

## 建议（按优先级）

1. **匹配成功后自动注入包内资源索引**（声明 + 目录推断），让模型知道有什么、各是什么 kind。这是「正确理解」的最小补丁。  
2. **`read_skill_resource` / `Skill.read_resource` 按 path+kind 读包内任意正规文件**，未声明用推断 kind；kind 不一致则拒绝。这是「需要时读进去」。  
3. **加 `list_skill_resources`**，按 kind 列目录；不要自动灌整棵模板树。  
4. **Model 保持第三条通道**：manifest 声明 + digest + host 准入 + cache/mmap；索引里只出现 id/runtime/size，内容不进 prompt。不完善包若磁盘有 `models/`，索引可标 `inferred`，但 **open 仍要求声明齐全**（sha256/runtime/devices），避免随便 mmap 不明权重。  
5. 不要把 skill 根并进 `fs_*`。

一句话：现在管理的是 **SKILL.md 文本**，不是包内资源；模型文件只在 **声明完整的 v1 Model** 上按需打开给 runtime。要让不完善 skill 可用，先自动给索引、再按类型按需读文本资源；权重继续走声明与准入，不要自动读进对话。
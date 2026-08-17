# 任务档位分类：语言文字歧义与 LLM 决策方案

> 实施状态：可执行 v2 闭环计划维护于
> [`af-term-v2-plan.md`](af-term-v2-plan.md)。本文保留为需求与问题基线，
> 避免把设计意图误写成已验证实现。

**文档性质**：问题记录与后续实现约束（本文不修改源码）  
**对象**：`src/conversation/task_classifier.cpp`  
**关联**：`af-session-long-task-integration-plan.md` AF-SLT4（LLM TaskClassifier + deterministic policy gate）  
**日期**：2026-08-15

## 1. 结论

任务执行档位（`TaskExecutionProfile`）决定副作用范围、是否进入长任务 / production composition，以及后续 Harness 与验收深度。该类决策依赖**意图、极性、辖域和「提及 vs 祈使」**，不能用无上下文的子串命中完成。

根据当前实现，权威路径应是 **隔离的 LLM 分类器 + 系统性语言约束 prompt + 用户确认回退**。LLM 无法可靠定档时，Agent 必须停下来向用户提问，给出封闭的标准档位词，由用户原样选择，再经精确匹配验证后才写入 `request.profile`。确定性函数不得用关键词猜档或抬档。部署侧显式配置的 `runtime.task_profile` 仍可覆盖模型输出。

## 2. 现状与失效模式

`deterministic_task_classification`（约 L54–71）对小写后的全文做 `find`。命中「修改 / 实现 / 代码 / build / test」则 `CodeChange`，否则「生成 / 报告 / 图 / 文件」则 `ArtifactDelivery`，否则「部署 / 发送 / 删除 / 发布」则 `ExternalAction`，否则「验证 / 验收 / 专业 / 全面分析」则 `Professional`，否则「分析 / 研究」则 `ReadOnlyAnalysis`。命中后四档会打开 `long_running`。`rationale` 固定为 `deterministic safety fallback`，`confidence` 为 `0.55`。

该函数不区分祈使、否定、让步或转述，优先级为固定 if/else。默认档为 `Conversation`，但任意蹭词即离开对话档。`LLMTaskClassifier` 失败后，demo / TUI 会把该结果写入 `request.profile`（见 `examples/common/agent_example_bootstrap.hpp`）。生产路径在 `confidence < 0.70` 时 fail closed，因此 `0.55` 过不了生产门闩，但非生产路径会吞下误分类。

现有单测 `tests/test_task_classifier.cpp` 只覆盖正向句「生成分析报告和图片文件」，未覆盖否定或「提及但不执行」。

### 2.1 误判表


| 用户原话                   | 子串命中          | 错分结果                                |
| ---------------------- | ------------- | ----------------------------------- |
| 不要发布，先讨论方案             | `发布`          | `ExternalAction` + `long_running`   |
| 跳过生成，只看现有结果            | `生成`          | `ArtifactDelivery` + `long_running` |
| 这段代码是别人写的，解释一下         | `代码`（优先于「解释」） | `CodeChange`                        |
| 天津天气，不要改文件             | `文件`          | `ArtifactDelivery`                  |
| 先分析，先别实现               | `实现` 优先于 `分析` | `CodeChange`                        |
| 全面分析一下北京空气             | `全面分析`        | `Professional` + `long_running`     |
| please don't run tests | `test`        | `CodeChange`                        |


短词误伤：`图` 命中地图 / 试图 / 图书馆 / 天气图；`test` 命中 latest / contest / testament。

根据上述数据，该函数不适合作为 safety fallback：安全回退应停下向用户索取标准档位词并校验，而不是用关键词升级权限。

## 3. 为何档位决策应交给 LLM

`TaskExecutionProfile` 的六档语义是**所需工作、副作用、产物与验收深度**，不是词表标签。自然语言中同一词根可表示禁止、推迟、转述或请求。LLM 分类器已存在（`LLMTaskClassifier`），且与主对话隔离：`temperature=0`、`max_tokens=300`、禁止回答用户任务。缺的是把语言文字约束写成**可检验的系统 prompt**，以及把失败回退改成「提问 + 封闭词验证」，而不是静默猜档。

规则引擎若要覆盖否定窗、祈使模板、中英混写和多意图取安全档，词表与窗口常数会持续膨胀，且仍无法处理「先别发布，等验收通过后再说」这类跨句辖域。该类问题属于语义分类，应先交给 LLM；LLM 仍不确定时，准确性只能来自**用户对标准词的显式选择**，不能来自另一套 `contains`。

## 4. 解决方案（文档约束，待后续改码）



### 4.1 决策顺序（不可颠倒）

1. **部署覆盖**：`runtime.task_profile` 若不是 `Conversation`，沿用 `apply_task_routing_policy` 的现语义，confidence=1.0，不跑分类器。
2. **控制类输入**：状态查询 / 取消 / 暂停不跑档位分类（已有 `classify_task_input`）。
3. **LLM 分类**：对用户原文调用隔离分类器；只接受 schema 合法且 confidence 达标的结果。
4. **用户确认回退**：LLM 不可用、非 JSON、非法 profile、confidence 越界或低于门闩时，**不得**用关键词填档。进入第 4.3 节：提问 → 用户给出标准词 → 精确匹配验证。验证通过前保持 `Conversation`、`long_running=false`，不启动高副作用执行。
5. **生产门闩**：`ExecutionTrustProfile::Production` 且 confidence &lt; 0.70 时，同样走第 4.3 节提问，不得改走关键词升级，也不得在未确认时继续 production composition。



### 4.2 系统性分类 prompt（后续替换 `LLMTaskClassifier::classify` 的 system 文本）

分类器只输出一个 JSON 对象，不得使用 markdown 围栏，不得回答用户问题。字段与现契约一致：`profile`、`long_running`、`confidence`、`rationale`、`classifier_id`。

`profile` 仅允许：


| 值                    | 选用条件                                          |
| -------------------- | --------------------------------------------- |
| `conversation`       | 问答、解释、澄清、闲聊；或用户明确要求先不要做有副作用的事                 |
| `read_only_analysis` | 需要检索或推理，但本轮不要求写仓库、交文件、改外部系统                   |
| `artifact_delivery`  | 用户**要求交付**报告、图、文件等可保存产物                       |
| `code_change`        | 用户**要求修改或实现**本仓库 / 工作区代码，或运行针对该变更的 build/test |
| `external_action`    | 用户**要求**部署、发送、删除、发布等对外或不可逆操作                  |
| `professional`       | 用户**要求**按专业验收标准做验证 / 全面评审，而不是口语里的「分析一下」       |


系统提示必须显式约束下列语言文字问题（写入 prompt 正文，而不是实现注释）：

1. **极性**：`不要`、`别`、`禁止`、`跳过`、`无需`、`先不`、`暂不`、`don't`、`do not`、`skip`、`without` 修饰的动作视为未请求该动作。
2. **辖域**：否定或让步只作用于其修饰的动词短语。「不要发布，先写报告」不得标 `external_action`；若明确要求写报告，才可标 `artifact_delivery`。
3. **提及 ≠ 祈使**：「这段代码」「已有文件」「别人发布的版本」是谈论对象，不是本轮要执行的动作。
4. **多意图取安全档**：同时存在「不要做 X」与「先讨论 / 先分析」时，选 `conversation` 或 `read_only_analysis`，不得选被否定的高副作用档。
5. **短词与词中词**：禁止因「图」「文件」「test」「代码」等子串单独升级。英文按词边界理解，中文按动宾或祈使结构理解。
6. **语言混合**：中英、简繁、口语省略按同一套极性规则处理，不得只对中文或只对英文生效。
7. **不确定则降档**：无法判断是否真的要求副作用时，选更低副作用档，并降低 `confidence`（建议 ≤ 0.60）。
8. `long_running`：仅当用户要求的工作明显跨多步、需等待外部系统或需验收闭环时为 true；闲聊与单轮解释为 false。
9. `rationale`：用短句写清选用档位的依据，并点名被否定或仅被提及的动作（例如 `negated:发布; mention_only:代码`）。
10. `classifier_id`：固定 `llm-task-classifier-v2`（prompt 修订后升版本，便于审计）。

user 侧继续传 JSON：`{"input":"...","has_active_task":bool}`。若 `has_active_task` 为 true，分类的是**本条输入相对当前任务的增量**（继续、补充、收窄、取消某步），不是把整段历史重新立档。

### 4.3 用户确认回退（封闭词匹配）

LLM 不能定档时，Agent 必须向用户提出**单轮澄清**，列出推荐标准词，要求用户**原样回复其中一个词**（可另附一句说明，但档位只认该词）。系统对回复做规范化后与封闭词表精确匹配；匹配成功才写入 `TaskClassification.profile`，`confidence=1.0`，`classifier_id=user-profile-confirm-v1`，`rationale` 记录所选标准词与校验结果。

推荐标准词与枚举一一对应，提问时必须完整列出，并给出一句话推荐（按副作用从低到高）。用户只许选词，不许用自然语言再描述意图。

| 用户必须回复的标准词 | 对应 `profile` | 推荐说明（写入提问） |
|---|---|---|
| `conversation` | `Conversation` | 只问答或解释，不改仓库、不交文件、不动外部系统（默认推荐） |
| `read_only_analysis` | `ReadOnlyAnalysis` | 可以检索和分析，但不写文件、不改代码、不发布 |
| `artifact_delivery` | `ArtifactDelivery` | 需要生成并交付报告、图或文件 |
| `code_change` | `CodeChange` | 需要修改或实现本仓库代码，或为此跑 build/test |
| `external_action` | `ExternalAction` | 需要部署、发送、删除或发布等对外 / 不可逆操作 |
| `professional` | `Professional` | 需要按专业标准验收或全面评审 |

提问模板（实现时原文或等价翻译均可，标准词不得改写）：

```text
当前无法从你的表述可靠判断任务档位（分类器不可用或置信度不足）。
请回复下面六个词之一，不要改写、不要翻译、不要加前缀：

- conversation — 只讨论或解释（推荐，若你只是问答）
- read_only_analysis — 只分析、不改动
- artifact_delivery — 要交付报告/图/文件
- code_change — 要改或写代码
- external_action — 要发布/部署/发送/删除
- professional — 要按专业标准验收

回复示例：conversation
```

匹配与验证规则：

1. 去掉首尾空白，英文小写；若整句仅含一个标准词，或标准词出现在行首且其后仅为空白 / 标点，则接受。
2. 命中且仅命中封闭表中的**一个**词 → 校验通过，写入对应 profile。`long_running` 仅在所选为 `artifact_delivery` / `code_change` / `external_action` / `professional` 时为 true。
3. 空回复、多个标准词、近义改写（如「随便聊聊」「发布一下」）、或标准词被否定（如 `不要 external_action`）→ 校验失败，再问一轮，最多 3 次。
4. 3 次仍失败：保持 `Conversation`，`error=profile_confirm_unmatched`，不升档，不进入长任务链。
5. 验证通过前，Turn 处于等待用户输入（`AwaitingInput` / 等价 HITL），不得调用会升档的执行入口。

允许：提问、展示上表、精确匹配、校验失败再问、超时或超次后停在 `Conversation`。

禁止：用 `contains("发布")` 等词表猜档；把未校验的自由文本当作 profile；在提问未完成时打开 `long_running` 或启动 production composition；把 `0.55` 当作「已分类」。

只读启发式若仍保留，仅可写入日志，**不得**写入 `TaskClassification.profile`。

### 4.4 评测与单测（改码时一并落地）

最低负例（期望均为 `conversation` 或 `read_only_analysis`，且 `long_running=false`）：

- 不要发布，先讨论方案
- 跳过生成，只看现有结果
- 这段代码是别人写的，解释一下
- 天津天气，不要改文件
- 先分析，先别实现
- please don't run tests
- 天气图长什么样

最低正例（保持现有正向语义）：

- 生成分析报告和图片文件 → `artifact_delivery`
- 把登录接口实现并补测试 → `code_change`
- 发布到生产并通知值班 → `external_action`

回退确认评测（期望精确匹配，不跑关键词猜档）：

- 分类器失败后必须出现含六词封闭表的提问，且本轮不升档
- 用户回复 `conversation` / `code_change` 等标准词 → 对应 profile，`confidence=1.0`
- 用户回复「不要发布」「生成报告」→ 不匹配，再次提问
- 连续 3 次非标准词 → `error=profile_confirm_unmatched`，停在 `Conversation`

生产评测另计：同一批句子上 LLM 分类器的档位准确率、否定句漏升档率、回退路径零擅自升档、以及标准词一次匹配成功率。

## 5. 与现实现的差距


| 项                 | 当前                          | 本文要求                  |
| ----------------- | --------------------------- | --------------------- |
| LLM system prompt | 仅要求按 work / side effects 分类 | 第 4.2 节十条语言约束 + v2 id |
| 失败回退 | 子串升档，`confidence=0.55` | 提问 + 六词封闭表 + 精确匹配；未确认前保持 `Conversation` |
| demo / TUI | 回退结果直接写入 `request.profile` | 未通过标准词校验不得写入高副作用档 |
| 单测                | 仅正向「生成…文件」                  | 第 4.4 节负例必过           |


在源码按第 4 节替换之前，非生产路径上「不要发布 / 跳过生成」仍会错分。运维侧可把 `AGENT` 任务档显式配成 `Conversation`，避免回退升档进入长任务链。

## 6. 验收口径

文档层面：档位决策的权威顺序、prompt 约束、以及「提问 → 标准词 → 匹配验证」回退已写清，并与 AF-SLT4 对齐。

实现层面（后续单独改码，不在本文提交）：`LLMTaskClassifier` 使用第 4.2 节 prompt；`deterministic_task_classification` 不再升档；分类失败进入第 4.3 节 HITL 确认；`test_task_classifier` 覆盖第 4.4 节负例与标准词匹配；生产低置信度改为提问，不得关键词升档。

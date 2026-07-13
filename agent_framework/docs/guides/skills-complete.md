如果你说的 **Agent Skills（类似 Claude Skills / OpenAI Agent Skills / Codex Skills 体系）**，那么一个完整的 skill 本质上不是一个脚本，而是一个**可被 Agent 加载、理解、调用、执行的能力封装单元**。除了你提到的：

- `script`
- `cli`
- `mcp`
- `reference`

通常还会包含下面这些资源类型。

---

## 1. Skill 元数据（Metadata）

这是 Skill 的入口描述，告诉 Agent：

- 这个能力是什么
- 什么时候应该调用
- 输入输出是什么
- 依赖什么资源

典型：

```
skill/
├── SKILL.md
├── manifest.yaml
└── config.json
```

例如：

```yaml
name: gnss-weather-analysis
version: 1.0
description: GNSS掩星数据质量控制与大气反演分析
triggers:
  - radio occultation
  - GNSS RO
  - bending angle
resources:
  - scripts
  - references
  - tools
```

类似软件包的 `package.json`。

---

# Skill 常见资源分类

一个成熟 Skill 通常包含：

```
skill_name/
│
├── SKILL.md              # 核心说明
├── manifest.yaml         # 元信息
│
├── scripts/              # 自动执行代码
├── tools/                # 工具接口
├── mcp/                  # 外部能力连接
├── cli/                  # 命令行能力
├── references/           # 知识资料
├── templates/            # 输出模板
├── examples/             # 示例
├── schemas/              # 数据结构定义
├── tests/                # 测试
├── assets/               # 静态资源
├── prompts/              # 子提示词
├── workflows/            # 工作流
└── configs/              # 配置
```

---

# 2. Scripts（执行资源）

你已经提到。

用于：

- 数据处理
- 自动计算
- 文件生成
- 调用算法


例如：

```
scripts/
├── preprocess.py
├── train_model.py
├── convert.py
└── validate.py
```

特点：

Agent 决策：

> "我要调用这个能力"

然后：

```
LLM
 |
 | tool call
 v
script.py
 |
 v
result
```


---

# 3. CLI（命令行能力）

CLI 是 Script 的工程化包装。

例如：

```
gnss-ro-qc \
   --input atm.nc \
   --output qc.nc
```


优势：

- 可以脱离 Agent 使用
- 容易测试
- 可以组合


例如：

```
Skill
 |
 +-- cli
      |
      +-- eccodes
      +-- gdal
      +-- wgrib2
      +-- pytorch
```


对于科研 Agent 非常重要。

例如：

```
ERA5 Skill

CLI:
    download-era5
    interpolate-era5
    compare-observation
```

---

# 4. MCP（Model Context Protocol）

MCP 是连接外部世界的标准接口。

例如：

```
Agent
 |
 MCP Client
 |
 +-------------+
 |             |
Database     API
 |             |
PostGIS      ECMWF
```


典型：

```
mcp/
├── server.py
├── tools.json
└── schemas/
```


适合：

### 数据源

- 数据库
- 云存储
- API
- 企业系统


例如：

GNSS Skill:

```
MCP servers:

gnss_obs_server
    query_RO_profiles()

ecmwf_server
    get_forecast()

satellite_server
    get_orbit()
```


---

# 5. References（知识库）

你提到的 reference。

它不是执行资源，而是**知识增强资源**。

例如：

```
references/

├── WMO_ROQC.pdf
├── ECMWF_ROMEX.pdf
├── ROPP_manual.pdf
├── equations.md
└── papers/
```


Agent 可以：

```
Question
 |
 Retrieval
 |
 Reference
 |
 Answer
```

类似 RAG。


---

# 6. Templates（模板资源）

非常重要，但很多 Skill 没有。


用于固定输出格式。


例如：

```
templates/

weather_report.md

paper_review.md

technical_report.docx
```


Agent：

输入：

```
分析2026年GNSS RO论文
```

输出：

套模板：

```
Title
Abstract
Methods
Results
Implications
```

---

# 7. Schemas（数据结构）

用于约束 Agent 输出。


例如：

```
schemas/

forecast.json

{
 "time":"",
 "lat":"",
 "lon":"",
 "tec":[],
 "uncertainty":""
}
```


作用：

避免 Agent 输出自由文本。


特别适合：

- 工作流
- 自动评估
- 多 Agent 协作


---

# 8. Workflows（流程）

这是 Agent Framework 中非常关键的一类。


例如：

```
workflow/

gnss_ro_processing.yaml
```


内容：

```yaml
steps:

- download:
    skill: data_access

- qc:
    skill: ro_quality_control

- inversion:
    skill: atm_retrieval

- assimilation:
    skill: nwp_assimilation
```


对应：

```
TaskFlow
    |
 Workflow
    |
 Agent
    |
 Skill
```

这与你现在开发的 taskflow → workflow → agent framework 非常相关。


---

# 9. Prompts（子提示词）

把复杂能力拆分。


例如：

```
prompts/

planner.md

reviewer.md

scientist.md
```


Agent 可以：

```
main agent

   |
   +-- planner prompt

   +-- analyst prompt

   +-- verifier prompt
```

类似多角色。


---

# 10. Assets（静态资源）

包括：

- 图片
- 模型
- 字典
- LUT
- 权重


例如：

```
assets/

├── coastline.geojson
├── ionosphere_model.dat
├── tokenizer.model
└── font.ttf
```


---

# 11. Tests（验证资源）

工业级 Skill 必须有。


例如：

```
tests/

test_download.py

test_qc.py

test_output_schema.py
```


用于：

```
Skill CI/CD

commit
 |
 test
 |
 deploy
```

---

# 12. Models（模型资源）

现在越来越重要。


例如：

```
models/

├── tec_forecaster.pt
├── embedding_model.onnx
└── llm_adapter/
```


Agent Skill 可以携带：

- 小模型
- LoRA
- classifier
- embedding


例如：

```
GNSS anomaly detection skill

Agent
 |
 Skill
 |
 ONNX model
 |
 TEC anomaly score
```

---

# 综合结构示例

一个科研 Agent Skill：

```
gnss_ro_skill/

SKILL.md

manifest.yaml


scripts/
    qc.py
    inversion.py


cli/
    ro_process


mcp/
    ecmwf_server
    gnss_database


references/
    WMO.pdf
    ECMWF_notes.md


schemas/
    occultation.json


templates/
    report.md


workflows/
    full_processing.yaml


models/
    multipath_detector.onnx


tests/
    test_pipeline.py
```

---

# 从 Agent Framework 角度看

如果映射到你正在做的 TaskFlow → Workflow → Agent Framework：

```
                 TaskFlow

                    |
              Workflow Engine

                    |
        +-----------+------------+
        |                        |
      Agent                  Agent
        |
     Skill Layer
        |
+-------+-------+-------+-------+
|       |       |       |       |
Tool   MCP    CLI   Script  RAG
API                  |
                 Model
```


更进一步：

未来企业级 Agent 平台通常会把 Skill 定义为：

> **Skill = Metadata + Capability + Knowledge + Execution + Validation**

也就是：

| 类型 | 作用 |
|-|-|
| metadata | 告诉 Agent 是什么 |
| prompt | 告诉 Agent 怎么思考 |
| reference | 提供知识 |
| script | 执行动作 |
| CLI | 工程接口 |
| MCP | 外部连接 |
| workflow | 编排 |
| schema | 约束 |
| model | 专用智能 |
| test | 保证可靠性 |


对于你目前的 **TaskFlow + Agent Framework**，我认为最值得重点设计的是：

1. **Skill Manifest 标准**
2. **Skill 生命周期管理**
3. **Skill 与 Workflow DAG 的映射**
4. **Skill 的输入输出 Schema**
5. **Skill Registry（类似 Docker Hub / Python Package Index）**

这几个部分决定后续 Agent 系统能否从 Demo 走向工程平台。
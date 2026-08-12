# Agent Framework 文档

本目录包含 Agent Framework 的完整文档。

## 目录结构

- **api/**：API 参考文档（使用 Doxygen 生成）
- **guides/**：使用指南和教程
- **architecture/**：架构设计和设计文档

## 主要文档

- **分阶段实施规划（深度）**：[guides/plan-detailed.md](guides/plan-detailed.md)
- **Skills 与 Harness（渐进式披露、SkillHarness 概念）**：[guides/skills.md](guides/skills.md)
- **内建网络工具（搜索 / 抓取 / 归档 / RSS）**：[guides/builtin-web-tools.md](guides/builtin-web-tools.md)
- **内建本地 fs 工具**：[guides/builtin-fs-tools.md](guides/builtin-fs-tools.md)
- 设计文档：`../readme/guide_agent.md`（在父目录的 readme 文件夹中；长文见根目录 `readme/guide_agent.v3.md`）
- Taskflow workflow 文档：`../../workflow/README.md`
- Phase 4 Cognition / Observability Closure：[guides/phase-4-cognition-observability-closure.md](guides/phase-4-cognition-observability-closure.md)

## 生成 API 文档

使用 Doxygen 生成 API 文档：

```bash
doxygen Doxyfile
持续追踪 '/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-status.md' '/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-cognition-observability-closure.md' '/home/Mapoet/projects/taskflow/agent_framework/docs/guides/phase-4-residual-closure-plan.md' 等文件中实现短板 ,继续进行下一阶段,进行结合已有状态进行系统规划,全覆盖功能实现,以及完备性且系统性测试
```

生成的文档将在 `api/html/` 目录中。

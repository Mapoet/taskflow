# WP4.5：OpenTelemetry、SLO 与证据化运行观测

**优先级**：P1  
**依赖**：稳定 Run/Plan/Evidence/MemorySnapshot/View identity  
**下游**：WP4.1、WP4.6、WP4.7、WP4.8

## 1. 目标

保留现有 Audit 的安全事件语义，同时建立标准 trace/metric/log。Audit 是不可否认的业务记录，Telemetry 是可采样的运行观测，二者不得混为同一可靠性等级。

## 2. 可执行任务

| ID | 任务 | 完成条件 |
|---|---|---|
| 4.5.1 | Telemetry contract | trace/span/metric/log、resource、correlation、redaction 和 sampling |
| 4.5.2 | Span taxonomy | intake/memory.retrieve-select-view-promote/investigate/plan/node/LLM/tool/MCP/sandbox/verify/approval |
| 4.5.3 | Context propagation | local thread/future、ChildTask、A2A、MCP、sandbox、SSE |
| 4.5.4 | Audit bridge | AuditEvent ↔ span link，审计失败/遥测失败互不改变业务语义 |
| 4.5.5 | OTLP exporter | gRPC/HTTP 可选、batch、backpressure、shutdown flush、loopback test |
| 4.5.6 | GenAI/tool metrics | tokens、cost、cache、tool success、retries、queue/approval delay |
| 4.5.7 | Run/memory metrics | checkpoint latency/size、recovery、replan、view build/cache、conflict、promotion、finding、acceptance outcome |
| 4.5.8 | Sandbox metrics | startup、CPU/RAM/PID/I/O/network、kill/cleanup |
| 4.5.9 | SLO registry | availability、latency、recovery、false completion、security budgets |
| 4.5.10 | Dashboard/alerts | run critical path、error budget、backlog、provider/model/capability revision |
| 4.5.11 | Cardinality/privacy gate | 禁止 prompt/secret/raw path 作为标签；高基数 digest 使用受控字段 |
| 4.5.12 | Trace E2E | 一次任务跨 cognition/execution/assurance/A2A/sandbox 单 trace 查询 |

## 3. 五层验收

模块验证 exporter/backpressure/redaction；集成验证跨线程/进程/A2A context；综合验证 trace 与 evidence/artifact/report 互相回链；指标验证 exporter 开销、丢弃率、cardinality、SLO 计算准确性。

## 4. DoD 与回滚

能按 task/plan/node/evidence/artifact/memory snapshot/view/capability revision 查询完整 critical path、成本和失败；能够解释记录为何被选择、排除、裁剪或晋升，但不得将正文、Prompt、secret 或个人数据用作标签。OTel 编译与运行均可选；关闭 exporter 不影响 Audit 和执行。语义约定升级支持版本/dual emit。

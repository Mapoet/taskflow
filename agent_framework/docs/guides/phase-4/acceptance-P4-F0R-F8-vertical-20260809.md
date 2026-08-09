# P4-F0R–P4-F8 首轮纵向实现验收报告

**Run**：`P4-F0R-F8-VERTICAL-20260809`  
**Plan revision**：`plan-v3`  
**日期/环境**：2026-08-09，Linux x86_64，GCC 11.4，CMake 3.29，Debug，SQLite WAL/FULL  
**决策**：`partial`——本批离线纵向目标 accepted；Phase 4 总体及真实 Live/HA 未验收。

## 1. 本批验收边界

本报告验收共享契约、持久 Run/Memory、动态 Memory View、计划/验收核心、runtime/telemetry/eval/live-certification/distributed 参考语义以及跨模块 digest 绑定。它不验收真实 LLM/IdP/MCP/A2A、容器/远程 sandbox、OTLP backend、远程 durable queue、PostgreSQL/object store、HA 或 UI；这些能力没有被 skip-as-pass。

## 2. 代码证据

- `include/agent/{contracts,planning,assurance,run,approval,memory_v2,sandbox,telemetry,eval,live,distributed}`；
- `src/{contracts,planning,assurance,run,approval,memory_v2,sandbox,telemetry,eval,live,distributed}`；
- `tests/test_phase4_*.cpp` 与 `tests/fixtures/phase4`；
- `test_phase4_vertical.cpp` 验证 authoritative Memory、Planning/Verification View、Plan digest、Run checkpoint、五层证据裁决和重启终态绑定；
- CI 增加不可静默跳过的 `phase4-offline` 数量门禁。

## 3. 实测命令与结果

```bash
cmake -S . -B /tmp/taskflow-phase4-f0r-make.2XyicC \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DAGENT_BUILD_EXAMPLES=OFF -DAGENT_BUILD_FAISS=OFF \
  -DAGENT_BUILD_IMGUI=OFF -DAGENT_BUILD_RICH_RENDERERS=OFF \
  -DAGENT_BUILD_TUI=OFF -DAGENT_BUILD_WEB_UI=OFF
cmake --build /tmp/taskflow-phase4-f0r-make.2XyicC \
  --target test_phase4_vertical test_phase4_live_distributed \
           test_phase4_runtime_observability -j2
ctest --test-dir /tmp/taskflow-phase4-f0r-make.2XyicC \
  -L phase4-offline --output-on-failure
```

结果：`11/11 PASS`，0 failed；包含 contract、durable、policy、memory、cognition、assurance、runtime/telemetry、eval、live/distributed contract 和 vertical integration。

回归结果：`phase3-offline 14/14 PASS`。首轮 Phase 4 与 Phase 3 合计 25 项确定性回归均通过。

## 4. 五层判断

| 层 | 证据 | 结果 |
|---|---|---|
| 功能 | 契约解析、CAS、View、PDP、裁决、queue 行为 | PASS（本批离线边界） |
| 模块 | 11 个 Phase 4 CTest | PASS |
| 集成 | Memory→Plan→Run→Assurance→restart | PASS |
| 综合 | Phase 3 14 项回归与 Phase 4 11 项共存 | PASS（本地 Debug） |
| 指标 | 测试数必须 ≥11、失败数=0、required live skip 判失败 | PASS（离线门禁）；真实 SLO 未验收 |

## 5. 修复发现

- SQLite Memory helper 与 `std::bind` 名称解析冲突导致参数未绑定；已改为显式 `bind_text` 并验证 scope/CAS/restart；
- Telemetry `dropped` 计数改为 relaxed atomic，消除并发数据竞争；
- queue 在租约过期且重试耗尽时改为直接进入 dead letter，防止超限再次认领；
- `fdatasync/getpid` 改为平台抽象，Windows/macOS 编译边界已修复；远程 macOS/Windows CI 因未 push 尚无运行证据。

## 6. 残余风险与下一批

1. Evidence/Plan/Approval/AcceptanceReport 尚缺统一持久 store 与事务绑定；
2. Cognition 尚缺真实 ToolBus/外部 investigator 和 GraphExecutor/replan 接入；
3. Assurance 尚缺真实 artifact/domain/security/metric adapter 与 remediation 闭环；
4. Sandbox/OTel/Eval 仍是内核/契约，不是完整生产 backend；
5. Live 只有 certification contract，未执行真实外部矩阵；
6. Distributed queue 明确为进程内参考实现，不具备远程 durability 或 HA；
7. UI 未改动，因此本批不产生 UI 截图；后续 UI 批次必须实际运行并截图验收。

下一批依照总体计划第 12 节按 P4-F2D→P4-F3I→P4-F4A→P4-F5P→P4-F6E→P4-F7L→P4-F8D→P4-FUI 推进。

# AF-TGUI v3 Residual Closure Implementation Plan

**Goal:** Close every still-open AF-TGUI v3 requirement with revision-aware production code and reproducible Offline, ProcessLive, ProviderLive and BrowserLive evidence.

**Evidence rule:** A skipped or unavailable test is never a pass. ProviderLive remains `NotCertified` until real provider and MCP endpoints are configured. UI completion requires screenshots from the running formal Workbench.

## RC0 — Freeze the residual contract

- Maintain the authoritative requirement-to-source-to-test matrix in `docs/af-task-semantics-traceability.md`.
- Record evidence level, command, artifact digest and blocker for each open checkbox.
- Preserve unrelated working-tree changes and update checkboxes only after their gates pass.

## RC1 — Ship the formal Workbench composition

- Extract the `/api/v1` HTTP registration, authenticated subject resolution and React distribution mounting into a reusable runtime composition.
- Make the shipped Web demo serve the formal React Workbench by default; retain the static legacy client only at an explicit compatibility path.
- Reuse the same composition from BrowserLive tests so test and product routing cannot drift.
- Verify Session creation/listing, Run start/steer/cancel, Decision answer, projections, evidence and restart behavior.

## RC2 — Retire legacy write authority

- Remove fixed `default` Session writes from the legacy client and server.
- Convert `/ui/run`, `/ui/cancel` and `/ui/operations/hitl` into explicit `410 Gone` migration responses naming the canonical `/api/v1` operation.
- Keep only read-compatible legacy projections during the observation window.
- Add a source/API boundary test that rejects new legacy mutations.

## RC3 — Calibrate task semantics and planning

- Add a typed campaign case/report API around `TaskSemanticCalibrationMetrics`.
- Execute the locked multilingual corpus through the actual classifier and planning policy.
- Measure false high-effect routing, missed planning, unnecessary planning, unnecessary clarification and controlled decision abandonment.
- Emit a text-free aggregate JSON evidence report with policy/model/corpus digests and fail-closed thresholds.

Offline release gates are zero false-high-effect and missed-planning cases, with no regression against the locked oracle. ProviderLive gates are reported separately and never replace deterministic safety gates.

## RC4 — Unify the fault matrix

- Extend recovery certification to name crash, busy, disk/write failure, incompatible schema, provider disconnect/reattach, MCP disconnect/late result, receipt loss/reconciliation and client reconnect.
- Bind existing repository and ProcessLive tests to canonical cells and add missing deterministic injections.
- Require an evidence digest for every executed cell; missing, skipped or failed cells remain blockers.

## RC5 — ProviderLive certification

- Add a provider-neutral runner that consumes environment configuration without logging secrets.
- Run semantic/planning cases through the real LLM adapter and recovery cases through a real MCP tool endpoint.
- Record provider/model/deployment/MCP capability digests, latency and failure classification.
- When configuration is absent, write `NotCertified` evidence and leave the associated plan item open.

## RC6 — Accessible bilingual Workbench

- Add typed English and Simplified Chinese resources, locale selection/persistence and synchronized document language.
- Implement tablist/tab/tabpanel semantics, current-Session indication, controlled mobile navigation, visible focus, status/error live regions and grouped Decision choices.
- Add axe-based WCAG 2.2 AA assertions, keyboard-only workflows and bilingual overflow tests.

## RC7 — Browser and visual certification

- Install Playwright WebKit host dependencies approved by the user.
- Run connected Chromium, Firefox and WebKit against the real SQLite Workbench runtime.
- Exercise desktop, tablet and mobile viewports and capture deterministic screenshots.
- Inspect screenshots for hierarchy, clipping, responsive navigation, interaction affordance and bilingual content.

## RC8 — Release closure

- Run C++ unit/repository/API/concurrency/restart/replay/projection/security suites in Release with assertions enabled.
- Run ProcessLive, single-node PostgreSQL, BrowserLive and, when configured, ProviderLive suites.
- Run the production Web build, TypeScript checks, axe checks and `git diff --check`.
- Update `docs/af-task-semantics-gui-v3-plan.md` and traceability with commands, evidence paths, digests and remaining external blockers.

## Required verification commands

```bash
cmake --build /home/Mapoet/projects/taskflow/build-ui --target af_phase4_offline_tests -j2
ctest --test-dir /home/Mapoet/projects/taskflow/build-ui/agent_framework --output-on-failure
cmake --build /home/Mapoet/projects/taskflow/build-ui --target workbench_runtime_server web_ui_demo -j2
cd /home/Mapoet/projects/taskflow/agent_framework/web && npm run build
cd /home/Mapoet/projects/taskflow/agent_framework/web && npx playwright test --project=chromium --project=firefox --project=webkit
git -C /home/Mapoet/projects/taskflow/agent_framework diff --check
```

ProviderLive is run only after `OPENAI_API_KEY` or the configured provider credential plus `AGENT_MCP_ENDPOINT` are present. Credentials must not be copied into reports, logs or documentation.

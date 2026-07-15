# Stage 7 Skill Resource Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Execute this plan task-by-task with explicit review checkpoints. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add offline-first, content-addressed and budgeted management for Reference, Asset and Model resources without allowing large binaries into prompts or automatically executing model packages.

**Architecture:** Extend Manifest v1 with auditable resource metadata, then introduce four focused services: snapshot-safe resource access, a content-addressed disk cache, paged/indexed references, and non-executing model admission. Stage 5 package leases and digests remain the source of identity; Stage 8 retains remote Registry, signature and concrete network-provider responsibilities.

**Tech Stack:** C++20, `std::filesystem`, nlohmann/json, OpenSSL SHA-256 when enabled, CMake/CTest, Linux read-only `mmap` with stream fallback on other platforms.

## Global Constraints

- Preserve legacy v0 and existing Manifest v1 behavior unless Stage 7 fields are present.
- Do not add a vector database, embedding model, HTTP downloader or third-party archive dependency.
- `source` is audit metadata in Stage 7 and must never trigger network access.
- Asset and Model resources must not enter the prompt or the existing unbounded string cache.
- Explicit `mmap` is supported on Linux; unsupported platforms return a stable error. `auto` may fall back to streaming.
- Every file path is package-jail relative, canonicalized and required to resolve to a regular file.
- Every cache/import write is temporary, bounded, digest-verified and atomically published.
- Model handling returns metadata and read-only handles only; it must not call `exec`, `dlopen`, installers or model initialization code.
- Stage 8 continues to own deterministic archives, signatures, trusted publishers, SBOM and remote Registry protocols.

---

## File Map

- `agent_framework/include/agent/skill_resource.hpp`: Manifest resource contract types.
- `agent_framework/include/agent/skill_resource_access.hpp`: snapshot-pinned file/range/stream/mmap access.
- `agent_framework/include/agent/skill_resource_cache.hpp`: cache objects, leases, quota and recovery.
- `agent_framework/include/agent/skill_reference.hpp`: paging, citation, index and search API.
- `agent_framework/include/agent/skill_artifact_import.hpp`: bounded stream/archive-entry import policy.
- `agent_framework/include/agent/skill_model.hpp`: host capability and model admission API.
- Corresponding `agent_framework/src/skills/*.cpp`: implementations.
- `agent_framework/schemas/skill-manifest-v1.schema.json`: Stage 7 schema contract.
- `agent_framework/src/skills/skill_manifest.cpp`: parser/serializer.
- `agent_framework/src/skills/skill_manifest_validate.cpp`: filesystem and metadata validation.
- `agent_framework/src/skills/skill_loader.cpp`: bounded compatibility materialization only.
- `agent_framework/src/skills/skill_services.cpp`: resource service construction.
- `agent_framework/src/skills/skill_doctor.cpp`: offline readiness diagnostics.
- `agent_framework/src/skills/skill_command.cpp`, `agent_framework/tools/skillctl.cpp`: Stage 7 CLI.
- `agent_framework/tests/test_skill_resource_*.cpp`, `test_skill_reference.cpp`, `test_skill_artifact_import.cpp`, `test_skill_model.cpp`: focused contracts.

---

### Task 0: Persist the Approved Stage 7 Plan

**Files:**
- Create: `docs/superpowers/plans/2026-07-15-skill-resource-stage7.md`

**Interfaces:**
- Consumes: approved Stage 7 scope and Stage 1-6 implementation evidence.
- Produces: the tracked execution checklist used by Tasks 1-10.

- [ ] **Step 1: Add this plan document**

- [ ] **Step 2: Verify formatting and scope**

Run separately: `git diff --check`, then `rg -n "Stage 8|mmap|lexical-v1|network" docs/superpowers/plans/2026-07-15-skill-resource-stage7.md`

Expected: no whitespace errors and all Stage 7 boundaries are explicit.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/plans/2026-07-15-skill-resource-stage7.md
git commit -m "docs: plan stage 7 skill resource management"
```

### Task 1: Freeze Stage 7 Manifest and Resource Contracts

**Files:**
- Modify: `agent_framework/include/agent/skill_resource.hpp`
- Modify: `agent_framework/schemas/skill-manifest-v1.schema.json`
- Modify: `agent_framework/src/skills/skill_manifest.cpp`
- Modify: `agent_framework/src/skills/skill_manifest_validate.cpp`
- Modify: `agent_framework/tests/test_skill_manifest_v1.cpp`
- Create: `agent_framework/tests/test_skill_resource_contract.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: `SkillResourceDescriptor`, Manifest v1 parser and validator.
- Produces: `SkillResourceReadMode`, `SkillCachePolicy`, `SkillCitationMetadata`, `SkillReferenceIndexConfig`, `SkillModelRequirements`, parse/to-string helpers and stable validation codes.

- [ ] **Step 1: Write failing contract tests**

Cover exact `size`, per-resource `license`, `source`, `read-mode`, cache policy, citation, `lexical-v1`, model devices/precisions/minimum memory and their JSON round trip. Add negative cases for missing Asset/Model audit fields, invalid enum/URI, size mismatch, overflow and executable Model declarations.

- [ ] **Step 2: Confirm the tests fail before implementation**

Run: `cmake --build build-stage6 --target test_skill_resource_contract -j8`

Expected: target or Stage 7 types are missing.

- [ ] **Step 3: Implement types, schema, parser and validation**

Use these exact YAML keys:

```yaml
read-mode: auto|text|binary|stream|mmap
size: 0
license: Apache-2.0
source: package://models/model.bin
cache-policy: no-store|on-demand|pin
citation: {title: title, authors: [author], published: "2026", url: https://example, locator: section}
index: {kind: lexical-v1}
requirements: {devices: [cpu], precisions: [fp32], min-memory-bytes: 0}
```

For Manifest v1, require `sha256`, exact `size`, `license` and `source` on Asset/Model. Do not impose the new requirements on legacy v0.

- [ ] **Step 4: Run focused and prior Manifest tests**

Run: `ctest --test-dir build-stage6 -R 'skill_(resource_contract|manifest_v1_contract)' --output-on-failure`

Expected: 2/2 passed.

- [ ] **Step 5: Commit**

```bash
git add agent_framework/include/agent/skill_resource.hpp agent_framework/schemas/skill-manifest-v1.schema.json agent_framework/src/skills/skill_manifest.cpp agent_framework/src/skills/skill_manifest_validate.cpp agent_framework/tests/test_skill_manifest_v1.cpp agent_framework/tests/test_skill_resource_contract.cpp agent_framework/CMakeLists.txt
git commit -m "feat: define stage 7 resource contracts"
```

### Task 2: Add Snapshot-Safe Resource Access

**Files:**
- Create: `agent_framework/include/agent/skill_resource_access.hpp`
- Create: `agent_framework/src/skills/skill_resource_access.cpp`
- Create: `agent_framework/tests/test_skill_resource_access.cpp`
- Modify: `agent_framework/src/skills/skill_loader.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: `SkillIndexEntry`, immutable `SkillManifest`, package/resource digests and Stage 5 package lease.
- Produces: `SkillResourceOpenOptions`, `SkillResourceHandle`, `SkillResourceResult`, `SkillResourceStream`, `SkillMappedResource`, and `SkillResourceAccess::open_snapshot`.

- [ ] **Step 1: Write failing access tests**

Test resource-ID authorization, range reads, chunk streaming, Linux read-only mapping, auto fallback, package lease survival across update, size/digest recheck, jail escape, symlink/special-file rejection and cancellation between stream chunks.

- [ ] **Step 2: Confirm failure**

Run: `cmake --build build-stage6 --target test_skill_resource_access -j8`

Expected: `skill_resource_access.hpp` is absent.

- [ ] **Step 3: Implement resolver and RAII handles**

Use the exact entry point:

```cpp
SkillResourceResult open_snapshot(
    const SkillIndexEntry& entry,
    std::shared_ptr<const SkillManifest> manifest,
    const std::string& resource_id,
    const SkillResourceOpenOptions& options) const;
```

The returned handle must retain `entry.package_lease`, path, size, media type, package digest and resource digest. Explicit mmap on unsupported platforms returns `skill_resource_mmap_unavailable`.

- [ ] **Step 4: Bound the compatibility loader**

Keep Stage 6 `read` behavior for small resources, but do not insert Asset/Model bodies into `SkillLoader::cache_`. All materialization must check `max_bytes` before allocation.

- [ ] **Step 5: Run tests**

Run: `ctest --test-dir build-stage6 -R 'skill_(resource_access|registry_wp18|cli_contract)' --output-on-failure`

Expected: all selected tests pass.

- [ ] **Step 6: Commit**

```bash
git add agent_framework/include/agent/skill_resource_access.hpp agent_framework/src/skills/skill_resource_access.cpp agent_framework/src/skills/skill_loader.cpp agent_framework/tests/test_skill_resource_access.cpp agent_framework/CMakeLists.txt
git commit -m "feat: add snapshot-safe resource access"
```

### Task 3: Add the Content-Addressed Cache Core

**Files:**
- Create: `agent_framework/include/agent/skill_resource_cache.hpp`
- Create: `agent_framework/src/skills/skill_resource_cache.cpp`
- Create: `agent_framework/tests/test_skill_resource_cache.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: verified `SkillResourceHandle` and SHA-256 helpers.
- Produces: `SkillCacheLimits`, `SkillCacheObject`, `SkillCacheLease`, `SkillCacheResult`, and `SkillResourceCache::acquire`.

- [ ] **Step 1: Write failing cache tests**

Test digest deduplication, atomic visibility, metadata contract, tamper detection, failed-copy cleanup, concurrent acquire and rejection of links/FIFO/device files.

- [ ] **Step 2: Confirm failure**

Run: `cmake --build build-stage6 --target test_skill_resource_cache -j8`

Expected: cache API is absent.

- [ ] **Step 3: Implement the cache layout and transactions**

Use:

```text
objects/sha256/<digest>
metadata/<digest>.json
derived/<kind>/<source-digest>/<config-digest>
transactions/<unique-id>
state.json
```

Copy through a bounded temporary file, verify size and digest, then atomically rename. Never publish a path supplied by metadata.

- [ ] **Step 4: Run the cache test**

Run: `ctest --test-dir build-stage6 -R skill_resource_cache --output-on-failure`

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add agent_framework/include/agent/skill_resource_cache.hpp agent_framework/src/skills/skill_resource_cache.cpp agent_framework/tests/test_skill_resource_cache.cpp agent_framework/CMakeLists.txt
git commit -m "feat: add content-addressed skill resource cache"
```

### Task 4: Enforce Quota, LRU, Pinning and Recovery

**Files:**
- Modify: `agent_framework/include/agent/skill_resource_cache.hpp`
- Modify: `agent_framework/src/skills/skill_resource_cache.cpp`
- Modify: `agent_framework/tests/test_skill_resource_cache.cpp`

**Interfaces:**
- Consumes: Task 3 cache objects and leases.
- Produces: `inspect`, `pin`, `unpin`, `collect`, `verify` and crash recovery behavior.

- [ ] **Step 1: Add failing policy/recovery cases**

Cover deterministic eviction by unpinned/unleased/oldest/digest order, pinned quota failure, active lease protection, stale transaction cleanup, metadata rebuild and corrupted-object quarantine.

- [ ] **Step 2: Implement policy operations**

Expose:

```cpp
SkillCacheReport inspect() const;
SkillCacheResult pin(const std::string& digest);
SkillCacheResult unpin(const std::string& digest);
SkillCacheResult collect();
SkillCacheResult verify();
```

- [ ] **Step 3: Run concurrency and repeat tests**

Run: `ctest --test-dir build-stage6 -R skill_resource_cache --repeat until-fail:20 --output-on-failure`

Expected: 20 consecutive passes.

- [ ] **Step 4: Commit**

```bash
git add agent_framework/include/agent/skill_resource_cache.hpp agent_framework/src/skills/skill_resource_cache.cpp agent_framework/tests/test_skill_resource_cache.cpp
git commit -m "feat: enforce skill cache quota and pinning"
```

### Task 5: Add Reference Paging and Structured Citations

**Files:**
- Create: `agent_framework/include/agent/skill_reference.hpp`
- Create: `agent_framework/src/skills/skill_reference.cpp`
- Create: `agent_framework/tests/test_skill_reference.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: snapshot-safe resource handle and Reference metadata.
- Produces: `SkillCitation`, `SkillReferencePage`, `SkillReferenceResult`, and `SkillReferenceService::read_page`.

- [ ] **Step 1: Write failing paging tests**

Cover UTF-8-safe page boundaries, stable byte cursors, citation digest/source/license/range, EOF, offset errors, invalid UTF-8, cumulative budget and binary Reference refusal.

- [ ] **Step 2: Implement bounded paging**

Use:

```cpp
SkillReferenceResult read_page(
    const SkillResourceHandle& handle,
    std::uint64_t offset,
    std::size_t max_bytes) const;
```

Never read beyond the requested page plus the bounded UTF-8 boundary lookahead.

- [ ] **Step 3: Run focused tests**

Run: `ctest --test-dir build-stage6 -R skill_reference --output-on-failure`

Expected: pass.

- [ ] **Step 4: Commit**

```bash
git add agent_framework/include/agent/skill_reference.hpp agent_framework/src/skills/skill_reference.cpp agent_framework/tests/test_skill_reference.cpp agent_framework/CMakeLists.txt
git commit -m "feat: add paged skill references and citations"
```

### Task 6: Add Deterministic Reference Retrieval

**Files:**
- Modify: `agent_framework/include/agent/skill_reference.hpp`
- Modify: `agent_framework/src/skills/skill_reference.cpp`
- Modify: `agent_framework/tests/test_skill_reference.cpp`

**Interfaces:**
- Consumes: Reference pages and Task 3 derived cache.
- Produces: `SkillReferenceSearchHit`, `SkillReferenceSearchResult`, index build and `search`.

- [ ] **Step 1: Add failing index/search tests**

Cover Latin normalization, CJK unigram/bigram lookup, deterministic BM25 ordering, byte-offset tie break, snippet provenance, index invalidation by digest and derived-cache quota.

- [ ] **Step 2: Implement `lexical-v1`**

Use UTF-8 code-point decoding. Normalize ASCII words to lowercase; emit CJK unigram and adjacent bigram terms. Key derived indexes by source digest, `lexical-v1` and configuration digest.

- [ ] **Step 3: Run deterministic repeats**

Run: `ctest --test-dir build-stage6 -R skill_reference --repeat until-fail:20 --output-on-failure`

Expected: identical ordering and 20 passes.

- [ ] **Step 4: Commit**

```bash
git add agent_framework/include/agent/skill_reference.hpp agent_framework/src/skills/skill_reference.cpp agent_framework/tests/test_skill_reference.cpp
git commit -m "feat: add deterministic reference retrieval"
```

### Task 7: Enforce Bounded Artifact Ingestion

**Files:**
- Create: `agent_framework/include/agent/skill_artifact_import.hpp`
- Create: `agent_framework/src/skills/skill_artifact_import.cpp`
- Create: `agent_framework/tests/test_skill_artifact_import.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: injected byte streams/archive entries and the content-addressed cache.
- Produces: `SkillArtifactLimits`, `SkillArtifactSource`, `SkillArchiveEntry`, `SkillArtifactImporter`.

- [ ] **Step 1: Write failing security tests**

Cover input-byte limit, digest mismatch, absolute/traversal/duplicate entry paths, links/special files, entry count, entry size, expanded size, expansion ratio and cancellation cleanup.

- [ ] **Step 2: Implement provider-neutral bounded ingestion**

Use:

```cpp
struct SkillArtifactLimits {
    std::uint64_t max_download_bytes;
    std::uint64_t max_expanded_bytes;
    std::uint64_t max_entry_bytes;
    std::size_t max_entries;
    double max_expansion_ratio;
};
```

The importer writes validated regular-file entries itself. It does not accept a provider-created destination path and does not implement HTTP.

- [ ] **Step 3: Run negative tests**

Run: `ctest --test-dir build-stage6 -R skill_artifact_import --output-on-failure`

Expected: all attacks fail closed and valid bounded input passes.

- [ ] **Step 4: Commit**

```bash
git add agent_framework/include/agent/skill_artifact_import.hpp agent_framework/src/skills/skill_artifact_import.cpp agent_framework/tests/test_skill_artifact_import.cpp agent_framework/CMakeLists.txt
git commit -m "feat: enforce bounded artifact ingestion"
```

### Task 8: Add Non-Executing Model Admission

**Files:**
- Create: `agent_framework/include/agent/skill_model.hpp`
- Create: `agent_framework/src/skills/skill_model.cpp`
- Create: `agent_framework/tests/test_skill_model.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: Model descriptor, verified resource/cache handle and explicit host capabilities.
- Produces: `SkillModelHostCapabilities`, `SkillModelAdmissionResult`, `SkillModelService::check` and read-only `open`.

- [ ] **Step 1: Write failing admission tests**

Cover runtime, device, precision and memory mismatch; unavailable cache; load/mmap byte limit; package update isolation; and descriptors attempting executable behavior.

- [ ] **Step 2: Implement admission without execution**

Use:

```cpp
SkillModelAdmissionResult check(
    const SkillResourceHandle& handle,
    const SkillModelHostCapabilities& host) const;
```

Only return diagnostics and a read-only resource/cache handle. Do not probe by loading the model library or running a package command.

- [ ] **Step 3: Run tests**

Run: `ctest --test-dir build-stage6 -R skill_model --output-on-failure`

Expected: pass with stable incompatibility codes.

- [ ] **Step 4: Commit**

```bash
git add agent_framework/include/agent/skill_model.hpp agent_framework/src/skills/skill_model.cpp agent_framework/tests/test_skill_model.cpp agent_framework/CMakeLists.txt
git commit -m "feat: add non-executing model admission"
```

### Task 9: Integrate Services, Doctor and skillctl

**Files:**
- Modify: `agent_framework/include/agent/skill_services.hpp`
- Modify: `agent_framework/src/skills/skill_services.cpp`
- Modify: `agent_framework/include/agent/skill_doctor.hpp`
- Modify: `agent_framework/src/skills/skill_doctor.cpp`
- Modify: `agent_framework/include/agent/skill_command.hpp`
- Modify: `agent_framework/src/skills/skill_command.cpp`
- Modify: `agent_framework/tools/skillctl.cpp`
- Modify: `agent_framework/tests/test_skill_doctor.cpp`
- Create: `agent_framework/tests/test_skill_cli_resources.cpp`
- Modify: `agent_framework/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 2-8 services.
- Produces: service wiring, offline readiness checks and Stage 7 CLI commands in the Stage 6 JSON envelope.

- [ ] **Step 1: Write failing CLI/doctor tests**

Exercise `reference page/search`, `cache status/verify/gc/pin/unpin` and `model check`, including usage errors, missing IDs, corrupt cache and incompatible host capabilities.

- [ ] **Step 2: Wire services and commands**

Add these exact commands:

```text
skillctl reference page ID RESOURCE --offset N --max-bytes N
skillctl reference search ID RESOURCE QUERY --limit N
skillctl cache status|verify|gc
skillctl cache pin|unpin DIGEST
skillctl model check ID RESOURCE --runtime R --device D --precision P --memory N
```

Continue to emit `agent.taskflow/skillctl-output/v1` and existing exit codes.

- [ ] **Step 3: Extend offline doctor**

Report audit metadata completeness, cache presence/integrity, offline readiness, runtime/device/precision/memory compatibility and the fact that automatic execution is disabled.

- [ ] **Step 4: Run CLI and doctor contracts**

Run: `ctest --test-dir build-stage6 -R 'skill_(cli_resources|cli_contract|doctor_offline)' --output-on-failure`

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```bash
git add agent_framework/include/agent/skill_services.hpp agent_framework/src/skills/skill_services.cpp agent_framework/include/agent/skill_doctor.hpp agent_framework/src/skills/skill_doctor.cpp agent_framework/include/agent/skill_command.hpp agent_framework/src/skills/skill_command.cpp agent_framework/tools/skillctl.cpp agent_framework/tests/test_skill_doctor.cpp agent_framework/tests/test_skill_cli_resources.cpp agent_framework/CMakeLists.txt
git commit -m "feat: expose skill resource operations"
```

### Task 10: Add CI, Installation, Documentation and Final Gates

**Files:**
- Modify: `agent_framework/CMakeLists.txt`
- Modify: `.github/workflows/ubuntu.yml`
- Modify: `agent_framework/docs/guides/skills.md`
- Modify: `agent_framework/docs/guides/skill-plan.md`

**Interfaces:**
- Consumes: all Stage 7 targets and commands.
- Produces: installable headers, offline CI labels, operator documentation and completion evidence.

- [ ] **Step 1: Add labels and offline CI job coverage**

Use:

```text
skill-resource-management
skill-cache-security
skill-reference-retrieval
skill-model-admission
```

The job must run without credentials and with network-related environment variables removed.

- [ ] **Step 2: Document Stage 7 operations and boundaries**

Document Manifest examples, paging/search, cache maintenance, model admission, environment limits and the Stage 8 boundary.

- [ ] **Step 3: Run Stage 1-7 Skill regression**

Run: `ctest --test-dir build-stage7 -L '^skill-' --output-on-failure`

Expected: all selected tests pass.

- [ ] **Step 4: Run Stage 7 stability repeats**

Run: `ctest --test-dir build-stage7 -L 'skill-(resource-management|cache-security|reference-retrieval|model-admission)' --repeat until-fail:20 --output-on-failure`

Expected: every selected test passes 20 times.

- [ ] **Step 5: Verify bounded large-file behavior**

Use a sparse fixture larger than the materialization limit and assert instrumented read/allocation counters stay within the requested page/chunk/mapping window. Do not use fragile process-RSS thresholds.

- [ ] **Step 6: Verify fresh build and install**

Run:

```bash
cmake -S . -B build-stage7 -DTF_BUILD_AGENT_FRAMEWORK=ON -DTF_BUILD_TESTS=ON -DAGENT_BUILD_EXAMPLES=OFF
cmake --build build-stage7 -j8
cmake --install build-stage7 --prefix /tmp/taskflow-stage7-install
```

Expected: build reaches 100%; Stage 7 headers, schemas and `skillctl` install successfully; installed CLI completes offline resource checks.

- [ ] **Step 7: Run complete available CTest**

Run: `ctest --test-dir build --output-on-failure -j8`

Expected: all locally supported tests pass; HTTP/A2A/online-model sandbox restrictions are reported separately from regressions.

- [ ] **Step 8: Commit**

```bash
git add agent_framework/CMakeLists.txt .github/workflows/ubuntu.yml agent_framework/docs/guides/skills.md agent_framework/docs/guides/skill-plan.md
git commit -m "docs: complete stage 7 resource management"
```

## Completion Definition

- [ ] Large References use bounded paging with digest- and byte-range-backed citations.
- [ ] Optional retrieval is deterministic and does not require an LLM, embedding model or network.
- [ ] Asset/Model resources are not materialized into prompt/string caches by default.
- [ ] Cache quota, LRU, pin, leases, atomic writes, recovery and tamper checks pass.
- [ ] Lockfile/package snapshots and cache leases preserve running-task resource identity.
- [ ] Model handling performs admission and read-only open only, with no automatic execution path.
- [ ] Offline doctor distinguishes ready, missing, corrupt and incompatible resources.
- [ ] Remote downloads, signing, trusted publishers, SBOM and Registry remain Stage 8 work.

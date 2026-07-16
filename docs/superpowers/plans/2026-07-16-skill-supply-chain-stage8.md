# Stage 8 Skill Supply Chain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Execute this plan task-by-task with explicit review checkpoints. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add deterministic portable Skill packages, SBOM/provenance, Ed25519 trust verification, digest-pinned remote Registry resolution, offline import and supply-chain CI gates.

**Architecture:** Build a store-only deterministic `.tfskill` ZIP whose byte digest covers the manifest, resources and generated metadata. Verify detached Ed25519 envelopes against a role-scoped local trust store before Package Gate or lifecycle mutation. A provider-neutral Registry client resolves a signed index to immutable digests, streams bounded artifacts through the Stage 7 importer/cache, and records verified identities in the lockfile.

**Tech Stack:** C++20, `std::filesystem`, nlohmann/json, OpenSSL EVP Ed25519/SHA-256, optional libcurl HTTPS transport, CMake/CTest.

## Global Constraints

- Preserve Manifest v0/v1 parsing and the existing directory preflight alias.
- `.tfskill` is ZIP32 store-only: sorted UTF-8 paths, fixed DOS epoch, no comments/extra fields, normalized modes, no ZIP64 or compression.
- Reject absolute/traversal/backslash/control/reserved/case-colliding paths and every link or special file.
- Package SHA-256 covers final archive bytes; detached signatures avoid circular identity.
- Signature preimages are fixed binary fields, never JSON serialization order.
- Remote and Registry installs require a trusted signature and exact digest; unsigned bypass is local-only and explicit.
- Registry indexes are signed and may contain only immutable package digests.
- Downloads, indexes, signatures and extraction remain bounded and transactionally published.
- Private key bytes never enter logs, lockfiles, events, CLI JSON or the content store.
- Tests use local/provider-neutral fixtures and do not require network credentials or listening sockets.

---

## File Map

- `agent_framework/include/agent/skill_archive.hpp`, `src/skills/skill_archive.cpp`: deterministic ZIP build/inspect/extract.
- `agent_framework/include/agent/skill_sbom.hpp`, `src/skills/skill_sbom.cpp`: CycloneDX 1.6 SBOM and provenance.
- `agent_framework/include/agent/skill_supply_chain.hpp`, `src/skills/skill_supply_chain.cpp`: signature, trust and revocation.
- `agent_framework/include/agent/skill_remote_registry.hpp`, `src/skills/skill_remote_registry.cpp`: signed index, resolution, mirrors and bounded transport.
- `agent_framework/include/agent/skill_lifecycle.hpp`, `src/skills/skill_lifecycle.cpp`: verified store metadata and lock identity.
- `agent_framework/include/agent/skill_package_gate.hpp`, `src/skills/skill_package_gate.cpp`: pre-mutation trust gate.
- `agent_framework/include/agent/skill_command.hpp`, `src/skills/skill_command.cpp`, `tools/skillctl.cpp`: CLI surface.
- `agent_framework/schemas/skill-{package,signature,provenance,trust-store,registry}-v1.schema.json`: JSON contracts.
- `agent_framework/tests/test_skill_{supply_chain_contract,archive,sbom,signature,remote_registry,supply_chain_e2e,cli_supply_chain}.cpp`: contracts and gates.

### Task 0: Persist the Approved Plan

- [ ] Add this document, run `git diff --check`, and commit `docs: plan stage 8 skill supply chain`.

### Task 1: Freeze Archive and Supply-Chain Contracts

- [ ] Write failing schema/type tests for package, signature, provenance, trust roles/revocations and Registry index.
- [ ] Add exact public types and five installed schemas with stable parse/validation codes.
- [ ] Run `ctest --test-dir build-stage7 -R skill_supply_chain_contract --output-on-failure`.
- [ ] Commit `feat: define skill supply chain contracts`.

### Task 2: Implement Deterministic `.tfskill` Archives

- [ ] Test byte-identical rebuilds across file order/mtime/umask and one-byte identity changes.
- [ ] Test traversal, links, special files, path collisions, compressed entries, truncation and limits.
- [ ] Implement `build_skill_archive`, `inspect_skill_archive` and `extract_skill_archive` with atomic output/extraction.
- [ ] Repeat the archive test 20 times and commit `feat: add deterministic skill archives`.

### Task 3: Generate SBOM and Provenance

- [ ] Test deterministic CycloneDX components for Script/CLI/MCP/Asset/Model/dependencies.
- [ ] Test source URI/revision/builder and resource inputs in `agent.taskflow/skill-provenance/v1`.
- [ ] Embed both under `META-INF/`, bind their digests in package metadata, and reject missing audit data.
- [ ] Commit `feat: generate skill sbom and provenance`.

### Task 4: Add Ed25519 Trust and Revocation

- [ ] Test valid signatures plus tamper, unknown key, wrong role/source, expiry and publisher/key/package revocation.
- [ ] Implement fixed-preimage detached signing and verification with key ID = SHA-256(public DER).
- [ ] Fail closed without OpenSSL and prove no private-key material is serialized.
- [ ] Commit `feat: verify signed skill packages`.

### Task 5: Integrate Trust into Package Gate

- [ ] Gate archive parse/digest/signature/extract/validate/lint/tests/repeated identity before mutation.
- [ ] Preserve directory preflight; require explicit `allow_unsigned_local` for unsigned local install and forbid it remotely.
- [ ] Test store/lock/history/generation invariance for every failure class.
- [ ] Commit `feat: gate package installation by trust policy`.

### Task 6: Persist Verified Store and Lock Identities

- [ ] Add archive/publisher/key/signature/SBOM/provenance/Registry digests to package records and lockfile.
- [ ] Revalidate archive, extracted package and metadata when loading the store.
- [ ] Read legacy locks as `legacyUnsigned` but prohibit remote automatic update.
- [ ] Prove lock replay ignores newer indexes and commit `feat: persist verified package identities`.

### Task 7: Add Signed Remote Registry and Bounded Transport

- [ ] Define `SkillRegistryTransport`, in-memory/file transports and optional libcurl HTTPS streaming.
- [ ] Verify a <=4 MiB signed index before reading entries; signatures <=64 KiB and packages <=512 MiB/4096 entries.
- [ ] Enforce HTTPS/SSRF/redirect policy and immutable digest entries.
- [ ] Commit `feat: add signed remote skill registry`.

### Task 8: Add Mirrors, Offline Import and Digest Pinning

- [ ] Test ordered mirror fallback with digest/signature verification at each candidate.
- [ ] Test `.tfskill` plus detached signature offline import and explicit digest mismatch cleanup.
- [ ] Test concurrent same-digest import and no state leakage after all mirrors fail.
- [ ] Commit `feat: support pinned and offline skill imports`.

### Task 9: Expose Supply-Chain CLI

- [ ] Add `package build/inspect/sign/verify/sbom`, `registry sync/resolve/install`, and verified archive install flags.
- [ ] Preserve stable `agent.taskflow/skillctl-output/v1`, exit codes and directory preflight alias.
- [ ] Test usage, JSON, secret redaction, strict remote policy and local-only unsigned override.
- [ ] Commit `feat: expose skill supply chain commands`.

### Task 10: Complete Security Gates, CI and Documentation

- [ ] Add labels `skill-package-archive`, `skill-signature-security`, `skill-sbom-contract`, `skill-registry-supply-chain`, `skill-supply-chain-e2e`.
- [ ] Extend the offline Ubuntu job without credentials or real network access.
- [ ] Document archive/sign/trust/Registry/offline workflows and the remaining non-goals.
- [ ] Configure and fully build fresh `build-stage8`; run Stage 8 labels and each focused test 20 times.
- [ ] Install to `/tmp/taskflow-stage8-install`, verify binaries/headers/schemas, then run all available CTest tests.
- [ ] Commit `docs: complete stage 8 skill supply chain`.

## Completion Criteria

- Identical inputs produce byte-identical archives on supported platforms.
- Tampered, unsigned, unknown, revoked, out-of-scope or digest-mismatched packages fail closed.
- Registry index and package identities are independently signed and verified.
- Mirror and offline paths never weaken digest/trust requirements.
- Lockfile replay resolves the same exact Skill graph independent of later Registry changes.
- Every failure precedes store/lock/history/Registry generation mutation.
- Focused Stage 8 gates pass 20 consecutive runs and are installed and exercised in offline CI.

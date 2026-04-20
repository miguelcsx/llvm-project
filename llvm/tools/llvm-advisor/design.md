# LLVM Advisor - Production Architecture Design

Status: Draft  
Audience: LLVM Advisor maintainers, compiler tooling engineers, infra owners

Implementation package:

1. `docs/README.md`
2. `docs/implementation-backlog/phase-1-foundation.md`
3. `docs/implementation-backlog/phase-2-capability-engine.md`
4. `docs/implementation-backlog/phase-3-compare-scale.md`
5. `docs/implementation-backlog/phase-4-hardening-lsp.md`

## 1. Problem Statement

LLVM Advisor must help users understand "what is happening in their code" at scale:

1. Extract compiler/build/runtime insights from LLVM/Clang internals.
2. Support dynamic exploration (users request more info later, without throwing away prior work).
3. Compare versions (A vs B) for changed code and full codebases (including full LLVM builds).
4. Be production-ready: stable, observable, incremental, recoverable, and fast.
5. Prefer library-first execution over shelling out to driver tools.

Current anti-pattern to avoid:

1. Parse everything eagerly.
2. Re-run full extraction whenever the user asks for a new view.
3. Couple UI endpoints directly to heavyweight scanning/parsing.

## 2. Goals

1. Snapshot-based architecture with incremental, on-demand analysis.
2. Capability graph: users request *information*, system resolves and computes only missing dependencies.
3. Stable compare engine across snapshots and compilation units.
4. Support very large projects via caching, sharding, and bounded-memory processing.
5. Library-first analyzers for Clang/LLVM internals.
6. Single data backend used by Web UI and optional LSP client.
7. Representation-graph storage model instead of fixed artifact categories.
8. REST as control plane only; large explorer data must use query/range access patterns.

## 3. Non-Goals

1. Replace build systems (CMake/Ninja/Bazel stay as source of truth).
2. Guarantee zero-cost collection for every capability (some deep analyses are expensive).
3. Perfectly avoid re-compilation for all future queries (some data can only be generated with specific compile-time instrumentation).

## 4. Design Principles

1. Capture once, derive many times.
2. Immutable snapshots + append-only metadata updates.
3. Content-addressed artifacts and deterministic cache keys.
4. Capability versioning for reproducibility.
5. Query-time planning (lazy materialization).
6. Strong provenance: every metric must trace to snapshot, unit, toolchain, and capability version.

## 5. High-Level Architecture

```text
Build System (CMake/Ninja/etc)
        |
        v
[Build Integration Layer]
  - compile_commands ingest
  - optional compiler-launcher hooks
        |
        v
[Capture Core]
  - unit identity
  - baseline representation capture
  - provenance metadata
        |
        v
[Capability Engine]
  - DAG planner
  - library-first analyzers
  - artifact/materialization cache
        |
        +---------------------------+
        |                           |
        v                           v
[Snapshot Store + CAS]        [Compare Engine]
        |                           |
        +-------------+-------------+
                      |
                      v
                [Query API]
             /               \
            v                 v
       [Web UI]         [LSP Client]
```

## 6. Core Components

### 6.1 Build Integration Layer

Responsibilities:

1. Ingest compile and link commands from `compile_commands.json` and build metadata.
2. Normalize command lines (toolchain, target, sysroot, include paths, defines, opt level).
3. Capture per-target/per-config metadata (Debug/Release, CMake preset, generator).
4. Optional launcher mode for real-time capture during builds.

Inputs:

1. Build directory.
2. Source root.
3. VCS metadata (`git sha`, branch, dirty flag if available).

Outputs:

1. `BuildManifest`.
2. `CompilationUnitPlan` list.

### 6.2 Capture Core

Responsibilities:

1. Create immutable snapshot IDs.
2. Build stable `UnitID` for every compilation command.
3. Capture baseline representations (profile-driven).
4. Persist provenance and fingerprint metadata.
5. Emit declarative representation manifests with explicit source linkage.

Unit identity (recommended key):

```text
UnitID = SHA256(
  normalized_source_path +
  language +
  normalized_compile_flags +
  target_triple +
  toolchain_version
)
```

### 6.3 Capability Engine (dynamic extraction)

A capability describes how to produce a specific information output.

`CapabilitySpec`:

1. `id` (example: `clang.ast.json`, `llvm.ir.optstats`, `diag.summary`)
2. `version` (semantic version, invalidates old cache on upgrade)
3. `required_inputs` (source, command, AST, IR, object, profile, trace, etc.)
4. `cost_class` (`cheap`, `moderate`, `expensive`)
5. `execution_mode` (`library`, `hybrid`, `external_fallback`)
6. `produces` (artifact schema IDs)

Execution contract:

1. Plan dependencies as DAG.
2. Reuse cached materializations by content key.
3. Compute only missing nodes.
4. Persist outputs in CAS + indexed metadata.

### 6.4 Snapshot Store and Artifact CAS

Two logical stores:

1. Metadata store:
   - snapshots, units, capability runs, metrics, compare results.
2. Blob store (CAS):
   - raw and processed artifacts addressed by digest.

Required properties:

1. Append-only records with compaction.
2. Crash-safe writes (atomic file replace + journal).
3. Schema versioning and migration support.

Note:

1. If strict "no external runtime dependency" is mandatory, implement metadata store as segmented JSONL + binary indexes.
2. If external dependency policy allows it, SQLite backend can be added as optional backend.

## 7. Library-First Analyzer Strategy

Primary rule:

1. Prefer LLVM/Clang C++ APIs to execute analysis in-process.

Examples:

1. Frontend/AST:
   - `clang::CompilerInvocation`
   - `clang::tooling` + custom `FrontendAction`
2. IR/passes:
   - `llvm::PassBuilder` + analysis managers
3. Object/binary/debug:
   - `llvm::object`, `llvm::DWARFContext`
4. Remarks/time:
   - parse with LLVM/YAML/JSON facilities where available

Capture rule:

1. The native layer must emit `representations[]` and mapping sidecars directly.
2. The Python backend should index those declarations, not reconstruct them from filenames.

Fallback rule:

1. If no stable library API exists for a capability, support a controlled wrapper capability (`external_fallback`) with full provenance and deterministic command templates.

## 8. Dynamic Extraction Model

User requirement: "do not rerun everything if more info is requested later."

Implemented via capability readiness levels:

1. L0 (Derivable): can be computed from already captured artifacts only.
2. L1 (Reanalyzable): can be computed from stored IR/object/AST without recompiling source.
3. L2 (Reinstrumentation): requires additional compile-time instrumentation not present in current snapshot.

Behavior:

1. Query planner returns immediate results for available L0/L1.
2. For missing L2, system proposes "augmentation jobs" on selected scope only (changed units, target, file glob), not full rebuild.
3. Augmentation writes new capability outputs under same snapshot lineage.

## 9. Snapshot and Compare Model

### 9.1 Snapshot entity

`Snapshot` fields:

1. `snapshot_id`
2. `project_id`
3. `source_revision`
4. `build_config` (preset/toolchain/target)
5. `created_at`
6. `parent_snapshot_id` (optional)

### 9.2 Unit matching for compare

Comparison algorithm:

1. Match by `UnitID` first.
2. Fallback by normalized source path + language + target.
3. Mark as `added`, `removed`, `matched`, `changed`.

### 9.3 Compare outputs

1. Global deltas (build time, diagnostics count, binary size, pass stats).
2. Per-target deltas.
3. Per-unit deltas.
4. Per-function/per-location deltas when capability exists.

### 9.4 Regression classifier

Each metric has:

1. direction (`higher_is_better`, `lower_is_better`, `target_band`)
2. threshold (absolute and relative)
3. confidence tag (`high`, `medium`, `low`)

## 10. API Design (service contract)

### 10.1 Control endpoints

1. `POST /snapshots`
2. `GET /snapshots/{id}`
3. `GET /capabilities`
4. `POST /jobs` (async materialization/augmentation/compare)
5. `GET /jobs/{id}`

### 10.2 Query endpoints

1. `POST /query`
   - request capabilities + scope + filters + pagination
2. `GET /blob/{artifact_id}` or equivalent ranged payload fetch
3. `POST /explorer/query`
   - range and mapping oriented explorer access
2. `POST /compare`
   - `base_snapshot`, `candidate_snapshot`, `scope`, `metrics`
3. `GET /units`
4. `GET /representations/{representation_id}`

Response requirements:

1. Paged results for large datasets.
2. Capability coverage metadata (`available`, `missing_l2`, `queued`).
3. Provenance fields in every payload.

## 11. UI and LSP Positioning

### 11.1 Web UI (primary)

Best for:

1. Large-scale exploration.
2. Historical compare and dashboards.
3. Cross-unit/cross-target aggregations.

### 11.2 LSP (secondary client)

Best for:

1. File-local overlays (diagnostics, remarks, hotspots).
2. Jump-to-web links for deep context.
3. Requesting focused capability jobs from editor context.

Recommendation:

1. Keep one backend and one schema.
2. Treat LSP as a thin consumer, not a separate analysis engine.

## 12. Performance and Scale Strategy

1. Shard work by `UnitID` and capability.
2. Bounded worker pools with backpressure.
3. Streaming parsers and paged queries for large artifacts.
4. Memory budget per worker and per request.
5. Warm caches for most-requested capabilities.
6. Avoid global scans in request path; always query indexed metadata first.

Target behavior for full LLVM:

1. Initial snapshot can be heavy.
2. Incremental snapshots should reuse unchanged units.
3. Compare should process changed units first and stream partial results.

## 13. Reliability and Production Readiness

1. Idempotent jobs (safe retries).
2. Journaled writes and crash recovery.
3. Capability timeout and cancellation.
4. Circuit breaker for expensive queries.
5. Strict schema compatibility checks.
6. Deterministic hash keys for cache correctness.
7. Full observability:
   - job latency
   - cache hit ratio
   - capability error rate
   - data freshness

## 14. Security and Multi-Tenant Safety

1. Path sandboxing for file access.
2. Strict allowlist for external fallback commands.
3. Resource limits (CPU/memory/time).
4. Snapshot-level ACLs if deployed as shared service.
5. Redaction rules for sensitive paths or command args.

## 15. Capture Profiles

Define profile presets so users can control cost:

1. `minimal`
   - diagnostics, command metadata, unit graph, basic timings
2. `balanced` (default)
   - minimal + remarks + selected IR/object summaries
3. `max-observability`
   - balanced + deep trace/style data for hot paths

Profiles map to capability allowlists and cost budgets.

## 16. Concrete Workflow (LLVM full build compare)

1. Create baseline snapshot from LLVM build dir (`S1`).
2. Build + capture with `balanced` profile.
3. Make code change.
4. Build incrementally and create `S2`.
5. Run compare job `compare(S1, S2)` for:
   - global metrics
   - changed units
   - selected targets
6. If user asks for deeper insight in regressed units:
   - request additional capabilities only for those units (augmentation job).

## 17. Migration Plan from Current Codebase

Phase 1:

1. Introduce canonical snapshot schema and unit IDs.
2. Separate discovery/indexing from HTTP handlers.
3. Add async job model and query planner skeleton.

Phase 2:

1. Move heavy analyzers to library-first C++ capability runners.
2. Keep Python web as frontend/API facade if needed.
3. Add capability registry and DAG planner.

Phase 3:

1. Implement compare engine with stable matching + regression classifier.
2. Add incremental snapshot reuse.
3. Add provenance-rich API responses.

Phase 4:

1. Add LSP client integration on top of same backend API.
2. Harden SLOs, retention, multi-tenant controls.

## 18. Risks and Mitigations

Risk: Some desired outputs require compile-time flags not present in snapshot.  
Mitigation: L2 augmentation jobs scoped to changed units/targets.

Risk: Capability drift across LLVM versions.  
Mitigation: capability versioning + compatibility matrix.

Risk: Full LLVM snapshots can be huge.  
Mitigation: CAS deduplication, profile defaults, retention and compaction.

Risk: Query latency spikes on cold caches.  
Mitigation: precompute key aggregates, async jobs, progressive responses.

## 19. Success Criteria

1. Users can request new insights without re-running whole pipeline.
2. Snapshot compare on large projects is stable and actionable.
3. API remains responsive under concurrent dashboard and compare workloads.
4. Every insight is reproducible from provenance metadata.

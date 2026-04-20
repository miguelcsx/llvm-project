# Phase 2 Backlog: Library-First Capability Engine

Status: Blocked on Phase 1 completion  
Goal: deliver dynamic, dependency-driven capability execution using LLVM/Clang libraries.

## Scope

1. Library-first capability runners in C++.
2. Planner execution pipeline with cache-aware materialization.
3. Selective augmentation jobs for missing L2 data.
4. Initial production capability packs (diagnostics, remarks, AST, IR summaries, binary summaries).

## Exit Criteria

1. At least 8 capabilities run through library-first path with declarative representation outputs.
2. Query planner executes only missing nodes (cache hits verified).
3. Augmentation jobs can target subset of units/targets.
4. Fallback execution path is controlled and fully audited.
5. Capability outputs include provenance and schema validation.

## Backlog

| ID | Title | Description | Deliverables | Dependencies | Acceptance Criteria |
|---|---|---|---|---|---|
| P2-001 | Runner Interface | Define C++ capability runner ABI with typed inputs/outputs and diagnostics. | `CapabilityRunner` interface + registry wiring. | P1-006 | Runner contract is stable and versioned. |
| P2-002 | AST Capability | Implement AST extraction capability via Clang tooling API. | `clang.ast.summary`, `clang.ast.json` runners. | P2-001 | Output stable across reruns and schema-valid. |
| P2-003 | Diagnostics Capability | Implement diagnostics summary capability from frontend pipeline. | `clang.diag.summary` runner. | P2-001 | Matches compiler output counts and levels within tolerance. |
| P2-004 | Remarks Capability | Implement remarks capability from internal APIs/parsers with schema versioning. | `llvm.remarks.summary` runner. | P2-001 | Produces deterministic pass/function/location aggregates. |
| P2-005 | IR Capability | Add IR summary and pass-stat capabilities via LLVM pass managers. | `llvm.ir.summary`, `llvm.pass.stats` runners. | P2-001 | Stable metrics and bounded memory usage on large units. |
| P2-006 | Binary Capability | Add object/binary/debug summary capabilities via `llvm::object` and DWARF APIs. | `llvm.obj.summary`, `llvm.debug.summary`. | P2-001 | Output validated against sample binaries and fixtures. |
| P2-007 | Planner Executor | Connect planner to worker runtime, using CAS keys for node materialization. | executor + node cache index. | P1-007, P2-001 | Planner skips cached nodes and runs only missing ones. |
| P2-008 | L2 Augmentation Jobs | Implement "request additional info later" pipeline with scoped recomputation only. | augmentation job type + scope filters. | P2-007 | User can request deeper capabilities for selected units without full rerun. |
| P2-009 | Fallback Guardrails | Add allowlisted external fallback runner policy for unsupported capabilities. | fallback policy + audit logs + deny by default. | P2-007 | Non-allowlisted fallback attempts are rejected and logged. |
| P2-010 | Capability Packs | Define profile packs: minimal, balanced, max-observability. | profile config files + defaults. | P2-002..P2-006 | Snapshot with balanced profile runs successfully on medium and large datasets. |
| P2-011 | API Materialization Views | Add query responses for partially materialized capability sets with queued jobs. | query response extensions. | P2-008, P1-010 | API clearly marks available vs queued vs missing-l2 outputs. |
| P2-012 | Scale Validation | Run capability workloads on LLVM-sized dataset and profile bottlenecks. | perf report + remediation tasks. | P2-007..P2-011 | Throughput and memory targets met or justified with follow-up plan. |

## Capability Pack v1 (must ship in Phase 2)

1. `clang.diag.summary`
2. `llvm.remarks.summary`
3. `clang.ast.summary`
4. `llvm.ir.summary`
5. `llvm.pass.stats`
6. `llvm.obj.summary`
7. `llvm.debug.summary`
8. `build.command.meta`

## Delivery Sequence

### Sprint A

1. P2-001, P2-002, P2-003.
2. End-to-end for first three capabilities.

### Sprint B

1. P2-004, P2-005, P2-006.
2. Schema and consistency tests.

### Sprint C

1. P2-007, P2-008, P2-009.
2. Augmentation and fallback guardrails.

### Sprint D

1. P2-010, P2-011, P2-012.
2. Benchmark and profile-pack freeze.

## Code Placement Guidance

1. Runners:
   - `src/Capabilities/*`
2. Planner execution:
   - `src/Core/CapabilityExecutor.*`
3. Profile definitions:
   - `config/profiles/*.json`
4. API updates:
   - `tools/webserver/api/service.py`

## Risks and Mitigations

1. Risk: capability output format drift.
   - Mitigation: strict schema validation and version pinning.
2. Risk: memory blowups on huge translation units.
   - Mitigation: streaming outputs and node-level memory budgets.
3. Risk: unsupported analysis requires external tools.
   - Mitigation: fallback path isolated, audited, and minimal.

## Definition of Done

1. Capability pack v1 production-ready with tests and benchmarks.
2. Dynamic augmentation proven in user workflow.
3. Cache hit ratio telemetry available per capability.

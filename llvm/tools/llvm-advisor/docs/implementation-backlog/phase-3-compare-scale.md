# Phase 3 Backlog: Compare Engine and Large-Scale Operation

Status: Blocked on Phase 2 completion  
Goal: robust A/B compare across snapshots, with incremental reuse for very large codebases.

## Scope

1. Snapshot comparison engine and stable unit matching.
2. Global/target/unit/function-level delta calculations.
3. Regression classifier and explainability metadata.
4. Incremental snapshot reuse and changed-unit prioritization.
5. API and UI endpoints for compare workflows.

## Exit Criteria

1. Compare `S1` vs `S2` supports at least 100k units and streams partial results.
2. Unit matching precision is validated on path changes and target variants.
3. Regression classifier emits consistent severity and confidence tags.
4. Compare can be scoped by target, directory, unit glob, and capability set.
5. End-to-end large-project compare succeeds with bounded memory.

## Backlog

| ID | Title | Description | Deliverables | Dependencies | Acceptance Criteria |
|---|---|---|---|---|---|
| P3-001 | Unit Matching Engine | Implement matching by `UnitID` with fallback strategy. | matching module + conflict resolver. | P1-002, P2-* | Match categories: matched, changed, added, removed. |
| P3-002 | Metric Normalization | Standardize metric directions and scales for cross-capability compare. | metric registry + normalization rules. | P2-010 | Metrics are comparable and units are explicit. |
| P3-003 | Delta Calculator | Compute deltas for global, target, and unit scopes. | compare core module + tests. | P3-001, P3-002 | Delta output deterministic and schema-valid. |
| P3-004 | Function/Location Diff | Add deep diff for capabilities with location/function granularity. | deep diff pipeline. | P3-003 | Function-level regressions are traceable to source locations. |
| P3-005 | Regression Classifier | Implement severity/confidence classification with thresholds. | classifier module + policy config. | P3-003 | Severity labels reproducible and configurable. |
| P3-006 | Streaming Compare Jobs | Add progressive compare output for very large projects. | chunked result persistence + API streaming. | P1-007, P3-003 | Client receives partial compare updates before completion. |
| P3-007 | Incremental Reuse | Skip unchanged units and reuse previous capability outputs safely. | reuse planner extension + cache proofs. | P2-007 | Compare runtime scales with changed units, not full corpus. |
| P3-008 | Compare API | Implement `POST /compare` and result retrieval endpoints. | API handlers + pagination + filters. | P3-003, P3-006 | Compare API supports query by severity, metric, scope. |
| P3-009 | Compare UI Workflow | Add web workflow for baseline selection and drill-down diff navigation. | UI pages/components + API client updates. | P3-008 | User can inspect regressions from global to file/function. |
| P3-010 | Large Dataset Qualification | Run compare qualification on full LLVM-like workloads. | perf report + operational limits doc. | P3-006, P3-007 | Meets configured SLA targets or includes approved exceptions. |

## Delivery Sequence

### Sprint A

1. P3-001, P3-002, P3-003.
2. Base compare outputs in API.

### Sprint B

1. P3-004, P3-005, P3-006.
2. Streaming and classifier.

### Sprint C

1. P3-007, P3-008, P3-009.
2. End-user compare workflow.

### Sprint D

1. P3-010.
2. Operational sign-off for large workloads.

## Code Placement Guidance

1. Core compare:
   - `src/Core/CompareEngine.*`
   - `src/Core/RegressionClassifier.*`
2. API:
   - `tools/webserver/api/service.py`
3. UI:
   - `tools/webserver/frontend/static/js/compare.js`

## Risks and Mitigations

1. Risk: matching ambiguity after path or flag changes.
   - Mitigation: fallback matching and confidence scoring.
2. Risk: noisy deltas overwhelm users.
   - Mitigation: thresholding, severity filters, top-K summaries.
3. Risk: compare job cost spikes.
   - Mitigation: changed-unit prioritization and progressive materialization.

## Definition of Done

1. Compare engine handles large workloads with streaming.
2. Regression results are actionable and explainable.
3. API and UI support complete baseline-candidate workflow.

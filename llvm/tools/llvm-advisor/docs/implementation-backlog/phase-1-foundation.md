# Phase 1 Backlog: Foundation and Contracts

Status: Ready to execute  
Goal: establish stable data contracts, storage, planner skeleton, and API baseline.

## Scope

1. Snapshot and unit identity model.
2. Storage layout and CAS primitives.
3. Capability registry format and planner scaffold.
4. Async job orchestration skeleton.
5. Unversioned API skeleton with stable request/response envelopes.
6. Manifest-backed explorer and dashboard on indexed data-path.

## Exit Criteria

1. `Snapshot`, `Unit`, `CapabilityRun`, `ArtifactRef`, and `RepresentationRef` schemas are frozen for the unversioned contract.
2. CAS and metadata store can persist and recover after restart.
3. Planner can resolve dependencies and produce deterministic plan graph.
4. Job API can create, run, retry, cancel, and query status.
5. Dashboard and explorer read from the canonical API envelope and manifest-backed store.
6. End-to-end test: create snapshot from a medium project and query at least 5 capabilities.

## Backlog

| ID | Title | Description | Deliverables | Dependencies | Acceptance Criteria |
|---|---|---|---|---|---|
| P1-001 | Freeze Data Contracts | Define schema files for snapshot, unit, artifact, capability-run, compare-run. | Schema docs + JSON examples + version field policy. | none | `specs/snapshot-and-data-model.md` implemented in code types with validation. |
| P1-002 | Deterministic UnitID | Implement normalized command parser and `UnitID` hashing in C++ core. | `UnitIdentity` module + tests. | P1-001 | Same command yields same `UnitID`; irrelevant arg order changes do not change ID. |
| P1-003 | CAS Writer/Reader | Add content-addressed blob store with digest-based paths and atomic write protocol. | CAS module + cleanup policy + tests. | P1-001 | Writes are atomic, digest validated, duplicate writes deduplicated. |
| P1-004 | Metadata Store | Implement append-only metadata journal and compacted indexes for snapshots and units. | Metadata store module + compaction task. | P1-001 | Restart recovery passes corruption and interrupted-write tests. |
| P1-005 | Capability Registry Format | Implement loader for capability descriptors and semantic version checks. | `capabilities/*.json` parser + validator. | P1-001 | Invalid descriptors fail fast with explicit reason. |
| P1-006 | Planner Skeleton | Build DAG planner that resolves requested capability set + dependencies per unit. | Planner module + deterministic topological ordering. | P1-005 | Same input generates same ordered plan graph hash. |
| P1-007 | Job State Machine | Add async jobs with state transitions and idempotency keys. | Job table + worker loop + cancellation token. | P1-004 | State graph is enforced; duplicate idempotency key returns existing job. |
| P1-008 | API Envelope | Introduce stable response envelope with provenance and paging metadata. | API middleware + structured errors. | P1-001, P1-007 | All endpoints return unified envelope and structured errors. |
| P1-009 | Snapshot Creation Endpoint | Implement `POST /snapshots` and `GET /snapshots/{id}`. | API handlers + integration tests. | P1-004, P1-007 | Snapshot can be created and listed with build metadata. |
| P1-010 | Query Endpoint Skeleton | Implement `POST /query` request parsing and planner invocation. | Query endpoint + mock runner integration. | P1-006, P1-008 | Query returns plan status and materialized results for mock capabilities. |
| P1-011 | Production UI Integration | Wire dashboard and explorer to the canonical snapshot store without legacy endpoint shims. | Manifest-backed snapshot views + integration tests. | P1-008, P1-010 | Dashboard and explorer load end-to-end from the canonical store and API contracts only. |
| P1-012 | Baseline Perf Harness | Create benchmark harness for snapshot creation and query latency baseline. | benchmark scripts + datasets. | P1-009, P1-010 | Baseline numbers recorded and added to CI artifacts. |

## Engineering Tasks by Week

### Week 1

1. P1-001, P1-002, P1-003.
2. Unit tests for hashing and CAS idempotency.

### Week 2

1. P1-004, P1-005, P1-006.
2. Planner deterministic output tests.

### Week 3

1. P1-007, P1-008, P1-009.
2. API and job integration tests.

### Week 4

1. P1-010, P1-011, P1-012.
2. Freeze phase artifacts and tag `phase1-ready`.

## Code Placement Guidance

1. C++ core:
   - `src/Core/SnapshotManager.*`
   - `src/Core/UnitIdentity.*`
   - `src/Core/CapabilityPlanner.*`
   - `src/Core/JobManager.*`
2. Python API surface:
   - `tools/webserver/api/*`
   - `tools/common/store/*`
3. Capability descriptors:
   - `config/capabilities/*.json`

## Risks and Mitigations

1. Risk: schema churn breaks early clients.
   - Mitigation: strict envelope/schema tests in CI and coordinated contract changes.
2. Risk: metadata journal bloat.
   - Mitigation: mandatory compaction trigger and retention policy.
3. Risk: planner complexity inflates before first delivery.
   - Mitigation: implement minimal DAG planner first, no speculative optimization.

## Definition of Done

1. All phase tasks merged with tests.
2. CI includes schema-validation and recovery tests.
3. Documentation pages linked from `docs/README.md`.
4. Demo run recorded: snapshot create, query, and API response capture.

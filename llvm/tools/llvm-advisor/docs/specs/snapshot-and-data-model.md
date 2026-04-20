# Spec: Snapshot and Data Model

Version: 1.0  
Status: Normative

## 1. Purpose

Define canonical entities and identifiers used across capture, query, and compare.

## 2. Global Rules

1. Every top-level entity includes `schema_version`.
2. Every persisted record includes `created_at` and `producer_version`.
3. IDs are immutable and never reused.
4. Numeric metrics include explicit units.
5. All timestamps are UTC ISO-8601.

## 3. Entity Definitions

## 3.1 Project

```json
{
  "schema_version": "1.0",
  "project_id": "proj_01J...",
  "name": "llvm-project",
  "source_root": "/abs/path",
  "created_at": "2026-02-20T12:00:00Z"
}
```

## 3.2 Snapshot

```json
{
  "schema_version": "1.0",
  "snapshot_id": "snap_01J...",
  "project_id": "proj_01J...",
  "source_revision": {
    "vcs": "git",
    "commit": "abc123",
    "branch": "main",
    "dirty": false
  },
  "build_config": {
    "preset": "RelWithDebInfo",
    "generator": "Ninja",
    "target_triple": "x86_64-unknown-linux-gnu",
    "toolchain": "clang-22"
  },
  "profile": "balanced",
  "parent_snapshot_id": null,
  "created_at": "2026-02-20T12:01:00Z",
  "producer_version": "llvm-advisor/1.0.0"
}
```

## 3.3 Unit

```json
{
  "schema_version": "1.0",
  "snapshot_id": "snap_01J...",
  "unit_id": "unit_01J...",
  "source_path": "llvm/lib/IR/PassManager.cpp",
  "language": "c++",
  "target_triple": "x86_64-unknown-linux-gnu",
  "command_fingerprint": "sha256:...",
  "command_normalized": ["clang++", "-O2", "..."],
  "source_fingerprint": "sha256:...",
  "created_at": "2026-02-20T12:02:00Z"
}
```

## 3.4 CapabilityRun

```json
{
  "schema_version": "1.0",
  "cap_run_id": "capr_01J...",
  "snapshot_id": "snap_01J...",
  "unit_id": "unit_01J...",
  "capability_id": "llvm.ir.summary",
  "capability_version": "1.2.0",
  "state": "succeeded",
  "cache_key": "sha256:...",
  "started_at": "2026-02-20T12:03:00Z",
  "ended_at": "2026-02-20T12:03:01Z",
  "error": null,
  "artifact_refs": ["art_01J..."]
}
```

## 3.5 ArtifactRef

```json
{
  "schema_version": "1.0",
  "artifact_id": "art_01J...",
  "snapshot_id": "snap_01J...",
  "unit_id": "unit_01J...",
  "capability_id": "llvm.ir.summary",
  "content_digest": "sha256:...",
  "content_type": "application/json",
  "byte_size": 4211,
  "schema_id": "capability/llvm.ir.summary/output@1",
  "created_at": "2026-02-20T12:03:01Z"
}
```

## 3.6 MetricPoint

```json
{
  "schema_version": "1.0",
  "snapshot_id": "snap_01J...",
  "unit_id": "unit_01J...",
  "metric_key": "diag.warning.count",
  "value": 12,
  "unit": "count",
  "direction": "lower_is_better",
  "capability_id": "clang.diag.summary",
  "capability_version": "1.0.0"
}
```

## 4. Identifier Generation

1. `snapshot_id`: ULID string prefixed with `snap_`.
2. `unit_id`: ULID with deterministic alias index using command/source fingerprint.
3. `artifact_id`: ULID prefixed with `art_`.
4. `cap_run_id`: ULID prefixed with `capr_`.

## 5. Unit Identity Algorithm

`command_fingerprint` input:

1. compiler executable basename
2. normalized flags order (sort commutative flags, preserve ordered include paths)
3. `-D`, `-I`, target triple, stdlib, sysroot
4. language mode and optimization mode

`source_fingerprint` input:

1. content hash of source file
2. optional include dependency hash if available

`unit_identity_hash = sha256(command_fingerprint + source_fingerprint + target_triple)`

## 6. Schema Compatibility

1. Additive fields are allowed in minor revisions.
2. Field removals or semantic changes require major revision.
3. Readers must reject unknown major versions.

## 7. Validation Requirements

1. Validate schema on write and on API response serialization.
2. Reject records missing `schema_version`.
3. Reject metrics without explicit units/direction.

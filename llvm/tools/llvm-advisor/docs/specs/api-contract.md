# Spec: API Contract

Version: 1.0  
Status: Normative

## 1. Response Envelope

All responses:

```json
{
  "success": true,
  "data": {},
  "error": null,
  "meta": {
    "request_id": "req_01J...",
    "timestamp": "2026-02-20T12:00:00Z",
    "page": null
  }
}
```

Error shape:

```json
{
  "success": false,
  "data": null,
  "error": {
    "code": "policy_denied",
    "message": "Capability not enabled by profile",
    "details": {}
  },
  "meta": {"request_id": "req_01J..."}
}
```

## 2. Endpoints

## 2.1 Create Snapshot

`POST /snapshots`

Request:

```json
{
  "project_id": "proj_01J...",
  "build_dir": "/abs/build",
  "source_root": "/abs/src",
  "profile": "balanced",
  "metadata": {
    "preset": "RelWithDebInfo",
    "generator": "Ninja"
  }
}
```

Response:

1. returns created snapshot object.
2. may enqueue background capture job depending on policy.

## 2.2 Get Snapshot

`GET /snapshots/{snapshot_id}`

Response contains snapshot metadata and high-level coverage.

## 2.3 Query

`POST /query`

Request:

```json
{
  "snapshot_id": "snap_01J...",
  "scope": {
    "targets": ["llvm-tblgen"],
    "unit_glob": ["llvm/lib/IR/*"]
  },
  "capabilities": [
    "clang.diag.summary",
    "llvm.ir.summary"
  ],
  "options": {
    "allow_l2_augmentation": false,
    "max_cost_class": "moderate"
  },
  "pagination": {"limit": 100, "cursor": null}
}
```

Response includes:

1. materialized results
2. missing L2 capabilities
3. queued job references if async continuation is needed

## 2.4 Compare

`POST /compare`

Request:

```json
{
  "base_snapshot_id": "snap_base",
  "candidate_snapshot_id": "snap_candidate",
  "scope": {
    "targets": ["clang"],
    "unit_glob": ["clang/lib/Sema/*"]
  },
  "metrics": [
    "diag.warning.count",
    "remarks.total",
    "binary.text.bytes"
  ],
  "options": {
    "include_function_diff": true,
    "severity_threshold": "medium"
  }
}
```

Response:

1. immediate summary or compare job handle.
2. pagination cursors for large result sets.

## 2.5 Jobs

1. `POST /jobs`
2. `GET /jobs/{job_id}`
3. `POST /jobs/{job_id}/cancel`

## 2.6 Capabilities

`GET /capabilities`

Returns registry metadata, versions, and profile inclusion flags.

## 3. Pagination Contract

1. Cursor-based pagination only.
2. Response `meta.page`:
   - `limit`
   - `next_cursor`
   - `total_estimate`

## 4. Provenance Contract

Every metric or finding must include:

1. `snapshot_id`
2. `unit_id` if unit scoped
3. `capability_id`
4. `capability_version`
5. `artifact_id` or source reference

## 5. Compatibility

1. Additive fields are non-breaking.
2. Endpoint behavior changes require API minor bump and changelog.
3. Breaking changes replace the existing contract directly; no versioned path is maintained.

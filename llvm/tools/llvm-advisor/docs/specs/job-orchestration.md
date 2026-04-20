# Spec: Job Orchestration

Version: 1.0  
Status: Normative

## 1. Purpose

Define async execution model for snapshot creation, capability materialization, augmentation, and compare.

## 2. Job Types

1. `snapshot_create`
2. `materialize`
3. `augment_l2`
4. `compare`
5. `maintenance_compact`
6. `maintenance_gc`

## 3. Job Record

```json
{
  "job_id": "job_01J...",
  "job_type": "materialize",
  "state": "queued",
  "idempotency_key": "sha256:...",
  "request": {},
  "result_ref": null,
  "progress": {"total": 0, "done": 0},
  "error": null,
  "created_at": "2026-02-20T12:00:00Z",
  "started_at": null,
  "ended_at": null
}
```

## 4. State Machine

Allowed states:

1. `queued`
2. `running`
3. `succeeded`
4. `failed`
5. `cancel_requested`
6. `cancelled`

Transitions:

1. `queued -> running`
2. `running -> succeeded`
3. `running -> failed`
4. `running -> cancel_requested -> cancelled`
5. `queued -> cancelled` (admin/manual)

## 5. Idempotency

Rule:

1. if a new request has same `job_type + idempotency_key`, return existing active or finished job.
2. idempotency key must hash request payload + relevant policy version.

## 6. Worker Model

1. Separate worker pools by cost class:
   - `cheap_pool`
   - `moderate_pool`
   - `expensive_pool`
2. Worker claims jobs atomically via journal append claim record.
3. Heartbeat updates liveness.
4. Stale running jobs are re-queued after timeout.

## 7. Retry Policy

1. retriable errors: transient IO, temporary lock contention, network timeout.
2. non-retriable: schema mismatch, policy denied, unsupported capability.
3. default retries:
   - `cheap`: 3
   - `moderate`: 2
   - `expensive`: 1

## 8. Cancellation

1. cancellation token checked between plan nodes.
2. long-running node must support cooperative cancellation points.
3. partial outputs remain valid if node-level artifacts are committed atomically.

## 9. Progress Reporting

Required fields:

1. `plan_total_nodes`
2. `nodes_completed`
3. `nodes_failed`
4. `cache_hits`
5. `eta_seconds` (best effort)

## 10. Backpressure

Admission controls:

1. max active jobs global
2. max active expensive jobs per project
3. queue size threshold per job type

On overload:

1. reject with `429` + `retry_after_seconds`
2. preserve compare jobs by priority policy if configured

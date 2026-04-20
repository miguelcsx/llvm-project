# Spec: Test, Release, and Observability

Version: 1.0  
Status: Normative

## 1. Purpose

Define quality gates required for production rollout.

## 2. Test Strategy

## 2.1 Unit Tests

Must cover:

1. Unit identity normalization and hashing.
2. CAS atomic writes and deduplication.
3. planner DAG resolution and cycle detection.
4. job state transitions and retries.
5. compare delta and severity logic.

## 2.2 Integration Tests

Must cover:

1. create snapshot -> materialize -> query.
2. augmentation flow for missing L2 capabilities.
3. compare `S1` vs `S2` with mixed matched/added/removed units.
4. restart recovery from interrupted writes.

## 2.3 Scale Tests

Datasets:

1. small (100 units)
2. medium (5k units)
3. large (100k+ units)

Assertions:

1. bounded memory.
2. stable API latency percentile targets.
3. no correctness drift vs reference fixtures.

## 3. CI Release Gates

Mandatory gates:

1. schema compatibility checks.
2. migration forward/backward checks for metadata store.
3. benchmark regression checks.
4. security policy tests.

Release blocked if any gate fails.

## 4. Observability Requirements

## 4.1 Metrics

1. request latency (`p50`, `p95`, `p99`)
2. job throughput by type
3. cache hit ratio by capability
4. planner node counts
5. failure rate by capability and error code
6. queue depth and wait time

## 4.2 Logs

1. structured JSON logs only.
2. include `request_id`, `snapshot_id`, `job_id` when applicable.
3. redact file paths if policy requires.

## 4.3 Traces

1. query planning stage
2. capability execution stage
3. compare pipeline stage

## 5. SLO Targets (initial)

1. API availability: 99.9%
2. query p95 latency (cached): <= 2s
3. compare first partial result: <= 15s for large workloads
4. job failure rate (non-user errors): < 1%

## 6. Runbooks

Required runbooks:

1. metadata corruption recovery
2. CAS repair and reindex
3. queue overload mitigation
4. emergency rollback

# Spec: Storage and CAS Layout

Version: 1.0  
Status: Normative

## 1. Purpose

Define on-disk layout, atomicity rules, and retention behavior for metadata and artifacts.

## 2. Root Layout

Default root:

`<data-dir>/.llvm-advisor-store/`

```text
.llvm-advisor-store/
  metadata/
    journal/
      000001.jsonl
      000002.jsonl
    index/
      snapshots.json
      units.json
      capability_runs.json
      artifacts.json
  cas/
    sha256/
      ab/
        cd/
          abcdef...blob
  jobs/
    active/
    finished/
    failed/
  locks/
  tmp/
```

## 3. CAS Rules

1. Digest format: `sha256:<hex>`.
2. Path mapping: first 4 hex chars as two-level directory.
3. Write protocol:
   - write to `tmp/<uuid>.part`
   - fsync
   - rename to CAS final path
4. If destination exists:
   - verify digest and size
   - discard temp file

## 4. Metadata Journal Rules

1. Journal files are append-only JSONL segments.
2. Segment rollover by size threshold (default 128 MB).
3. Every record has `record_type`, `record_id`, `schema_version`.
4. Compaction builds fresh `index/*.json` snapshots from journal.

## 5. Recovery Rules

On startup:

1. Acquire process lock from `locks/`.
2. Load latest complete indexes.
3. Replay journal segments newer than index watermark.
4. Ignore trailing partial JSON line in last segment.
5. Validate referential links (`snapshot_id` exists for all unit records).

## 6. Index Requirements

Minimum indexes:

1. by `snapshot_id`
2. by `unit_id`
3. by `capability_id + capability_version`
4. by `artifact_digest`
5. by `job_id + state`

Indexes are immutable snapshots replaced atomically on compaction.

## 7. Retention Policy

Policy keys:

1. `max_snapshots_per_project`
2. `max_age_days`
3. `max_store_size_gb`

Eviction order:

1. old failed jobs
2. old compare outputs
3. snapshot metadata and unreferenced artifacts

CAS garbage collection:

1. mark referenced digests from live indexes
2. sweep unreferenced digests

## 8. Concurrency and Locking

1. Single writer for metadata journal.
2. Multiple readers for indexes and CAS.
3. Job workers coordinate via lock-free claim records in metadata journal.

## 9. Integrity Checks

Background tasks:

1. CAS digest verification sample scan.
2. index-to-journal consistency check.
3. orphan artifact detection.

## 10. Operational Controls

Config knobs:

1. `journal_rollover_mb`
2. `compaction_interval_minutes`
3. `gc_interval_minutes`
4. `max_parallel_writers`
5. `cas_verify_sample_rate`

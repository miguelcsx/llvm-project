from __future__ import annotations

import json
import os
import time
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from .api_contract import utc_timestamp
from .snapshot_records import _JOB_ACTIVE_STATES, JobRecord, generate_ulid_like_id


class SnapshotStoreJobsMixin:
    def list_jobs(self) -> List[JobRecord]:
        with self._lock:
            return [JobRecord.from_dict(payload) for payload in self._read_index(self._job_index_path)]

    def get_job(self, job_id: str) -> Optional[JobRecord]:
        return next((job for job in self.list_jobs() if job.job_id == job_id), None)

    def create_job(self, *, job_type: str, request_payload: Dict[str, Any], idempotency_key: str) -> JobRecord:
        with self._lock:
            for existing_job in self.list_jobs():
                if (
                    existing_job.job_type == job_type
                    and existing_job.idempotency_key == idempotency_key
                    and existing_job.state in _JOB_ACTIVE_STATES | {"succeeded"}
                ):
                    return existing_job
            job_record = JobRecord(
                schema_version="1.0",
                job_id=generate_ulid_like_id("job"),
                job_type=job_type,
                state="queued",
                idempotency_key=idempotency_key,
                request=request_payload,
                result_ref=None,
                progress={
                    "plan_total_nodes": 0,
                    "nodes_completed": 0,
                    "nodes_failed": 0,
                    "cache_hits": 0,
                    "eta_seconds": None,
                },
                error=None,
                created_at=utc_timestamp(),
                started_at=None,
                ended_at=None,
                heartbeat_at=None,
                retries=0,
                result=None,
            )
            jobs = self._read_index(self._job_index_path)
            jobs.append(job_record.to_dict())
            self._write_index(self._job_index_path, jobs)
            self._sync_job_state_marker(job_record)
            self._append_journal("job", job_record.job_id, job_record.to_dict())
            return job_record

    def claim_next_job(self) -> Optional[JobRecord]:
        with self._lock:
            jobs = [JobRecord.from_dict(payload) for payload in self._read_index(self._job_index_path)]
            claimed_job: Optional[JobRecord] = None
            updated_jobs: List[Dict[str, Any]] = []
            for job_record in jobs:
                if claimed_job is None and job_record.state == "queued":
                    claimed_job = JobRecord(
                        schema_version=job_record.schema_version,
                        job_id=job_record.job_id,
                        job_type=job_record.job_type,
                        state="running",
                        idempotency_key=job_record.idempotency_key,
                        request=job_record.request,
                        result_ref=job_record.result_ref,
                        progress=job_record.progress,
                        error=job_record.error,
                        created_at=job_record.created_at,
                        started_at=utc_timestamp(),
                        ended_at=None,
                        heartbeat_at=utc_timestamp(),
                        retries=job_record.retries,
                        result=job_record.result,
                    )
                    updated_jobs.append(claimed_job.to_dict())
                else:
                    updated_jobs.append(job_record.to_dict())
            if claimed_job is None:
                return None
            self._write_index(self._job_index_path, updated_jobs)
            self._sync_job_state_marker(claimed_job)
            self._append_journal("job_claim", claimed_job.job_id, claimed_job.to_dict())
            return claimed_job

    def update_job_progress(self, job_id: str, progress: Dict[str, Any]) -> Optional[JobRecord]:
        with self._lock:
            return self._replace_job(
                job_id,
                lambda job_record: JobRecord(
                    schema_version=job_record.schema_version,
                    job_id=job_record.job_id,
                    job_type=job_record.job_type,
                    state=job_record.state,
                    idempotency_key=job_record.idempotency_key,
                    request=job_record.request,
                    result_ref=job_record.result_ref,
                    progress=progress,
                    error=job_record.error,
                    created_at=job_record.created_at,
                    started_at=job_record.started_at,
                    ended_at=job_record.ended_at,
                    heartbeat_at=utc_timestamp(),
                    retries=job_record.retries,
                    result=job_record.result,
                ),
                "job_progress",
            )

    def complete_job(self, job_id: str, result: Dict[str, Any], result_ref: Optional[str] = None) -> Optional[JobRecord]:
        with self._lock:
            return self._replace_job(
                job_id,
                lambda job_record: JobRecord(
                    schema_version=job_record.schema_version,
                    job_id=job_record.job_id,
                    job_type=job_record.job_type,
                    state="succeeded",
                    idempotency_key=job_record.idempotency_key,
                    request=job_record.request,
                    result_ref=result_ref,
                    progress=job_record.progress,
                    error=None,
                    created_at=job_record.created_at,
                    started_at=job_record.started_at or utc_timestamp(),
                    ended_at=utc_timestamp(),
                    heartbeat_at=utc_timestamp(),
                    retries=job_record.retries,
                    result=result,
                ),
                "job_complete",
            )

    def fail_job(self, job_id: str, error: Dict[str, Any], *, retriable: bool = False) -> Optional[JobRecord]:
        with self._lock:
            job_record = self.get_job(job_id)
            if job_record is None:
                return None
            replacement = JobRecord(
                schema_version=job_record.schema_version,
                job_id=job_record.job_id,
                job_type=job_record.job_type,
                state="queued" if retriable else "failed",
                idempotency_key=job_record.idempotency_key,
                request=job_record.request,
                result_ref=job_record.result_ref,
                progress=job_record.progress,
                error=error,
                created_at=job_record.created_at,
                started_at=job_record.started_at,
                ended_at=None if retriable else utc_timestamp(),
                heartbeat_at=utc_timestamp(),
                retries=job_record.retries + 1 if retriable else job_record.retries,
                result=job_record.result,
            )
            return self._replace_job(job_id, lambda _: replacement, "job_fail")

    def request_job_cancel(self, job_id: str) -> Optional[JobRecord]:
        with self._lock:
            return self._replace_job(
                job_id,
                self._build_cancel_requested_job,
                "job_cancel_requested",
                allowed_states={"queued", "running"},
            )

    def cancel_job(self, job_id: str) -> Optional[JobRecord]:
        with self._lock:
            return self._replace_job(
                job_id,
                self._build_cancelled_job,
                "job_cancelled",
                allowed_states={"queued", "running", "cancel_requested"},
            )

    def heartbeat_job(self, job_id: str) -> Optional[JobRecord]:
        with self._lock:
            return self._replace_job(
                job_id,
                lambda job_record: JobRecord(
                    schema_version=job_record.schema_version,
                    job_id=job_record.job_id,
                    job_type=job_record.job_type,
                    state=job_record.state,
                    idempotency_key=job_record.idempotency_key,
                    request=job_record.request,
                    result_ref=job_record.result_ref,
                    progress=job_record.progress,
                    error=job_record.error,
                    created_at=job_record.created_at,
                    started_at=job_record.started_at,
                    ended_at=job_record.ended_at,
                    heartbeat_at=utc_timestamp(),
                    retries=job_record.retries,
                    result=job_record.result,
                ),
                "job_heartbeat",
                allowed_states={"running", "cancel_requested"},
            )

    def requeue_stale_jobs(self, *, timeout_seconds: int = 30) -> int:
        with self._lock:
            jobs = [JobRecord.from_dict(payload) for payload in self._read_index(self._job_index_path)]
            now = time.time()
            requeued_count = 0
            updated_jobs: List[Dict[str, Any]] = []
            for job_record in jobs:
                if job_record.state != "running" or not job_record.heartbeat_at:
                    updated_jobs.append(job_record.to_dict())
                    continue
                try:
                    heartbeat_epoch = datetime.fromisoformat(str(job_record.heartbeat_at).replace("Z", "+00:00")).astimezone(timezone.utc).timestamp()
                except ValueError:
                    updated_jobs.append(job_record.to_dict())
                    continue
                if now - heartbeat_epoch <= timeout_seconds:
                    updated_jobs.append(job_record.to_dict())
                    continue
                requeued = JobRecord(
                    schema_version=job_record.schema_version,
                    job_id=job_record.job_id,
                    job_type=job_record.job_type,
                    state="queued",
                    idempotency_key=job_record.idempotency_key,
                    request=job_record.request,
                    result_ref=job_record.result_ref,
                    progress=job_record.progress,
                    error=job_record.error,
                    created_at=job_record.created_at,
                    started_at=job_record.started_at,
                    ended_at=None,
                    heartbeat_at=utc_timestamp(),
                    retries=job_record.retries,
                    result=job_record.result,
                )
                updated_jobs.append(requeued.to_dict())
                self._sync_job_state_marker(requeued)
                self._append_journal("job_requeue", requeued.job_id, requeued.to_dict())
                requeued_count += 1
            if requeued_count:
                self._write_index(self._job_index_path, updated_jobs)
            return requeued_count

    def _replace_job(self, job_id: str, builder, journal_record_type: str, allowed_states: Optional[set[str]] = None) -> Optional[JobRecord]:
        jobs = [JobRecord.from_dict(payload) for payload in self._read_index(self._job_index_path)]
        replacement: Optional[JobRecord] = None
        updated_jobs: List[Dict[str, Any]] = []
        for job_record in jobs:
            if job_record.job_id != job_id:
                updated_jobs.append(job_record.to_dict())
                continue
            if allowed_states is not None and job_record.state not in allowed_states:
                updated_jobs.append(job_record.to_dict())
                continue
            replacement = builder(job_record)
            updated_jobs.append(replacement.to_dict())
        if replacement is None:
            return None
        self._write_index(self._job_index_path, updated_jobs)
        self._sync_job_state_marker(replacement)
        self._append_journal(journal_record_type, replacement.job_id, replacement.to_dict())
        return replacement

    def _build_cancel_requested_job(self, job_record: JobRecord) -> JobRecord:
        return JobRecord(
            schema_version=job_record.schema_version,
            job_id=job_record.job_id,
            job_type=job_record.job_type,
            state="cancel_requested",
            idempotency_key=job_record.idempotency_key,
            request=job_record.request,
            result_ref=job_record.result_ref,
            progress=job_record.progress,
            error=job_record.error,
            created_at=job_record.created_at,
            started_at=job_record.started_at,
            ended_at=None,
            heartbeat_at=utc_timestamp(),
            retries=job_record.retries,
            result=job_record.result,
        )

    def _build_cancelled_job(self, job_record: JobRecord) -> JobRecord:
        return JobRecord(
            schema_version=job_record.schema_version,
            job_id=job_record.job_id,
            job_type=job_record.job_type,
            state="cancelled",
            idempotency_key=job_record.idempotency_key,
            request=job_record.request,
            result_ref=job_record.result_ref,
            progress=job_record.progress,
            error=job_record.error,
            created_at=job_record.created_at,
            started_at=job_record.started_at,
            ended_at=utc_timestamp(),
            heartbeat_at=utc_timestamp(),
            retries=job_record.retries,
            result=job_record.result,
        )

    def _sync_job_state_marker(self, job_record: JobRecord) -> None:
        for marker_directory in (self._jobs_root / "active", self._jobs_root / "finished", self._jobs_root / "failed"):
            marker_path = marker_directory / f"{job_record.job_id}.json"
            if marker_path.exists():
                marker_path.unlink()
        if job_record.state in {"queued", "running", "cancel_requested"}:
            marker_directory = self._jobs_root / "active"
        elif job_record.state == "failed":
            marker_directory = self._jobs_root / "failed"
        else:
            marker_directory = self._jobs_root / "finished"
        marker_path = marker_directory / f"{job_record.job_id}.json"
        marker_path.write_text(json.dumps(job_record.to_dict(), indent=2, sort_keys=True) + "\n", encoding="utf-8")

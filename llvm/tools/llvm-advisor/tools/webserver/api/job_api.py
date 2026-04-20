from __future__ import annotations

from typing import TYPE_CHECKING, Any, Dict, Optional

from common.api_contract import build_error_envelope, build_success_envelope
from common.snapshot_store import JobRecord

if TYPE_CHECKING:
    from .service import APIService


class JobAPI:
    def __init__(self, service: "APIService") -> None:
        self._service = service

    def handle_list_jobs(self, request_id: str) -> Dict[str, Any]:
        return build_success_envelope(
            request_id=request_id,
            data={
                "jobs": [
                    job_record.to_dict()
                    for job_record in self._service._snapshot_store.list_jobs()
                ]
            },
        )

    def handle_create_job(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        job_type = str(request_payload.get("job_type") or "").strip()
        if job_type not in {
            "snapshot_create",
            "materialize",
            "augment_l2",
            "compare",
            "maintenance_compact",
            "maintenance_gc",
        }:
            return build_error_envelope(
                request_id=request_id,
                code="invalid_job_request",
                message="job_type must be one of snapshot_create, materialize, augment_l2, compare, maintenance_compact, maintenance_gc",
                status=400,
            )

        job_record = self._service._snapshot_store.create_job(
            job_type=job_type,
            request_payload=request_payload,
            idempotency_key=self._service._hash_payload(request_payload),
        )
        self._service._job_worker.wake()
        return build_success_envelope(
            request_id=request_id,
            data=job_record.to_dict(),
            status=201,
        )

    def handle_get_job(self, request_id: str, job_id: str) -> Dict[str, Any]:
        job_record = self._service._snapshot_store.get_job(job_id)
        if job_record is None:
            return build_error_envelope(
                request_id=request_id,
                code="job_not_found",
                message=f"Job '{job_id}' was not found",
                status=404,
            )
        return build_success_envelope(request_id=request_id, data=job_record.to_dict())

    def handle_cancel_job(self, request_id: str, job_id: str) -> Dict[str, Any]:
        job_record = self._service._snapshot_store.request_job_cancel(job_id)
        if job_record is None:
            return build_error_envelope(
                request_id=request_id,
                code="job_not_cancellable",
                message=f"Job '{job_id}' could not be cancelled",
                status=404,
            )
        return build_success_envelope(request_id=request_id, data=job_record.to_dict())

    def claim_next_job(self) -> Optional[JobRecord]:
        return self._service._snapshot_store.claim_next_job()

    def requeue_stale_jobs(self) -> None:
        self._service._snapshot_store.requeue_stale_jobs()

    def execute_job(self, job_record: JobRecord) -> None:
        try:
            if job_record.state == "cancel_requested":
                self._service._snapshot_store.cancel_job(job_record.job_id)
                return

            if job_record.job_type == "snapshot_create":
                self._run_snapshot_create(job_record)
                return
            if job_record.job_type in {"materialize", "augment_l2"}:
                self._run_query_job(job_record)
                return
            if job_record.job_type == "compare":
                self._run_compare_job(job_record)
                return
            if job_record.job_type == "maintenance_compact":
                self._service._snapshot_store.compact_indexes()
                self._complete_job(job_record.job_id, {"status": "completed"})
                return
            if job_record.job_type == "maintenance_gc":
                result = self._service._snapshot_store.run_garbage_collection()
                self._complete_job(job_record.job_id, result)
                return
            raise ValueError(f"Unsupported job type '{job_record.job_type}'")
        except Exception as exc:
            self._service._snapshot_store.fail_job(
                job_record.job_id,
                {"message": str(exc), "job_type": job_record.job_type},
                retriable=False,
            )

    def _run_snapshot_create(self, job_record: JobRecord) -> None:
        snapshot_response = self._service.handle_create_snapshot(
            request_id=job_record.job_id,
            request_payload=job_record.request,
        )
        self._complete_job(job_record.job_id, snapshot_response.get("data", {}))

    def _run_query_job(self, job_record: JobRecord) -> None:
        self._service._snapshot_store.heartbeat_job(job_record.job_id)
        request_payload = dict(job_record.request)
        if job_record.job_type == "augment_l2":
            request_policy = dict(request_payload.get("policy", {}))
            request_policy["include_l2"] = True
            request_payload["policy"] = request_policy
        query_response = self._service._query_api.run_query_request(
            request_payload,
            job_id=job_record.job_id,
        )
        if "error" in query_response:
            raise ValueError(query_response["error"]["message"])
        self._complete_job(job_record.job_id, query_response["data"])

    def _run_compare_job(self, job_record: JobRecord) -> None:
        self._service._snapshot_store.heartbeat_job(job_record.job_id)
        compare_response = self._service._compare_api.run_compare_request(job_record.request)
        if "error" in compare_response:
            raise ValueError(compare_response["error"]["message"])
        self._complete_job(job_record.job_id, compare_response["data"])

    def _complete_job(self, job_id: str, payload: Dict[str, Any]) -> None:
        self._service._snapshot_store.complete_job(job_id, payload)

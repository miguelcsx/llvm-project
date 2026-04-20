from __future__ import annotations

from typing import TYPE_CHECKING, Any, Dict, Optional

from common.api_contract import ResponsePage, build_error_envelope, build_success_envelope
from common.planner import PlannerError, PlannerPolicy, QueryScope

if TYPE_CHECKING:
    from .service import APIService


class QueryAPI:
    def __init__(self, service: "APIService") -> None:
        self._service = service

    def handle_query(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        query_outcome = self.run_query_request(request_payload)
        if "error" in query_outcome:
            return build_error_envelope(
                request_id=request_id,
                code=query_outcome["error"]["code"],
                message=query_outcome["error"]["message"],
                details=query_outcome["error"].get("details"),
                status=query_outcome["error"]["status"],
            )

        result_payload = query_outcome["data"]
        result_count = len(result_payload.get("results", []))
        return build_success_envelope(
            request_id=request_id,
            data=result_payload,
            page=ResponsePage(
                limit=result_count,
                next_cursor=None,
                total_estimate=result_count,
            ),
        )

    def run_query_request(
        self,
        request_payload: Dict[str, Any],
        *,
        job_id: Optional[str] = None,
    ) -> Dict[str, Any]:
        snapshot_id = str(request_payload.get("snapshot_id") or "").strip()
        if not snapshot_id:
            snapshot_id = self._service._snapshot_store.ensure_bootstrap_snapshot(
                data_dir=self._service._default_data_dir
            ).snapshot_id

        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return {
                "error": {
                    "code": "snapshot_not_found",
                    "message": f"Snapshot '{snapshot_id}' was not found",
                    "status": 404,
                }
            }

        requested_capabilities = [
            str(capability_id).strip()
            for capability_id in request_payload.get("capabilities", [])
            if str(capability_id).strip()
        ]
        requested_profile = str(
            request_payload.get("profile") or snapshot_record.profile or "balanced"
        ).strip()
        policy = PlannerPolicy.from_payload(request_payload.get("policy", {}))

        if not requested_capabilities:
            requested_capabilities = self._service._registry.resolve_capabilities(
                requested_profile,
                include_l2=policy.include_l2,
            )
        if not requested_capabilities:
            return {
                "error": {
                    "code": "invalid_query_request",
                    "message": "At least one capability is required or resolvable from the selected profile",
                    "status": 400,
                }
            }

        scope = QueryScope.from_payload(request_payload.get("scope", {}))
        try:
            plan = self._service._planner.build_plan(
                snapshot_id=snapshot_id,
                requested_capabilities=requested_capabilities,
                scope=scope,
                policy=policy,
            )
        except PlannerError as exc:
            return {
                "error": {
                    "code": exc.error_code,
                    "message": exc.hint,
                    "details": exc.to_dict(),
                    "status": 400,
                }
            }

        self._update_job_progress(job_id, plan)
        query_result = self._service._capability_engine.execute_plan(
            snapshot_id=snapshot_id,
            requested_capabilities=requested_capabilities,
            plan=plan,
        )
        self._update_job_progress(job_id, plan, completed=True)

        return {
            "data": {
                "snapshot_id": snapshot_id,
                "profile": requested_profile,
                "capabilities": requested_capabilities,
                "results": query_result.results,
                "missing_l2": list(plan.missing_l2),
                "plan": {
                    "node_count": plan.node_count,
                    "cache_hits": plan.cache_hits,
                    "plan_hash": plan.plan_hash,
                },
            }
        }

    def _update_job_progress(self, job_id: Optional[str], plan: Any, completed: bool = False) -> None:
        if not job_id:
            return
        self._service._snapshot_store.update_job_progress(
            job_id,
            {
                "plan_total_nodes": plan.node_count,
                "nodes_completed": plan.node_count if completed else plan.cache_hits,
                "nodes_failed": 0,
                "cache_hits": plan.cache_hits,
                "eta_seconds": 0 if completed else None,
            },
        )

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any, Dict, List, Optional

from common.api_contract import ResponsePage, build_error_envelope, build_success_envelope
from common.artifact_store import RepresentationStore
from common.capability_engine import CapabilityEngine
from common.capability_registry import CapabilityRegistry
from common.collector import ArtifactCollector
from common.native_capability_runner import NativeCapabilityRunner
from common.planner import Planner
from common.representation_registry import RepresentationRegistry
from common.snapshot_store import SnapshotRecord, SnapshotStore
from .compare_api import CompareAPI
from .job_api import JobAPI
from .job_worker import JobWorker
from .query_api import QueryAPI
from .representation_api import RepresentationAPI
from .snapshot_api import SnapshotAPI
from .snapshot_analysis_views import SnapshotAnalysisViews
from .snapshot_views import SnapshotViews


class APIService:
    def __init__(self, data_dir: Path, config_root: Path) -> None:
        self._default_data_dir = Path(data_dir)
        self._collector = ArtifactCollector()
        self._registry = CapabilityRegistry(config_root)
        self._representation_registry = RepresentationRegistry()
        self._snapshot_store = SnapshotStore(self._default_data_dir)
        self._planner = Planner(self._registry, self._snapshot_store)
        self._capability_engine = CapabilityEngine(self._snapshot_store)
        self._native_runner = NativeCapabilityRunner()
        self._snapshot_api = SnapshotAPI(self)
        self._representation_api = RepresentationAPI(self)
        self._compare_api = CompareAPI(self)
        self._query_api = QueryAPI(self)
        self._job_api = JobAPI(self)

        bootstrap_snapshot = self._snapshot_store.ensure_bootstrap_snapshot(
            data_dir=self._default_data_dir
        )
        self._snapshot_store.sync_snapshot_units(bootstrap_snapshot, self._collector)
        self._job_worker = JobWorker(self)

    def shutdown(self) -> None:
        self._job_worker.stop()

    def handle_health(self, request_id: str) -> Dict[str, Any]:
        active_jobs = [
            job_record.to_dict()
            for job_record in self._snapshot_store.list_jobs()
            if job_record.state in {"queued", "running", "cancel_requested"}
        ]
        return build_success_envelope(
            request_id=request_id,
            data={
                "status": "healthy",
                "snapshot_count": len(self._snapshot_store.list_snapshots()),
                "active_job_count": len(active_jobs),
                "data_dir": str(self._default_data_dir),
            },
        )

    def handle_capabilities(self, request_id: str) -> Dict[str, Any]:
        return build_success_envelope(
            request_id=request_id,
            data={
                "capabilities": [
                    descriptor.to_dict()
                    for descriptor in self._registry.list_capabilities()
                ],
                "profiles": [
                    profile.to_dict() for profile in self._registry.list_profiles()
                ],
            },
        )

    def handle_representation_capabilities(self, request_id: str) -> Dict[str, Any]:
        return self._representation_api.handle_representation_capabilities(request_id)

    def handle_snapshot_representation_request(
        self,
        request_id: str,
        snapshot_id: str,
        request_payload: Dict[str, Any],
    ) -> Dict[str, Any]:
        return self._representation_api.handle_snapshot_representation_request(
            request_id, snapshot_id, request_payload
        )
    def handle_list_snapshots(self, request_id: str) -> Dict[str, Any]:
        return self._snapshot_api.handle_list_snapshots(request_id)

    def handle_create_snapshot(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        return self._snapshot_api.handle_create_snapshot(request_id, request_payload)

    def handle_get_snapshot(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        return self._snapshot_api.handle_get_snapshot(request_id, snapshot_id)

    def handle_query(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        return self._query_api.handle_query(request_id, request_payload)

    def handle_list_jobs(self, request_id: str) -> Dict[str, Any]:
        return self._job_api.handle_list_jobs(request_id)

    def handle_create_job(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        return self._job_api.handle_create_job(request_id, request_payload)

    def handle_get_job(self, request_id: str, job_id: str) -> Dict[str, Any]:
        return self._job_api.handle_get_job(request_id, job_id)

    def handle_cancel_job(self, request_id: str, job_id: str) -> Dict[str, Any]:
        return self._job_api.handle_cancel_job(request_id, job_id)

    def handle_compare(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        return self._compare_api.handle_compare(request_id, request_payload)

    def handle_snapshot_health(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        return self._snapshot_api.handle_snapshot_health(request_id, snapshot_id)

    def handle_snapshot_summary(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        return self._snapshot_api.handle_snapshot_summary(request_id, snapshot_id)

    def handle_snapshot_units(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        return self._snapshot_api.handle_snapshot_units(request_id, snapshot_id)

    def handle_snapshot_unit_detail(
        self, request_id: str, snapshot_id: str, unit_name: str
    ) -> Dict[str, Any]:
        return self._snapshot_api.handle_snapshot_unit_detail(
            request_id, snapshot_id, unit_name
        )

    def handle_snapshot_representation_types(
        self, request_id: str, snapshot_id: str
    ) -> Dict[str, Any]:
        return self._snapshot_api.handle_snapshot_representation_types(
            request_id, snapshot_id
        )

    def handle_snapshot_representation_data(
        self, request_id: str, snapshot_id: str, representation_kind: str
    ) -> Dict[str, Any]:
        return self._snapshot_api.handle_snapshot_representation_data(
            request_id, snapshot_id, representation_kind
        )

    def handle_snapshot_files(
        self, request_id: str, snapshot_id: str, unit_name: Optional[str]
    ) -> Dict[str, Any]:
        return self._representation_api.handle_snapshot_files(
            request_id, snapshot_id, unit_name
        )

    def handle_snapshot_source(
        self,
        request_id: str,
        snapshot_id: str,
        file_path: str,
        unit_name: Optional[str] = None,
    ) -> Dict[str, Any]:
        return self._representation_api.handle_snapshot_source(
            request_id, snapshot_id, file_path, unit_name
        )

    def handle_snapshot_explorer_representation(
        self,
        request_id: str,
        snapshot_id: str,
        representation_kind: str,
        file_path: str,
        unit_name: Optional[str] = None,
    ) -> Dict[str, Any]:
        return self._representation_api.handle_snapshot_explorer_representation(
            request_id,
            snapshot_id,
            representation_kind,
            file_path,
            unit_name,
        )

    def handle_snapshot_representation_query(
        self,
        request_id: str,
        snapshot_id: str,
        query_params: Dict[str, List[str]],
    ) -> Dict[str, Any]:
        return self._representation_api.handle_snapshot_representation_query(
            request_id, snapshot_id, query_params
        )

    def handle_snapshot_analysis(
        self,
        request_id: str,
        snapshot_id: str,
        analysis_type: str,
        sub_endpoint: str,
        query_params: Dict[str, List[str]],
    ) -> Dict[str, Any]:
        return self._snapshot_api.handle_snapshot_analysis(
            request_id, snapshot_id, analysis_type, sub_endpoint, query_params
        )

    def claim_next_job(self) -> Optional[Any]:
        return self._job_api.claim_next_job()

    def requeue_stale_jobs(self) -> None:
        self._job_api.requeue_stale_jobs()

    def execute_job(self, job_record: Any) -> None:
        self._job_api.execute_job(job_record)

    def _build_snapshot_payload(
        self, snapshot_record: SnapshotRecord, include_units: bool = False
    ) -> Dict[str, Any]:
        units = self._snapshot_store.list_units(snapshot_record.snapshot_id)
        payload = snapshot_record.to_dict()
        payload["summary"] = {
            "unit_count": len(units),
            "capability_run_count": len(
                self._snapshot_store.list_capability_runs(snapshot_record.snapshot_id)
            ),
            "representation_count": len(
                self._snapshot_store.list_representations(snapshot_record.snapshot_id)
            ),
            "representation_descriptor_count": len(
                self._snapshot_store.list_representation_descriptors(
                    snapshot_record.snapshot_id
                )
            ),
        }
        if include_units:
            payload["units"] = [
                {
                    "unit_id": unit_record.unit_id,
                    "name": unit_record.unit_name,
                    "path": unit_record.unit_path,
                    "source_path": unit_record.source_path,
                    "language": unit_record.language,
                }
                for unit_record in units
            ]
        return payload

    def _make_representation_store(self, data_dir: Path) -> RepresentationStore:
        return RepresentationStore(str(data_dir), self._collector)

    def _build_snapshot_views(self, snapshot_record: SnapshotRecord) -> SnapshotViews:
        return SnapshotViews(
            data_dir=Path(snapshot_record.data_dir),
            collector=self._collector,
            representation_store=self._make_representation_store(
                Path(snapshot_record.data_dir)
            ),
        )

    def _build_snapshot_analysis_views(
        self, snapshot_record: SnapshotRecord
    ) -> SnapshotAnalysisViews:
        return SnapshotAnalysisViews(
            self._make_representation_store(Path(snapshot_record.data_dir))
        )

    def _snapshot_not_found(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        return build_error_envelope(
            request_id=request_id,
            code="snapshot_not_found",
            message=f"Snapshot '{snapshot_id}' was not found",
            status=404,
        )

    def _hash_payload(self, payload: Dict[str, Any]) -> str:
        return "sha256:" + hashlib.sha256(
            json.dumps(payload, sort_keys=True).encode("utf-8")
        ).hexdigest()

    def _resolve_unit_name(
        self,
        snapshot_id: str,
        source_ref: str,
        representation_kind: str,
        *,
        unit_name: Optional[str] = None,
    ) -> Optional[str]:
        descriptors = self._snapshot_store.list_representation_descriptors(
            snapshot_id,
            unit_name=unit_name,
            source_ref=source_ref,
            representation_kind=representation_kind,
        )
        if not descriptors and representation_kind == "source":
            descriptors = self._snapshot_store.list_representation_descriptors(
                snapshot_id,
                unit_name=unit_name,
                source_ref=source_ref,
            )
        if not descriptors:
            return None
        return descriptors[0].unit_name

    def _materialize_representation_payload(
        self,
        *,
        snapshot_record: SnapshotRecord,
        unit_name: str,
        source_ref: str,
        kind: str,
    ) -> Optional[Dict[str, Any]]:
        try:
            response = self._native_runner.materialize_representation(
                data_dir=snapshot_record.data_dir,
                unit_name=unit_name,
                source_ref=source_ref,
                kind=kind,
            )
        except RuntimeError:
            return None
        data = response.get("data")
        return dict(data) if isinstance(data, dict) else None

    def _correlate_signals_payload(
        self,
        *,
        snapshot_record: SnapshotRecord,
        unit_name: str,
        source_ref: str,
        kind: str,
    ) -> Optional[Dict[str, Any]]:
        try:
            response = self._native_runner.correlate_signals(
                data_dir=snapshot_record.data_dir,
                unit_name=unit_name,
                source_ref=source_ref,
                kind=kind,
            )
        except RuntimeError:
            return None
        data = response.get("data")
        return dict(data) if isinstance(data, dict) else None

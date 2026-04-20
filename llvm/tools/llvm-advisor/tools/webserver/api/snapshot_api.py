from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any, Dict

from common.api_contract import build_error_envelope, build_success_envelope

if TYPE_CHECKING:
    from .service import APIService


class SnapshotAPI:
    def __init__(self, service: "APIService") -> None:
        self._service = service

    def handle_list_snapshots(self, request_id: str) -> Dict[str, Any]:
        return build_success_envelope(
            request_id=request_id,
            data={
                "snapshots": [
                    self._service._build_snapshot_payload(snapshot_record)
                    for snapshot_record in self._service._snapshot_store.list_snapshots()
                ]
            },
        )

    def handle_create_snapshot(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        project_id = str(request_payload.get("project_id") or "").strip()
        source_root = str(request_payload.get("source_root") or "").strip()
        build_dir = str(request_payload.get("build_dir") or "").strip()
        profile = str(request_payload.get("profile") or "balanced").strip()
        data_dir = Path(
            str(
                request_payload.get("data_dir") or self._service._default_data_dir
            ).strip()
        )

        if not project_id or not source_root or not build_dir:
            return build_error_envelope(
                request_id=request_id,
                code="invalid_snapshot_request",
                message="project_id, source_root, and build_dir are required",
                status=400,
            )

        snapshot_record = self._service._snapshot_store.create_snapshot(
            project_id=project_id,
            source_root=source_root,
            build_dir=build_dir,
            data_dir=str(data_dir),
            profile=profile,
            metadata=dict(request_payload.get("metadata", {})),
        )
        self._service._snapshot_store.sync_snapshot_units(
            snapshot_record, self._service._collector
        )
        return build_success_envelope(
            request_id=request_id,
            data=self._service._build_snapshot_payload(
                snapshot_record, include_units=True
            ),
            status=201,
        )

    def handle_get_snapshot(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        return build_success_envelope(
            request_id=request_id,
            data=self._service._build_snapshot_payload(snapshot_record, include_units=True),
        )

    def handle_snapshot_health(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        return build_success_envelope(
            request_id=request_id,
            data=self._service._build_snapshot_views(snapshot_record).build_health(),
        )

    def handle_snapshot_summary(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        return build_success_envelope(
            request_id=request_id,
            data=self._service._build_snapshot_views(snapshot_record).build_summary(),
        )

    def handle_snapshot_units(self, request_id: str, snapshot_id: str) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        return build_success_envelope(
            request_id=request_id,
            data=self._service._build_snapshot_views(snapshot_record).build_units(),
        )

    def handle_snapshot_unit_detail(
        self, request_id: str, snapshot_id: str, unit_name: str
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        payload = self._service._build_snapshot_views(snapshot_record).build_unit_detail(
            unit_name
        )
        if payload is None:
            return build_error_envelope(
                request_id=request_id,
                code="unit_not_found",
                message=f"Compilation unit '{unit_name}' was not found",
                status=404,
            )
        return build_success_envelope(request_id=request_id, data=payload)

    def handle_snapshot_representation_types(
        self, request_id: str, snapshot_id: str
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        return build_success_envelope(
            request_id=request_id,
            data=self._service._build_snapshot_views(
                snapshot_record
            ).build_representation_kinds(),
        )

    def handle_snapshot_representation_data(
        self, request_id: str, snapshot_id: str, representation_kind: str
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        payload = self._service._build_snapshot_views(snapshot_record).build_representation_data(
            representation_kind
        )
        if payload is None:
            return build_error_envelope(
                request_id=request_id,
                code="invalid_representation_kind",
                message=f"Representation kind '{representation_kind}' is not supported",
                status=400,
            )
        return build_success_envelope(request_id=request_id, data=payload)

    def handle_snapshot_analysis(
        self,
        request_id: str,
        snapshot_id: str,
        analysis_type: str,
        sub_endpoint: str,
        query_params: Dict[str, list[str]],
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        payload = self._service._build_snapshot_analysis_views(snapshot_record).build(
            analysis_type,
            sub_endpoint,
            query_params,
        )
        if payload is None:
            return build_error_envelope(
                request_id=request_id,
                code="analysis_not_found",
                message=f"Analysis endpoint '{analysis_type}/{sub_endpoint}' is not available",
                status=404,
            )
        return build_success_envelope(request_id=request_id, data=payload)

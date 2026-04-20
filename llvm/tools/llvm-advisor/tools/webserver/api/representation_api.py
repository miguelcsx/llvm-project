from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any, Dict, Optional

from common.api_contract import build_error_envelope, build_success_envelope

if TYPE_CHECKING:
    from .service import APIService


class RepresentationAPI:
    def __init__(self, service: "APIService") -> None:
        self._service = service

    def handle_representation_capabilities(self, request_id: str) -> Dict[str, Any]:
        return build_success_envelope(
            request_id=request_id,
            data={
                "capabilities": [
                    capability.to_dict()
                    for capability in self._service._representation_registry.list_capabilities()
                ]
            },
        )

    def handle_snapshot_representation_request(
        self,
        request_id: str,
        snapshot_id: str,
        request_payload: Dict[str, Any],
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)

        action = str(request_payload.get("action") or "materialize").strip().lower()
        unit_name = str(request_payload.get("unit_name") or "").strip() or None
        source_ref = str(request_payload.get("source_ref") or "").strip()
        requests = request_payload.get("requests", [])
        include_inline_data = bool(request_payload.get("include_inline_data", True))
        if not source_ref or not isinstance(requests, list) or not requests:
            return build_error_envelope(
                request_id=request_id,
                code="invalid_representation_request",
                message="source_ref and at least one representation request are required",
                status=400,
            )
        if action not in {"plan", "materialize"}:
            return build_error_envelope(
                request_id=request_id,
                code="invalid_representation_request",
                message="action must be one of: plan, materialize",
                status=400,
            )

        available_kinds = {
            descriptor.representation_kind
            for descriptor in self._service._snapshot_store.list_representation_descriptors(
                snapshot_id,
                unit_name=unit_name,
                source_ref=source_ref,
            )
        }
        planned_requests = []
        for request_entry in requests:
            if not isinstance(request_entry, dict):
                continue
            kind = str(request_entry.get("kind") or "").strip()
            capability = self._service._representation_registry.get(kind)
            if capability is None:
                planned_requests.append(
                    {
                        "kind": kind,
                        "status": "unsupported",
                        "reason": f"Representation kind '{kind}' is not registered",
                        "action": action,
                    }
                )
                continue

            planned_requests.append(
                self._build_request_entry(
                    snapshot_record=snapshot_record,
                    snapshot_id=snapshot_id,
                    unit_name=unit_name,
                    source_ref=source_ref,
                    action=action,
                    include_inline_data=include_inline_data,
                    available_kinds=available_kinds,
                    request_entry=request_entry,
                    capability=capability,
                )
            )

        return build_success_envelope(
            request_id=request_id,
            data={
                "action": action,
                "snapshot_id": snapshot_id,
                "unit_name": unit_name,
                "source_ref": source_ref,
                "requests": planned_requests,
            },
        )

    def handle_snapshot_files(
        self, request_id: str, snapshot_id: str, unit_name: Optional[str]
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        if not unit_name:
            return build_error_envelope(
                request_id=request_id,
                code="invalid_request",
                message="unit query parameter is required",
                status=400,
            )
        files = self._service._snapshot_store.build_source_file_listing(
            snapshot_id,
            unit_name=unit_name,
        )
        if not files:
            return build_error_envelope(
                request_id=request_id,
                code="unit_not_found",
                message=f"Compilation unit '{unit_name}' was not found",
                status=404,
            )
        return build_success_envelope(
            request_id=request_id,
            data={"files": files, "count": len(files)},
        )

    def handle_snapshot_source(
        self,
        request_id: str,
        snapshot_id: str,
        file_path: str,
        unit_name: Optional[str] = None,
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        resolved_unit_name = self._service._resolve_unit_name(
            snapshot_id,
            file_path,
            "source",
            unit_name=unit_name,
        )
        if not resolved_unit_name:
            return build_error_envelope(
                request_id=request_id,
                code="source_not_found",
                message=f"Source file '{file_path}' was not found",
                status=404,
            )
        payload = self._service._materialize_representation_payload(
            snapshot_record=snapshot_record,
            unit_name=resolved_unit_name,
            source_ref=file_path,
            kind="source",
        )
        if payload is None:
            return build_error_envelope(
                request_id=request_id,
                code="source_not_found",
                message=f"Source file '{file_path}' was not found",
                status=404,
            )
        signals_payload = self._service._correlate_signals_payload(
            snapshot_record=snapshot_record,
            unit_name=resolved_unit_name,
            source_ref=file_path,
            kind="source",
        ) or {}
        payload["source"] = payload.pop("content")
        payload["inline_data"] = dict(signals_payload.get("source_signals") or {})
        return build_success_envelope(request_id=request_id, data=payload)

    def handle_snapshot_explorer_representation(
        self,
        request_id: str,
        snapshot_id: str,
        representation_kind: str,
        file_path: str,
        unit_name: Optional[str] = None,
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        resolved_unit_name = self._service._resolve_unit_name(
            snapshot_id,
            file_path,
            representation_kind,
            unit_name=unit_name,
        )
        if not resolved_unit_name:
            return build_error_envelope(
                request_id=request_id,
                code="representation_not_found",
                message=f"Representation '{representation_kind}' for '{file_path}' was not found",
                status=404,
            )
        payload = self._service._materialize_representation_payload(
            snapshot_record=snapshot_record,
            unit_name=resolved_unit_name,
            source_ref=file_path,
            kind=representation_kind,
        )
        if payload is None:
            return build_error_envelope(
                request_id=request_id,
                code="representation_not_found",
                message=f"Representation '{representation_kind}' for '{file_path}' was not found",
                status=404,
            )
        signals_payload = self._service._correlate_signals_payload(
            snapshot_record=snapshot_record,
            unit_name=resolved_unit_name,
            source_ref=file_path,
            kind=representation_kind,
        ) or {}
        payload["inline_data"] = dict(
            signals_payload.get("representation_signals") or {}
        )
        if "source_signals" in signals_payload:
            payload["source_signals"] = signals_payload["source_signals"]
        return build_success_envelope(request_id=request_id, data=payload)

    def handle_snapshot_representation_query(
        self,
        request_id: str,
        snapshot_id: str,
        query_params: Dict[str, list[str]],
    ) -> Dict[str, Any]:
        snapshot_record = self._service._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            return self._service._snapshot_not_found(request_id, snapshot_id)
        unit_name = query_params.get("unit", [None])[0]
        source_ref = query_params.get("source", [None])[0]
        representation_kind = query_params.get("kind", [None])[0]
        payload = self._service._snapshot_store.query_representation_catalog(
            snapshot_id,
            unit_name=unit_name,
            source_ref=source_ref,
            representation_kind=representation_kind,
        )
        return build_success_envelope(request_id=request_id, data=payload)

    def _build_request_entry(
        self,
        *,
        snapshot_record: Any,
        snapshot_id: str,
        unit_name: Optional[str],
        source_ref: str,
        action: str,
        include_inline_data: bool,
        available_kinds: set[str],
        request_entry: Dict[str, Any],
        capability: Any,
    ) -> Dict[str, Any]:
        kind = str(request_entry.get("kind") or "").strip()
        if kind == "diff":
            return self._build_diff_request_entry(
                snapshot_record=snapshot_record,
                snapshot_id=snapshot_id,
                unit_name=unit_name,
                source_ref=source_ref,
                action=action,
                available_kinds=available_kinds,
                capability=capability,
            )

        descriptor_count = 1 if kind in available_kinds else 0
        payload = None
        if descriptor_count and action == "materialize":
            resolved_unit_name = self._service._resolve_unit_name(
                snapshot_id,
                source_ref,
                kind,
                unit_name=unit_name,
            )
            if resolved_unit_name:
                payload = self._service._materialize_representation_payload(
                    snapshot_record=snapshot_record,
                    unit_name=resolved_unit_name,
                    source_ref=source_ref,
                    kind=kind,
                )
            if payload is not None and include_inline_data:
                signals_payload = self._service._correlate_signals_payload(
                    snapshot_record=snapshot_record,
                    unit_name=resolved_unit_name,
                    source_ref=source_ref,
                    kind=kind,
                ) or {}
                payload["inline_data"] = dict(
                    signals_payload.get("representation_signals") or {}
                )
                if "source_signals" in signals_payload:
                    payload["source_signals"] = signals_payload["source_signals"]

        return {
            "kind": kind,
            "status": "ready" if descriptor_count else "missing",
            "action": action,
            "materialization_mode": capability.materialization_mode,
            "cost_class": capability.cost_class,
            "descriptor_count": descriptor_count,
            "variant": str(request_entry.get("variant") or capability.default_variant),
            "options": dict(request_entry.get("options", {}))
            if isinstance(request_entry.get("options"), dict)
            else {},
            "payload": payload,
        }

    def _build_diff_request_entry(
        self,
        *,
        snapshot_record: Any,
        snapshot_id: str,
        unit_name: Optional[str],
        source_ref: str,
        action: str,
        available_kinds: set[str],
        capability: Any,
    ) -> Dict[str, Any]:
        base_kind = capability.diff_base_kind or "ir"
        ready = {base_kind, "optimized-ir"}.issubset(available_kinds)
        payload: Dict[str, Any] = {}
        if ready and action == "materialize":
            resolved_unit_name = self._service._resolve_unit_name(
                snapshot_id,
                source_ref,
                "diff",
                unit_name=unit_name,
            )
            if resolved_unit_name:
                payload = self._service._materialize_representation_payload(
                    snapshot_record=snapshot_record,
                    unit_name=resolved_unit_name,
                    source_ref=source_ref,
                    kind="diff",
                ) or {}
        return {
            "kind": "diff",
            "status": "ready" if ready else "unavailable",
            "action": action,
            "materialization_mode": capability.materialization_mode,
            "cost_class": capability.cost_class,
            "requires": [base_kind, "optimized-ir"],
            "payload": payload,
        }

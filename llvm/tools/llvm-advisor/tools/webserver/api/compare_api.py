from __future__ import annotations

from collections import Counter
from typing import TYPE_CHECKING, Any, Dict, List

from common.api_contract import build_error_envelope, build_success_envelope
from common.planner import PlannerError, QueryScope

if TYPE_CHECKING:
    from .service import APIService


class CompareAPI:
    def __init__(self, service: "APIService") -> None:
        self._service = service

    def handle_compare(
        self, request_id: str, request_payload: Dict[str, Any]
    ) -> Dict[str, Any]:
        compare_outcome = self.run_compare_request(request_payload)
        if "error" in compare_outcome:
            return build_error_envelope(
                request_id=request_id,
                code=compare_outcome["error"]["code"],
                message=compare_outcome["error"]["message"],
                details=compare_outcome["error"].get("details"),
                status=compare_outcome["error"]["status"],
            )
        return build_success_envelope(request_id=request_id, data=compare_outcome["data"])

    def run_compare_request(self, request_payload: Dict[str, Any]) -> Dict[str, Any]:
        base_snapshot_id = str(request_payload.get("base_snapshot_id") or "").strip()
        candidate_snapshot_id = str(request_payload.get("candidate_snapshot_id") or "").strip()
        capability_ids = [
            str(capability_id).strip()
            for capability_id in request_payload.get("capabilities", [])
            if str(capability_id).strip()
        ] or [
            "clang.diag.summary",
            "llvm.remarks.summary",
            "llvm.ir.summary",
        ]

        if not base_snapshot_id or not candidate_snapshot_id:
            return {
                "error": {
                    "code": "invalid_compare_request",
                    "message": "base_snapshot_id and candidate_snapshot_id are required",
                    "status": 400,
                }
            }

        if self._service._snapshot_store.get_snapshot(base_snapshot_id) is None:
            return self._snapshot_missing(base_snapshot_id)
        if self._service._snapshot_store.get_snapshot(candidate_snapshot_id) is None:
            return self._snapshot_missing(candidate_snapshot_id)

        try:
            payload = self._run_native_compare(
                base_snapshot_id=base_snapshot_id,
                candidate_snapshot_id=candidate_snapshot_id,
                capability_ids=capability_ids,
                scope_payload=dict(request_payload.get("scope", {})),
            )
        except (PlannerError, RuntimeError, KeyError) as exc:
            details = exc.to_dict() if isinstance(exc, PlannerError) else None
            return {
                "error": {
                    "code": "compare_failed"
                    if not isinstance(exc, PlannerError)
                    else exc.error_code,
                    "message": str(exc),
                    "details": details,
                    "status": 400 if isinstance(exc, PlannerError) else 500,
                }
            }
        return {"data": payload}

    def _run_native_compare(
        self,
        *,
        base_snapshot_id: str,
        candidate_snapshot_id: str,
        capability_ids: List[str],
        scope_payload: Dict[str, Any],
    ) -> Dict[str, Any]:
        base_snapshot = self._service._snapshot_store.get_snapshot(base_snapshot_id)
        candidate_snapshot = self._service._snapshot_store.get_snapshot(candidate_snapshot_id)
        if base_snapshot is None or candidate_snapshot is None:
            raise KeyError("compare snapshot is missing")

        scope = QueryScope.from_payload(scope_payload)
        base_units = [
            unit
            for unit in self._service._snapshot_store.list_units(base_snapshot_id)
            if scope.matches(unit)
        ]
        candidate_units = [
            unit
            for unit in self._service._snapshot_store.list_units(candidate_snapshot_id)
            if scope.matches(unit)
        ]
        base_units_by_name = {unit.unit_name: unit for unit in base_units}
        candidate_units_by_name = {unit.unit_name: unit for unit in candidate_units}
        matched_names = sorted(set(base_units_by_name) & set(candidate_units_by_name))
        added_names = sorted(set(candidate_units_by_name) - set(base_units_by_name))
        removed_names = sorted(set(base_units_by_name) - set(candidate_units_by_name))

        changes: List[Dict[str, Any]] = []
        changed_units = Counter()
        requested_unit_names = list(scope.unit_names) if scope.unit_names else [""]
        for capability_id in capability_ids:
            unit_targets = requested_unit_names if scope.unit_names else [""]
            for target_unit_name in unit_targets:
                native_compare = self._service._native_runner.compare(
                    base_data_dir=base_snapshot.data_dir,
                    candidate_data_dir=candidate_snapshot.data_dir,
                    capability_id=capability_id,
                    unit_name=target_unit_name,
                )
                self._append_native_changes(
                    changes=changes,
                    changed_units=changed_units,
                    native_compare=native_compare,
                    capability_id=capability_id,
                    base_units_by_name=base_units_by_name,
                    candidate_units_by_name=candidate_units_by_name,
                )

        changes.sort(
            key=lambda change: (
                self._change_rank(str(change.get("change_magnitude") or "stable")),
                abs(float(change.get("absolute_delta") or 0.0)),
            ),
            reverse=True,
        )

        changed_names = {
            str(change.get("scope", {}).get("candidate_unit_name") or "")
            for change in changes
            if str(change.get("change_magnitude") or "stable") != "stable"
        }
        changed_names.discard("")
        unchanged_unit_pairs = sum(
            1
            for unit_name in matched_names
            if (
                base_units_by_name[unit_name].command_fingerprint
                == candidate_units_by_name[unit_name].command_fingerprint
                and base_units_by_name[unit_name].source_fingerprint
                == candidate_units_by_name[unit_name].source_fingerprint
                and base_units_by_name[unit_name].target_triple
                == candidate_units_by_name[unit_name].target_triple
            )
        )

        return {
            "base_snapshot_id": base_snapshot_id,
            "candidate_snapshot_id": candidate_snapshot_id,
            "summary": {
                "change_count": len(changes),
                "change_magnitude_counts": dict(
                    Counter(str(item.get("change_magnitude") or "stable") for item in changes)
                ),
                "metric_counts": dict(Counter(str(item.get("metric_key") or "") for item in changes)),
                "match_counts": {
                    "matched": len(matched_names),
                    "changed": len(changed_names),
                    "added": len(added_names),
                    "removed": len(removed_names),
                },
                "coverage": {
                    "missing_base": 0,
                    "missing_candidate": 0,
                    "matched_units": len(matched_names),
                },
            },
            "incremental_reuse": {
                "unchanged_unit_pairs": unchanged_unit_pairs,
            },
            "unit_matches": self._build_unit_matches(
                matched_names,
                added_names,
                removed_names,
                base_units_by_name,
                candidate_units_by_name,
            ),
            "unit_change_leaderboard": [
                {"unit_name": unit_name, "change_count": count}
                for unit_name, count in changed_units.most_common(20)
            ],
            "top_changes": changes[:20],
            "changes": changes,
        }

    def _append_native_changes(
        self,
        *,
        changes: List[Dict[str, Any]],
        changed_units: Counter[str],
        native_compare: Dict[str, Any],
        capability_id: str,
        base_units_by_name: Dict[str, Any],
        candidate_units_by_name: Dict[str, Any],
    ) -> None:
        native_entries = native_compare.get("data", {}).get("changes", [])
        if not isinstance(native_entries, list):
            native_entries = native_compare.get("data", {}).get("findings", [])
        for native_change in native_entries:
            if not isinstance(native_change, dict):
                continue
            unit_name = str(native_change.get("unit_name") or "")
            metric_key = str(native_change.get("metric_key") or "")
            absolute_delta = float(native_change.get("absolute_delta") or 0.0)
            relative_delta = float(native_change.get("relative_delta") or 0.0)
            change_magnitude = str(
                native_change.get("change_magnitude")
                or self._classify_change_magnitude(relative_delta)
            )
            delta_sign = str(
                native_change.get("delta_sign")
                or self._classify_delta_sign(absolute_delta)
            )
            base_unit = base_units_by_name.get(unit_name)
            candidate_unit = candidate_units_by_name.get(unit_name)
            change = {
                "change_id": self._service._hash_payload(
                    {
                        "capability_id": capability_id,
                        "metric_key": metric_key,
                        "unit_name": unit_name,
                    }
                )[:32],
                "metric_key": metric_key,
                "scope": {
                    "base_unit_id": base_unit.unit_id if base_unit else "",
                    "candidate_unit_id": candidate_unit.unit_id if candidate_unit else "",
                    "base_unit_name": unit_name,
                    "candidate_unit_name": unit_name,
                },
                "match_category": "matched",
                "match_strategy": "unit_name",
                "base_value": float(native_change.get("base_value") or 0.0),
                "candidate_value": float(native_change.get("candidate_value") or 0.0),
                "absolute_delta": absolute_delta,
                "relative_delta": relative_delta,
                "delta_sign": delta_sign,
                "change_magnitude": change_magnitude,
                "unit": "count",
                "confidence": "high",
                "partial_coverage": False,
                "provenance": {
                    "capability_id": capability_id,
                },
            }
            changes.append(change)
            if change_magnitude != "stable":
                changed_units[unit_name] += 1

    def _build_unit_matches(
        self,
        matched_names: List[str],
        added_names: List[str],
        removed_names: List[str],
        base_units_by_name: Dict[str, Any],
        candidate_units_by_name: Dict[str, Any],
    ) -> List[Dict[str, Any]]:
        return [
            {
                "category": "matched",
                "confidence": "high",
                "strategy": "unit_name",
                "base_unit": self._unit_payload(base_units_by_name[unit_name], unit_name),
                "candidate_unit": self._unit_payload(
                    candidate_units_by_name[unit_name], unit_name
                ),
            }
            for unit_name in matched_names
        ] + [
            {
                "category": "removed",
                "confidence": "high",
                "strategy": "missing_candidate",
                "base_unit": self._unit_payload(base_units_by_name[unit_name], unit_name),
                "candidate_unit": None,
            }
            for unit_name in removed_names
        ] + [
            {
                "category": "added",
                "confidence": "high",
                "strategy": "missing_base",
                "base_unit": None,
                "candidate_unit": self._unit_payload(
                    candidate_units_by_name[unit_name], unit_name
                ),
            }
            for unit_name in added_names
        ]

    def _unit_payload(self, unit: Any, unit_name: str) -> Dict[str, Any]:
        return {
            "unit_id": unit.unit_id,
            "unit_name": unit_name,
            "source_path": unit.source_path,
            "language": unit.language,
            "target_triple": unit.target_triple,
        }

    def _classify_delta_sign(self, absolute_delta: float) -> str:
        if absolute_delta > 0:
            return "increase"
        if absolute_delta < 0:
            return "decrease"
        return "stable"

    def _classify_change_magnitude(self, relative_delta: float) -> str:
        magnitude = abs(relative_delta)
        if magnitude >= 1.0:
            return "very_high"
        if magnitude >= 0.5:
            return "high"
        if magnitude >= 0.2:
            return "medium"
        if magnitude >= 0.05:
            return "low"
        return "stable"

    def _change_rank(self, change_magnitude: str) -> int:
        return {
            "stable": 0,
            "low": 1,
            "medium": 2,
            "high": 3,
            "very_high": 4,
        }.get(change_magnitude, 0)

    def _snapshot_missing(self, snapshot_id: str) -> Dict[str, Any]:
        return {
            "error": {
                "code": "snapshot_not_found",
                "message": f"Snapshot '{snapshot_id}' was not found",
                "status": 404,
            }
        }

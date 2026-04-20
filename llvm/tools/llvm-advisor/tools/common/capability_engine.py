from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Tuple

from .planner import PlanGraph, PlanNode
from .snapshot_store import SnapshotStore, UnitRecord
from .native_capability_runner import NativeCapabilityRunner


@dataclass(frozen=True)
class QueryResult:
    snapshot_id: str
    plan: PlanGraph
    results: List[Dict[str, Any]]


class CapabilityEngine:
    def __init__(self, snapshot_store: SnapshotStore) -> None:
        self._snapshot_store = snapshot_store
        self._native_runner = NativeCapabilityRunner()

    def execute_plan(
        self,
        *,
        snapshot_id: str,
        requested_capabilities: List[str],
        plan: PlanGraph,
    ) -> QueryResult:
        snapshot_record = self._snapshot_store.get_snapshot(snapshot_id)
        if snapshot_record is None:
            raise KeyError(snapshot_id)

        units_by_id = {
            unit_record.unit_id: unit_record
            for unit_record in self._snapshot_store.list_units(snapshot_id)
        }
        nodes_by_key = {node.key(): node for node in plan.nodes}

        for node_key in plan.ordered_keys:
            node = nodes_by_key[node_key]
            if node.state != "ready":
                continue
            unit_record = units_by_id.get(node.unit_id)
            if unit_record is None:
                continue
            capability_response = self._native_runner.execute(
                data_dir=snapshot_record.data_dir,
                capability_id=node.capability_id,
                unit_name=unit_record.unit_name,
            )
            capability_payload = dict(capability_response.get("data", {}))
            schema_id = f"capability/{node.capability_id}/output@1"
            self._snapshot_store.store_capability_result(
                snapshot_id=snapshot_id,
                unit_id=node.unit_id,
                capability_id=node.capability_id,
                capability_version=node.capability_version,
                cache_key=node.cache_key,
                schema_id=schema_id,
                payload=capability_payload,
            )

        aggregate_results = [
            self._aggregate_capability(snapshot_id, capability_id, plan, units_by_id)
            for capability_id in requested_capabilities
        ]
        return QueryResult(
            snapshot_id=snapshot_id,
            plan=plan,
            results=aggregate_results,
        )

    def _aggregate_capability(
        self,
        snapshot_id: str,
        capability_id: str,
        plan: PlanGraph,
        units_by_id: Dict[str, UnitRecord],
    ) -> Dict[str, Any]:
        matching_nodes = [
            node
            for node in plan.nodes
            if node.capability_id == capability_id and node.state in {"ready", "cached"}
        ]
        unit_payloads: List[Tuple[UnitRecord, Dict[str, Any]]] = []
        for node in matching_nodes:
            capability_run = self._snapshot_store.get_cached_capability_run(
                snapshot_id=snapshot_id,
                unit_id=node.unit_id,
                capability_id=node.capability_id,
                capability_version=node.capability_version,
                cache_key=node.cache_key,
            )
            if capability_run is None or not capability_run.representation_refs:
                continue
            representation_payload = self._snapshot_store.read_json_representation(
                capability_run.representation_refs[0]
            )
            if representation_payload is None:
                continue
            unit_payloads.append((units_by_id[node.unit_id], representation_payload))

        return {
            "capability_id": capability_id,
            "capability_version": matching_nodes[0].capability_version if matching_nodes else "1.0.0",
            "scope": "snapshot",
            "data": self._merge_capability_payloads(capability_id, unit_payloads),
            "units": [
                {
                    "unit_id": unit_record.unit_id,
                    "unit_name": unit_record.unit_name,
                    "data": payload,
                }
                for unit_record, payload in unit_payloads
            ],
        }

    def _merge_capability_payloads(
        self,
        capability_id: str,
        unit_payloads: List[Tuple[UnitRecord, Dict[str, Any]]],
    ) -> Dict[str, Any]:
        if capability_id == "build.command.meta":
            return {
                "unit_count": len(unit_payloads),
                "units": [unit_record.unit_name for unit_record, _ in unit_payloads],
            }

        if capability_id == "clang.diag.summary":
            aggregate = Counter()
            top_files = Counter()
            for _unit_record, payload in unit_payloads:
                aggregate.update(
                    {
                        "warning_count": int(payload.get("warning_count", 0)),
                        "error_count": int(payload.get("error_count", 0)),
                        "note_count": int(payload.get("note_count", 0)),
                        "info_count": int(payload.get("info_count", 0)),
                        "total": int(payload.get("total", 0)),
                    }
                )
                top_files.update(payload.get("top_files", {}))
            merged = dict(aggregate)
            merged["top_files"] = dict(top_files.most_common(10))
            return merged

        if capability_id == "llvm.remarks.summary":
            totals = Counter()
            pass_counts = Counter()
            function_counts = Counter()
            file_counts = Counter()
            for _unit_record, payload in unit_payloads:
                totals["total"] += int(payload.get("total", 0))
                pass_counts.update(payload.get("top_passes", {}))
                function_counts.update(payload.get("top_functions", {}))
                file_counts.update(payload.get("top_files", {}))
            return {
                "total": totals["total"],
                "top_passes": dict(pass_counts.most_common(10)),
                "top_functions": dict(function_counts.most_common(10)),
                "top_files": dict(file_counts.most_common(10)),
            }

        if capability_id == "llvm.ir.summary":
            totals = Counter()
            for _unit_record, payload in unit_payloads:
                totals.update(
                    {
                        "ir_file_count": int(payload.get("ir_file_count", 0)),
                        "function_count": int(payload.get("function_count", 0)),
                        "global_count": int(payload.get("global_count", 0)),
                        "type_count": int(payload.get("type_count", 0)),
                        "instruction_count": int(payload.get("instruction_count", 0)),
                    }
                )
            return dict(totals)

        if capability_id == "clang.ast.summary":
            totals = Counter()
            decl_kinds = Counter()
            for _unit_record, payload in unit_payloads:
                totals.update(
                    {
                        "decl_count": int(payload.get("decl_count", 0)),
                        "function_count": int(payload.get("function_count", 0)),
                        "record_count": int(payload.get("record_count", 0)),
                        "namespace_count": int(payload.get("namespace_count", 0)),
                        "template_count": int(payload.get("template_count", 0)),
                        "variable_count": int(payload.get("variable_count", 0)),
                    }
                )
                decl_kinds.update(payload.get("top_decl_kinds", {}))
            merged = dict(totals)
            merged["top_decl_kinds"] = dict(decl_kinds.most_common(12))
            return merged

        if capability_id == "llvm.pass.stats":
            totals = Counter()
            pass_counts = Counter()
            function_counts = Counter()
            for _unit_record, payload in unit_payloads:
                totals.update(
                    {
                        "remark_count": int(payload.get("remark_count", 0)),
                        "unique_pass_count": int(payload.get("unique_pass_count", 0)),
                        "unique_function_count": int(
                            payload.get("unique_function_count", 0)
                        ),
                        "hottest_pass_count": int(payload.get("hottest_pass_count", 0)),
                        "hottest_function_count": int(
                            payload.get("hottest_function_count", 0)
                        ),
                    }
                )
                pass_counts.update(payload.get("pass_counts", {}))
                function_counts.update(payload.get("function_counts", {}))
            merged = dict(totals)
            merged["pass_counts"] = dict(pass_counts.most_common(12))
            merged["function_counts"] = dict(function_counts.most_common(12))
            return merged

        if capability_id == "llvm.obj.summary":
            totals = Counter()
            sections = Counter()
            for _unit_record, payload in unit_payloads:
                totals.update(
                    {
                        "symbol_count": int(payload.get("symbol_count", 0)),
                        "function_symbol_count": int(
                            payload.get("function_symbol_count", 0)
                        ),
                        "external_symbol_count": int(
                            payload.get("external_symbol_count", 0)
                        ),
                        "section_count": int(payload.get("section_count", 0)),
                        "total_bytes": int(payload.get("total_bytes", 0)),
                        "text_bytes": int(payload.get("text_bytes", 0)),
                    }
                )
                sections.update(payload.get("sections", {}))
            merged = dict(totals)
            merged["sections"] = dict(sections.most_common(12))
            return merged

        if capability_id == "llvm.debug.summary":
            totals = Counter()
            tags = Counter()
            for _unit_record, payload in unit_payloads:
                totals.update(
                    {
                        "debug_section_count": int(
                            payload.get("debug_section_count", 0)
                        ),
                        "tag_count": int(payload.get("tag_count", 0)),
                        "compile_unit_count": int(
                            payload.get("compile_unit_count", 0)
                        ),
                        "subprogram_count": int(payload.get("subprogram_count", 0)),
                        "variable_count": int(payload.get("variable_count", 0)),
                    }
                )
                tags.update(payload.get("top_tags", {}))
            merged = dict(totals)
            merged["top_tags"] = dict(tags.most_common(12))
            return merged

        return {"unit_count": len(unit_payloads)}

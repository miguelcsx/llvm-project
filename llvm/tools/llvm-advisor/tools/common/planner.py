from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Set, Tuple

from .capability_registry import CapabilityRegistry, CapabilityDescriptor
from .snapshot_store import SnapshotStore, UnitRecord

_COST_ORDER = {"cheap": 0, "moderate": 1, "expensive": 2}


@dataclass(frozen=True)
class QueryScope:
    unit_names: Tuple[str, ...] = ()

    @classmethod
    def from_payload(cls, payload: Any) -> "QueryScope":
        if not isinstance(payload, dict):
            return cls()
        return cls(
            unit_names=tuple(
                sorted(
                    str(unit_name).strip()
                    for unit_name in payload.get("unit_names", [])
                    if str(unit_name).strip()
                )
            )
        )

    def matches(self, unit_record: UnitRecord) -> bool:
        if not self.unit_names:
            return True
        return unit_record.unit_name in self.unit_names


@dataclass(frozen=True)
class PlannerPolicy:
    max_cost_class: str = "moderate"
    include_l2: bool = False

    @classmethod
    def from_payload(cls, payload: Any) -> "PlannerPolicy":
        if not isinstance(payload, dict):
            return cls()
        return cls(
            max_cost_class=str(payload.get("max_cost_class", "moderate")),
            include_l2=bool(payload.get("include_l2", False)),
        )

    def allows_cost(self, cost_class: str) -> bool:
        return _COST_ORDER.get(cost_class, 99) <= _COST_ORDER.get(
            self.max_cost_class, 99
        )


@dataclass(frozen=True)
class PlanNode:
    unit_id: str
    unit_name: str
    capability_id: str
    capability_version: str
    cache_key: str
    state: str
    dependencies: Tuple[Tuple[str, str], ...]
    cost_class: str

    def key(self) -> Tuple[str, str]:
        return (self.unit_id, self.capability_id)


@dataclass(frozen=True)
class PlanGraph:
    snapshot_id: str
    requested_capabilities: Tuple[str, ...]
    unit_ids: Tuple[str, ...]
    nodes: Tuple[PlanNode, ...]
    ordered_keys: Tuple[Tuple[str, str], ...]
    missing_l2: Tuple[Dict[str, Any], ...]
    plan_hash: str

    @property
    def node_count(self) -> int:
        return len(self.nodes)

    @property
    def cache_hits(self) -> int:
        return sum(1 for node in self.nodes if node.state == "cached")


class PlannerError(ValueError):
    def __init__(self, error_code: str, capability_id: str, hint: str) -> None:
        super().__init__(hint)
        self.error_code = error_code
        self.capability_id = capability_id
        self.hint = hint

    def to_dict(self) -> Dict[str, str]:
        return {
            "error_code": self.error_code,
            "capability_id": self.capability_id,
            "hint": self.hint,
        }


class Planner:
    def __init__(self, registry: CapabilityRegistry, snapshot_store: SnapshotStore) -> None:
        self._registry = registry
        self._snapshot_store = snapshot_store

    def build_plan(
        self,
        *,
        snapshot_id: str,
        requested_capabilities: List[str],
        scope: QueryScope,
        policy: PlannerPolicy,
    ) -> PlanGraph:
        selected_units = [
            unit_record
            for unit_record in self._snapshot_store.list_units(snapshot_id)
            if scope.matches(unit_record)
        ]
        requested = tuple(sorted(set(requested_capabilities)))
        closure = self._expand_capability_closure(requested)

        nodes: List[PlanNode] = []
        missing_l2: List[Dict[str, Any]] = []

        for unit_record in sorted(selected_units, key=lambda unit: unit.unit_name):
            for capability_id in closure:
                descriptor = self._get_descriptor(capability_id)
                if "unit" not in descriptor.supports_scope:
                    continue

                if not policy.allows_cost(descriptor.cost_class):
                    missing_l2.append(
                        {
                            "unit_id": unit_record.unit_id,
                            "unit_name": unit_record.unit_name,
                            "capability_id": capability_id,
                            "reason": "skipped_by_policy",
                            "hint": f"Increase max_cost_class to allow '{descriptor.cost_class}' capabilities.",
                        }
                    )
                    state = "skipped_by_policy"
                else:
                    state = "ready"

                cache_key = self._build_cache_key(unit_record, descriptor, policy)
                if (
                    state == "ready"
                    and self._snapshot_store.get_cached_capability_run(
                        snapshot_id=snapshot_id,
                        unit_id=unit_record.unit_id,
                        capability_id=descriptor.capability_id,
                        capability_version=descriptor.capability_version,
                        cache_key=cache_key,
                    )
                    is not None
                ):
                    state = "cached"

                nodes.append(
                    PlanNode(
                        unit_id=unit_record.unit_id,
                        unit_name=unit_record.unit_name,
                        capability_id=descriptor.capability_id,
                        capability_version=descriptor.capability_version,
                        cache_key=cache_key,
                        state=state,
                        dependencies=tuple(
                            sorted((unit_record.unit_id, dependency) for dependency in descriptor.depends_on)
                        ),
                        cost_class=descriptor.cost_class,
                    )
                )

        ordered_keys = tuple(self._topological_order(nodes))
        plan_hash = self._hash_payload(
            {
                "snapshot_id": snapshot_id,
                "requested_capabilities": requested,
                "units": [unit.unit_id for unit in selected_units],
                "nodes": [
                    {
                        "unit_id": node.unit_id,
                        "capability_id": node.capability_id,
                        "cache_key": node.cache_key,
                        "state": node.state,
                    }
                    for node in sorted(nodes, key=lambda item: (item.unit_name, item.capability_id))
                ],
            }
        )
        return PlanGraph(
            snapshot_id=snapshot_id,
            requested_capabilities=requested,
            unit_ids=tuple(unit.unit_id for unit in selected_units),
            nodes=tuple(nodes),
            ordered_keys=ordered_keys,
            missing_l2=tuple(missing_l2),
            plan_hash=plan_hash,
        )

    def _expand_capability_closure(self, requested: Tuple[str, ...]) -> Tuple[str, ...]:
        resolved: List[str] = []
        visiting: Set[str] = set()
        visited: Set[str] = set()

        def visit(capability_id: str) -> None:
            if capability_id in visited:
                return
            if capability_id in visiting:
                raise PlannerError(
                    "cyclic_dependency",
                    capability_id,
                    f"Capability dependency cycle detected at '{capability_id}'.",
                )

            descriptor = self._get_descriptor(capability_id)
            visiting.add(capability_id)
            for dependency in descriptor.depends_on:
                visit(dependency)
            visiting.remove(capability_id)
            visited.add(capability_id)
            resolved.append(capability_id)

        for capability_id in requested:
            visit(capability_id)
        return tuple(resolved)

    def _get_descriptor(self, capability_id: str) -> CapabilityDescriptor:
        descriptor = self._registry.get_capability(capability_id)
        if descriptor is None:
            raise PlannerError(
                "unknown_capability",
                capability_id,
                f"Capability '{capability_id}' is not registered.",
            )
        return descriptor

    def _build_cache_key(
        self,
        unit_record: UnitRecord,
        descriptor: CapabilityDescriptor,
        policy: PlannerPolicy,
    ) -> str:
        payload = {
            "unit_id": unit_record.unit_id,
            "command_fingerprint": unit_record.command_fingerprint,
            "source_fingerprint": unit_record.source_fingerprint,
            "capability_id": descriptor.capability_id,
            "capability_version": descriptor.capability_version,
            "depends_on": descriptor.depends_on,
            "policy": {
                "max_cost_class": policy.max_cost_class,
                "include_l2": policy.include_l2,
            },
        }
        return "sha256:" + hashlib.sha256(
            json.dumps(payload, sort_keys=True).encode("utf-8")
        ).hexdigest()

    def _topological_order(self, nodes: List[PlanNode]) -> List[Tuple[str, str]]:
        nodes_by_key = {node.key(): node for node in nodes}
        temporary: Set[Tuple[str, str]] = set()
        permanent: Set[Tuple[str, str]] = set()
        ordered: List[Tuple[str, str]] = []

        def visit(node_key: Tuple[str, str]) -> None:
            if node_key in permanent:
                return
            if node_key in temporary:
                raise PlannerError(
                    "cyclic_dependency",
                    node_key[1],
                    f"Plan cycle detected at '{node_key[1]}'.",
                )
            temporary.add(node_key)
            node = nodes_by_key[node_key]
            for dependency_key in node.dependencies:
                if dependency_key in nodes_by_key:
                    visit(dependency_key)
            temporary.remove(node_key)
            permanent.add(node_key)
            ordered.append(node_key)

        for node in sorted(nodes, key=lambda item: (item.unit_name, item.capability_id)):
            visit(node.key())
        return ordered

    def _hash_payload(self, payload: Dict[str, Any]) -> str:
        return "sha256:" + hashlib.sha256(
            json.dumps(payload, sort_keys=True).encode("utf-8")
        ).hexdigest()

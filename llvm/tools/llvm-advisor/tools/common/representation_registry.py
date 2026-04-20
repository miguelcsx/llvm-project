from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Optional


@dataclass(frozen=True)
class RepresentationCapability:
    kind: str
    label: str
    syntax: str
    category: str
    display_rank: int
    materialization_mode: str
    cache_policy: str
    cost_class: str
    supports_time_compare: bool = True
    supports_range_queries: bool = False
    supports_line_mapping: bool = False
    diff_base_kind: Optional[str] = None
    default_variant: str = "default"

    def to_dict(self) -> Dict[str, Any]:
        return {
            "kind": self.kind,
            "label": self.label,
            "syntax": self.syntax,
            "category": self.category,
            "display_rank": self.display_rank,
            "materialization_mode": self.materialization_mode,
            "cache_policy": self.cache_policy,
            "cost_class": self.cost_class,
            "supports_time_compare": self.supports_time_compare,
            "supports_range_queries": self.supports_range_queries,
            "supports_line_mapping": self.supports_line_mapping,
            "diff_base_kind": self.diff_base_kind,
            "default_variant": self.default_variant,
        }


class RepresentationRegistry:
    def __init__(self) -> None:
        self._capabilities = {
            capability.kind: capability
            for capability in (
                RepresentationCapability(
                    kind="source",
                    label="Source",
                    syntax="cpp",
                    category="source",
                    display_rank=0,
                    materialization_mode="eager",
                    cache_policy="snapshot",
                    cost_class="low",
                    supports_range_queries=True,
                    supports_line_mapping=True,
                ),
                RepresentationCapability(
                    kind="assembly",
                    label="Assembly",
                    syntax="assembly",
                    category="explorer",
                    display_rank=10,
                    materialization_mode="lazy",
                    cache_policy="snapshot",
                    cost_class="moderate",
                    supports_range_queries=True,
                    supports_line_mapping=True,
                ),
                RepresentationCapability(
                    kind="ir",
                    label="LLVM IR",
                    syntax="llvm-ir",
                    category="explorer",
                    display_rank=20,
                    materialization_mode="lazy",
                    cache_policy="snapshot",
                    cost_class="moderate",
                    supports_range_queries=True,
                    supports_line_mapping=True,
                ),
                RepresentationCapability(
                    kind="optimized-ir",
                    label="Optimized IR",
                    syntax="llvm-ir",
                    category="explorer",
                    display_rank=30,
                    materialization_mode="lazy",
                    cache_policy="snapshot",
                    cost_class="high",
                    supports_range_queries=True,
                    supports_line_mapping=True,
                ),
                RepresentationCapability(
                    kind="diff",
                    label="IR Diff",
                    syntax="diff",
                    category="explorer",
                    display_rank=40,
                    materialization_mode="derived",
                    cache_policy="request",
                    cost_class="moderate",
                    diff_base_kind="ir",
                ),
                RepresentationCapability(
                    kind="object",
                    label="Object Code",
                    syntax="text",
                    category="explorer",
                    display_rank=60,
                    materialization_mode="lazy",
                    cache_policy="snapshot",
                    cost_class="high",
                ),
                RepresentationCapability(
                    kind="ast-json",
                    label="AST JSON",
                    syntax="json",
                    category="explorer",
                    display_rank=50,
                    materialization_mode="lazy",
                    cache_policy="snapshot",
                    cost_class="high",
                    supports_range_queries=True,
                ),
                RepresentationCapability(
                    kind="preprocessed",
                    label="Preprocessed",
                    syntax="cpp",
                    category="explorer",
                    display_rank=70,
                    materialization_mode="lazy",
                    cache_policy="snapshot",
                    cost_class="moderate",
                    supports_range_queries=True,
                ),
                RepresentationCapability(
                    kind="macro-expansion",
                    label="Macro Expansion",
                    syntax="cpp",
                    category="explorer",
                    display_rank=80,
                    materialization_mode="lazy",
                    cache_policy="snapshot",
                    cost_class="moderate",
                    supports_range_queries=True,
                ),
            )
        }

    def list_capabilities(self) -> List[RepresentationCapability]:
        return sorted(
            self._capabilities.values(),
            key=lambda capability: (
                capability.category,
                capability.display_rank,
            ),
        )

    def get(self, kind: str) -> Optional[RepresentationCapability]:
        return self._capabilities.get(str(kind))

    def resolve_many(self, kinds: List[str]) -> List[RepresentationCapability]:
        return [
            capability
            for kind in kinds
            if (capability := self.get(kind)) is not None
        ]

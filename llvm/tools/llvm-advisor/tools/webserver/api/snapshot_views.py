from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from common.artifact_store import RepresentationStore
from common.collector import ArtifactCollector
from common.models import FileType, ParsedFile
from .snapshot_manifest import SnapshotManifestResolver
from .signal_correlator import SignalCorrelator


class SnapshotViews:
    _EXPLORER_ARTIFACT_TYPES = {
        "assembly": FileType.ASSEMBLY,
        "ir": FileType.IR,
        "optimized-ir": FileType.IR,
        "object": FileType.OBJDUMP,
        "ast-json": FileType.AST_JSON,
        "preprocessed": FileType.PREPROCESSED,
        "macro-expansion": FileType.MACRO_EXPANSION,
    }

    def __init__(
        self,
        *,
        data_dir: Path,
        collector: ArtifactCollector,
        representation_store: RepresentationStore,
    ) -> None:
        self._data_dir = data_dir
        self._collector = collector
        self._representation_store = representation_store
        self._manifest_resolver = SnapshotManifestResolver()
        self._signal_correlator = SignalCorrelator()

    def build_health(self) -> Dict[str, Any]:
        units = self._representation_store.get_compilation_units()
        total_files = sum(
            len(files) for unit in units for files in unit.representations.values()
        )
        return {
            "status": "healthy",
            "data_dir": str(self._data_dir),
            "data_dir_exists": self._data_dir.exists(),
            "data_dir_is_directory": self._data_dir.is_dir(),
            "compilation_units": len(units),
            "total_files": total_files,
        }

    def build_summary(self) -> Dict[str, Any]:
        parsed_data = self._representation_store.get_parsed_data()
        stats = self._collector.get_summary_statistics(parsed_data)
        total_files = max(int(stats.get("total_files", 0)), 0)
        errors = max(int(stats.get("errors", 0)), 0)
        success_rate = 0.0 if total_files == 0 else ((max(total_files - errors, 0) / total_files) * 100.0)
        return {
            **stats,
            "status": "success" if errors == 0 else "partial_errors",
            "success_rate": success_rate,
        }

    def build_units(self) -> Dict[str, Any]:
        units = self._representation_store.get_compilation_units()
        unit_payloads = [self._build_unit_overview(unit) for unit in units]
        unit_payloads.sort(key=lambda payload: payload["name"])
        return {
            "units": unit_payloads,
            "total_units": len(unit_payloads),
            "total_files": sum(payload["total_files"] for payload in unit_payloads),
        }

    def build_unit_detail(self, unit_name: str) -> Optional[Dict[str, Any]]:
        parsed_data = self._representation_store.get_parsed_data(unit_name=unit_name)
        unit_payload = parsed_data.get(unit_name)
        if unit_payload is None:
            return None

        representation_kinds = {
            file_type.value: self._summarize_parsed_files(parsed_files)
            for file_type, parsed_files in sorted(unit_payload.items(), key=lambda item: item[0].value)
        }
        total_files = sum(group["count"] for group in representation_kinds.values())
        total_errors = sum(group["errors"] for group in representation_kinds.values())
        return {
            "unit_name": unit_name,
            "representation_kinds": representation_kinds,
            "summary": {
                "total_representation_kinds": len(representation_kinds),
                "total_files": total_files,
                "total_errors": total_errors,
            },
        }

    def build_representation_kinds(self) -> Dict[str, Any]:
        parsed_data = self._representation_store.get_parsed_data()
        available_types: Dict[str, Dict[str, Any]] = {}
        for unit_name, unit_payload in parsed_data.items():
            for file_type, parsed_files in unit_payload.items():
                type_payload = available_types.setdefault(
                    file_type.value,
                    {"total_files": 0, "total_errors": 0, "units": []},
                )
                type_payload["total_files"] += len(parsed_files)
                type_payload["total_errors"] += sum(
                    1 for parsed_file in parsed_files if "error" in parsed_file.metadata
                )
                type_payload["units"].append(unit_name)
        return {
            "supported_types": [file_type.value for file_type in FileType],
            "available_types": available_types,
            "total_types_found": len(available_types),
        }

    def build_representation_data(
        self, representation_kind: str
    ) -> Optional[Dict[str, Any]]:
        try:
            requested_file_type = FileType(representation_kind)
        except ValueError:
            return None

        parsed_data = self._representation_store.get_parsed_data()
        units: Dict[str, Dict[str, Any]] = {}
        global_summary = {"total_files": 0, "total_errors": 0, "units_with_type": 0}

        for unit_name, unit_payload in parsed_data.items():
            parsed_files = unit_payload.get(requested_file_type)
            if not parsed_files:
                continue
            unit_summary = self._summarize_parsed_files(parsed_files)
            units[unit_name] = unit_summary
            global_summary["total_files"] += unit_summary["count"]
            global_summary["total_errors"] += unit_summary["errors"]
            global_summary["units_with_type"] += 1

        return {
            "representation_kind": requested_file_type.value,
            "units": units,
            "global_summary": global_summary,
        }

    def build_files(self, unit_name: str) -> Optional[Dict[str, Any]]:
        unit = self._get_compilation_unit(unit_name)
        manifest = self._manifest_resolver.get_unit_manifest(unit)
        if unit is None or manifest is None:
            return None
        files = self._manifest_resolver.build_manifest_file_listing(unit_name, manifest)
        return {"files": files, "count": len(files)}

    def build_source(self, file_path: str) -> Optional[Dict[str, Any]]:
        source_entry = self._find_source_entry(file_path)
        if source_entry is None:
            return None

        parsed_data = self._representation_store.get_parsed_data()
        source_content = self._manifest_resolver.read_text_file(
            self._data_dir / str(source_entry.get("source_path") or "")
        )
        if source_content is None:
            source_content = self._manifest_resolver.read_text_file(
                Path(os.path.abspath(str(source_entry.get("source_path") or "")))
            )
        if source_content is None:
            return None
        return {
            "source": source_content,
            "file_path": file_path,
            "language": self._manifest_resolver.normalize_source_language(
                source_entry.get("language"),
                file_path,
            ),
            "inline_data": self._build_inline_data(
                parsed_data,
                file_path,
                str(source_entry.get("source_path") or ""),
            ),
        }

    def build_representation_content(
        self, representation_kind: str, file_path: str
    ) -> Optional[Dict[str, Any]]:
        content = self._manifest_resolver.load_manifest_representation_content(
            file_path=file_path,
            representation_kind=representation_kind,
            units=self._representation_store.get_compilation_units(),
        )
        if content is None:
            return None
        parsed_data = self._representation_store.get_parsed_data()
        return {
            "content": content,
            "file_path": file_path,
            "representation_kind": representation_kind,
            "language": self._representation_view_language(representation_kind),
            "inline_data": self._build_representation_inline_data(
                parsed_data,
                file_path,
                representation_kind,
                content,
            ),
        }

    def _build_unit_overview(self, unit: Any) -> Dict[str, Any]:
        representation_counts = {
            file_type.value: len(files)
            for file_type, files in sorted(
                unit.representations.items(), key=lambda item: item[0].value
            )
        }
        payload = {
            "name": unit.name,
            "path": unit.path,
            "representation_kinds": list(representation_counts.keys()),
            "representation_counts": representation_counts,
            "total_files": sum(representation_counts.values()),
        }
        unit_metadata = getattr(unit, "metadata", {}) or {}
        for field_name in ("run_timestamp", "available_runs", "run_path"):
            if field_name in unit_metadata:
                payload[field_name] = unit_metadata[field_name]
        payload["metadata"] = unit_metadata
        return payload

    def _get_compilation_unit(self, unit_name: str) -> Optional[Any]:
        return next(
            (
                unit
                for unit in self._representation_store.get_compilation_units()
                if getattr(unit, "name", None) == unit_name
            ),
            None,
        )

    def _get_unit_manifest(self, unit: Any) -> Optional[Dict[str, Any]]:
        return self._manifest_resolver.get_unit_manifest(unit)

    def _find_source_entry(self, file_path: str) -> Optional[Dict[str, Any]]:
        for unit in self._representation_store.get_compilation_units():
            manifest = self._get_unit_manifest(unit)
            if manifest is None:
                continue
            for source_entry in manifest.get("sources", []):
                if not isinstance(source_entry, dict):
                    continue
                if str(source_entry.get("source_ref") or "") == file_path:
                    return source_entry
        return None

    def _summarize_parsed_files(self, parsed_files: Iterable[ParsedFile]) -> Dict[str, Any]:
        files: List[Dict[str, Any]] = []
        errors = 0
        summary_stats: Dict[str, float] = {}
        for parsed_file in parsed_files:
            file_payload = {
                "file_name": os.path.basename(parsed_file.file_path),
                "file_path": parsed_file.file_path,
                "file_size_bytes": parsed_file.metadata.get("file_size", 0),
                "has_error": "error" in parsed_file.metadata,
                "metadata": parsed_file.metadata,
            }
            if isinstance(parsed_file.data, dict) and "summary" in parsed_file.data:
                file_payload["summary"] = parsed_file.data["summary"]
                for key, value in parsed_file.data["summary"].items():
                    if isinstance(value, (int, float)):
                        summary_stats[key] = summary_stats.get(key, 0) + value
            elif isinstance(parsed_file.data, list):
                file_payload["item_count"] = len(parsed_file.data)
                summary_stats["total_items"] = summary_stats.get("total_items", 0) + len(parsed_file.data)
            files.append(file_payload)
            errors += int(file_payload["has_error"])
        return {
            "files": files,
            "count": len(files),
            "errors": errors,
            "summary_stats": summary_stats,
        }

    def _build_inline_data(
        self,
        parsed_data: Dict[str, Dict[FileType, List[ParsedFile]]],
        file_path: str,
        resolved_source_path: str = "",
    ) -> Dict[str, List[Dict[str, Any]]]:
        return self._signal_correlator.build_source_signals(
            parsed_data,
            file_path,
            resolved_source_path,
        )

    def _representation_view_language(self, representation_kind: str) -> str:
        return {
            "assembly": "assembly",
            "ir": "llvm-ir",
            "optimized-ir": "llvm-ir",
            "ast-json": "json",
            "preprocessed": "cpp",
            "macro-expansion": "cpp",
        }.get(representation_kind, "text")

    def _iter_list_payload(self, parsed_file: ParsedFile) -> Iterable[Any]:
        return parsed_file.data if isinstance(parsed_file.data, list) else []

    def _build_representation_inline_data(
        self,
        parsed_data: Dict[str, Dict[FileType, List[ParsedFile]]],
        file_path: str,
        representation_kind: str,
        content: str,
    ) -> Dict[str, List[Dict[str, Any]]]:
        source_entry = self._find_source_entry(file_path) or {}
        resolved_source_path = str(source_entry.get("source_path") or "")
        return self._signal_correlator.build_representation_signals(
            parsed_data,
            file_path,
            representation_kind,
            content,
            resolved_source_path,
        )

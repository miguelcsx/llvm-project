from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Optional

from .api_contract import utc_timestamp
from .models import CompilationUnit, FileType
from .snapshot_records import (
    RepresentationDescriptorRecord,
    UnitRecord,
    generate_ulid_like_id,
    sha256_digest,
    sha256_hex,
    stable_json_bytes,
)


class SnapshotStoreSyncMixin:
    def _build_unit_record(self, snapshot_record, compilation_unit: CompilationUnit) -> UnitRecord:
        manifest = dict((compilation_unit.metadata or {}).get("manifest", {}))
        source_path = self._infer_source_path(compilation_unit)
        source_metadata = self._find_manifest_source(manifest, source_path)
        manifest_unit_id = str(manifest.get("unit_id") or "").strip()
        manifest_command_fingerprint = str(manifest.get("command_fingerprint") or "").strip()
        manifest_source_fingerprint = str(manifest.get("source_fingerprint") or "").strip()
        manifest_target_triple = str(manifest.get("target_triple") or "").strip()
        manifest_language = str(manifest.get("language") or "").strip()
        normalized_command = self._build_command_normalized(snapshot_record, compilation_unit, manifest, source_path)
        command_fingerprint = manifest_command_fingerprint or sha256_digest(stable_json_bytes(normalized_command))
        source_fingerprint = manifest_source_fingerprint or self._build_source_fingerprint(source_metadata.get("source_path"), source_path)
        target_triple = manifest_target_triple or str(snapshot_record.build_config.get("target_triple", "unknown"))
        unit_identity = sha256_hex((f"{command_fingerprint}|{source_fingerprint}|{target_triple}").encode("utf-8"))
        return UnitRecord(
            schema_version="1.0",
            snapshot_id=snapshot_record.snapshot_id,
            unit_id=manifest_unit_id or f"unit_{unit_identity[:26].upper()}",
            unit_name=compilation_unit.name,
            unit_path=compilation_unit.path,
            source_path=source_path,
            language=manifest_language or self._detect_language(str(source_metadata.get("source_ref") or source_path)),
            target_triple=target_triple,
            command_fingerprint=command_fingerprint,
            command_normalized=normalized_command,
            source_fingerprint=source_fingerprint,
            created_at=utc_timestamp(),
            metadata=dict(compilation_unit.metadata or {}),
        )

    def _build_representation_descriptor_records(self, snapshot_record, unit_record: UnitRecord, compilation_unit: CompilationUnit) -> List[RepresentationDescriptorRecord]:
        manifest = dict((compilation_unit.metadata or {}).get("manifest", {}))
        sources_by_ref = {
            str(source_entry.get("source_ref") or "").strip(): source_entry
            for source_entry in manifest.get("sources", [])
            if isinstance(source_entry, dict) and str(source_entry.get("source_ref") or "").strip()
        }
        descriptors: List[RepresentationDescriptorRecord] = []
        for source_ref, source_entry in sources_by_ref.items():
            source_path = str(source_entry.get("source_path") or "")
            try:
                byte_size = Path(source_path).stat().st_size if source_path else 0
            except OSError:
                byte_size = 0
            descriptors.append(
                RepresentationDescriptorRecord(
                    schema_version="1.0",
                    descriptor_id=generate_ulid_like_id("rdesc"),
                    snapshot_id=snapshot_record.snapshot_id,
                    unit_id=unit_record.unit_id,
                    unit_name=unit_record.unit_name,
                    source_id=str(source_entry.get("source_id") or ""),
                    source_ref=source_ref,
                    source_path=source_path,
                    representation_kind="source",
                    variant="primary" if source_ref == unit_record.source_path else "included",
                    materialization_policy="eager",
                    content_type="text/plain",
                    relative_path="",
                    mapping_relative_path="",
                    mapping_format="",
                    producer="llvm-advisor",
                    category="sources",
                    byte_size=byte_size,
                    created_at=utc_timestamp(),
                )
            )
        for representation_entry in manifest.get("representations", []):
            if not isinstance(representation_entry, dict):
                continue
            source_ref = str(representation_entry.get("source_ref") or "").strip()
            relative_path = str(representation_entry.get("relative_path") or "").strip()
            representation_kind = self._manifest_category_to_representation_kind(
                str((representation_entry.get("provenance") or {}).get("category") or "").strip()
            )
            if not source_ref or not relative_path or not representation_kind:
                continue
            source_entry = sources_by_ref.get(source_ref, {})
            absolute_path = Path(compilation_unit.path) / relative_path
            try:
                byte_size = absolute_path.stat().st_size if absolute_path.exists() else 0
            except OSError:
                byte_size = 0
            descriptors.append(
                RepresentationDescriptorRecord(
                    schema_version="1.0",
                    descriptor_id=generate_ulid_like_id("rdesc"),
                    snapshot_id=snapshot_record.snapshot_id,
                    unit_id=unit_record.unit_id,
                    unit_name=unit_record.unit_name,
                    source_id=str(source_entry.get("source_id") or ""),
                    source_ref=source_ref,
                    source_path=str(source_entry.get("source_path") or ""),
                    representation_kind=representation_kind,
                    variant=str(representation_entry.get("variant") or ""),
                    materialization_policy=str(representation_entry.get("materialization_policy") or "eager"),
                    content_type=str(representation_entry.get("content_type") or "text/plain"),
                    relative_path=relative_path,
                    mapping_relative_path=str(((representation_entry.get("mapping") or {}).get("relative_path")) or ""),
                    mapping_format=str(((representation_entry.get("mapping") or {}).get("format")) or ""),
                    producer=str((representation_entry.get("provenance") or {}).get("producer") or ""),
                    category=str((representation_entry.get("provenance") or {}).get("category") or ""),
                    byte_size=byte_size,
                    created_at=utc_timestamp(),
                )
            )
        return descriptors

    def _infer_source_path(self, compilation_unit: CompilationUnit) -> str:
        manifest = dict((compilation_unit.metadata or {}).get("manifest", {}))
        primary_source_ref = str(manifest.get("primary_source_ref") or "").strip()
        if primary_source_ref:
            return primary_source_ref
        sources = compilation_unit.representations.get(FileType.SOURCES, [])
        if sources:
            preferred_names = {
                compilation_unit.name,
                f"{compilation_unit.name}.c",
                f"{compilation_unit.name}.cc",
                f"{compilation_unit.name}.cpp",
                f"{compilation_unit.name}.cxx",
            }
            for source_path in sources:
                if Path(str(source_path)).name in preferred_names:
                    return str(source_path)
            for source_path in sources:
                if Path(str(source_path)).suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}:
                    return str(source_path)
            return str(sources[0])
        return compilation_unit.path

    def _find_manifest_source(self, manifest: Dict[str, Any], source_ref: str) -> Dict[str, Any]:
        for source_entry in manifest.get("sources", []):
            if isinstance(source_entry, dict) and str(source_entry.get("source_ref") or "") == source_ref:
                return source_entry
        return {}

    def _build_command_normalized(self, snapshot_record, compilation_unit: CompilationUnit, manifest: Dict[str, Any], source_ref: str) -> List[str]:
        del compilation_unit
        command_payload = manifest.get("command")
        if isinstance(command_payload, dict):
            command_normalized = command_payload.get("command_normalized")
            if isinstance(command_normalized, list):
                normalized = [str(entry).strip() for entry in command_normalized if str(entry).strip()]
                if normalized:
                    return normalized
            compiler_path = str(command_payload.get("compiler_path") or snapshot_record.build_config.get("toolchain", "clang"))
            compile_flags = [str(flag) for flag in command_payload.get("compile_flags", []) if str(flag).strip()]
            return [compiler_path, *compile_flags, source_ref]
        return [str(snapshot_record.build_config.get("toolchain", "clang")), source_ref]

    def _build_source_fingerprint(self, absolute_source_path: Any, source_ref: str) -> str:
        candidate_path = Path(str(absolute_source_path or "")).expanduser()
        if absolute_source_path and candidate_path.exists():
            try:
                return sha256_digest(candidate_path.read_bytes())
            except OSError:
                pass
        return sha256_digest(str(source_ref).encode("utf-8"))

    def _build_source_revision(self, data_dir: str) -> Dict[str, Any]:
        run_summary = self._load_run_summary(Path(data_dir))
        revision = run_summary.get("source_revision")
        if isinstance(revision, dict):
            return revision
        return {
            "vcs": "unknown",
            "commit": run_summary.get("commit", ""),
            "branch": run_summary.get("branch", ""),
            "dirty": bool(run_summary.get("dirty", False)),
        }

    def _build_build_config(self, metadata: Dict[str, Any]) -> Dict[str, Any]:
        build_config = metadata.get("build_config")
        if isinstance(build_config, dict):
            return build_config
        return {
            "preset": metadata.get("preset", "unknown"),
            "generator": metadata.get("generator", "unknown"),
            "target_triple": metadata.get("target_triple", "unknown"),
            "toolchain": metadata.get("toolchain", "clang"),
        }

    def _load_run_summary(self, data_dir: Path) -> Dict[str, Any]:
        summary_path = data_dir / "run-summary.json"
        if not summary_path.exists():
            return {}
        try:
            payload = json.loads(summary_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {}
        return payload if isinstance(payload, dict) else {}

    def _detect_language(self, source_path: str) -> str:
        return {
            ".c": "c",
            ".cc": "c++",
            ".cpp": "c++",
            ".cxx": "c++",
            ".h": "c-header",
            ".hpp": "c++-header",
            ".ll": "llvm-ir",
        }.get(Path(source_path).suffix.lower(), "unknown")

    def _manifest_category_to_representation_kind(self, category: str) -> Optional[str]:
        return {
            "assembly": "assembly",
            "ir": "ir",
            "objdump": "object",
            "ast-json": "ast-json",
            "preprocessed": "preprocessed",
            "macro-expansion": "macro-expansion",
            "sources": "source",
        }.get(category)

    def _representation_view_language(self, representation_kind: str) -> str:
        return {
            "source": "cpp",
            "assembly": "assembly",
            "ir": "llvm-ir",
            "optimized-ir": "llvm-ir",
            "ast-json": "json",
            "preprocessed": "cpp",
            "macro-expansion": "cpp",
            "object": "text",
        }.get(representation_kind, "text")

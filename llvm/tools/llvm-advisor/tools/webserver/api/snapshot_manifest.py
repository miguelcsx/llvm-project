from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional


class SnapshotManifestResolver:
    def get_unit_manifest(self, unit: Any) -> Optional[Dict[str, Any]]:
        if unit is None:
            return None
        metadata = getattr(unit, "metadata", {}) or {}
        manifest = metadata.get("manifest")
        return manifest if isinstance(manifest, dict) else None

    def build_manifest_file_listing(
        self, unit_name: str, manifest: Dict[str, Any]
    ) -> List[Dict[str, Any]]:
        representation_index: Dict[str, set[str]] = {}
        for representation_entry in manifest.get("representations", []):
            if not isinstance(representation_entry, dict):
                continue
            source_ref = str(representation_entry.get("source_ref") or "").strip()
            view_type = self.manifest_category_to_view_type(
                str((representation_entry.get("provenance") or {}).get("category") or "").strip()
            )
            if not source_ref or not view_type:
                continue
            representation_index.setdefault(source_ref, set()).add(view_type)

        primary_source_ref = str(manifest.get("primary_source_ref") or "").strip()
        files = []
        for source_entry in manifest.get("sources", []):
            if not isinstance(source_entry, dict):
                continue
            source_ref = str(source_entry.get("source_ref") or "").strip()
            if not source_ref:
                continue
            available_representations = sorted(
                {"source", *representation_index.get(source_ref, set())}
            )
            files.append(
                {
                    "path": source_ref,
                    "name": os.path.basename(source_ref),
                    "display_name": source_ref,
                    "unit": unit_name,
                    "is_primary": source_ref == primary_source_ref,
                    "available_representations": available_representations,
                }
            )
        return files

    def load_manifest_representation_content(
        self,
        *,
        file_path: str,
        representation_kind: str,
        units: List[Any],
    ) -> Optional[str]:
        expected_category = self.view_type_to_manifest_category(representation_kind)
        if expected_category is None:
            return None

        for unit in units:
            manifest = self.get_unit_manifest(unit)
            if manifest is None:
                continue
            for representation_entry in manifest.get("representations", []):
                if not isinstance(representation_entry, dict):
                    continue
                if str(representation_entry.get("source_ref") or "") != file_path:
                    continue
                category = str(
                    (representation_entry.get("provenance") or {}).get("category") or ""
                )
                if category != expected_category:
                    continue
                relative_path = str(representation_entry.get("relative_path") or "").strip()
                if not relative_path:
                    continue
                return self.read_text_file(Path(unit.path) / relative_path)
        return None

    def manifest_category_to_view_type(self, category: str) -> Optional[str]:
        return {
            "assembly": "assembly",
            "ir": "ir",
            "objdump": "object",
            "ast-json": "ast-json",
            "preprocessed": "preprocessed",
            "macro-expansion": "macro-expansion",
        }.get(category)

    def view_type_to_manifest_category(self, view_type: str) -> Optional[str]:
        return {
            "assembly": "assembly",
            "ir": "ir",
            "optimized-ir": "ir",
            "object": "objdump",
            "ast-json": "ast-json",
            "preprocessed": "preprocessed",
            "macro-expansion": "macro-expansion",
        }.get(view_type)

    def normalize_source_language(self, language: Any, file_path: str) -> str:
        normalized_language = str(language or "").strip().lower()
        language_map = {
            "c": "c",
            "c++": "cpp",
            "cpp": "cpp",
            "objective-c": "objective-c",
            "objective-c++": "objective-cpp",
        }
        return language_map.get(normalized_language, self.detect_language(file_path))

    def read_text_file(self, file_path: Path) -> Optional[str]:
        if not file_path.exists():
            return None
        try:
            return file_path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            return None

    def detect_language(self, file_path: str) -> str:
        return {
            ".c": "c",
            ".cc": "cpp",
            ".cpp": "cpp",
            ".cxx": "cpp",
            ".h": "cpp",
            ".hpp": "cpp",
            ".hh": "cpp",
            ".m": "objective-c",
            ".mm": "objective-cpp",
            ".ll": "llvm-ir",
        }.get(Path(file_path).suffix.lower(), "text")

from __future__ import annotations

import os
from typing import Any, Dict, Iterable, List, Optional, Set

from common.models import Diagnostic, FileType, ParsedFile, Remark


class SignalCorrelator:
    def build_source_signals(
        self,
        parsed_data: Dict[str, Dict[FileType, List[ParsedFile]]],
        file_path: str,
        resolved_source_path: str = "",
    ) -> Dict[str, List[Dict[str, Any]]]:
        requested_filename = os.path.basename(file_path)
        expected_paths = self._build_expected_paths(file_path, resolved_source_path)
        diagnostics: List[Dict[str, Any]] = []
        remarks: List[Dict[str, Any]] = []

        for unit_payload in parsed_data.values():
            diagnostics.extend(
                self._collect_diagnostics(
                    unit_payload,
                    requested_filename,
                    expected_paths,
                )
            )
            remarks.extend(
                self._collect_source_remarks(
                    unit_payload,
                    requested_filename,
                    expected_paths,
                )
            )

        return {"diagnostics": diagnostics, "remarks": remarks}

    def build_representation_signals(
        self,
        parsed_data: Dict[str, Dict[FileType, List[ParsedFile]]],
        file_path: str,
        representation_kind: str,
        content: str,
        resolved_source_path: str = "",
    ) -> Dict[str, List[Dict[str, Any]]]:
        if representation_kind not in {"assembly", "ir", "optimized-ir", "object"}:
            return {"diagnostics": [], "remarks": []}

        function_lines = self._build_representation_function_line_map(
            content, representation_kind
        )
        if not function_lines:
            return {"diagnostics": [], "remarks": []}

        requested_filename = os.path.basename(file_path)
        expected_paths = self._build_expected_paths(file_path, resolved_source_path)
        remarks: List[Dict[str, Any]] = []
        for unit_payload in parsed_data.values():
            remarks.extend(
                self._collect_representation_remarks(
                    unit_payload,
                    function_lines,
                    requested_filename,
                    expected_paths,
                )
            )

        remarks.sort(
            key=lambda entry: (
                int(entry.get("line") or 0),
                str(entry.get("pass_name") or ""),
                str(entry.get("message") or ""),
            )
        )
        return {"diagnostics": [], "remarks": remarks}

    def _collect_diagnostics(
        self,
        unit_payload: Dict[FileType, List[ParsedFile]],
        requested_filename: str,
        expected_paths: Set[str],
    ) -> List[Dict[str, Any]]:
        diagnostics = []
        for parsed_file in unit_payload.get(FileType.DIAGNOSTICS, []):
            for diagnostic in self._iter_list_payload(parsed_file):
                if not isinstance(diagnostic, Diagnostic) or diagnostic.location is None:
                    continue
                location_file = str(diagnostic.location.file or "")
                if not self._location_matches_source(
                    location_file, requested_filename, expected_paths
                ):
                    continue
                diagnostics.append(
                    {
                        "line": diagnostic.location.line,
                        "column": diagnostic.location.column,
                        "level": diagnostic.level,
                        "message": diagnostic.message,
                    }
                )
        return diagnostics

    def _collect_source_remarks(
        self,
        unit_payload: Dict[FileType, List[ParsedFile]],
        requested_filename: str,
        expected_paths: Set[str],
    ) -> List[Dict[str, Any]]:
        remarks = []
        for parsed_file in unit_payload.get(FileType.REMARKS, []):
            for remark in self._iter_list_payload(parsed_file):
                if not isinstance(remark, Remark) or remark.location is None:
                    continue
                location_file = str(remark.location.file or "")
                if not self._location_matches_source(
                    location_file, requested_filename, expected_paths
                ):
                    continue
                remarks.append(
                    {
                        "line": remark.location.line,
                        "column": remark.location.column,
                        "pass_name": remark.pass_name,
                        "function": remark.function,
                        "message": remark.message,
                    }
                )
        return remarks

    def _collect_representation_remarks(
        self,
        unit_payload: Dict[FileType, List[ParsedFile]],
        function_lines: Dict[str, int],
        requested_filename: str,
        expected_paths: Set[str],
    ) -> List[Dict[str, Any]]:
        remarks = []
        for parsed_file in unit_payload.get(FileType.REMARKS, []):
            for remark in self._iter_list_payload(parsed_file):
                if not isinstance(remark, Remark):
                    continue
                location_file = str((remark.location.file if remark.location else "") or "")
                if location_file and not self._location_matches_source(
                    location_file, requested_filename, expected_paths
                ):
                    continue
                line_number = function_lines.get(str(remark.function or "").strip())
                if line_number is None:
                    continue
                remarks.append(
                    {
                        "line": line_number,
                        "column": None,
                        "pass_name": remark.pass_name,
                        "function": remark.function,
                        "message": remark.message,
                        "source_line": remark.location.line if remark.location else None,
                    }
                )
        return remarks

    def _build_expected_paths(
        self, file_path: str, resolved_source_path: str
    ) -> Set[str]:
        normalized_source_path = (
            os.path.normpath(resolved_source_path) if resolved_source_path else ""
        )
        return {file_path, resolved_source_path, normalized_source_path}

    def _location_matches_source(
        self,
        location_file: str,
        requested_filename: str,
        expected_paths: Set[str],
    ) -> bool:
        if not location_file:
            return False
        if location_file in expected_paths:
            return True
        normalized_location = os.path.normpath(location_file)
        if normalized_location in expected_paths:
            return True
        return os.path.basename(location_file) == requested_filename

    def _build_representation_function_line_map(
        self, content: str, representation_kind: str
    ) -> Dict[str, int]:
        line_map: Dict[str, int] = {}
        for line_number, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            candidate_name = self._extract_symbol_name(stripped, representation_kind)
            if candidate_name and candidate_name not in line_map:
                line_map[candidate_name] = line_number
        return line_map

    def _extract_symbol_name(
        self, stripped_line: str, representation_kind: str
    ) -> Optional[str]:
        if representation_kind == "assembly":
            assembly_label = stripped_line.split("#", 1)[0].strip()
            if (
                assembly_label.endswith(":")
                and not assembly_label.startswith(".L")
                and not assembly_label.startswith("#")
            ):
                return assembly_label[:-1].strip()
            return None
        if representation_kind in {"ir", "optimized-ir"}:
            if stripped_line.startswith("define ") and "@" in stripped_line and "(" in stripped_line:
                return stripped_line.split("@", 1)[1].split("(", 1)[0].strip()
            return None
        if representation_kind == "object":
            parts = stripped_line.split()
            if len(parts) >= 4 and parts[1] == "FUNC":
                return parts[-1]
        return None

    def _iter_list_payload(self, parsed_file: ParsedFile) -> Iterable[Any]:
        return parsed_file.data if isinstance(parsed_file.data, list) else []

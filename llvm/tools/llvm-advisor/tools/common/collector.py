# ===----------------------------------------------------------------------===//
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ===----------------------------------------------------------------------===//
#
# This is the representation collector module. It provides logic for discovering
# and parsing captured representations for LLVM Advisor analysis.
#
# ===----------------------------------------------------------------------===#

import importlib
import json
import os
from pathlib import Path
from typing import Any, Dict, List, Optional

from .models import FileType, CompilationUnit, ParsedFile


_PARSER_SPECS = (
    (FileType.REMARKS, "remarks_parser", "RemarksParser"),
    (FileType.TIME_TRACE, "time_trace_parser", "TimeTraceParser"),
    (FileType.DIAGNOSTICS, "diagnostics_parser", "DiagnosticsParser"),
    (FileType.AST_JSON, "ast_parser", "ASTParser"),
    (FileType.PGO_PROFILE, "pgo_profile_parser", "PGOProfileParser"),
    (FileType.XRAY, "xray_parser", "XRayParser"),
    (FileType.STATIC_ANALYZER, "static_analyzer_parser", "StaticAnalyzerParser"),
    (FileType.IR, "ir_parser", "IRParser"),
    (FileType.OBJDUMP, "objdump_parser", "ObjdumpParser"),
    (FileType.INCLUDE_TREE, "include_tree_parser", "IncludeTreeParser"),
    (FileType.ASSEMBLY, "assembly_parser", "AssemblyParser"),
    (FileType.PREPROCESSED, "preprocessed_parser", "PreprocessedParser"),
    (FileType.STATIC_ANALYSIS_SARIF, "sarif_parser", "SARIFParser"),
    (FileType.MACRO_EXPANSION, "macro_expansion_parser", "MacroExpansionParser"),
    (FileType.DEPENDENCIES, "dependencies_parser", "DependenciesParser"),
    (FileType.BINARY_SIZE, "binary_size_parser", "BinarySizeParser"),
    (FileType.DEBUG, "debug_parser", "DebugParser"),
    (FileType.SYMBOLS, "symbols_parser", "SymbolsParser"),
    (FileType.RUNTIME_TRACE, "runtime_trace_parser", "RuntimeTraceParser"),
    (FileType.COMPILATION_PHASES, "compilation_phases_parser", "CompilationPhasesParser"),
    (FileType.FTIME_REPORT, "ftime_report_parser", "FTimeReportParser"),
    (FileType.VERSION_INFO, "version_info_parser", "VersionInfoParser"),
    (FileType.SOURCES, "preprocessed_parser", "PreprocessedParser"),
)


class ArtifactCollector:
    def __init__(self):
        self.parsers = self._build_parsers()

        # Map directory names to file types
        self.dir_to_type = {
            "remarks": FileType.REMARKS,
            "time-trace": FileType.TIME_TRACE,
            "diagnostics": FileType.DIAGNOSTICS,
            "ast-json": FileType.AST_JSON,
            "pgo-profile": FileType.PGO_PROFILE,
            "xray": FileType.XRAY,
            "static-analyzer": FileType.STATIC_ANALYZER,
            "ir": FileType.IR,
            "objdump": FileType.OBJDUMP,
            "include-tree": FileType.INCLUDE_TREE,
            "assembly": FileType.ASSEMBLY,
            "preprocessed": FileType.PREPROCESSED,
            "static-analysis-sarif": FileType.STATIC_ANALYSIS_SARIF,
            "macro-expansion": FileType.MACRO_EXPANSION,
            "dependencies": FileType.DEPENDENCIES,
            "binary-size": FileType.BINARY_SIZE,
            "debug": FileType.DEBUG,
            "symbols": FileType.SYMBOLS,
            "runtime-trace": FileType.RUNTIME_TRACE,
            "compilation-phases": FileType.COMPILATION_PHASES,
            "ftime-report": FileType.FTIME_REPORT,
            "version-info": FileType.VERSION_INFO,
            "sources": FileType.SOURCES,
        }

    def _build_parsers(self) -> Dict[FileType, Any]:
        parsers: Dict[FileType, Any] = {}

        for file_type, module_name, class_name in _PARSER_SPECS:
            parser = self._instantiate_parser(module_name, class_name)
            if parser is not None:
                parsers[file_type] = parser

        return parsers

    def _instantiate_parser(self, module_name: str, class_name: str) -> Optional[Any]:
        try:
            module = importlib.import_module(f".parsers.{module_name}", __package__)
            parser_class = getattr(module, class_name)
            return parser_class()
        except ModuleNotFoundError as exc:
            if exc.name in {module_name, f"{__package__}.parsers.{module_name}"}:
                raise
            return None
        except ImportError:
            return None

    def discover_compilation_units(self, advisor_dir: str) -> List[CompilationUnit]:
        """Discover all compilation units in the .llvm-advisor directory."""
        compilation_units = []
        advisor_path = Path(advisor_dir)

        if not advisor_path.exists():
            return compilation_units

        # Each subdirectory represents a compilation unit
        for unit_dir in advisor_path.iterdir():
            if not unit_dir.is_dir():
                continue

            # Check if this is the new nested structure or old flat structure
            units = self._scan_compilation_unit_with_runs(unit_dir)
            compilation_units.extend(units)

        return compilation_units

    def _scan_compilation_unit_with_runs(self, unit_dir: Path) -> List[CompilationUnit]:
        """Scan a compilation unit directory that contains timestamped runs."""
        units = []

        # unit_dir contains timestamped run directories
        run_dirs = []
        for item in unit_dir.iterdir():
            if item.is_dir() and item.name.startswith(unit_dir.name + "_"):
                run_dirs.append(item)

        if not run_dirs:
            # No timestamped runs found, skip this unit
            return units

        # Sort by timestamp (newest first)
        run_dirs.sort(key=lambda x: x.name, reverse=True)

        # Use the most recent run
        latest_run = run_dirs[0]
        unit = self._scan_single_run(latest_run, unit_dir.name)
        if unit:
            # Store run timestamp info in metadata
            unit.metadata = getattr(unit, "metadata", {})
            unit.metadata["run_timestamp"] = latest_run.name.split("_", 1)[-1]
            unit.metadata["run_path"] = str(latest_run)
            unit.metadata["available_runs"] = [r.name for r in run_dirs]
            units.append(unit)

        return units

    def _scan_single_run(
        self, run_dir: Path, unit_name: str
    ) -> Optional[CompilationUnit]:
        """Scan a single run directory using the canonical manifest."""
        manifest = self._load_unit_manifest(run_dir)
        if manifest is None:
            return None

        representations = self._build_representations_from_manifest(run_dir, manifest)
        if not representations:
            return None

        metadata = {
            "manifest": manifest,
            "primary_source_ref": manifest.get("primary_source_ref"),
            "primary_source_path": manifest.get("primary_source_path"),
            "sources": list(manifest.get("sources", [])),
            "representation_entries": list(manifest.get("representations", [])),
        }

        return CompilationUnit(
            name=unit_name,
            path=str(run_dir),
            representations=representations,
            metadata=metadata,
        )

    def _load_unit_manifest(self, run_dir: Path) -> Optional[Dict[str, Any]]:
        manifest_path = run_dir / "unit-manifest.json"
        if not manifest_path.exists():
            return None

        try:
            payload = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None

        return payload if isinstance(payload, dict) else None

    def _build_representations_from_manifest(
        self, run_dir: Path, manifest: Dict[str, Any]
    ) -> Dict[FileType, List[str]]:
        representations: Dict[FileType, List[str]] = {}

        for representation_entry in manifest.get("representations", []):
            if not isinstance(representation_entry, dict):
                continue

            category = str(
                (representation_entry.get("provenance") or {}).get("category") or ""
            ).strip()
            relative_path = str(
                representation_entry.get("relative_path") or ""
            ).strip()
            file_type = self.dir_to_type.get(category)
            if file_type is None or not relative_path:
                continue

            resolved_path = run_dir / relative_path
            if not resolved_path.exists() or not resolved_path.is_file():
                continue

            representations.setdefault(file_type, []).append(str(resolved_path))

        return representations

    def parse_compilation_unit(
        self, unit: CompilationUnit
    ) -> Dict[FileType, List[ParsedFile]]:
        """Parse all captured representations for a compilation unit."""
        parsed_representations = {}

        for file_type, file_paths in unit.representations.items():
            if file_type not in self.parsers:
                continue

            parser = self.parsers[file_type]
            parsed_files = []

            for file_path in file_paths:
                try:
                    if parser.can_parse(file_path):
                        parsed_file = parser.parse(file_path)
                        parsed_files.append(parsed_file)
                except Exception as e:
                    # Create error entry for failed parsing
                    error_file = ParsedFile(
                        file_type=file_type,
                        file_path=file_path,
                        data={},
                        metadata={"error": f"Failed to parse: {str(e)}"},
                    )
                    parsed_files.append(error_file)

            if parsed_files:
                parsed_representations[file_type] = parsed_files

        return parsed_representations

    def parse_all_units(
        self, advisor_dir: str
    ) -> Dict[str, Dict[FileType, List[ParsedFile]]]:
        """Parse all compilation units in the advisor directory."""
        units = self.discover_compilation_units(advisor_dir)
        parsed_units = {}

        for unit in units:
            parsed_representations = self.parse_compilation_unit(unit)
            if parsed_representations:
                parsed_units[unit.name] = parsed_representations

        return parsed_units

    def get_summary_statistics(
        self, parsed_units: Dict[str, Dict[FileType, List[ParsedFile]]]
    ) -> Dict[str, Any]:
        """Generate summary statistics for all parsed data."""
        stats = {
            "total_units": len(parsed_units),
            "total_files": 0,
            "file_types": {},
            "errors": 0,
            "units": {},
        }

        for unit_name, artifacts in parsed_units.items():
            unit_stats = {"file_types": {}, "total_files": 0, "errors": 0}

            for file_type, parsed_files in artifacts.items():
                type_name = file_type.value
                file_count = len(parsed_files)
                error_count = sum(1 for f in parsed_files if "error" in f.metadata)

                unit_stats["file_types"][type_name] = {
                    "count": file_count,
                    "errors": error_count,
                }
                unit_stats["total_files"] += file_count
                unit_stats["errors"] += error_count

                # Update global stats
                if type_name not in stats["file_types"]:
                    stats["file_types"][type_name] = {"count": 0, "errors": 0}

                stats["file_types"][type_name]["count"] += file_count
                stats["file_types"][type_name]["errors"] += error_count

            stats["units"][unit_name] = unit_stats
            stats["total_files"] += unit_stats["total_files"]
            stats["errors"] += unit_stats["errors"]

        return stats

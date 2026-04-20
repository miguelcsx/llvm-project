from __future__ import annotations

import os
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from common.artifact_store import RepresentationStore
from common.models import BinarySize, Diagnostic, FileType, Remark, TraceEvent
from common.parsers.runtime_trace_parser import RuntimeTraceParser
from common.parsers.time_trace_parser import TimeTraceParser


class SnapshotAnalysisViews:
    def __init__(self, artifact_store: RepresentationStore) -> None:
        self._artifact_store = artifact_store
        self._time_trace_parser = TimeTraceParser()
        self._runtime_trace_parser = RuntimeTraceParser()

    def build(self, analysis_type: str, sub_endpoint: str, query_params: Dict[str, List[str]]) -> Optional[Dict[str, Any]]:
        handler_name = f"_build_{analysis_type.replace('-', '_')}_{sub_endpoint.replace('-', '_')}"
        handler = getattr(self, handler_name, None)
        if handler is None:
            return None
        return handler(query_params)

    def _parsed_data(self) -> Dict[str, Dict[FileType, List[Any]]]:
        return self._artifact_store.get_parsed_data()

    def _build_remarks_overview(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        pass_counts = Counter()
        function_counts = Counter()
        file_counts = Counter()
        total = 0

        for remark in self._iter_remarks():
            total += 1
            if remark.pass_name:
                pass_counts[remark.pass_name] += 1
            if remark.function:
                function_counts[remark.function] += 1
            if remark.location and remark.location.file:
                file_counts[remark.location.file] += 1

        return {
            "totals": {
                "remarks": total,
                "unique_passes": len(pass_counts),
                "unique_functions": len(function_counts),
                "source_files": len(file_counts),
            },
            "top_passes": dict(pass_counts.most_common(10)),
            "top_functions": dict(function_counts.most_common(10)),
            "top_files": dict(file_counts.most_common(10)),
        }

    def _build_remarks_passes(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        passes = defaultdict(lambda: {"remarks_count": 0, "functions": set(), "files": set(), "sample_messages": []})
        for remark in self._iter_remarks():
            bucket = passes[remark.pass_name or "unknown"]
            bucket["remarks_count"] += 1
            if remark.function:
                bucket["functions"].add(remark.function)
            if remark.location and remark.location.file:
                bucket["files"].add(remark.location.file)
            if len(bucket["sample_messages"]) < 5 and remark.message:
                bucket["sample_messages"].append(remark.message)
        result = {
            pass_name: {
                "remarks_count": data["remarks_count"],
                "unique_functions": len(data["functions"]),
                "unique_files": len(data["files"]),
                "sample_messages": data["sample_messages"],
            }
            for pass_name, data in passes.items()
        }
        return {"passes": dict(sorted(result.items(), key=lambda item: item[1]["remarks_count"], reverse=True))}

    def _build_remarks_functions(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        functions = defaultdict(lambda: {"remarks_count": 0, "passes": set(), "locations": set(), "messages": []})
        for remark in self._iter_remarks():
            function_name = remark.function or "unknown"
            bucket = functions[function_name]
            bucket["remarks_count"] += 1
            if remark.pass_name:
                bucket["passes"].add(remark.pass_name)
            if remark.location and remark.location.file and remark.location.line:
                bucket["locations"].add((remark.location.file, remark.location.line))
            if len(bucket["messages"]) < 5 and remark.message:
                bucket["messages"].append(remark.message)
        result = {
            function_name: {
                "remarks_count": data["remarks_count"],
                "unique_passes": len(data["passes"]),
                "unique_locations": len(data["locations"]),
                "passes": sorted(data["passes"]),
                "sample_messages": data["messages"],
            }
            for function_name, data in functions.items()
        }
        return {"functions": dict(sorted(result.items(), key=lambda item: item[1]["remarks_count"], reverse=True)), "total_functions": len(result)}

    def _build_remarks_hotspots(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        hotspots = defaultdict(lambda: {"remarks_count": 0, "line_distribution": Counter(), "passes": set(), "functions": set()})
        for remark in self._iter_remarks():
            if not remark.location or not remark.location.file:
                continue
            bucket = hotspots[remark.location.file]
            bucket["remarks_count"] += 1
            if remark.location.line:
                bucket["line_distribution"][remark.location.line] += 1
            if remark.pass_name:
                bucket["passes"].add(remark.pass_name)
            if remark.function:
                bucket["functions"].add(remark.function)
        payload = [
            {
                "file": file_path,
                "file_name": os.path.basename(file_path),
                "remarks_count": data["remarks_count"],
                "unique_passes": len(data["passes"]),
                "unique_functions": len(data["functions"]),
                "hot_lines": dict(data["line_distribution"].most_common(10)),
            }
            for file_path, data in hotspots.items()
        ]
        payload.sort(key=lambda item: item["remarks_count"], reverse=True)
        return {"hotspots": payload[:20], "total_files_with_remarks": len(payload)}

    def _build_diagnostics_overview(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        level_counts = Counter()
        file_counts = Counter()
        total_diagnostics = 0
        for diagnostic in self._iter_diagnostics():
            total_diagnostics += 1
            level_counts[diagnostic.level] += 1
            if diagnostic.location and diagnostic.location.file:
                file_counts[diagnostic.location.file] += 1
        return {
            "totals": {
                "diagnostics": total_diagnostics,
                "unique_files": len(file_counts),
            },
            "by_level": dict(level_counts),
            "top_files": dict(file_counts.most_common(10)),
        }

    def _build_diagnostics_by_level(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        grouped = defaultdict(lambda: {"count": 0, "files": set(), "samples": []})
        for diagnostic in self._iter_diagnostics():
            bucket = grouped[diagnostic.level]
            bucket["count"] += 1
            if diagnostic.location and diagnostic.location.file:
                bucket["files"].add(diagnostic.location.file)
            if len(bucket["samples"]) < 5:
                bucket["samples"].append(diagnostic.message)
        return {
            "levels": {
                level: {
                    "count": data["count"],
                    "unique_files": len(data["files"]),
                    "samples": data["samples"],
                }
                for level, data in grouped.items()
            }
        }

    def _build_diagnostics_files(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        files = defaultdict(lambda: {"count": 0, "levels": Counter(), "samples": []})
        for diagnostic in self._iter_diagnostics():
            file_path = diagnostic.location.file if diagnostic.location and diagnostic.location.file else "<unknown>"
            bucket = files[file_path]
            bucket["count"] += 1
            bucket["levels"][diagnostic.level] += 1
            if len(bucket["samples"]) < 5:
                bucket["samples"].append(diagnostic.message)
        result = {
            file_path: {
                "file_name": os.path.basename(file_path),
                "count": data["count"],
                "levels": dict(data["levels"]),
                "samples": data["samples"],
            }
            for file_path, data in files.items()
        }
        return {"files": dict(sorted(result.items(), key=lambda item: item[1]["count"], reverse=True)), "total_files": len(result)}

    def _build_diagnostics_patterns(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        patterns = Counter()
        samples = defaultdict(list)
        for diagnostic in self._iter_diagnostics():
            message = diagnostic.message.strip()
            normalized = re.sub(r"\d+", "<n>", message.lower())
            normalized = re.sub(r"'[^']+'", "'<id>'", normalized)
            patterns[normalized] += 1
            if len(samples[normalized]) < 3:
                samples[normalized].append(message)
        return {
            "patterns": [
                {"pattern": pattern, "count": count, "samples": samples[pattern]}
                for pattern, count in patterns.most_common(20)
            ],
            "total_patterns": len(patterns),
        }

    def _build_compilation_phases_overview(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        phase_times = defaultdict(list)
        unit_times = {}
        total_time = 0
        for unit_name, unit_payload in self._parsed_data().items():
            unit_total = 0
            for parsed_file in unit_payload.get(FileType.COMPILATION_PHASES, []):
                for phase in self._iter_phase_records(parsed_file.data.get("phases", []) if isinstance(parsed_file.data, dict) else []):
                    duration = float(phase.get("duration") or 0)
                    phase_name = str(phase.get("name") or "unknown")
                    phase_times[phase_name].append(duration)
                    unit_total += duration
                    total_time += duration
            if unit_total > 0:
                unit_times[unit_name] = unit_total
        phase_breakdown = {
            phase_name: {
                "total_time": sum(times),
                "avg_time": sum(times) / len(times),
                "max_time": max(times),
                "min_time": min(times),
                "occurrences": len(times),
                "percentage": (sum(times) / total_time * 100.0) if total_time else 0.0,
            }
            for phase_name, times in phase_times.items()
            if times
        }
        phase_breakdown = dict(sorted(phase_breakdown.items(), key=lambda item: item[1]["total_time"], reverse=True))
        return {
            "totals": {
                "compilation_time": total_time,
                "unique_phases": len(phase_breakdown),
                "compilation_units": len(unit_times),
            },
            "phase_breakdown": phase_breakdown,
            "unit_times": dict(sorted(unit_times.items(), key=lambda item: item[1], reverse=True)),
            "top_time_consumers": dict(list(phase_breakdown.items())[:5]),
        }

    def _build_compilation_phases_phases(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        phase_data = defaultdict(lambda: {"times": [], "units": set(), "files": []})
        for unit_name, unit_payload in self._parsed_data().items():
            for parsed_file in unit_payload.get(FileType.COMPILATION_PHASES, []):
                for phase in self._iter_phase_records(parsed_file.data.get("phases", []) if isinstance(parsed_file.data, dict) else []):
                    duration = float(phase.get("duration") or 0)
                    phase_name = str(phase.get("name") or "unknown")
                    phase_data[phase_name]["times"].append(duration)
                    phase_data[phase_name]["units"].add(unit_name)
                    phase_data[phase_name]["files"].append({
                        "unit": unit_name,
                        "file": parsed_file.file_path,
                        "duration": duration,
                        "info": phase.get("info", ""),
                    })
        result = {}
        for phase_name, data in phase_data.items():
            times = sorted(data["times"])
            if not times:
                continue
            result[phase_name] = {
                "statistics": {
                    "count": len(times),
                    "total_time": sum(times),
                    "average_time": sum(times) / len(times),
                    "median_time": times[len(times) // 2],
                    "max_time": max(times),
                    "min_time": min(times),
                    "std_deviation": self._stddev(times),
                },
                "distribution": {
                    "units_involved": len(data["units"]),
                    "files_processed": len(data["files"]),
                },
                "performance_insights": {
                    "variability": "high" if max(times) > (sum(times) / len(times)) * 2 else "low",
                    "consistency": "variable" if max(times) > (sum(times) / len(times)) * 1.5 else "consistent",
                },
                "slowest_instances": sorted(data["files"], key=lambda item: item["duration"], reverse=True)[:3],
            }
        return {"phases": dict(sorted(result.items(), key=lambda item: item[1]["statistics"]["total_time"], reverse=True)), "total_phases_analyzed": len(result)}

    def _build_compilation_phases_bottlenecks(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        all_durations = []
        unit_bottlenecks = {}
        phase_outliers = defaultdict(list)
        for unit_name, unit_payload in self._parsed_data().items():
            phases = []
            for parsed_file in unit_payload.get(FileType.COMPILATION_PHASES, []):
                for phase in self._iter_phase_records(parsed_file.data.get("phases", []) if isinstance(parsed_file.data, dict) else []):
                    duration = float(phase.get("duration") or 0)
                    all_durations.append(duration)
                    phases.append({"name": phase.get("name", "unknown"), "duration": duration, "file": parsed_file.file_path})
            if phases:
                phases.sort(key=lambda item: item["duration"], reverse=True)
                total_time = sum(phase["duration"] for phase in phases)
                unit_bottlenecks[unit_name] = {
                    "total_time": total_time,
                    "slowest_phase": phases[0],
                    "top_3_phases": phases[:3],
                }
        all_durations.sort()
        p95 = all_durations[int(len(all_durations) * 0.95)] if all_durations else 0
        p99 = all_durations[int(len(all_durations) * 0.99)] if all_durations else 0
        for unit_name, data in unit_bottlenecks.items():
            for phase in data["top_3_phases"]:
                if phase["duration"] >= p99:
                    phase_outliers["p99"].append({"unit": unit_name, **phase})
                elif phase["duration"] >= p95:
                    phase_outliers["p95"].append({"unit": unit_name, **phase})
        recommendations = []
        if phase_outliers.get("p99"):
            recommendations.append("Investigate phases above the p99 duration threshold.")
        if unit_bottlenecks:
            recommendations.append("Focus on the slowest compilation units first.")
        return {
            "global_thresholds": {"p95_threshold": p95, "p99_threshold": p99},
            "outliers": dict(phase_outliers),
            "unit_bottlenecks": dict(sorted(unit_bottlenecks.items(), key=lambda item: item[1]["total_time"], reverse=True)),
            "recommendations": recommendations,
        }

    def _build_compilation_phases_trends(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        unit_patterns = {}
        totals = []
        for unit_name, unit_payload in self._parsed_data().items():
            phase_times = defaultdict(list)
            for parsed_file in unit_payload.get(FileType.COMPILATION_PHASES, []):
                for phase in self._iter_phase_records(parsed_file.data.get("phases", []) if isinstance(parsed_file.data, dict) else []):
                    phase_times[str(phase.get("name") or "unknown")].append(float(phase.get("duration") or 0))
            if not phase_times:
                continue
            aggregated = {name: sum(values) for name, values in phase_times.items()}
            total = sum(aggregated.values())
            totals.append(total)
            dominant_phase = max(aggregated.items(), key=lambda item: item[1]) if aggregated else ("unknown", 0)
            unit_patterns[unit_name] = {
                "total_time": total,
                "phase_distribution": {name: (value / total * 100.0) if total else 0.0 for name, value in aggregated.items()},
                "dominant_phase": {"name": dominant_phase[0], "duration": dominant_phase[1]},
            }
        return {
            "units": unit_patterns,
            "summary": {
                "unit_count": len(unit_patterns),
                "average_total_time": sum(totals) / len(totals) if totals else 0.0,
                "max_total_time": max(totals) if totals else 0.0,
            },
        }

    def _build_compilation_phases_bindings(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        all_bindings = []
        tool_counts = Counter()
        target_counts = Counter()
        unit_summaries = {}
        for unit_name, unit_payload in self._parsed_data().items():
            unit_bindings = []
            unit_tool_counts = Counter()
            for parsed_file in unit_payload.get(FileType.COMPILATION_PHASES, []):
                bindings = parsed_file.data.get("bindings", []) if isinstance(parsed_file.data, dict) else []
                for binding in bindings:
                    if not isinstance(binding, dict):
                        continue
                    unit_bindings.append(binding)
                    all_bindings.append(binding)
                    tool_counts[binding.get("tool", "unknown")] += 1
                    target_counts[binding.get("target", "unknown")] += 1
                    unit_tool_counts[binding.get("tool", "unknown")] += 1
            if unit_bindings:
                unit_summaries[unit_name] = {
                    "total_bindings": len(unit_bindings),
                    "unique_tools": len(unit_tool_counts),
                    "tool_counts": dict(unit_tool_counts),
                    "bindings": unit_bindings,
                }
        return {
            "summary": {
                "total_bindings": len(all_bindings),
                "unique_tools": len(tool_counts),
                "unique_targets": len(target_counts),
                "compilation_units": len(unit_summaries),
            },
            "tool_counts": dict(tool_counts.most_common()),
            "target_counts": dict(target_counts.most_common()),
            "unit_summaries": unit_summaries,
            "all_bindings": all_bindings,
        }

    def _build_time_trace_overview(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_overview(FileType.TIME_TRACE)

    def _build_time_trace_timeline(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        return self._build_trace_timeline(FileType.TIME_TRACE, query_params)

    def _build_time_trace_hotspots(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_hotspots(FileType.TIME_TRACE, "time-trace")

    def _build_time_trace_categories(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_categories(FileType.TIME_TRACE)

    def _build_time_trace_parallelism(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_parallelism(FileType.TIME_TRACE)

    def _build_time_trace_flamegraph(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        return self._build_trace_flamegraph(FileType.TIME_TRACE, query_params, self._time_trace_parser, "time-trace")

    def _build_time_trace_sandwich(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        return self._build_trace_sandwich(FileType.TIME_TRACE, query_params, self._time_trace_parser, "time-trace")

    def _build_runtime_trace_overview(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_overview(FileType.RUNTIME_TRACE)

    def _build_runtime_trace_timeline(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        return self._build_trace_timeline(FileType.RUNTIME_TRACE, query_params)

    def _build_runtime_trace_hotspots(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_hotspots(FileType.RUNTIME_TRACE, "runtime")

    def _build_runtime_trace_categories(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_categories(FileType.RUNTIME_TRACE)

    def _build_runtime_trace_parallelism(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        return self._build_trace_parallelism(FileType.RUNTIME_TRACE)

    def _build_runtime_trace_flamegraph(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        return self._build_trace_flamegraph(FileType.RUNTIME_TRACE, query_params, self._runtime_trace_parser, "runtime")

    def _build_runtime_trace_sandwich(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        return self._build_trace_sandwich(FileType.RUNTIME_TRACE, query_params, self._runtime_trace_parser, "runtime")

    def _build_binary_size_overview(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        section_sizes = Counter()
        section_counts = Counter()
        size_distribution = []
        total_size = 0
        for size_entry in self._iter_binary_sizes():
            total_size += size_entry.size
            section_sizes[size_entry.section] += size_entry.size
            section_counts[size_entry.section] += 1
            size_distribution.append(size_entry.size)
        size_distribution.sort()
        return {
            "size_statistics": {
                "total_size": total_size,
                "average_section_size": total_size / len(size_distribution) if size_distribution else 0,
                "median_section_size": size_distribution[len(size_distribution) // 2] if size_distribution else 0,
                "largest_section_size": max(size_distribution) if size_distribution else 0,
                "smallest_section_size": min(size_distribution) if size_distribution else 0,
                "total_sections": len(size_distribution),
            },
            "section_breakdown": dict(section_sizes.most_common(15)),
            "section_counts": dict(section_counts.most_common(10)),
            "size_insights": {
                "largest_section": section_sizes.most_common(1)[0][0] if section_sizes else None,
                "dominant_share_percent": (section_sizes.most_common(1)[0][1] / total_size * 100.0) if section_sizes and total_size else 0.0,
            },
        }

    def _build_binary_size_sections(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        sections = defaultdict(lambda: {"total_size": 0, "occurrences": 0, "sizes": []})
        for size_entry in self._iter_binary_sizes():
            bucket = sections[size_entry.section]
            bucket["total_size"] += size_entry.size
            bucket["occurrences"] += 1
            bucket["sizes"].append(size_entry.size)
        result = {}
        for section, data in sections.items():
            sizes = sorted(data["sizes"])
            result[section] = {
                "total_size": data["total_size"],
                "occurrences": data["occurrences"],
                "average_size": data["total_size"] / data["occurrences"] if data["occurrences"] else 0,
                "size_range": {
                    "min": sizes[0] if sizes else 0,
                    "max": sizes[-1] if sizes else 0,
                    "median": sizes[len(sizes) // 2] if sizes else 0,
                },
            }
        return {"sections": dict(sorted(result.items(), key=lambda item: item[1]["total_size"], reverse=True)), "total_unique_sections": len(result)}

    def _build_binary_size_optimization(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        overview = self._build_binary_size_overview({})
        section_breakdown = overview["section_breakdown"]
        total_size = overview["size_statistics"]["total_size"] or 1
        large_sections = [
            {
                "section": section,
                "total_size": size,
                "percentage": size / total_size * 100.0,
                "optimization_type": "large_section",
            }
            for section, size in section_breakdown.items()
            if size / total_size * 100.0 > 5.0
        ]
        return {
            "large_sections": large_sections,
            "redundant_sections": {},
            "optimization_opportunities": [
                f"Investigate size growth in section '{entry['section']}'." for entry in large_sections[:5]
            ],
            "potential_savings": {
                "estimated_bytes": int(sum(entry["total_size"] * 0.1 for entry in large_sections)),
            },
            "binary_size_breakdown": {
                "total_size": total_size,
                "largest_contributors": [entry["section"] for entry in large_sections[:5]],
            },
        }

    def _build_binary_size_comparison(self, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        del query_params
        unit_sizes = {}
        for unit_name, unit_payload in self._parsed_data().items():
            unit_total = 0
            sections = {}
            for parsed_file in unit_payload.get(FileType.BINARY_SIZE, []):
                if not isinstance(parsed_file.data, list):
                    continue
                for size_entry in parsed_file.data:
                    if not isinstance(size_entry, BinarySize):
                        continue
                    unit_total += size_entry.size
                    sections[size_entry.section] = sections.get(size_entry.section, 0) + size_entry.size
            if sections:
                largest_section = max(sections.items(), key=lambda item: item[1])
                unit_sizes[unit_name] = {
                    "total_size": unit_total,
                    "section_count": len(sections),
                    "sections": sections,
                    "largest_section": largest_section,
                }
        return {
            "units": dict(sorted(unit_sizes.items(), key=lambda item: item[1]["total_size"], reverse=True)),
            "summary": {
                "unit_count": len(unit_sizes),
                "largest_unit": max(unit_sizes.items(), key=lambda item: item[1]["total_size"])[0] if unit_sizes else None,
            },
        }

    def _build_trace_overview(self, file_type: FileType) -> Dict[str, Any]:
        categories = Counter()
        phases = Counter()
        durations = []
        total_events = 0
        for event in self._iter_trace_events(file_type):
            total_events += 1
            categories[event.category or "uncategorized"] += 1
            phases[event.phase] += 1
            if event.duration is not None:
                durations.append(event.duration)
        durations.sort()
        return {
            "totals": {
                "events": total_events,
                "categories": len(categories),
                "unique_phases": len(phases),
            },
            "timing_statistics": {
                "total_duration": sum(durations),
                "average_duration": sum(durations) / len(durations) if durations else 0,
                "median_duration": durations[len(durations) // 2] if durations else 0,
                "p95_duration": durations[int(len(durations) * 0.95)] if durations else 0,
                "max_duration": max(durations) if durations else 0,
                "events_with_duration": len(durations),
            },
            "category_distribution": dict(categories.most_common(10)),
            "phase_distribution": dict(phases.most_common(10)),
        }

    def _build_trace_timeline(self, file_type: FileType, query_params: Dict[str, List[str]]) -> Dict[str, Any]:
        limit = int(query_params.get("limit", ["1000"])[0])
        timeline = []
        processes = set()
        threads = set()
        for unit_name, unit_payload in self._parsed_data().items():
            for parsed_file in unit_payload.get(file_type, []):
                if not isinstance(parsed_file.data, list):
                    continue
                for event in parsed_file.data:
                    if not isinstance(event, TraceEvent):
                        continue
                    if event.pid is not None:
                        processes.add(event.pid)
                    if event.tid is not None:
                        threads.add(event.tid)
                    timeline.append({
                        "unit": unit_name,
                        "timestamp": event.timestamp,
                        "name": event.name,
                        "category": event.category,
                        "phase": event.phase,
                        "duration": event.duration,
                        "pid": event.pid,
                        "tid": event.tid,
                        "args": event.args or {},
                    })
        timeline.sort(key=lambda item: item["timestamp"])
        timeline = timeline[:limit]
        return {
            "timeline": timeline,
            "metadata": {
                "total_events_shown": len(timeline),
                "unique_processes": len(processes),
                "unique_threads": len(threads),
                "time_range": {
                    "start": min((item["timestamp"] for item in timeline), default=0),
                    "end": max((item["timestamp"] for item in timeline), default=0),
                },
            },
        }

    def _build_trace_hotspots(self, file_type: FileType, source_name: str) -> Dict[str, Any]:
        durations = defaultdict(list)
        category_times = defaultdict(float)
        thread_times = defaultdict(float)
        for event in self._iter_trace_events(file_type):
            if event.duration is None:
                continue
            durations[event.name].append(event.duration)
            category_times[event.category or "uncategorized"] += event.duration
            if event.tid is not None:
                thread_times[event.tid] += event.duration
        event_hotspots = []
        total_trace_time = sum(sum(values) for values in durations.values())
        for event_name, values in durations.items():
            total_time = sum(values)
            event_hotspots.append({
                "event_name": event_name,
                "total_time": total_time,
                "average_time": total_time / len(values),
                "max_time": max(values),
                "occurrences": len(values),
                "percentage_of_total": (total_time / total_trace_time * 100.0) if total_trace_time else 0.0,
                "source": source_name,
            })
        event_hotspots.sort(key=lambda item: item["total_time"], reverse=True)
        return {
            "event_hotspots": event_hotspots[:20],
            "category_hotspots": dict(sorted(category_times.items(), key=lambda item: item[1], reverse=True)[:10]),
            "thread_hotspots": dict(sorted(thread_times.items(), key=lambda item: item[1], reverse=True)[:10]),
            "total_trace_time": total_trace_time,
        }

    def _build_trace_categories(self, file_type: FileType) -> Dict[str, Any]:
        categories = defaultdict(lambda: {"events": [], "total_time": 0, "event_names": Counter(), "phases": Counter()})
        for event in self._iter_trace_events(file_type):
            category_name = event.category or "uncategorized"
            bucket = categories[category_name]
            bucket["event_names"][event.name] += 1
            bucket["phases"][event.phase] += 1
            if event.duration is not None:
                bucket["total_time"] += event.duration
            if len(bucket["events"]) < 5:
                bucket["events"].append({"name": event.name, "duration": event.duration, "timestamp": event.timestamp, "args": event.args or {}})
        result = {
            category_name: {
                "total_time": data["total_time"],
                "unique_event_names": len(data["event_names"]),
                "total_events": sum(data["event_names"].values()),
                "top_events": dict(data["event_names"].most_common(5)),
                "phase_distribution": dict(data["phases"]),
                "sample_events": data["events"],
            }
            for category_name, data in categories.items()
        }
        return {"categories": dict(sorted(result.items(), key=lambda item: item[1]["total_time"], reverse=True))}

    def _build_trace_parallelism(self, file_type: FileType) -> Dict[str, Any]:
        thread_times = defaultdict(float)
        thread_counts = Counter()
        max_overlap = 0
        intervals = []
        for event in self._iter_trace_events(file_type):
            if event.tid is not None and event.duration is not None:
                thread_times[event.tid] += event.duration
                thread_counts[event.tid] += 1
                intervals.append((event.timestamp, 1))
                intervals.append((event.timestamp + event.duration, -1))
        overlap = 0
        for _, delta in sorted(intervals):
            overlap += delta
            max_overlap = max(max_overlap, overlap)
        return {
            "parallelism": {
                "thread_count": len(thread_times),
                "max_concurrent_events": max_overlap,
                "thread_time": dict(sorted(thread_times.items(), key=lambda item: item[1], reverse=True)),
                "thread_event_count": dict(thread_counts),
            }
        }

    def _build_trace_flamegraph(self, file_type: FileType, query_params: Dict[str, List[str]], parser: Any, source_name: str) -> Dict[str, Any]:
        unit_filter = query_params.get("unit", [None])[0]
        samples = []
        for unit_name, unit_payload in self._parsed_data().items():
            if unit_filter and unit_name != unit_filter:
                continue
            for parsed_file in unit_payload.get(file_type, []):
                if not isinstance(parsed_file.data, list):
                    continue
                flamegraph_data = parser.get_flamegraph_data(parsed_file.data)
                for sample in flamegraph_data.get("samples", []):
                    sample["unit"] = unit_name
                    sample["source"] = source_name
                    samples.append(sample)
        samples.sort(key=lambda item: item.get("timestamp", 0))
        return {"samples": samples, "total_samples": len(samples)}

    def _build_trace_sandwich(self, file_type: FileType, query_params: Dict[str, List[str]], parser: Any, source_name: str) -> Dict[str, Any]:
        unit_filter = query_params.get("unit", [None])[0]
        function_map = {}
        for unit_name, unit_payload in self._parsed_data().items():
            if unit_filter and unit_name != unit_filter:
                continue
            for parsed_file in unit_payload.get(file_type, []):
                if not isinstance(parsed_file.data, list):
                    continue
                sandwich_data = parser.get_sandwich_data(parsed_file.data)
                for function in sandwich_data.get("functions", []):
                    name = function["name"]
                    if name not in function_map:
                        function_map[name] = {
                            "name": name,
                            "total_time": 0,
                            "call_count": 0,
                            "category": function.get("category", "unknown"),
                            "units": [],
                            "source": source_name,
                        }
                    function_map[name]["total_time"] += function["total_time"]
                    function_map[name]["call_count"] += function["call_count"]
                    function_map[name]["units"].append(unit_name)
        functions = []
        for function in function_map.values():
            function["avg_time"] = function["total_time"] / function["call_count"] if function["call_count"] else 0
            functions.append(function)
        functions.sort(key=lambda item: item["total_time"], reverse=True)
        return {"functions": functions, "total_functions": len(functions)}

    def _iter_diagnostics(self) -> Iterable[Diagnostic]:
        for unit_payload in self._parsed_data().values():
            for parsed_file in unit_payload.get(FileType.DIAGNOSTICS, []):
                if not isinstance(parsed_file.data, list):
                    continue
                for diagnostic in parsed_file.data:
                    if isinstance(diagnostic, Diagnostic):
                        yield diagnostic

    def _iter_remarks(self) -> Iterable[Remark]:
        for unit_payload in self._parsed_data().values():
            for parsed_file in unit_payload.get(FileType.REMARKS, []):
                if not isinstance(parsed_file.data, list):
                    continue
                for remark in parsed_file.data:
                    if isinstance(remark, Remark):
                        yield remark

    def _iter_binary_sizes(self) -> Iterable[BinarySize]:
        for unit_payload in self._parsed_data().values():
            for parsed_file in unit_payload.get(FileType.BINARY_SIZE, []):
                if not isinstance(parsed_file.data, list):
                    continue
                for size_entry in parsed_file.data:
                    if isinstance(size_entry, BinarySize):
                        yield size_entry

    def _iter_trace_events(self, file_type: FileType) -> Iterable[TraceEvent]:
        for unit_payload in self._parsed_data().values():
            for parsed_file in unit_payload.get(file_type, []):
                if not isinstance(parsed_file.data, list):
                    continue
                for event in parsed_file.data:
                    if isinstance(event, TraceEvent):
                        yield event

    def _iter_phase_records(self, phase_records: Iterable[Any]) -> Iterable[Dict[str, Any]]:
        for phase in phase_records:
            if isinstance(phase, dict):
                yield phase

    def _stddev(self, values: List[float]) -> float:
        if len(values) <= 1:
            return 0.0
        mean = sum(values) / len(values)
        variance = sum((value - mean) ** 2 for value in values) / len(values)
        return variance ** 0.5

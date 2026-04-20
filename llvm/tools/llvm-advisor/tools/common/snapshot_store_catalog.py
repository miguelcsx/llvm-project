from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional

from .collector import ArtifactCollector
from .snapshot_records import RepresentationDescriptorRecord, UnitRecord


class SnapshotStoreCatalogMixin:
    def sync_snapshot_units(self, snapshot_record, collector: ArtifactCollector) -> List[UnitRecord]:
        with self._lock:
            discovered_units = collector.discover_compilation_units(snapshot_record.data_dir)
            existing_units = [
                UnitRecord.from_dict(payload).to_dict()
                for payload in self._read_index(self._unit_index_path)
                if payload.get("snapshot_id") != snapshot_record.snapshot_id
            ]
            snapshot_units = [
                self._build_unit_record(snapshot_record, compilation_unit)
                for compilation_unit in discovered_units
            ]
            representation_descriptors = [
                descriptor.to_dict()
                for unit_record, compilation_unit in zip(snapshot_units, discovered_units)
                for descriptor in self._build_representation_descriptor_records(
                    snapshot_record,
                    unit_record,
                    compilation_unit,
                )
            ]
            self._write_index(
                self._unit_index_path,
                [*existing_units, *[unit.to_dict() for unit in snapshot_units]],
            )
            existing_descriptors = [
                payload
                for payload in self._read_index(self._representation_descriptor_index_path)
                if payload.get("snapshot_id") != snapshot_record.snapshot_id
            ]
            self._write_index(
                self._representation_descriptor_index_path,
                [*existing_descriptors, *representation_descriptors],
            )
            for unit_record in snapshot_units:
                self._append_journal("unit", unit_record.unit_id, unit_record.to_dict())
            for descriptor_payload in representation_descriptors:
                descriptor_id = str(descriptor_payload.get("descriptor_id") or "").strip()
                if descriptor_id:
                    self._append_journal(
                        "representation_descriptor",
                        descriptor_id,
                        descriptor_payload,
                    )
            return snapshot_units

    def list_units(self, snapshot_id: str) -> List[UnitRecord]:
        with self._lock:
            return [
                UnitRecord.from_dict(payload)
                for payload in self._read_index(self._unit_index_path)
                if payload.get("snapshot_id") == snapshot_id
            ]

    def get_unit(self, snapshot_id: str, unit_id: str) -> Optional[UnitRecord]:
        return next((unit for unit in self.list_units(snapshot_id) if unit.unit_id == unit_id), None)

    def get_unit_by_name(self, snapshot_id: str, unit_name: str) -> Optional[UnitRecord]:
        return next((unit for unit in self.list_units(snapshot_id) if unit.unit_name == unit_name), None)

    def list_representation_descriptors(
        self,
        snapshot_id: str,
        *,
        unit_name: Optional[str] = None,
        source_ref: Optional[str] = None,
        representation_kind: Optional[str] = None,
    ) -> List[RepresentationDescriptorRecord]:
        with self._lock:
            descriptors = [
                RepresentationDescriptorRecord.from_dict(payload)
                for payload in self._read_index(self._representation_descriptor_index_path)
                if str(payload.get("snapshot_id") or "") == snapshot_id
            ]
            if unit_name:
                descriptors = [d for d in descriptors if d.unit_name == unit_name]
            if source_ref:
                descriptors = [d for d in descriptors if d.source_ref == source_ref]
            if representation_kind:
                descriptors = [
                    d for d in descriptors if d.representation_kind == representation_kind
                ]
            descriptors.sort(
                key=lambda descriptor: (
                    descriptor.unit_name,
                    descriptor.source_ref,
                    descriptor.representation_kind,
                    descriptor.variant,
                )
            )
            return descriptors

    def build_source_file_listing(self, snapshot_id: str, *, unit_name: Optional[str] = None) -> List[Dict[str, Any]]:
        descriptors = self.list_representation_descriptors(snapshot_id, unit_name=unit_name)
        files: Dict[tuple[str, str], Dict[str, Any]] = {}
        for descriptor in descriptors:
            unit_record = self.get_unit(snapshot_id, descriptor.unit_id)
            file_key = (descriptor.unit_name, descriptor.source_ref)
            entry = files.setdefault(
                file_key,
                {
                    "path": descriptor.source_ref,
                    "name": Path(descriptor.source_ref).name,
                    "display_name": descriptor.source_ref,
                    "unit": descriptor.unit_name,
                    "is_primary": bool(unit_record and descriptor.source_ref == unit_record.source_path),
                    "available_representations": [],
                },
            )
            if descriptor.representation_kind and descriptor.representation_kind not in entry["available_representations"]:
                entry["available_representations"].append(descriptor.representation_kind)
        payload = list(files.values())
        for entry in payload:
            entry["available_representations"].sort()
        payload.sort(key=lambda entry: (str(entry["unit"]), str(entry["path"])))
        return payload

    def get_source_file_entry(self, snapshot_id: str, file_path: str, *, unit_name: Optional[str] = None) -> Optional[Dict[str, Any]]:
        for entry in self.build_source_file_listing(snapshot_id, unit_name=unit_name):
            if entry["path"] == file_path:
                return entry
        return None

    def read_source_content(self, snapshot_id: str, file_path: str, *, unit_name: Optional[str] = None) -> Optional[Dict[str, Any]]:
        descriptors = self.list_representation_descriptors(
            snapshot_id,
            unit_name=unit_name,
            source_ref=file_path,
        )
        if not descriptors:
            return None
        source_path = next((descriptor.source_path for descriptor in descriptors if descriptor.source_path), "")
        if not source_path:
            return None
        content = self._read_text_path(Path(source_path))
        if content is None:
            return None
        return {
            "file_path": file_path,
            "source_path": source_path,
            "unit_name": descriptors[0].unit_name,
            "language": self._detect_language(file_path),
            "content": content,
        }

    def read_representation_content(
        self,
        snapshot_id: str,
        representation_kind: str,
        file_path: str,
        *,
        unit_name: Optional[str] = None,
    ) -> Optional[Dict[str, Any]]:
        descriptors = self.list_representation_descriptors(
            snapshot_id,
            unit_name=unit_name,
            source_ref=file_path,
            representation_kind=representation_kind,
        )
        if not descriptors:
            return None
        descriptor = descriptors[0]
        unit_record = self.get_unit(snapshot_id, descriptor.unit_id)
        if unit_record is None:
            return None
        absolute_path = Path(unit_record.unit_path) / descriptor.relative_path
        content = self._read_text_path(absolute_path)
        if content is None:
            return None
        return {
            "representation_kind": representation_kind,
            "file_path": file_path,
            "unit_name": descriptor.unit_name,
            "descriptor_id": descriptor.descriptor_id,
            "content": content,
            "language": self._representation_view_language(representation_kind),
            "mapping_format": descriptor.mapping_format,
            "line_mapping": self._read_json_path(
                Path(unit_record.unit_path) / descriptor.mapping_relative_path
            )
            if descriptor.mapping_relative_path
            else None,
        }

    def query_representation_catalog(
        self,
        snapshot_id: str,
        *,
        unit_name: Optional[str] = None,
        source_ref: Optional[str] = None,
        representation_kind: Optional[str] = None,
    ) -> Dict[str, Any]:
        descriptors = self.list_representation_descriptors(
            snapshot_id,
            unit_name=unit_name,
            source_ref=source_ref,
            representation_kind=representation_kind,
        )
        return {
            "snapshot_id": snapshot_id,
            "filters": {
                "unit_name": unit_name,
                "source_ref": source_ref,
                "representation_kind": representation_kind,
            },
            "descriptors": [descriptor.to_dict() for descriptor in descriptors],
            "count": len(descriptors),
        }

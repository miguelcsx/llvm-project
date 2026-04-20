from __future__ import annotations

import threading
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from .api_contract import utc_timestamp
from .collector import ArtifactCollector
from .snapshot_records import (
    _INDEX_FILE_NAMES,
    CapabilityRunRecord,
    JobRecord,
    RepresentationDescriptorRecord,
    RepresentationRecord,
    SnapshotRecord,
    UnitRecord,
    generate_ulid_like_id,
    sha256_digest,
    stable_json_bytes,
)
from .snapshot_store_catalog import SnapshotStoreCatalogMixin
from .snapshot_store_jobs import SnapshotStoreJobsMixin
from .snapshot_store_persistence import SnapshotStorePersistenceMixin
from .snapshot_store_sync import SnapshotStoreSyncMixin


class SnapshotStore(
    SnapshotStoreCatalogMixin,
    SnapshotStoreJobsMixin,
    SnapshotStorePersistenceMixin,
    SnapshotStoreSyncMixin,
):
    def __init__(self, data_root: Path) -> None:
        self._root = data_root / ".llvm-advisor-store"
        self._metadata_root = self._root / "metadata"
        self._journal_root = self._metadata_root / "journal"
        self._index_root = self._metadata_root / "index"
        self._cas_root = self._root / "cas" / "sha256"
        self._jobs_root = self._root / "jobs"
        self._locks_root = self._root / "locks"
        self._tmp_root = self._root / "tmp"
        self._lock = threading.RLock()

        for directory in (
            self._journal_root,
            self._index_root,
            self._cas_root,
            self._jobs_root / "active",
            self._jobs_root / "finished",
            self._jobs_root / "failed",
            self._locks_root,
            self._tmp_root,
        ):
            directory.mkdir(parents=True, exist_ok=True)

        self._snapshot_index_path = self._index_root / _INDEX_FILE_NAMES["snapshots"]
        self._unit_index_path = self._index_root / _INDEX_FILE_NAMES["units"]
        self._capability_run_index_path = self._index_root / _INDEX_FILE_NAMES["capability_runs"]
        self._representation_index_path = self._index_root / _INDEX_FILE_NAMES["representations"]
        self._representation_descriptor_index_path = self._index_root / _INDEX_FILE_NAMES["representation_descriptors"]
        self._job_index_path = self._index_root / _INDEX_FILE_NAMES["jobs"]
        self._recover_indexes_from_journal()

    def list_snapshots(self) -> List[SnapshotRecord]:
        with self._lock:
            return [
                SnapshotRecord.from_dict(payload)
                for payload in self._read_index(self._snapshot_index_path)
            ]

    def get_snapshot(self, snapshot_id: str) -> Optional[SnapshotRecord]:
        return next((record for record in self.list_snapshots() if record.snapshot_id == snapshot_id), None)

    def create_snapshot(
        self,
        *,
        project_id: str,
        source_root: str,
        build_dir: str,
        data_dir: str,
        profile: str,
        metadata: Optional[Dict[str, Any]] = None,
        parent_snapshot_id: Optional[str] = None,
    ) -> SnapshotRecord:
        with self._lock:
            snapshot_record = SnapshotRecord(
                schema_version="1.0",
                snapshot_id=generate_ulid_like_id("snap"),
                project_id=project_id,
                source_root=source_root,
                build_dir=build_dir,
                data_dir=data_dir,
                profile=profile,
                created_at=utc_timestamp(),
                producer_version="llvm-advisor/1.0.0",
                parent_snapshot_id=parent_snapshot_id,
                source_revision=self._build_source_revision(data_dir),
                build_config=self._build_build_config(metadata or {}),
                metadata=metadata or {},
            )
            snapshots = self._read_index(self._snapshot_index_path)
            snapshots.append(snapshot_record.to_dict())
            self._write_index(self._snapshot_index_path, snapshots)
            self._append_journal("snapshot", snapshot_record.snapshot_id, snapshot_record.to_dict())
            return snapshot_record

    def ensure_bootstrap_snapshot(self, *, data_dir: Path) -> SnapshotRecord:
        with self._lock:
            normalized_data_dir = str(Path(data_dir).expanduser())
            existing = [
                snapshot for snapshot in self.list_snapshots() if str(snapshot.data_dir) == normalized_data_dir
            ]
            if existing:
                return existing[-1]
            run_summary = self._load_run_summary(data_dir)
            snapshot_record = self.create_snapshot(
                project_id=data_dir.name or "llvm-project",
                source_root=str(data_dir.parent),
                build_dir=str(data_dir.parent),
                data_dir=normalized_data_dir,
                profile="balanced",
                metadata=run_summary,
            )
            self.sync_snapshot_units(snapshot_record, ArtifactCollector())
            return snapshot_record

    def list_capability_runs(self, snapshot_id: Optional[str] = None) -> List[CapabilityRunRecord]:
        with self._lock:
            runs = [
                CapabilityRunRecord.from_dict(payload)
                for payload in self._read_index(self._capability_run_index_path)
            ]
            return runs if snapshot_id is None else [run for run in runs if run.snapshot_id == snapshot_id]

    def get_cached_capability_run(
        self,
        *,
        snapshot_id: str,
        unit_id: str,
        capability_id: str,
        capability_version: str,
        cache_key: str,
    ) -> Optional[CapabilityRunRecord]:
        return next(
            (
                run
                for run in self.list_capability_runs(snapshot_id)
                if run.unit_id == unit_id
                and run.capability_id == capability_id
                and run.capability_version == capability_version
                and run.cache_key == cache_key
                and run.state == "succeeded"
            ),
            None,
        )

    def list_representations(self, snapshot_id: Optional[str] = None) -> List[RepresentationRecord]:
        with self._lock:
            representations = [
                RepresentationRecord.from_dict(payload)
                for payload in self._read_index(self._representation_index_path)
            ]
            return representations if snapshot_id is None else [r for r in representations if r.snapshot_id == snapshot_id]

    def get_representation(self, representation_id: str) -> Optional[RepresentationRecord]:
        return next(
            (representation for representation in self.list_representations() if representation.representation_id == representation_id),
            None,
        )

    def read_json_representation(self, representation_id: str) -> Optional[Dict[str, Any]]:
        representation_record = self.get_representation(representation_id)
        if representation_record is None:
            return None
        payload = self._read_cas_payload(representation_record.content_digest)
        if payload is None:
            return None
        try:
            decoded = __import__("json").loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return None
        return decoded if isinstance(decoded, dict) else None

    def store_capability_result(
        self,
        *,
        snapshot_id: str,
        unit_id: str,
        capability_id: str,
        capability_version: str,
        cache_key: str,
        schema_id: str,
        payload: Dict[str, Any],
    ) -> CapabilityRunRecord:
        with self._lock:
            existing_run = self.get_cached_capability_run(
                snapshot_id=snapshot_id,
                unit_id=unit_id,
                capability_id=capability_id,
                capability_version=capability_version,
                cache_key=cache_key,
            )
            if (
                existing_run is not None
                and existing_run.representation_refs
                and all(
                    self.read_json_representation(representation_id) is not None
                    for representation_id in existing_run.representation_refs
                )
            ):
                return existing_run

            payload_bytes = stable_json_bytes(payload)
            content_digest = self._write_cas_payload(payload_bytes)
            representation_record = RepresentationRecord(
                schema_version="1.0",
                representation_id=generate_ulid_like_id("repr"),
                snapshot_id=snapshot_id,
                unit_id=unit_id,
                capability_id=capability_id,
                capability_version=capability_version,
                content_digest=content_digest,
                content_type="application/json",
                byte_size=len(payload_bytes),
                schema_id=schema_id,
                created_at=utc_timestamp(),
            )
            representations = self._read_index(self._representation_index_path)
            representations.append(representation_record.to_dict())
            self._write_index(self._representation_index_path, representations)
            self._append_journal("representation", representation_record.representation_id, representation_record.to_dict())

            capability_run = CapabilityRunRecord(
                schema_version="1.0",
                cap_run_id=generate_ulid_like_id("capr"),
                snapshot_id=snapshot_id,
                unit_id=unit_id,
                capability_id=capability_id,
                capability_version=capability_version,
                state="succeeded",
                cache_key=cache_key,
                started_at=utc_timestamp(),
                ended_at=utc_timestamp(),
                representation_refs=[representation_record.representation_id],
                error=None,
            )
            runs = self._read_index(self._capability_run_index_path)
            runs.append(capability_run.to_dict())
            self._write_index(self._capability_run_index_path, runs)
            self._append_journal("capability_run", capability_run.cap_run_id, capability_run.to_dict())
            return capability_run

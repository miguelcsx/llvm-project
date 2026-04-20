from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from .api_contract import utc_timestamp
from .snapshot_records import _JOURNAL_ROLLOVER_BYTES, generate_ulid_like_id, sha256_digest


class SnapshotStorePersistenceMixin:
    def compact_indexes(self) -> None:
        with self._lock:
            for index_path in (
                self._snapshot_index_path,
                self._unit_index_path,
                self._capability_run_index_path,
                self._representation_index_path,
                self._representation_descriptor_index_path,
                self._job_index_path,
            ):
                self._write_index(index_path, self._read_index(index_path))
            self._rewrite_compacted_journal()
            self._append_journal("maintenance", "compact", {"completed_at": utc_timestamp()})

    def run_garbage_collection(self) -> Dict[str, int]:
        with self._lock:
            live_digests = {representation.content_digest for representation in self.list_representations()}
            deleted_blob_count = 0
            deleted_byte_count = 0
            for blob_path in self._cas_root.glob("*/*/*.blob"):
                digest = f"sha256:{blob_path.stem}"
                if digest in live_digests:
                    continue
                try:
                    deleted_byte_count += blob_path.stat().st_size
                    blob_path.unlink()
                    deleted_blob_count += 1
                except OSError:
                    continue
            result = {
                "deleted_blob_count": deleted_blob_count,
                "deleted_byte_count": deleted_byte_count,
            }
            self._append_journal("maintenance", "gc", result)
            return result

    def _read_index(self, index_path: Path) -> List[Dict[str, Any]]:
        if not index_path.exists():
            return []
        try:
            payload = json.loads(index_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return []
        return [entry for entry in payload if isinstance(entry, dict)] if isinstance(payload, list) else []

    def _write_index(self, index_path: Path, records: Iterable[Dict[str, Any]]) -> None:
        temp_path = self._tmp_root / f"{index_path.name}.{generate_ulid_like_id('tmp')}.part"
        with temp_path.open("w", encoding="utf-8") as handle:
            json.dump(list(records), handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_path, index_path)

    def _append_journal(self, record_type: str, record_id: str, payload: Dict[str, Any]) -> None:
        entry = {
            "record_type": record_type,
            "record_id": record_id,
            "schema_version": "1.0",
            "timestamp": utc_timestamp(),
            "payload": payload,
        }
        with self._current_journal_path().open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(entry, sort_keys=True))
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())

    def _write_cas_payload(self, payload: bytes) -> str:
        content_digest = sha256_digest(payload)
        digest_hex = content_digest.split(":", 1)[1]
        destination_path = self._cas_root / digest_hex[:2] / digest_hex[2:4] / f"{digest_hex}.blob"
        destination_path.parent.mkdir(parents=True, exist_ok=True)
        if destination_path.exists():
            existing_payload = self._read_cas_payload(content_digest)
            if existing_payload == payload:
                return content_digest
            return content_digest
        temp_path = self._tmp_root / f"{generate_ulid_like_id('cas')}.part"
        with temp_path.open("wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_path, destination_path)
        return content_digest

    def _read_cas_payload(self, content_digest: str) -> Optional[bytes]:
        try:
            _, digest_hex = content_digest.split(":", 1)
        except ValueError:
            return None
        cas_path = self._cas_root / digest_hex[:2] / digest_hex[2:4] / f"{digest_hex}.blob"
        if not cas_path.exists():
            return None
        try:
            payload = cas_path.read_bytes()
        except OSError:
            return None
        return payload if sha256_digest(payload) == content_digest else None

    def _read_text_path(self, file_path: Path) -> Optional[str]:
        try:
            return file_path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            return None

    def _read_json_path(self, file_path: Path) -> Optional[Dict[str, Any]]:
        try:
            payload = json.loads(file_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None
        return payload if isinstance(payload, dict) else None

    def _journal_segment_paths(self) -> List[Path]:
        return sorted(self._journal_root.glob("*.jsonl"))

    def _current_journal_path(self) -> Path:
        segments = self._journal_segment_paths()
        if not segments:
            return self._journal_root / "000001.jsonl"
        current = segments[-1]
        try:
            if current.stat().st_size < _JOURNAL_ROLLOVER_BYTES:
                return current
        except OSError:
            return current
        try:
            next_index = int(current.stem) + 1
        except ValueError:
            next_index = len(segments) + 1
        return self._journal_root / f"{next_index:06d}.jsonl"

    def _recover_indexes_from_journal(self) -> None:
        snapshots = {str(payload.get("snapshot_id") or ""): payload for payload in self._read_index(self._snapshot_index_path) if str(payload.get("snapshot_id") or "").strip()}
        units = {str(payload.get("unit_id") or ""): payload for payload in self._read_index(self._unit_index_path) if str(payload.get("unit_id") or "").strip()}
        capability_runs = {str(payload.get("cap_run_id") or ""): payload for payload in self._read_index(self._capability_run_index_path) if str(payload.get("cap_run_id") or "").strip()}
        representations = {str(payload.get("representation_id") or ""): payload for payload in self._read_index(self._representation_index_path) if str(payload.get("representation_id") or "").strip()}
        representation_descriptors = {str(payload.get("descriptor_id") or ""): payload for payload in self._read_index(self._representation_descriptor_index_path) if str(payload.get("descriptor_id") or "").strip()}
        jobs = {str(payload.get("job_id") or ""): payload for payload in self._read_index(self._job_index_path) if str(payload.get("job_id") or "").strip()}
        saw_entries = False

        for journal_path in self._journal_segment_paths():
            try:
                lines = journal_path.read_text(encoding="utf-8").splitlines()
            except OSError:
                continue
            for line in lines:
                line = line.strip()
                if not line:
                    continue
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if not isinstance(entry, dict):
                    continue
                payload = entry.get("payload")
                record_type = str(entry.get("record_type") or "")
                record_id = str(entry.get("record_id") or "")
                if not isinstance(payload, dict) or not record_id:
                    continue
                saw_entries = True
                if record_type == "snapshot":
                    snapshots[record_id] = payload
                elif record_type == "unit":
                    units[record_id] = payload
                elif record_type == "representation":
                    representations[record_id] = payload
                elif record_type == "representation_descriptor":
                    representation_descriptors[record_id] = payload
                elif record_type == "capability_run":
                    capability_runs[record_id] = payload
                elif record_type.startswith("job"):
                    jobs[record_id] = payload

        valid_representations = {
            representation_id: payload
            for representation_id, payload in representations.items()
            if self._read_cas_payload(str(payload.get("content_digest") or "")) is not None
        }
        valid_capability_runs = {}
        for run_id, payload in capability_runs.items():
            representation_refs = [
                str(representation_id)
                for representation_id in payload.get("representation_refs", [])
                if str(representation_id) in valid_representations
            ]
            if representation_refs:
                valid_capability_runs[run_id] = {**payload, "representation_refs": representation_refs}
        if not saw_entries and snapshots:
            valid_representations = valid_representations or representations
            valid_capability_runs = valid_capability_runs or capability_runs

        self._write_index(self._snapshot_index_path, snapshots.values())
        self._write_index(self._unit_index_path, units.values())
        self._write_index(self._capability_run_index_path, valid_capability_runs.values())
        self._write_index(self._representation_index_path, valid_representations.values())
        self._write_index(self._representation_descriptor_index_path, representation_descriptors.values())
        self._write_index(self._job_index_path, jobs.values())

    def _rewrite_compacted_journal(self) -> None:
        temp_path = self._tmp_root / f"{generate_ulid_like_id('journal')}.jsonl"
        with temp_path.open("w", encoding="utf-8") as handle:
            for record_type, record_id_key, index_path in (
                ("snapshot", "snapshot_id", self._snapshot_index_path),
                ("unit", "unit_id", self._unit_index_path),
                ("representation", "representation_id", self._representation_index_path),
                ("representation_descriptor", "descriptor_id", self._representation_descriptor_index_path),
                ("capability_run", "cap_run_id", self._capability_run_index_path),
                ("job", "job_id", self._job_index_path),
            ):
                for payload in self._read_index(index_path):
                    record_id = str(payload.get(record_id_key) or "").strip()
                    if not record_id:
                        continue
                    handle.write(json.dumps({
                        "record_type": record_type,
                        "record_id": record_id,
                        "schema_version": "1.0",
                        "timestamp": utc_timestamp(),
                        "payload": payload,
                    }, sort_keys=True))
                    handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        compacted_path = self._journal_root / "000001.jsonl"
        os.replace(temp_path, compacted_path)
        for journal_path in self._journal_segment_paths():
            if journal_path != compacted_path:
                try:
                    journal_path.unlink()
                except OSError:
                    continue

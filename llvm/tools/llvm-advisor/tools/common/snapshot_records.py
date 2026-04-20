from __future__ import annotations

import hashlib
import json
import random
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

_CROCKFORD_BASE32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
_INDEX_FILE_NAMES = {
    "snapshots": "snapshots.json",
    "units": "units.json",
    "capability_runs": "capability_runs.json",
    "representations": "representations.json",
    "representation_descriptors": "representation_descriptors.json",
    "jobs": "jobs.json",
}
_JOB_ACTIVE_STATES = {"queued", "running", "cancel_requested"}
_JOURNAL_ROLLOVER_BYTES = 128 * 1024 * 1024


def _encode_crockford(value: int, length: int) -> str:
    encoded = []
    for _ in range(length):
        encoded.append(_CROCKFORD_BASE32[value & 0x1F])
        value >>= 5
    return "".join(reversed(encoded))


def generate_ulid_like_id(prefix: str) -> str:
    timestamp_millis = int(time.time() * 1000)
    randomness = random.getrandbits(80)
    return (
        f"{prefix}_"
        f"{_encode_crockford(timestamp_millis, 10)}"
        f"{_encode_crockford(randomness, 16)}"
    )


def stable_json_bytes(payload: Any) -> bytes:
    return json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")


def sha256_hex(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_digest(payload: bytes) -> str:
    return "sha256:" + sha256_hex(payload)


@dataclass(frozen=True)
class SnapshotRecord:
    schema_version: str
    snapshot_id: str
    project_id: str
    source_root: str
    build_dir: str
    data_dir: str
    profile: str
    created_at: str
    producer_version: str
    parent_snapshot_id: Optional[str]
    source_revision: Dict[str, Any]
    build_config: Dict[str, Any]
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "snapshot_id": self.snapshot_id,
            "project_id": self.project_id,
            "source_root": self.source_root,
            "build_dir": self.build_dir,
            "data_dir": self.data_dir,
            "profile": self.profile,
            "created_at": self.created_at,
            "producer_version": self.producer_version,
            "parent_snapshot_id": self.parent_snapshot_id,
            "source_revision": self.source_revision,
            "build_config": self.build_config,
            "metadata": self.metadata,
        }

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "SnapshotRecord":
        return cls(
            schema_version=str(payload["schema_version"]),
            snapshot_id=str(payload["snapshot_id"]),
            project_id=str(payload["project_id"]),
            source_root=str(payload.get("source_root", "")),
            build_dir=str(payload.get("build_dir", "")),
            data_dir=str(payload.get("data_dir", "")),
            profile=str(payload.get("profile", "balanced")),
            created_at=str(payload["created_at"]),
            producer_version=str(payload["producer_version"]),
            parent_snapshot_id=payload.get("parent_snapshot_id"),
            source_revision=dict(payload.get("source_revision", {})),
            build_config=dict(payload.get("build_config", {})),
            metadata=dict(payload.get("metadata", {})),
        )


@dataclass(frozen=True)
class UnitRecord:
    schema_version: str
    snapshot_id: str
    unit_id: str
    unit_name: str
    unit_path: str
    source_path: str
    language: str
    target_triple: str
    command_fingerprint: str
    command_normalized: List[str]
    source_fingerprint: str
    created_at: str
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "snapshot_id": self.snapshot_id,
            "unit_id": self.unit_id,
            "unit_name": self.unit_name,
            "unit_path": self.unit_path,
            "source_path": self.source_path,
            "language": self.language,
            "target_triple": self.target_triple,
            "command_fingerprint": self.command_fingerprint,
            "command_normalized": self.command_normalized,
            "source_fingerprint": self.source_fingerprint,
            "created_at": self.created_at,
            "metadata": self.metadata,
        }

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "UnitRecord":
        return cls(
            schema_version=str(payload["schema_version"]),
            snapshot_id=str(payload["snapshot_id"]),
            unit_id=str(payload["unit_id"]),
            unit_name=str(payload["unit_name"]),
            unit_path=str(payload.get("unit_path", "")),
            source_path=str(payload.get("source_path", "")),
            language=str(payload.get("language", "unknown")),
            target_triple=str(payload.get("target_triple", "unknown")),
            command_fingerprint=str(payload.get("command_fingerprint", "")),
            command_normalized=list(payload.get("command_normalized", [])),
            source_fingerprint=str(payload.get("source_fingerprint", "")),
            created_at=str(payload["created_at"]),
            metadata=dict(payload.get("metadata", {})),
        )


@dataclass(frozen=True)
class RepresentationRecord:
    schema_version: str
    representation_id: str
    snapshot_id: str
    unit_id: str
    capability_id: str
    capability_version: str
    content_digest: str
    content_type: str
    byte_size: int
    schema_id: str
    created_at: str

    def to_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "representation_id": self.representation_id,
            "snapshot_id": self.snapshot_id,
            "unit_id": self.unit_id,
            "capability_id": self.capability_id,
            "capability_version": self.capability_version,
            "content_digest": self.content_digest,
            "content_type": self.content_type,
            "byte_size": self.byte_size,
            "schema_id": self.schema_id,
            "created_at": self.created_at,
        }

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "RepresentationRecord":
        return cls(
            schema_version=str(payload["schema_version"]),
            representation_id=str(payload["representation_id"]),
            snapshot_id=str(payload["snapshot_id"]),
            unit_id=str(payload["unit_id"]),
            capability_id=str(payload["capability_id"]),
            capability_version=str(payload["capability_version"]),
            content_digest=str(payload["content_digest"]),
            content_type=str(payload["content_type"]),
            byte_size=int(payload["byte_size"]),
            schema_id=str(payload["schema_id"]),
            created_at=str(payload["created_at"]),
        )


@dataclass(frozen=True)
class CapabilityRunRecord:
    schema_version: str
    cap_run_id: str
    snapshot_id: str
    unit_id: str
    capability_id: str
    capability_version: str
    state: str
    cache_key: str
    started_at: str
    ended_at: str
    representation_refs: List[str]
    error: Optional[Dict[str, Any]]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "cap_run_id": self.cap_run_id,
            "snapshot_id": self.snapshot_id,
            "unit_id": self.unit_id,
            "capability_id": self.capability_id,
            "capability_version": self.capability_version,
            "state": self.state,
            "cache_key": self.cache_key,
            "started_at": self.started_at,
            "ended_at": self.ended_at,
            "representation_refs": self.representation_refs,
            "error": self.error,
        }

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "CapabilityRunRecord":
        return cls(
            schema_version=str(payload["schema_version"]),
            cap_run_id=str(payload["cap_run_id"]),
            snapshot_id=str(payload["snapshot_id"]),
            unit_id=str(payload["unit_id"]),
            capability_id=str(payload["capability_id"]),
            capability_version=str(payload["capability_version"]),
            state=str(payload["state"]),
            cache_key=str(payload["cache_key"]),
            started_at=str(payload["started_at"]),
            ended_at=str(payload["ended_at"]),
            representation_refs=list(payload.get("representation_refs", [])),
            error=payload.get("error"),
        )


@dataclass(frozen=True)
class RepresentationDescriptorRecord:
    schema_version: str
    descriptor_id: str
    snapshot_id: str
    unit_id: str
    unit_name: str
    source_id: str
    source_ref: str
    source_path: str
    representation_kind: str
    variant: str
    materialization_policy: str
    content_type: str
    relative_path: str
    mapping_relative_path: str
    mapping_format: str
    producer: str
    category: str
    byte_size: int
    created_at: str

    def to_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "descriptor_id": self.descriptor_id,
            "snapshot_id": self.snapshot_id,
            "unit_id": self.unit_id,
            "unit_name": self.unit_name,
            "source_id": self.source_id,
            "source_ref": self.source_ref,
            "source_path": self.source_path,
            "representation_kind": self.representation_kind,
            "variant": self.variant,
            "materialization_policy": self.materialization_policy,
            "content_type": self.content_type,
            "relative_path": self.relative_path,
            "mapping_relative_path": self.mapping_relative_path,
            "mapping_format": self.mapping_format,
            "producer": self.producer,
            "category": self.category,
            "byte_size": self.byte_size,
            "created_at": self.created_at,
        }

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "RepresentationDescriptorRecord":
        return cls(
            schema_version=str(payload["schema_version"]),
            descriptor_id=str(payload["descriptor_id"]),
            snapshot_id=str(payload["snapshot_id"]),
            unit_id=str(payload["unit_id"]),
            unit_name=str(payload.get("unit_name", "")),
            source_id=str(payload.get("source_id", "")),
            source_ref=str(payload.get("source_ref", "")),
            source_path=str(payload.get("source_path", "")),
            representation_kind=str(payload.get("representation_kind", "")),
            variant=str(payload.get("variant", "")),
            materialization_policy=str(payload.get("materialization_policy", "")),
            content_type=str(payload.get("content_type", "")),
            relative_path=str(payload.get("relative_path", "")),
            mapping_relative_path=str(payload.get("mapping_relative_path", "")),
            mapping_format=str(payload.get("mapping_format", "")),
            producer=str(payload.get("producer", "")),
            category=str(payload.get("category", "")),
            byte_size=int(payload.get("byte_size", 0)),
            created_at=str(payload["created_at"]),
        )


@dataclass(frozen=True)
class JobRecord:
    schema_version: str
    job_id: str
    job_type: str
    state: str
    idempotency_key: str
    request: Dict[str, Any]
    result_ref: Optional[str]
    progress: Dict[str, Any]
    error: Optional[Dict[str, Any]]
    created_at: str
    started_at: Optional[str]
    ended_at: Optional[str]
    heartbeat_at: Optional[str]
    retries: int
    result: Optional[Dict[str, Any]] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "job_id": self.job_id,
            "job_type": self.job_type,
            "state": self.state,
            "idempotency_key": self.idempotency_key,
            "request": self.request,
            "result_ref": self.result_ref,
            "progress": self.progress,
            "error": self.error,
            "created_at": self.created_at,
            "started_at": self.started_at,
            "ended_at": self.ended_at,
            "heartbeat_at": self.heartbeat_at,
            "retries": self.retries,
            "result": self.result,
        }

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "JobRecord":
        return cls(
            schema_version=str(payload["schema_version"]),
            job_id=str(payload["job_id"]),
            job_type=str(payload["job_type"]),
            state=str(payload["state"]),
            idempotency_key=str(payload["idempotency_key"]),
            request=dict(payload.get("request", {})),
            result_ref=payload.get("result_ref"),
            progress=dict(payload.get("progress", {})),
            error=payload.get("error"),
            created_at=str(payload["created_at"]),
            started_at=payload.get("started_at"),
            ended_at=payload.get("ended_at"),
            heartbeat_at=payload.get("heartbeat_at"),
            retries=int(payload.get("retries", 0)),
            result=payload.get("result"),
        )

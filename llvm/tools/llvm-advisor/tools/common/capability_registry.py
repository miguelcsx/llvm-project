from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional


@dataclass(frozen=True)
class CapabilityDescriptor:
    capability_id: str
    capability_version: str
    execution_mode: str
    cost_class: str
    required_inputs: List[str]
    depends_on: List[str]
    produces: List[Dict[str, Any]]
    supports_scope: List[str]
    feature_flags: List[str]

    @classmethod
    def from_json(cls, payload: Dict[str, Any]) -> "CapabilityDescriptor":
        return cls(
            capability_id=str(payload["capability_id"]),
            capability_version=str(payload["capability_version"]),
            execution_mode=str(payload["execution_mode"]),
            cost_class=str(payload["cost_class"]),
            required_inputs=list(payload.get("required_inputs", [])),
            depends_on=list(payload.get("depends_on", [])),
            produces=list(payload.get("produces", [])),
            supports_scope=list(payload.get("supports_scope", [])),
            feature_flags=list(payload.get("feature_flags", [])),
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "capability_id": self.capability_id,
            "capability_version": self.capability_version,
            "execution_mode": self.execution_mode,
            "cost_class": self.cost_class,
            "required_inputs": self.required_inputs,
            "depends_on": self.depends_on,
            "produces": self.produces,
            "supports_scope": self.supports_scope,
            "feature_flags": self.feature_flags,
        }


@dataclass(frozen=True)
class CapabilityProfile:
    profile_id: str
    label: str
    description: str
    default_capabilities: List[str]
    l2_capabilities: List[str]

    @classmethod
    def from_json(cls, payload: Dict[str, Any]) -> "CapabilityProfile":
        return cls(
            profile_id=str(payload["profile_id"]),
            label=str(payload.get("label", payload["profile_id"])),
            description=str(payload.get("description", "")),
            default_capabilities=list(payload.get("default_capabilities", [])),
            l2_capabilities=list(payload.get("l2_capabilities", [])),
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "profile_id": self.profile_id,
            "label": self.label,
            "description": self.description,
            "default_capabilities": self.default_capabilities,
            "l2_capabilities": self.l2_capabilities,
        }


class CapabilityRegistry:
    def __init__(self, config_root: Path) -> None:
        self._config_root = config_root
        self._descriptor_dir = config_root / "capabilities"
        self._profile_dir = config_root / "profiles"
        self._descriptors = self._load_descriptors()
        self._profiles = self._load_profiles()

    def list_capabilities(self) -> List[CapabilityDescriptor]:
        return list(self._descriptors.values())

    def list_profiles(self) -> List[CapabilityProfile]:
        return list(self._profiles.values())

    def get_capability(self, capability_id: str) -> Optional[CapabilityDescriptor]:
        return self._descriptors.get(capability_id)

    def get_profile(self, profile_id: str) -> Optional[CapabilityProfile]:
        return self._profiles.get(profile_id)

    def has_capability(self, capability_id: str) -> bool:
        return capability_id in self._descriptors

    def resolve_capabilities(
        self, profile_id: str, *, include_l2: bool = False
    ) -> List[str]:
        profile = self.get_profile(profile_id)
        if profile is None:
            return []
        capabilities = list(profile.default_capabilities)
        if include_l2:
            capabilities.extend(profile.l2_capabilities)
        # Preserve declaration order while removing duplicates.
        return list(dict.fromkeys(capabilities))

    def _load_descriptors(self) -> Dict[str, CapabilityDescriptor]:
        descriptors: Dict[str, CapabilityDescriptor] = {}
        if not self._descriptor_dir.exists():
            return descriptors

        for descriptor_path in sorted(self._descriptor_dir.glob("*.json")):
            with descriptor_path.open("r", encoding="utf-8") as descriptor_file:
                payload = json.load(descriptor_file)
            descriptor = CapabilityDescriptor.from_json(payload)
            descriptors[descriptor.capability_id] = descriptor

        return descriptors

    def _load_profiles(self) -> Dict[str, CapabilityProfile]:
        profiles: Dict[str, CapabilityProfile] = {}
        if not self._profile_dir.exists():
            return profiles

        for profile_path in sorted(self._profile_dir.glob("*.json")):
            with profile_path.open("r", encoding="utf-8") as profile_file:
                payload = json.load(profile_file)
            profile = CapabilityProfile.from_json(payload)
            profiles[profile.profile_id] = profile

        return profiles

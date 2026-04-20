from __future__ import annotations

import ctypes
import json
import os
import shutil
from pathlib import Path
from typing import Any, Dict


class NativeCapabilityRunner:
    def __init__(self) -> None:
        library_path = self._resolve_library_path()
        self._library = ctypes.CDLL(str(library_path))
        self._library.llvm_advisor_run_capability.argtypes = [ctypes.c_char_p]
        self._library.llvm_advisor_run_capability.restype = ctypes.c_void_p
        self._library.llvm_advisor_materialize_representation.argtypes = [
            ctypes.c_char_p
        ]
        self._library.llvm_advisor_materialize_representation.restype = ctypes.c_void_p
        self._library.llvm_advisor_correlate_signals.argtypes = [ctypes.c_char_p]
        self._library.llvm_advisor_correlate_signals.restype = ctypes.c_void_p
        self._library.llvm_advisor_compare_capabilities.argtypes = [ctypes.c_char_p]
        self._library.llvm_advisor_compare_capabilities.restype = ctypes.c_void_p
        self._library.llvm_advisor_list_units.argtypes = [ctypes.c_char_p]
        self._library.llvm_advisor_list_units.restype = ctypes.c_void_p
        self._library.llvm_advisor_free_string.argtypes = [ctypes.c_void_p]
        self._library.llvm_advisor_free_string.restype = None

    def execute(
        self,
        *,
        data_dir: str,
        capability_id: str,
        unit_name: str,
    ) -> Dict[str, Any]:
        return self._invoke_json_api(
            self._library.llvm_advisor_run_capability,
            {
                "data_dir": data_dir,
                "capability_id": capability_id,
                "unit_name": unit_name,
            },
            empty_error="Native capability runner returned no response",
        )

    def materialize_representation(
        self,
        *,
        data_dir: str,
        unit_name: str,
        source_ref: str,
        kind: str,
    ) -> Dict[str, Any]:
        return self._invoke_json_api(
            self._library.llvm_advisor_materialize_representation,
            {
                "data_dir": data_dir,
                "unit_name": unit_name,
                "source_ref": source_ref,
                "kind": kind,
            },
            empty_error="Native materializer returned no response",
        )

    def compare(
        self,
        *,
        base_data_dir: str,
        candidate_data_dir: str,
        capability_id: str,
        unit_name: str = "",
    ) -> Dict[str, Any]:
        payload = {
            "base_data_dir": base_data_dir,
            "candidate_data_dir": candidate_data_dir,
            "capability_id": capability_id,
        }
        if unit_name:
            payload["unit_name"] = unit_name
        return self._invoke_json_api(
            self._library.llvm_advisor_compare_capabilities,
            payload,
            empty_error="Native compare returned no response",
        )

    def correlate_signals(
        self,
        *,
        data_dir: str,
        unit_name: str,
        source_ref: str,
        kind: str,
    ) -> Dict[str, Any]:
        return self._invoke_json_api(
            self._library.llvm_advisor_correlate_signals,
            {
                "data_dir": data_dir,
                "unit_name": unit_name,
                "source_ref": source_ref,
                "kind": kind,
            },
            empty_error="Native signal correlation returned no response",
        )

    def list_units(self, *, data_dir: str) -> Dict[str, Any]:
        result_pointer = self._library.llvm_advisor_list_units(
            str(data_dir).encode("utf-8")
        )
        return self._decode_response(
            result_pointer,
            empty_error="Native unit listing returned no response",
        )

    def _invoke_json_api(
        self,
        function: Any,
        payload: Dict[str, Any],
        *,
        empty_error: str,
    ) -> Dict[str, Any]:
        request_payload = json.dumps(payload, sort_keys=True).encode("utf-8")
        result_pointer = function(request_payload)
        return self._decode_response(result_pointer, empty_error=empty_error)

    def _decode_response(
        self,
        result_pointer: Any,
        *,
        empty_error: str,
    ) -> Dict[str, Any]:
        if not result_pointer:
            raise RuntimeError(empty_error)
        try:
            response_text = ctypes.string_at(result_pointer).decode("utf-8")
        finally:
            self._library.llvm_advisor_free_string(result_pointer)

        try:
            response_payload = json.loads(response_text)
        except json.JSONDecodeError as exc:
            raise RuntimeError(response_text or str(exc)) from exc
        if not isinstance(response_payload, dict):
            raise RuntimeError("Native capability runner returned invalid JSON")
        if response_payload.get("success") is not True:
            raise RuntimeError(
                str(response_payload.get("error") or "Native operation failed")
            )
        return response_payload

    def _resolve_library_path(self) -> Path:
        configured_path = os.environ.get("LLVM_ADVISOR_NATIVE_LIBRARY")
        if configured_path:
            candidate = Path(configured_path).expanduser().resolve()
            if candidate.exists():
                return candidate

        executable_path = os.environ.get("LLVM_ADVISOR_EXECUTABLE")
        if executable_path:
            sibling = Path(executable_path).resolve().parent / self._library_basename()
            if sibling.exists():
                return sibling

        executable = shutil.which("llvm-advisor")
        if executable:
            sibling = Path(executable).resolve().parent / self._library_basename()
            if sibling.exists():
                return sibling

        workspace_root = Path(__file__).resolve().parents[5]
        build_sibling = workspace_root / "build" / "bin" / self._library_basename()
        if build_sibling.exists():
            return build_sibling

        raise RuntimeError("Native capability runtime library was not found")

    def _library_basename(self) -> str:
        if os.name == "nt":
            return "LLVMAdvisorNative.dll"
        if os.uname().sysname == "Darwin":
            return "libLLVMAdvisorNative.dylib"
        return "libLLVMAdvisorNative.so"

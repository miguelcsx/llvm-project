# Spec: Representation Graph

Version: 2.0  
Status: Draft

## 1. Purpose

Define the canonical native capture format for LLVM Advisor. The capture layer
must emit declarative representations and relationships, not hardcoded artifact
directories inferred later by Python.

## 2. Design Rules

1. A compilation unit owns source nodes and representation nodes.
2. Every representation has a stable kind and variant.
3. Source linkage is explicit via `source_ref`; it is never reconstructed from
   filenames in the backend.
4. Producer provenance is embedded in the manifest.
5. Large payloads remain blob-backed; the manifest carries metadata only.

## 3. Unit Manifest

```json
{
  "schema_version": "2.0",
  "unit_id": "unit_...",
  "unit_name": "clang/lib/Sema/SemaExpr.cpp",
  "primary_source_ref": "clang/lib/Sema/SemaExpr.cpp",
  "language": "c++",
  "target_triple": "x86_64-unknown-linux-gnu",
  "command_fingerprint": "sha256:...",
  "source_fingerprint": "sha256:...",
  "sources": [
    {
      "source_id": "unit::src::clang/lib/Sema/SemaExpr.cpp",
      "source_ref": "clang/lib/Sema/SemaExpr.cpp",
      "source_path": "/abs/path/clang/lib/Sema/SemaExpr.cpp",
      "language": "C++",
      "is_header": false
    }
  ],
  "representations": [
    {
      "representation_id": "unit::ir::SemaExpr_hash.ll",
      "kind": "llvm-ir",
      "variant": "frontend",
      "materialization_policy": "eager",
      "storage": "blob",
      "content_type": "text/plain",
      "relative_path": "ir/SemaExpr_hash.ll",
      "source_ref": "clang/lib/Sema/SemaExpr.cpp",
      "provenance": {
        "producer": "llvm-advisor-native",
        "capture_mode": "library-first",
        "category": "ir"
      }
    }
  ],
  "command": {}
}
```

## 4. Required Representation Fields

1. `representation_id`
2. `kind`
3. `variant`
4. `materialization_policy`
5. `storage`
6. `content_type`
7. `relative_path`
8. `provenance`

## 5. Representation Kinds

Current native kinds:

1. `llvm-ir`
2. `assembly`
3. `objdump`
4. `clang-ast`
5. `preprocessed-source`
6. `macro-state`
7. `optimization-remarks`
8. `diagnostics`
9. `time-trace`
10. `runtime-trace`
11. `debug-info`
12. `source-copy`
13. `derived-artifact`

This list is open-ended. Backends and UIs must key behavior on `kind` and
`variant`, not directory names.

## 6. Backend Contract

The Python backend must:

1. Read `representations[]` as the source of truth.
2. Avoid deriving ownership from filenames where `source_ref` exists.
3. Expose explorer/query APIs in terms of representations, not fixed artifact
   slots.
4. Treat `provenance.category` as a compatibility bridge only.

## 7. Migration

1. New manifests are `schema_version = 2.0`.
2. Backends may support `1.x` only as a temporary import path during migration.
3. New query/storage work should target the representation graph directly.

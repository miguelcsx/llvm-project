# Spec: Capability Registry and Planner

Version: 1.0  
Status: Normative

## 1. Purpose

Define capability descriptor format and planning algorithm for dynamic extraction.

## 2. Capability Descriptor

Location: `config/capabilities/*.json`

Example:

```json
{
  "capability_id": "llvm.ir.summary",
  "capability_version": "1.0.0",
  "execution_mode": "library",
  "cost_class": "moderate",
  "required_inputs": ["unit.command", "unit.source"],
  "depends_on": ["clang.ast.summary"],
  "produces": [
    {
      "schema_id": "capability/llvm.ir.summary/output@1",
      "content_type": "application/json"
    }
  ],
  "supports_scope": ["unit", "target", "snapshot"],
  "feature_flags": []
}
```

## 3. Descriptor Rules

1. `capability_id` immutable once published.
2. `capability_version` semantic versioning.
3. `depends_on` must be acyclic.
4. `execution_mode` values:
   - `library`
   - `hybrid`
   - `external_fallback`
5. `external_fallback` requires allowlisted command template in policy config.

## 4. Planner Inputs

1. `snapshot_id`
2. `scope`:
   - full snapshot
   - target list
   - unit list
   - source path glob
3. requested capabilities
4. execution policy:
   - max cost class
   - include L2 augmentation or not

## 5. Planner Outputs

`PlanGraph`:

1. nodes (`unit_id`, `capability_id`, `capability_version`)
2. directed dependencies
3. node state:
   - `cached`
   - `ready`
   - `blocked_missing_l2`
   - `skipped_by_policy`
4. deterministic `plan_hash`

## 6. Cache Key Contract

`cache_key = sha256(unit_identity_hash + capability_id + capability_version + dependency_artifact_digests + planner_policy_hash)`

If `cache_key` hit exists, node is `cached`.

## 7. L0/L1/L2 Classification

1. `L0`: derived from existing artifacts and summaries.
2. `L1`: derived from stored AST/IR/object artifacts without recompiling source.
3. `L2`: needs additional compile-time instrumentation.

Planner must annotate missing requests with `missing_l2` and suggest scoped augmentation.

## 8. Planning Algorithm

1. Expand requested capabilities into dependency closure.
2. Validate DAG acyclicity.
3. Instantiate nodes per scope.
4. Resolve cache keys and classify state.
5. Topologically sort runnable nodes.
6. Emit `plan_hash`.

## 9. Policy Checks

Before job creation:

1. verify capability is enabled by profile.
2. reject capabilities above allowed cost class unless explicitly requested.
3. reject non-allowlisted fallback capabilities.

## 10. Error Model

Planner errors:

1. unknown capability
2. version mismatch
3. cyclic dependency
4. policy denied
5. missing required base input

Each error must include:

1. `error_code`
2. `capability_id`
3. `hint`

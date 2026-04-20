# Spec: Compare Engine

Version: 1.0  
Status: Normative

## 1. Purpose

Define deterministic comparison between snapshots and regression classification.

## 2. Inputs

1. `base_snapshot_id`
2. `candidate_snapshot_id`
3. scope filters
4. metric set
5. compare options

## 3. Matching Strategy

Order:

1. exact match by `UnitID`
2. fallback by normalized source path + language + target triple
3. fuzzy fallback by basename + command similarity score (optional, low confidence)

Output category per unit:

1. `matched`
2. `changed`
3. `added`
4. `removed`
5. `unmatched_low_confidence`

## 4. Delta Calculation

For each metric:

1. `absolute_delta = candidate - base`
2. `relative_delta = (candidate - base) / max(abs(base), epsilon)`

Metric direction:

1. `lower_is_better`
2. `higher_is_better`
3. `target_band`

## 5. Severity and Confidence

Severity:

1. `info`
2. `low`
3. `medium`
4. `high`
5. `critical`

Confidence:

1. `high` for exact match and stable capability versions.
2. `medium` for fallback match or partial data.
3. `low` for fuzzy mapping.

## 6. Explainability Fields

Every finding must include:

1. base/candidate values and units
2. direction and thresholds used
3. match strategy
4. capability provenance
5. optional source location/function context

## 7. Compare Result Schema

Top level:

1. summary counts by severity and metric.
2. changed-units leaderboard.
3. top regressions and top improvements.

Detailed item:

```json
{
  "finding_id": "find_01J...",
  "metric_key": "diag.warning.count",
  "scope": {"unit_id": "unit_..."},
  "base_value": 10,
  "candidate_value": 15,
  "absolute_delta": 5,
  "relative_delta": 0.5,
  "direction": "lower_is_better",
  "severity": "medium",
  "confidence": "high",
  "provenance": {
    "capability_id": "clang.diag.summary",
    "capability_version": "1.0.0"
  }
}
```

## 8. Performance Rules

1. Stream partial results for large compares.
2. Prioritize changed-unit candidates first.
3. Bound per-request memory and materialize chunks.

## 9. Failure Handling

1. Missing capabilities in base or candidate are reported as partial coverage.
2. Compare does not fail globally if a subset is missing.
3. Severity output must mark partial data to avoid false certainty.

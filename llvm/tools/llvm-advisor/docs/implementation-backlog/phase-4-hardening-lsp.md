# Phase 4 Backlog: Production Hardening and Optional LSP Integration

Status: Blocked on Phase 3 completion  
Goal: production operations, reliability hardening, and optional integration with the existing LSP stack.

## Scope

1. Reliability and operations hardening (SLOs, alerting, recovery).
2. Security and policy controls.
3. Release and migration automation.
4. Optional LSP integration that reuses current LSP deployment.

## Exit Criteria

1. Service SLOs and error budgets are tracked and enforced.
2. Disaster recovery and rollback runbooks are validated.
3. Security controls for file access and external fallback are in place.
4. Optional LSP integration works through existing LSP client/server setup (no new LSP server).

## Backlog

| ID | Title | Description | Deliverables | Dependencies | Acceptance Criteria |
|---|---|---|---|---|---|
| P4-001 | SLO Dashboard | Implement metrics, dashboards, and alert thresholds. | observability dashboards + alert rules. | P1..P3 | SLO indicators visible and alert routing tested. |
| P4-002 | Recovery and Backup | Add snapshot backup/export and recovery verification jobs. | backup tool + restore tests + runbook. | P1-004, P3-* | Recovery validated from backup artifact in staging. |
| P4-003 | Policy and Security Controls | Enforce path sandboxing, fallback allowlist, resource quotas. | policy engine + enforcement tests. | P2-009 | Policy violations blocked and audited. |
| P4-004 | Release Gates | Add release gates for schema compatibility and perf regressions. | CI/CD gates + signed release checklist. | P3-010 | Release blocked if gate checks fail. |
| P4-005 | Data Retention and Compaction | Define retention tiers and background compaction jobs. | retention config + compaction metrics. | P1-004 | Disk growth bounded by policy in soak tests. |
| P4-006 | Existing LSP Integration Adapter | Build optional adapter consumed by current LSP client/plugin stack. | adapter API + client integration docs. | P3-008 | Inline hints/links work with existing LSP setup; no new server process. |
| P4-007 | LSP UX Features | Add opt-in diagnostics overlays, code lenses, and quick links to advisor compare pages. | editor integration package updates. | P4-006 | Features can be enabled/disabled per workspace. |
| P4-008 | Tenant and Access Controls | Add project/snapshot ACL and audit trails for shared deployments. | ACL middleware + tests. | P4-003 | Unauthorized access tests pass. |
| P4-009 | Production Soak | 2-week soak test with large projects and compare-heavy workload. | soak report + incident log + fixes. | P4-001..P4-008 | Meets reliability and performance gates. |

## LSP Constraint (explicit)

1. Do not create a new language server.
2. Integrate with existing LSP setup by adding an advisor-aware client adapter.
3. LSP integration is optional and can be deferred without blocking core platform rollout.

## Delivery Sequence

### Sprint A

1. P4-001, P4-002, P4-003.

### Sprint B

1. P4-004, P4-005, P4-008.

### Sprint C

1. P4-006, P4-007.

### Sprint D

1. P4-009 and launch decision.

## Code Placement Guidance

1. Ops and security:
   - `tools/common/ops/*`
   - `tools/webserver/middleware/*`
2. LSP adapter:
   - `tools/integration/lsp-adapter/*`
3. CI gates:
   - `.github/workflows/*` or project CI equivalents

## Risks and Mitigations

1. Risk: feature creep in LSP integration.
   - Mitigation: strict scope to adapter + links + overlays.
2. Risk: production incidents from unbounded jobs.
   - Mitigation: quotas, timeouts, and cancellation by policy.
3. Risk: operational overhead.
   - Mitigation: runbooks, automation, and predictable retention.

## Definition of Done

1. Platform operates under declared SLOs.
2. Security and policy controls enforced in production.
3. Optional LSP integration available through existing client/server stack.

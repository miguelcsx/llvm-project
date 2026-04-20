# Spec: Optional LSP Integration (Existing LSP Stack)

Version: 1.0  
Status: Informative + Implementation Guidance

## 1. Constraint

1. Do not build a new LSP server.
2. Integrate advisor outputs into the current LSP workflow used by the team.
3. LSP integration is optional and must not block core platform delivery.

## 2. Integration Model

Use an adapter layer that reads LLVM Advisor API and feeds existing client UX primitives.

Components:

1. Advisor backend APIs (`/query`, `/compare`)
2. LSP adapter client (editor plugin extension or existing LSP client hook)
3. Current LSP server (unchanged)

## 3. Supported UX Features

1. Inline diagnostics overlays from advisor findings.
2. Code lens links:
   - "View advisor diff"
   - "View optimization remarks"
3. Hover snippets with short summaries and deep-link URL to web UI.
4. Optional command palette actions:
   - "Request deeper analysis for current file"
   - "Compare current file across snapshots"

## 4. Data Flow

1. User opens file in editor.
2. Existing LSP provides normal language features.
3. Adapter maps file to `unit_id` and calls advisor query API.
4. Adapter renders additional diagnostics/lenses via existing editor extension APIs.
5. For deep operations, adapter triggers async job and polls job endpoint.

## 5. Configuration

Workspace settings:

1. `advisor.enabled` (default false)
2. `advisor.baseUrl`
3. `advisor.snapshotId`
4. `advisor.severityThreshold`
5. `advisor.autoRefreshSeconds`

## 6. Performance and Safety

1. Adapter must debounce requests on document changes.
2. Adapter must cache per-file responses with short TTL.
3. Adapter must not block editor typing path.
4. Adapter must fail open (if advisor unavailable, base LSP remains unaffected).

## 7. Security

1. API auth handled through existing editor secret storage.
2. Do not send unsaved buffer contents unless explicitly enabled.
3. Respect project ACLs from advisor backend.

## 8. Implementation Tasks

1. File-to-unit mapping service in advisor API.
2. Adapter module in current editor integration repo.
3. Diagnostics overlay and code lens rendering.
4. Deep-link navigation to advisor web compare views.

## 9. Success Criteria

1. Engineers can see advisor insights in editor without changing LSP server.
2. No measurable degradation in baseline editor responsiveness.
3. Users can jump from inline insight to full web investigation.

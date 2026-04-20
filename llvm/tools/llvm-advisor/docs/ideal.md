# LLVM Advisor Ideal End State

Status: North Star  
Audience: LLVM Advisor maintainers, compiler engineers, tooling engineers, infra owners

This document describes the ideal product we want LLVM Advisor to become, even if
parts of it are not implemented yet.

The goal is to make the intended direction explicit so design, implementation,
and review decisions can be evaluated against one clear target.

## 1. What LLVM Advisor Should Be

LLVM Advisor should be the single place where a developer can understand what
LLVM-based compilation is doing for a program, a project, a file, a function, a
kernel, or a source range.

It should not be just:

1. a viewer for compiler outputs
2. a wrapper around a few reports
3. a dashboard with disconnected cards
4. a web UI that mirrors existing plain-text tool output

It should be:

1. a capture system for LLVM and Clang internals
2. a correlation system that connects data across representations and tools
3. a query system that answers focused questions
4. a compare and history system across snapshots and revisions
5. a CLI-first product with a web UI as an additional client
6. an extensible integration point for more LLVM tools and internal data over time

The product value is not "store many files". The value is "make all relevant
compiler information understandable, navigable, and comparable from one place".

## 2. Product Goals

LLVM Advisor should allow users to:

1. create a project and capture its compilation state
2. understand a whole program and also drill down to individual compile units
3. inspect real source code and real generated representations together
4. correlate diagnostics, remarks, pass signals, mappings, debug info, offload
   data, and future LLVM tool outputs
5. request additional representations on demand for exploration
6. compare snapshots, units, files, functions, and ranges over time
7. understand changes without forcing the tool to make subjective judgments
8. use the same data through a CLI, a web UI, and future machine integrations

LLVM Advisor should represent the data well. It should not tell the user a
claim as truth when that claim is an interpretation.

Good example:

1. "instruction_count changed from 44 to 76"
2. "remark count increased in function `foo`"
3. "these remarks map to this source range and this IR region"

Bad example:

1. "this is downgrading performance"
2. "this optimization is wrong"
3. "this code is better"

Advisor should expose evidence and context. The user decides what it means.

## 3. Core Principles

1. CLI-first, not UI-first
2. library-first, not driver-first
3. capture once, query many times
4. immutable snapshots, explicit lineage
5. declarative representations, not hardcoded view types
6. correlation first, not disconnected files
7. scalable by default for large projects
8. neutral presentation of data
9. strong provenance for every result
10. modular architecture with clear ownership boundaries

## 4. Mental Model

The user workflow should look like this:

1. define a project or import one
2. capture a snapshot of the program/build
3. query what exists
4. materialize more detail only where needed
5. explore a file, function, range, target, or kernel
6. correlate findings across source, IR, assembly, diagnostics, remarks, and more
7. make code or build changes
8. capture another snapshot
9. compare current vs previous snapshots
10. repeat over time

The tool should support this equally well for:

1. a single source file
2. a medium-size application
3. a very large codebase
4. host plus device/offload compilations
5. future integrations such as MLIR, MIR, LTO, ThinLTO, and runtime traces

## 5. The Ideal CLI

The C++ binary is the main product entrypoint.

Python services, web tooling, and indexing helpers are important, but they are
supporting layers. The canonical user workflow should be expressible through the
native CLI.

### 5.1 CLI Goals

The CLI should:

1. feel like one coherent tool, not a bag of unrelated subcommands
2. return structured output by default when useful
3. make ad hoc exploration easy
4. support automation and CI
5. expose enough context that users do not need to parse raw LLVM text themselves

### 5.2 Ideal CLI Command Model

Ideal top-level command families:

1. `llvm-advisor project`
2. `llvm-advisor capture`
3. `llvm-advisor query`
4. `llvm-advisor materialize`
5. `llvm-advisor signals`
6. `llvm-advisor compare`
7. `llvm-advisor explain`
8. `llvm-advisor inspect`
9. `llvm-advisor serve`

### 5.3 What Each Command Family Should Mean

`project`

1. create or register a project
2. define source root, build root, profiles, target scopes
3. track project-level metadata and defaults

`capture`

1. ingest build metadata
2. capture baseline representations and low-cost signals
3. persist immutable snapshot state
4. support selective scope for large projects

`query`

1. answer focused questions over indexed data
2. support filters by snapshot, unit, file, function, range, target, pass, and tool
3. never require users to know storage layout

`materialize`

1. request a representation or derived view on demand
2. select variants and options instead of using hardcoded slots
3. reuse cached outputs if already available

`signals`

1. correlate diagnostics, remarks, and other findings across representations
2. focus on evidence and linkage, not interpretation
3. support both source-centered and representation-centered views

`compare`

1. compare snapshots, units, ranges, targets, or functions
2. emit neutral changes with provenance
3. support history and trend views later

`explain`

1. summarize connected evidence for a selected scope
2. help users understand what data exists and what it means structurally
3. remain evidence-based and never claim certainty it does not have

`inspect`

1. expose internals of stored entities, manifests, mappings, and provenance
2. help debugging the tool itself

`serve`

1. run the API and web UI over the same indexed data model
2. keep the web UI as a client of the same core system

### 5.4 CLI Output Rules

CLI output should follow these rules:

1. human mode should be compact and readable
2. structured mode should be stable and machine-readable
3. every result should identify snapshot, scope, and provenance
4. output should prefer summaries plus links to more detail
5. raw LLVM output should still be accessible, but Advisor should add structure,
   correlation, and context on top

## 6. The Ideal Data Model

The data model should describe semantic entities and relationships, not just
files on disk.

Required first-class entities:

1. project
2. program
3. snapshot
4. source revision
5. build configuration
6. compilation unit
7. source file
8. representation
9. mapping
10. finding
11. event
12. tool run
13. comparison
14. host/device compilation graph

### 6.1 Snapshot

A snapshot is an immutable capture of a project state and its associated
analysis/materialization lineage.

A snapshot should include:

1. source revision identity
2. build configuration identity
3. capture profile
4. provenance and producer versions
5. links to units, representations, mappings, and findings

### 6.2 Compilation Unit

A unit should have a stable identity derived from normalized command context and
source identity, not just a filename.

A unit should expose:

1. source membership
2. language
3. target triple
4. host/device role
5. command fingerprint
6. source fingerprint
7. representation inventory

### 6.3 Representation

Representations should be declarative and extensible.

They should not be modeled as a fixed list of hardcoded outputs in the explorer.

Each representation should carry:

1. `kind`
2. `variant`
3. `materialization_policy`
4. `cost_class`
5. `content_type`
6. `syntax`
7. `producer`
8. `scope`
9. `mapping_availability`
10. `cacheability`
11. provenance metadata

Examples of representation kinds:

1. `source`
2. `preprocessed`
3. `llvm-ir`
4. `optimized-llvm-ir`
5. `assembly`
6. `machine-ir`
7. `cfg`
8. `objdump`
9. `ast-json`
10. `debug-line-table`
11. `remarks`
12. `diagnostics`
13. `time-trace`
14. `offload-runtime-trace`
15. `device-llvm-ir`
16. `mlir`

### 6.4 Mapping

Mappings are critical and must be first-class data.

Mappings should support:

1. source line to representation line
2. source range to representation range
3. address to source
4. instruction to source
5. source to function and function to source
6. inline/inlined-at relationships
7. discriminator-aware locations

The backend should not primarily reconstruct mappings heuristically from text.
The native layer should emit exact mappings whenever LLVM internals allow it.

### 6.5 Finding

A finding is a neutral record of something observed.

Examples:

1. diagnostic
2. optimization remark
3. analysis result
4. pass event
5. offload event
6. debug discrepancy
7. runtime observation

A finding should include:

1. kind
2. location or scope
3. provenance
4. related entities
5. payload data
6. confidence or origin if approximate

## 7. Representation Strategy

The explorer should not be hardwired to a small set of fixed outputs.

Instead, the explorer and CLI should request representations through a registry.

### 7.1 Materialization Policies

Representations should support:

1. `eager`
2. `lazy`
3. `derived`
4. `ephemeral`
5. `cacheable`

### 7.2 Capture Tiers

Not all data should be collected the same way.

Always-on capture should include low-cost, broadly useful data:

1. structured diagnostics
2. structured remarks
3. source identity
4. command provenance
5. target/offload topology
6. baseline mappings when cheap and available

On-demand capture/materialization should include more expensive or exploration-
specific data:

1. full IR variants
2. assembly variants
3. MIR
4. CFG
5. special debug views
6. offload/device-specific views

This gives users the flexibility of Compiler Explorer-style exploration without
forcing the system to precompute everything for every unit.

## 8. Correlation Model

Correlation is the center of the product.

The system should connect:

1. source lines and ranges
2. diagnostics
3. remarks
4. IR
5. assembly
6. object/debug data
7. pass pipeline data
8. offload host/device relationships
9. time and runtime traces
10. snapshot-to-snapshot changes

The same underlying data should support multiple points of view:

1. source-centered
2. representation-centered
3. finding-centered
4. function-centered
5. target-centered
6. history-centered

The product should not treat data as isolated categories. It should treat them
as connected evidence.

## 9. Diagnostics, Remarks, and Insights

Diagnostics and remarks are not enough by themselves. What matters is how they
connect to code, representations, and historical changes.

### 9.1 Diagnostics

Advisor should preserve diagnostics in structured form, including later if
possible:

1. severity
2. file, line, column, range
3. notes
4. fix-its
5. include stacks
6. macro context

### 9.2 Remarks

Advisor should preserve remarks in structured form, including:

1. pass name
2. function
3. location
4. message
5. related arguments
6. mapped representation locations

### 9.3 Insights

The term "insight" should mean a useful view over correlated data, not a hardcoded
opinion.

Examples of acceptable insight views:

1. "all remarks touching this source range"
2. "all changes in IR summary metrics for this unit across three snapshots"
3. "all device-side findings related to this offload region"
4. "all diagnostics and remarks affecting this function"

Examples of bad insight behavior:

1. claiming a performance outcome without evidence
2. inventing a causality chain not captured by the data
3. collapsing multiple findings into one opaque summary sentence

## 10. Explorer UX Direction

The web explorer should feel closer to a compiler engineer's workstation than a
generic dashboard.

It should prioritize:

1. real source code
2. real generated representations
3. correlated findings
4. exact or explicitly approximate mappings
5. range- and line-based navigation
6. responsive interaction with large files

The explorer should not be the product center. It is one client of the system.
The same operations should be available in the CLI and API.

### 10.1 Explorer Requirements

1. source pane and representation pane should be integrated
2. correlated findings should be navigable by line and by function
3. representation selection should come from the representation registry
4. loading should prefer range queries over full-file transfers when possible
5. the UI should clearly show whether a link is exact or fallback-based

## 11. Scalability Requirements

LLVM Advisor must work for large projects, not only demos.

### 11.1 Data Plane Rules

1. do not load all artifacts for a snapshot into memory
2. do not force the UI to fetch full representations by default
3. do not recompute derived information on every request if it can be indexed
4. keep blob storage separate from metadata/index storage
5. make expensive materializations explicit and cacheable

### 11.2 Query Model

REST is acceptable as a control plane, but it should not be the only data access
pattern for large explorer workloads.

We should support:

1. range-based representation reads
2. filtered finding queries
3. mapping queries
4. cursor-based pagination
5. streaming for long-running jobs and live updates where needed

### 11.3 Compare and History

Compare should operate over indexed entities and metrics, not over ad hoc file
diffing of raw outputs.

History should support:

1. snapshot lineage
2. per-unit change history
3. trend views over metrics or findings
4. targeted compare for files, functions, and targets

## 12. Offloading and Multi-Target Compilation

Offloading is a first-class interest area and should be modeled explicitly.

Advisor should represent:

1. host compilation
2. device compilations
3. offload regions
4. kernel identities
5. host-device linkage
6. target-specific representations and findings

This should not be hidden as a minor attribute on a generic unit. It should be
a linked compilation graph.

## 13. Extensibility Model

LLVM Advisor should be easy to extend with more internal LLVM data and more tool
outputs without redesigning the core model each time.

### 13.1 Extension Rules

1. new data sources should register new representation or finding kinds
2. the storage model should not need a schema rewrite for every new view
3. correlations should be additive
4. provenance must always identify the producer and version
5. existing clients should be able to ignore new fields safely where appropriate

### 13.2 Reuse Existing LLVM Tools

We should not redo the wheel.

If an LLVM tool already provides authoritative structured information, Advisor
should reuse it or normalize it instead of rebuilding it from scratch.

Advisor's value is:

1. orchestration
2. normalization
3. indexing
4. correlation
5. queryability
6. compare and history

Not duplicating every LLVM tool UI.

## 14. Repository Structure We Want

The repository should reflect clear ownership boundaries.

### 14.1 `src/`

Native product layer.

Responsibilities:

1. CLI entrypoint
2. capture orchestration
3. native capability execution
4. native materialization
5. native mapping generation
6. shared library ABI for higher-level clients

### 14.2 `tools/common/`

Shared support runtime for indexed storage, query planning, and service-side
composition.

Responsibilities:

1. snapshot/index storage
2. representation catalog logic
3. orchestration helpers
4. planner support
5. web/runtime integration support

### 14.3 `tools/webserver/`

Web delivery layer only.

Responsibilities:

1. API handlers
2. job orchestration
3. frontend delivery
4. web-specific request composition

It should not become the primary owner of core product semantics.

### 14.4 `config/`

Declarative product configuration.

Responsibilities:

1. capability definitions
2. profiles
3. policy defaults
4. future registry metadata

### 14.5 `docs/`

Design, specs, roadmap, and operating model.

## 15. What "Library-First" Means Here

LLVM Advisor should prefer LLVM and Clang APIs whenever that is the correct
engineering path.

That means:

1. collect and derive data through LLVM internals where possible
2. avoid shelling out to compiler drivers for core logic
3. generate mappings natively from real compiler data structures
4. use external tools only when they are already the correct authoritative
   producer and the integration remains deterministic

The native layer should be the source of truth for capture, materialization, and
core correlation.

## 16. Security and Robustness Expectations

The production design should assume hostile inputs and large workloads.

Requirements:

1. all manifest-relative paths must be validated
2. storage writes must be atomic and recoverable
3. API responses must not expose unchecked filesystem traversal
4. large inputs must be processed incrementally where possible
5. long-running work must use jobs with progress and cancellation
6. provenance and schema versions must be explicit

## 17. Current Gaps Versus Ideal

The current implementation is moving toward this design but does not yet fully
meet it.

Known gap categories:

1. exact mappings are stronger for IR than for assembly/object
2. range-query explorer data is not complete yet
3. more LLVM internals and tool integrations remain to be added
4. project/program-level workflow is not complete end to end yet
5. history/trend views are not complete yet
6. some data models still reflect transition from artifact-era design

This document is intentionally ahead of the current implementation.

## 18. Decision Rule

When choosing between two designs, prefer the one that better supports:

1. CLI-first workflows
2. native ownership of core semantics
3. declarative representations
4. correlation across data sources
5. scale for large projects
6. neutral evidence-based presentation
7. future integration with more LLVM internals and tools

If a change makes LLVM Advisor more like a disconnected viewer and less like a
compiler intelligence system, it is moving in the wrong direction.

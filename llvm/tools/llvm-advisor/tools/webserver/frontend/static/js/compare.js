// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { Utils } from "./utils.js";
import { renderResultDetails, setStatus } from "./compare-presenter.js";

const DEFAULT_CAPABILITIES = Object.freeze([
    "clang.diag.summary",
    "clang.ast.summary",
    "llvm.remarks.summary",
    "llvm.ir.summary",
    "llvm.pass.stats",
    "llvm.obj.summary",
    "llvm.debug.summary",
]);

function safeArray(value) {
    return Array.isArray(value) ? value : [];
}

export class CompareWorkspace {
    constructor(apiClient, app = null) {
        this.apiClient = apiClient;
        this.app = app;

        this.snapshots = [];
        this.capabilityDescriptors = [];
        this.selectedCapabilities = new Set(DEFAULT_CAPABILITIES);
        this.compareResult = null;
        this.isInitialized = false;

        this.baseSelector = null;
        this.candidateSelector = null;
        this.runButton = null;
        this.statusLabel = null;
        this.capabilityList = null;
        this.emptyState = null;
        this.resultsRoot = null;
        this.findingsBody = null;
        this.findingsCaption = null;
        this.matchSummary = null;
        this.hotUnits = null;
        this.findingCount = null;
        this.matchedUnits = null;
        this.changedUnits = null;
        this.unchangedUnits = null;
        this.changeFilter = null;
        this.searchInput = null;
        this.contextCopy = null;
    }

    init() {
        this.baseSelector = document.getElementById("compare-base-selector");
        this.candidateSelector = document.getElementById(
            "compare-candidate-selector",
        );
        this.runButton = document.getElementById("compare-run-btn");
        this.statusLabel = document.getElementById("compare-status");
        this.capabilityList = document.getElementById("compare-capability-list");
        this.emptyState = document.getElementById("compare-empty-state");
        this.resultsRoot = document.getElementById("compare-results");
        this.findingsBody = document.getElementById("compare-findings-body");
        this.findingsCaption = document.getElementById(
            "compare-findings-caption",
        );
        this.matchSummary = document.getElementById("compare-match-summary");
        this.hotUnits = document.getElementById("compare-hot-units");
        this.findingCount = document.getElementById("compare-finding-count");
        this.matchedUnits = document.getElementById("compare-matched-units");
        this.changedUnits = document.getElementById("compare-changed-units");
        this.unchangedUnits = document.getElementById("compare-unchanged-units");
        this.changeFilter = document.getElementById("compare-change-filter");
        this.searchInput = document.getElementById("compare-search-input");
        this.contextCopy = document.getElementById("compare-context-copy");

        this.runButton?.addEventListener("click", () => {
            void this.runCompare();
        });
        this.changeFilter?.addEventListener("change", () =>
            this.renderResultDetails(),
        );
        this.searchInput?.addEventListener("input", Utils.debounce(() => {
            this.renderResultDetails();
        }, 120));

        this.isInitialized = true;
    }

    async onActivate() {
        if (!this.isInitialized) {
            return;
        }
        if (this.capabilityDescriptors.length === 0) {
            await this.loadCapabilityDescriptors();
        }
        this.renderCapabilitySelector();
        this.renderSnapshotSelectors();
    }

    async loadCapabilityDescriptors() {
        const response = await this.apiClient.getCapabilities();
        if (!response.success) {
            return;
        }
        this.capabilityDescriptors = safeArray(response.data?.capabilities);

        const balancedProfile = safeArray(response.data?.profiles).find(
            (profile) => profile.profile_id === "balanced",
        );
        const balancedCapabilities = safeArray(
            balancedProfile?.default_capabilities,
        );
        if (balancedCapabilities.length > 0) {
            this.selectedCapabilities = new Set(balancedCapabilities);
        }
    }

    setContext({ snapshots = [], currentSnapshotId = null, currentUnitName = null } = {}) {
        const contextChanged =
            this.currentSnapshotId !== currentSnapshotId ||
            this.currentUnitName !== currentUnitName ||
            this.snapshots.length !== safeArray(snapshots).length;
        this.snapshots = [...safeArray(snapshots)];
        this.currentSnapshotId = currentSnapshotId;
        this.currentUnitName = currentUnitName;
        if (contextChanged) {
            this.compareResult = null;
        }

        if (!this.isInitialized) {
            return;
        }

        if (this.contextCopy) {
            this.contextCopy.textContent = currentUnitName
                ? `Compare two snapshots for ${currentUnitName} first, then drill into metric changes by pass, file, function, and section.`
                : "Compare two snapshots and inspect metric changes from summary counters down to pass, file, function, and section deltas.";
        }

        this.renderSnapshotSelectors();
        this.renderResultDetails();
    }

    renderSnapshotSelectors() {
        if (!this.baseSelector || !this.candidateSelector) {
            return;
        }

        const snapshotsNewestFirst = [...this.snapshots].reverse();
        const defaultCandidate =
            snapshotsNewestFirst.find(
                (snapshot) => snapshot.snapshot_id === this.currentSnapshotId,
            ) || snapshotsNewestFirst[0] || null;
        const defaultBase =
            snapshotsNewestFirst.find(
                (snapshot) =>
                    snapshot.snapshot_id !== defaultCandidate?.snapshot_id,
            ) || defaultCandidate;

        this.populateSnapshotSelect(
            this.baseSelector,
            snapshotsNewestFirst,
            this.baseSelector.value || defaultBase?.snapshot_id || "",
        );
        this.populateSnapshotSelect(
            this.candidateSelector,
            snapshotsNewestFirst,
            this.candidateSelector.value || defaultCandidate?.snapshot_id || "",
        );

        const hasComparableSnapshots = snapshotsNewestFirst.length >= 2;
        if (this.runButton) {
            this.runButton.disabled = !hasComparableSnapshots;
        }

        if (!hasComparableSnapshots) {
            this.setStatus("At least two snapshots are required to compare.");
        } else if (!this.compareResult) {
            this.setStatus("Select two snapshots to compare.");
        }
    }

    populateSnapshotSelect(select, snapshots, selectedValue) {
        select.innerHTML = "";
        snapshots.forEach((snapshot) => {
            const option = document.createElement("option");
            option.value = snapshot.snapshot_id;
            option.selected = snapshot.snapshot_id === selectedValue;
            const summary = snapshot.summary || {};
            option.textContent = `${snapshot.snapshot_id.slice(0, 16)} • ${snapshot.profile || "balanced"} • ${summary.unit_count || 0} units`;
            option.title = snapshot.snapshot_id;
            select.appendChild(option);
        });
    }

    renderCapabilitySelector() {
        if (!this.capabilityList) {
            return;
        }
        const capabilities =
            this.capabilityDescriptors.length > 0
                ? this.capabilityDescriptors
                : DEFAULT_CAPABILITIES.map((capabilityId) => ({
                      capability_id: capabilityId,
                  }));

        this.capabilityList.innerHTML = "";
        capabilities.forEach((descriptor) => {
            const capabilityId = descriptor.capability_id;
            const button = document.createElement("button");
            button.type = "button";
            button.dataset.capabilityId = capabilityId;
            button.className =
                "border px-3 py-2 text-sm font-medium rounded-lg";
            button.addEventListener("click", () => {
                if (this.selectedCapabilities.has(capabilityId)) {
                    this.selectedCapabilities.delete(capabilityId);
                } else {
                    this.selectedCapabilities.add(capabilityId);
                }
                if (this.selectedCapabilities.size === 0) {
                    this.selectedCapabilities.add(capabilityId);
                }
                this.renderCapabilitySelector();
            });

            const selected = this.selectedCapabilities.has(capabilityId);
            button.classList.add(
                ...(selected
                    ? [
                          "border-llvm-blue",
                          "bg-blue-50",
                          "text-blue-800",
                      ]
                    : ["border-gray-200", "bg-white", "text-gray-600"]),
            );
            button.textContent = capabilityId;
            this.capabilityList.appendChild(button);
        });
    }

    async runCompare() {
        const baseSnapshotId = this.baseSelector?.value || "";
        const candidateSnapshotId = this.candidateSelector?.value || "";
        if (!baseSnapshotId || !candidateSnapshotId) {
            this.setStatus("Select both baseline and candidate snapshots.");
            return;
        }
        if (baseSnapshotId === candidateSnapshotId) {
            this.setStatus("Choose two different snapshots.");
            return;
        }

        this.setStatus("Running compare...");
        if (this.runButton) {
            this.runButton.disabled = true;
        }

        const payload = {
            base_snapshot_id: baseSnapshotId,
            candidate_snapshot_id: candidateSnapshotId,
            capabilities: Array.from(this.selectedCapabilities),
            scope: this.currentUnitName
                ? { unit_names: [this.currentUnitName] }
                : {},
        };

        const response = await this.apiClient.compare(payload);
        if (!response.success) {
            this.setStatus(response.error || "Compare failed.");
            if (this.runButton) {
                this.runButton.disabled = false;
            }
            return;
        }

        this.compareResult = response.data || null;
        this.renderResultDetails();
        this.setStatus("Compare completed.");
        if (this.runButton) {
            this.runButton.disabled = false;
        }
    }

    renderResultDetails() {
        return renderResultDetails(this);
    }

    setStatus(message) {
        return setStatus(this, message);
    }
}

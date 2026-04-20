// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { Utils } from "./utils.js";

const CHANGE_MAGNITUDE_ORDER = Object.freeze({
    very_high: 4,
    high: 3,
    medium: 2,
    low: 1,
    stable: 0,
});

function escapeHtml(value) {
    const div = document.createElement("div");
    div.textContent = value ?? "";
    return div.innerHTML;
}

function safeArray(value) {
    return Array.isArray(value) ? value : [];
}

function changeBadgeClasses(changeMagnitude) {
    return {
        very_high: "bg-red-50 text-red-700 border-red-200",
        high: "bg-amber-50 text-amber-800 border-amber-200",
        medium: "bg-blue-50 text-blue-700 border-blue-200",
        low: "bg-slate-50 text-slate-700 border-slate-200",
        stable: "bg-gray-50 text-gray-700 border-gray-200",
    }[changeMagnitude] || "bg-slate-50 text-slate-700 border-slate-200";
}

function formatSignedNumber(value) {
    const numeric = Number(value || 0);
    const formatted = Number.isInteger(numeric)
        ? Utils.formatNumber(numeric)
        : numeric.toFixed(2);
    return numeric > 0 ? `+${formatted}` : formatted;
}

function formatScopeLabel(scope) {
    const entityKind = String(scope?.entity_kind || "").trim();
    const entityName = String(scope?.entity_name || "").trim();
    if (entityKind && entityName) {
        return `${entityKind}: ${entityName}`;
    }
    return String(
        scope?.candidate_unit_name || scope?.base_unit_name || "Snapshot",
    );
}

export function renderResultDetails(workspace) {
    if (!workspace.emptyState || !workspace.resultsRoot) {
        return;
    }
    if (!workspace.compareResult) {
        workspace.emptyState.classList.remove("hidden");
        workspace.resultsRoot.classList.add("hidden");
        return;
    }

    workspace.emptyState.classList.add("hidden");
    workspace.resultsRoot.classList.remove("hidden");

    const summary = workspace.compareResult.summary || {};
    const filteredChanges = getFilteredChanges(workspace);
    const matchCounts = summary.match_counts || {};
    const changedPairCount =
        Number(matchCounts.changed || 0) +
        Number(matchCounts.added || 0) +
        Number(matchCounts.removed || 0);

    if (workspace.findingCount) {
        workspace.findingCount.textContent = Utils.formatNumber(
            summary.change_count || 0,
        );
    }
    if (workspace.matchedUnits) {
        workspace.matchedUnits.textContent = Utils.formatNumber(
            summary.coverage?.matched_units || 0,
        );
    }
    if (workspace.changedUnits) {
        workspace.changedUnits.textContent = Utils.formatNumber(changedPairCount);
    }
    if (workspace.unchangedUnits) {
        workspace.unchangedUnits.textContent = Utils.formatNumber(
            workspace.compareResult.incremental_reuse?.unchanged_unit_pairs || 0,
        );
    }
    if (workspace.findingsCaption) {
        workspace.findingsCaption.textContent = `${filteredChanges.length} visible changes`;
    }

    renderChangesTable(workspace, filteredChanges);
    renderMatchSummary(workspace, summary);
    renderHotUnits(workspace);
}

function getFilteredChanges(workspace) {
    const changes = safeArray(workspace.compareResult?.changes);
    const changeMagnitude = workspace.changeFilter?.value || "all";
    const searchQuery = String(workspace.searchInput?.value || "")
        .trim()
        .toLowerCase();

    return changes
        .filter((change) =>
            changeMagnitude === "all"
                ? true
                : change.change_magnitude === changeMagnitude,
        )
        .filter((change) => {
            if (!searchQuery) {
                return true;
            }
            const searchTarget = [
                change.metric_key,
                change.provenance?.capability_id,
                change.scope?.entity_name,
                change.scope?.entity_kind,
                change.scope?.candidate_unit_name,
                change.scope?.base_unit_name,
            ]
                .filter(Boolean)
                .join(" ")
                .toLowerCase();
            return searchTarget.includes(searchQuery);
        })
        .sort((left, right) => {
            const magnitudeDelta =
                (CHANGE_MAGNITUDE_ORDER[right.change_magnitude] || 0) -
                (CHANGE_MAGNITUDE_ORDER[left.change_magnitude] || 0);
            if (magnitudeDelta !== 0) {
                return magnitudeDelta;
            }
            return (
                Math.abs(Number(right.absolute_delta || 0)) -
                Math.abs(Number(left.absolute_delta || 0))
            );
        })
        .slice(0, 60);
}

function renderChangesTable(workspace, changes) {
    if (!workspace.findingsBody) {
        return;
    }
    workspace.findingsBody.innerHTML = "";
    if (changes.length === 0) {
        workspace.findingsBody.innerHTML = `
            <tr>
                <td colspan="5" class="px-4 py-8 text-center text-sm text-gray-500">
                    No changes match the current filters.
                </td>
            </tr>
        `;
        return;
    }

    changes.forEach((change) => {
        const row = document.createElement("tr");
        row.innerHTML = `
            <td class="px-4 py-3 align-top">
                <span class="inline-flex rounded-full border px-2 py-1 text-xs font-semibold ${changeBadgeClasses(
                    change.change_magnitude,
                )}">
                    ${escapeHtml(String(change.change_magnitude || "stable"))}
                </span>
            </td>
            <td class="px-4 py-3 align-top text-gray-700">
                ${escapeHtml(String(change.provenance?.capability_id || ""))}
            </td>
            <td class="px-4 py-3 align-top text-gray-700">
                ${escapeHtml(String(change.metric_key || ""))}
            </td>
            <td class="px-4 py-3 align-top">
                <div class="font-medium text-gray-900">${escapeHtml(
                    formatScopeLabel(change.scope || {}),
                )}</div>
                <div class="mt-1 text-xs text-gray-500">${escapeHtml(
                    String(
                        change.scope?.candidate_unit_name ||
                            change.scope?.base_unit_name ||
                            "",
                    ),
                )}</div>
            </td>
            <td class="px-4 py-3 align-top">
                <div class="font-medium text-gray-900">${escapeHtml(
                    formatSignedNumber(change.absolute_delta),
                )}</div>
                <div class="mt-1 text-xs text-gray-500">${escapeHtml(
                    `${String(change.delta_sign || "stable")}: ${Utils.formatPercentage(
                        Number(change.relative_delta || 0) * 100,
                        1,
                    )}`,
                )}</div>
            </td>
        `;
        workspace.findingsBody.appendChild(row);
    });
}

function renderMatchSummary(workspace, summary) {
    if (!workspace.matchSummary) {
        return;
    }
    const matchCounts = summary.match_counts || {};
    const changeMagnitudeCounts = summary.change_magnitude_counts || {};
    workspace.matchSummary.innerHTML = [
        ["Matched", matchCounts.matched || 0],
        ["Changed", matchCounts.changed || 0],
        ["Added", matchCounts.added || 0],
        ["Removed", matchCounts.removed || 0],
        ["Very high", changeMagnitudeCounts.very_high || 0],
        ["High", changeMagnitudeCounts.high || 0],
    ]
        .map(
            ([label, value]) => `
                <div class="flex items-center justify-between rounded-lg border border-gray-200 bg-gray-50/70 px-3 py-2">
                    <span class="text-sm text-gray-600">${escapeHtml(label)}</span>
                    <strong class="text-sm text-gray-900">${escapeHtml(
                        Utils.formatNumber(value),
                    )}</strong>
                </div>
            `,
        )
        .join("");
}

function renderHotUnits(workspace) {
    if (!workspace.hotUnits) {
        return;
    }
    const units = safeArray(workspace.compareResult?.unit_change_leaderboard).slice(
        0,
        8,
    );
    if (units.length === 0) {
        workspace.hotUnits.innerHTML =
            '<p class="text-sm text-gray-500">No changed units detected.</p>';
        return;
    }
    workspace.hotUnits.innerHTML = units
        .map(
            (unit) => `
                <div class="rounded-lg border border-gray-200 bg-gray-50/40 px-3 py-3">
                    <div class="font-medium text-gray-900">${escapeHtml(
                        String(unit.unit_name || ""),
                    )}</div>
                    <div class="mt-1 text-xs text-gray-500">${escapeHtml(
                        `${Utils.formatNumber(unit.change_count || 0)} changes`,
                    )}</div>
                </div>
            `,
        )
        .join("");
}

export function setStatus(workspace, message) {
    if (workspace.statusLabel) {
        workspace.statusLabel.textContent = message;
    }
}

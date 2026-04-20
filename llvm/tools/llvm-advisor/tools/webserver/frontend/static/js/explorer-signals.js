// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { escapeHtml, safeArray } from "./explorer-shared.js";

export function rebuildIntegratedFindings(explorer) {
    const findings = [];
    let nextId = 0;

    safeArray(explorer.sourceInlineData?.diagnostics).forEach((diagnostic) => {
        const outputLine = findMappedOutputLine(explorer, diagnostic.line);
        findings.push({
            id: `src-diag-${nextId++}`,
            kind: "diagnostic",
            message: diagnostic.message,
            level: diagnostic.level || "info",
            sourceLine: diagnostic.line || null,
            sourceColumn: diagnostic.column || null,
            outputLine,
            passName: null,
            viewer: "source",
        });
    });

    safeArray(explorer.sourceInlineData?.remarks).forEach((remark) => {
        const outputMatch = safeArray(explorer.outputInlineData?.remarks).find(
            (entry) =>
                Number(entry.source_line || 0) === Number(remark.line || 0) &&
                String(entry.message || "") === String(remark.message || ""),
        );
        const mappedOutputLine =
            outputMatch?.line || findMappedOutputLine(explorer, remark.line);
        findings.push({
            id: `src-remark-${nextId++}`,
            kind: "remark",
            message: remark.message,
            level: "remark",
            sourceLine: remark.line || null,
            sourceColumn: remark.column || null,
            outputLine: mappedOutputLine || null,
            passName: remark.pass_name || outputMatch?.pass_name || null,
            viewer: "source",
        });
    });

    safeArray(explorer.outputInlineData?.remarks).forEach((remark) => {
        const sourceLine = remark.source_line || null;
        const duplicate = findings.some(
            (entry) =>
                entry.kind === "remark" &&
                entry.outputLine === remark.line &&
                entry.sourceLine === sourceLine &&
                entry.message === remark.message,
        );
        if (duplicate) {
            return;
        }
        findings.push({
            id: `out-remark-${nextId++}`,
            kind: "remark",
            message: remark.message,
            level: "remark",
            sourceLine,
            sourceColumn: null,
            outputLine: remark.line || null,
            passName: remark.pass_name || null,
            viewer: "output",
        });
    });

    findings.sort((left, right) => {
        const leftLine = Number(left.sourceLine || left.outputLine || 0);
        const rightLine = Number(right.sourceLine || right.outputLine || 0);
        return leftLine - rightLine;
    });

    explorer.integratedFindings = findings;
    if (!findings.some((entry) => entry.id === explorer.activeFindingId)) {
        explorer.activeFindingId = findings[0]?.id || null;
    }
    renderIntegratedFindings(explorer);
}

export function findMappedOutputLine(explorer, sourceLine) {
    const numericSourceLine = Number(sourceLine || 0);
    if (!numericSourceLine) {
        return null;
    }
    const entry = explorer.outputLineMapping.find(
        (mappingEntry) =>
            Number(mappingEntry?.source_line || 0) === numericSourceLine,
    );
    return entry ? Number(entry.representation_line || 0) || null : null;
}

export function renderIntegratedFindings(explorer) {
    if (!explorer.findingsList) {
        return;
    }
    const findings = explorer.integratedFindings;
    if (explorer.findingsCount) {
        explorer.findingsCount.textContent = `${findings.length} signal${findings.length === 1 ? "" : "s"}`;
    }
    if (findings.length === 0) {
        if (explorer.selectionSummary) {
            explorer.selectionSummary.textContent =
                "Select a line, remark, or diagnostic to sync source and generated output.";
        }
        explorer.findingsList.innerHTML = `
            <div class="empty-state h-full col-span-full">
                <div class="text-center">
                    <p class="mt-2">No collected diagnostics or remarks are available for this file yet.</p>
                </div>
            </div>
        `;
        return;
    }

    explorer.findingsList.innerHTML = findings
        .map(
            (finding) => `
                <button
                    type="button"
                    class="finding-card text-left ${finding.id === explorer.activeFindingId ? "active" : ""}"
                    data-finding-id="${escapeHtml(finding.id)}"
                >
                    <div class="finding-meta">
                        <span class="finding-kind" data-kind="${escapeHtml(finding.kind)}">${escapeHtml(finding.kind)}</span>
                        <span class="finding-location">${escapeHtml(formatFindingLocation(finding))}</span>
                    </div>
                    <div class="finding-message">${escapeHtml(finding.message || "")}</div>
                    <div class="finding-context">${escapeHtml(formatFindingContext(finding))}</div>
                </button>
            `,
        )
        .join("");

    explorer.findingsList.querySelectorAll("[data-finding-id]").forEach((node) => {
        node.addEventListener("click", () => {
            const finding = explorer.integratedFindings.find(
                (entry) => entry.id === node.dataset.findingId,
            );
            if (finding) {
                activateFinding(explorer, finding);
            }
        });
    });

    if (!explorer.activeFindingId && explorer.selectionSummary) {
        explorer.selectionSummary.textContent =
            "Select a line, remark, or diagnostic to sync source and generated output.";
    }
}

export function formatFindingLocation(finding) {
    const segments = [];
    if (finding.sourceLine) {
        segments.push(`src:${finding.sourceLine}`);
    }
    if (finding.outputLine) {
        segments.push(`out:${finding.outputLine}`);
    }
    return segments.join(" -> ") || "unmapped";
}

export function formatFindingContext(finding) {
    if (finding.passName) {
        return `Pass: ${finding.passName}`;
    }
    if (finding.kind === "diagnostic") {
        return `Level: ${finding.level}`;
    }
    return "Connected across collected source and representation views.";
}

export function activateFinding(explorer, finding) {
    explorer.activeFindingId = finding.id;
    renderIntegratedFindings(explorer);
    if (finding.sourceLine && explorer.sourceViewer) {
        explorer.sourceViewer.focusLine(finding.sourceLine);
    }
    if (finding.outputLine && explorer.outputViewer) {
        explorer.outputViewer.focusLine(finding.outputLine);
    }
    if (explorer.selectionSummary) {
        explorer.selectionSummary.textContent = formatSelectionSummary(finding);
    }
}

export function formatSelectionSummary(finding) {
    const location = formatFindingLocation(finding);
    if (finding.passName) {
        return `${location} • ${finding.passName}`;
    }
    if (finding.kind === "diagnostic") {
        return `${location} • ${finding.level}`;
    }
    return location;
}

export function handleSourceLineSelected(explorer, lineNumber, inlineData) {
    const finding =
        findIntegratedFinding(
            explorer,
            (entry) =>
                Number(entry.sourceLine || 0) === Number(lineNumber) &&
                (inlineData?.diagnostics?.length || inlineData?.remarks?.length),
        ) ||
        findIntegratedFinding(
            explorer,
            (entry) => Number(entry.sourceLine || 0) === Number(lineNumber),
        );
    if (finding) {
        activateFinding(explorer, finding);
        return;
    }
    explorer.sourceViewer?.setActiveLine(lineNumber);
    if (explorer.outputViewer) {
        explorer.outputViewer.setActiveLine(null);
    }
    if (explorer.selectionSummary) {
        explorer.selectionSummary.textContent = `Source line ${lineNumber}`;
    }
}

export function handleOutputLineSelected(explorer, lineNumber, inlineData) {
    const finding =
        findIntegratedFinding(
            explorer,
            (entry) =>
                Number(entry.outputLine || 0) === Number(lineNumber) &&
                (inlineData?.remarks?.length || inlineData?.diagnostics?.length),
        ) ||
        findIntegratedFinding(
            explorer,
            (entry) => Number(entry.outputLine || 0) === Number(lineNumber),
        );
    if (finding) {
        activateFinding(explorer, finding);
        return;
    }
    explorer.outputViewer?.setActiveLine(lineNumber);
    if (explorer.sourceViewer) {
        explorer.sourceViewer.setActiveLine(null);
    }
    if (explorer.selectionSummary) {
        explorer.selectionSummary.textContent = `Output line ${lineNumber}`;
    }
}

export function findIntegratedFinding(explorer, predicate) {
    return explorer.integratedFindings.find((entry) => {
        try {
            return predicate(entry);
        } catch {
            return false;
        }
    });
}

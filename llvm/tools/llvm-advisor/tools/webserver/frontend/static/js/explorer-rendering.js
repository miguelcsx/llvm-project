// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { CodeViewer } from "./code-viewer.js";
import { DiffViewer } from "./diff-viewer.js";
import {
    createCenteredMessageMarkup,
    createLoadingMarkup,
    safeArray,
} from "./explorer-shared.js";

export function applySourceResponse(explorer, sourceData) {
    if (!sourceData) {
        explorer.sourceContent = null;
        explorer.sourceInlineData = { diagnostics: [], remarks: [] };
        explorer.renderSourceEmptyState("Source code unavailable.");
        explorer.updateInlineToggleButtons();
        return;
    }

    explorer.sourceContent = sourceData;
    explorer.sourceInlineData = sourceData.inline_data || {
        diagnostics: [],
        remarks: [],
    };
    displaySourceCode(explorer, sourceData);
    explorer.updateInlineToggleButtons();
    explorer.rebuildIntegratedFindings();
}

export function applyOutputResponse(explorer, outputData) {
    if (!outputData) {
        explorer.outputContent = null;
        explorer.outputInlineData = { diagnostics: [], remarks: [] };
        explorer.outputLineMapping = [];
        explorer.renderOutputEmptyState("Selected representation is unavailable.");
        explorer.updateInlineToggleButtons();
        return;
    }

    explorer.outputContent = outputData;
    explorer.outputInlineData = outputData.inline_data || {
        diagnostics: [],
        remarks: [],
    };
    explorer.outputLineMapping = safeArray(outputData.line_mapping?.entries);
    displayOutput(explorer, outputData);
    explorer.updateInlineToggleButtons();
    explorer.rebuildIntegratedFindings();
}

export function displaySourceCode(explorer, sourceData) {
    if (!explorer.sourceContainer) {
        return;
    }

    destroySourceViewer(explorer);
    explorer.sourceViewer = new CodeViewer(explorer.sourceContainer, {
        language: sourceData.language || "text",
        showLineNumbers: true,
        readOnly: true,
        onLineSelect: ({ lineNumber, inlineData }) =>
            explorer.handleSourceLineSelected(lineNumber, inlineData),
    });

    explorer.sourceViewer.render(
        sourceData.source || "",
        sourceData.language || "text",
        explorer.sourceInlineData,
    );
    Object.entries(explorer.inlineDataVisible).forEach(([type, visible]) => {
        if (visible) {
            explorer.sourceViewer.toggleInlineData(type, true);
        }
    });
}

export function displayOutput(explorer, outputData) {
    if (!explorer.outputContainer) {
        return;
    }
    if (outputData.type === "diff") {
        displayDiffOutput(explorer, outputData);
        return;
    }

    const renderableContent = extractRenderableContent(outputData);
    const language = getOutputLanguage(explorer, outputData, explorer.currentViewType);
    destroyOutputViewers(explorer);
    explorer.outputViewer = new CodeViewer(explorer.outputContainer, {
        language,
        showLineNumbers: true,
        readOnly: true,
        onLineSelect: ({ lineNumber, inlineData }) =>
            explorer.handleOutputLineSelected(lineNumber, inlineData),
    });
    explorer.outputViewer.render(
        renderableContent,
        language,
        explorer.outputInlineData,
    );
    Object.entries(explorer.inlineDataVisible).forEach(([type, visible]) => {
        if (visible) {
            explorer.outputViewer.toggleInlineData(type, true);
        }
    });
}

export function displayDiffOutput(explorer, outputData) {
    destroyOutputViewers(explorer);
    if (!explorer.outputContainer) {
        return;
    }
    explorer.diffViewer = new DiffViewer(explorer.outputContainer, {
        language: outputData.language || "llvm",
        readOnly: true,
    });
    explorer.diffViewer.render(
        outputData.original || "",
        outputData.modified || "",
        outputData.language || "llvm",
    );
}

export function extractRenderableContent(outputData) {
    if (typeof outputData?.content === "string") {
        return outputData.content;
    }
    if (typeof outputData?.source === "string") {
        return outputData.source;
    }
    if (typeof outputData?.data === "string") {
        return outputData.data;
    }
    if (outputData?.data && typeof outputData.data === "object") {
        return JSON.stringify(outputData.data, null, 2);
    }
    if (outputData?.summary && typeof outputData.summary === "object") {
        return JSON.stringify(outputData.summary, null, 2);
    }
    return "";
}

export function getOutputLanguage(explorer, outputData, viewType) {
    if (viewType === "assembly") {
        return "assembly";
    }
    if (viewType === "ir" || viewType === "optimized-ir") {
        return "llvm-ir";
    }
    if (viewType === "ast-json") {
        return "json";
    }
    if (viewType === "object") {
        return "text";
    }
    if (viewType === "preprocessed" || viewType === "macro-expansion") {
        return explorer.sourceContent?.language || "text";
    }
    return outputData?.language || "text";
}

export function toggleInlineData(explorer, type) {
    explorer.inlineDataVisible[type] = !explorer.inlineDataVisible[type];
    updateInlineToggleButtons(explorer);

    if (explorer.sourceViewer) {
        explorer.sourceViewer.toggleInlineData(
            type,
            explorer.inlineDataVisible[type],
        );
    }
    if (explorer.outputViewer) {
        explorer.outputViewer.toggleInlineData(
            type,
            explorer.inlineDataVisible[type],
        );
    }
}

export function updateInlineToggleButtons(explorer) {
    updateInlineToggleButton(
        explorer,
        explorer.toggleDiagnosticsBtn,
        "diagnostics",
        getInlineEntryCount(explorer, "diagnostics"),
    );
    updateInlineToggleButton(
        explorer,
        explorer.toggleRemarksBtn,
        "remarks",
        getInlineEntryCount(explorer, "remarks"),
    );
}

export function getInlineEntryCount(explorer, type) {
    return (
        safeArray(explorer.sourceInlineData?.[type]).length +
        safeArray(explorer.outputInlineData?.[type]).length
    );
}

export function updateInlineToggleButton(explorer, button, type, count) {
    if (!button) {
        return;
    }

    const isVisible = explorer.inlineDataVisible?.[type] ?? false;
    const hasEntries = count > 0;
    button.disabled = !hasEntries;
    button.classList.toggle("opacity-50", !hasEntries);
    button.classList.toggle("cursor-not-allowed", !hasEntries);
    button.classList.toggle("bg-slate-100", isVisible && hasEntries);
    button.classList.toggle("text-slate-800", isVisible && hasEntries);

    const label = type === "diagnostics" ? "Diagnostics" : "Remarks";
    button.textContent = hasEntries ? `${label} (${count})` : label;
}

export function updateActionButtons(explorer) {
    const hasOutput = Boolean(explorer.outputContent);
    if (explorer.copyBtn) {
        explorer.copyBtn.disabled = !hasOutput;
        explorer.copyBtn.classList.toggle("opacity-50", !hasOutput);
    }
    if (explorer.downloadBtn) {
        explorer.downloadBtn.disabled = !hasOutput;
        explorer.downloadBtn.classList.toggle("opacity-50", !hasOutput);
    }
}

export async function copyToClipboard(explorer) {
    if (!explorer.outputContent) {
        return;
    }

    try {
        const content =
            explorer.outputContent.type === "diff"
                ? buildDiffClipboardText(explorer.outputContent)
                : extractRenderableContent(explorer.outputContent);
        await navigator.clipboard.writeText(content);
        explorer.app?.showInfo?.("Output copied to clipboard.");
    } catch {
        explorer.showError("Failed to copy output.");
    }
}

export function buildDiffClipboardText(diffData) {
    return [
        "=== Original ===",
        diffData.original || "",
        "",
        "=== Modified ===",
        diffData.modified || "",
    ].join("\n");
}

export function downloadOutput(explorer) {
    if (!explorer.outputContent) {
        return;
    }
    const content =
        explorer.outputContent.type === "diff"
            ? buildDiffClipboardText(explorer.outputContent)
            : extractRenderableContent(explorer.outputContent);
    const extension = getDownloadExtension(explorer);
    const fileName = buildDownloadFileName(explorer, extension);
    const blob = new Blob([content], { type: "text/plain;charset=utf-8" });
    const objectUrl = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = objectUrl;
    link.download = fileName;
    link.click();
    URL.revokeObjectURL(objectUrl);
}

export function buildDownloadFileName(explorer, extension) {
    const baseName =
        explorer.currentFile?.split("/").pop()?.replace(/\.[^.]+$/, "") || "artifact";
    return `${baseName}-${explorer.currentViewType}.${extension}`;
}

export function getDownloadExtension(explorer) {
    switch (explorer.currentViewType) {
        case "assembly":
            return "s";
        case "ir":
        case "optimized-ir":
        case "diff":
            return "ll";
        case "ast-json":
            return "json";
        case "preprocessed":
        case "macro-expansion":
            return "txt";
        default:
            return "txt";
    }
}

export function renderEmptyState(explorer, message = "Select a file to begin exploring.") {
    renderSourceEmptyState(explorer, message);
    renderOutputEmptyState(explorer, "Select a representation to view.");
    explorer.setArtifactStatus("Select a source file to inspect collected representations.");
    explorer.integratedFindings = [];
    explorer.activeFindingId = null;
    explorer.renderIntegratedFindings();
}

export function renderSourceEmptyState(explorer, message) {
    if (explorer.sourceContainer) {
        explorer.sourceContainer.innerHTML = createCenteredMessageMarkup(message);
    }
}

export function renderOutputEmptyState(explorer, message) {
    if (explorer.outputContainer) {
        explorer.outputContainer.innerHTML = createCenteredMessageMarkup(message);
    }
}

export function showSourceLoading(explorer) {
    if (explorer.sourceContainer) {
        explorer.sourceContainer.innerHTML = createLoadingMarkup("Loading source...");
    }
}

export function showOutputLoading(explorer) {
    if (explorer.outputContainer) {
        explorer.outputContainer.innerHTML = createLoadingMarkup("Loading output...");
    }
}

export function showOutputError(explorer, message) {
    if (explorer.outputContainer) {
        explorer.outputContainer.innerHTML = createCenteredMessageMarkup(
            message,
            "text-red-600",
        );
    }
}

export function destroySourceViewer(explorer) {
    if (explorer.sourceViewer && typeof explorer.sourceViewer.clear === "function") {
        explorer.sourceViewer.clear();
    }
    explorer.sourceViewer = null;
}

export function destroyOutputViewers(explorer) {
    if (explorer.outputViewer && typeof explorer.outputViewer.clear === "function") {
        explorer.outputViewer.clear();
    }
    if (explorer.diffViewer && typeof explorer.diffViewer.destroy === "function") {
        explorer.diffViewer.destroy();
    }
    explorer.outputViewer = null;
    explorer.diffViewer = null;
}

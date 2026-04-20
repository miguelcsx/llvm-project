// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Explorer Module
 * Keeps the controller thin and delegates loading, rendering, and
 * signal correlation to focused modules.
 */

import {
    changeViewType,
    loadAvailableFiles,
    loadCurrentSelection,
    loadCurrentOutput,
    loadRepresentationCapabilities,
    selectFile,
    setArtifactStatus,
    updateFileSelector,
    updateRightPanelTitle,
    updateViewTypeSelector,
} from "./explorer-data.js";
import {
    applyOutputResponse,
    applySourceResponse,
    copyToClipboard,
    destroyOutputViewers,
    destroySourceViewer,
    downloadOutput,
    getInlineEntryCount,
    renderEmptyState,
    renderOutputEmptyState,
    renderSourceEmptyState,
    showOutputError,
    showOutputLoading,
    showSourceLoading,
    toggleInlineData,
    updateActionButtons,
    updateInlineToggleButtons,
} from "./explorer-rendering.js";
import {
    activateFinding,
    findIntegratedFinding,
    findMappedOutputLine,
    formatFindingContext,
    formatFindingLocation,
    formatSelectionSummary,
    handleOutputLineSelected,
    handleSourceLineSelected,
    rebuildIntegratedFindings,
    renderIntegratedFindings,
} from "./explorer-signals.js";
import { isNonEmptyString } from "./explorer-shared.js";

export class Explorer {
    constructor(apiClient, app) {
        this.apiClient = apiClient;
        this.app = app;
        this.currentFile = null;
        this.currentViewType = "assembly";
        this.representationCapabilities = new Map();
        this.availableFiles = [];
        this.availableFilesByPath = new Map();
        this.sourceContent = null;
        this.outputContent = null;
        this.sourceInlineData = { diagnostics: [], remarks: [] };
        this.outputInlineData = { diagnostics: [], remarks: [] };
        this.outputLineMapping = [];
        this.inlineDataVisible = { diagnostics: false, remarks: false };
        this.integratedFindings = [];
        this.activeFindingId = null;
        this.isActive = false;
        this.isLoading = false;
        this.lastLoadedRequestKey = null;
        this.sourceViewer = null;
        this.outputViewer = null;
        this.diffViewer = null;
        this.initializeElements();
    }

    initializeElements() {
        this.fileSelector = document.getElementById("file-selector");
        this.viewTypeSelector = document.getElementById("view-type-selector");
        this.artifactStatus = document.getElementById("explorer-artifact-status");
        this.sourceContainer = document.getElementById("source-code-container");
        this.outputContainer = document.getElementById("output-container");
        this.rightPanelTitle = document.getElementById("right-panel-title");
        this.copyBtn = document.getElementById("copy-output-btn");
        this.downloadBtn = document.getElementById("download-output-btn");
        this.toggleDiagnosticsBtn = document.getElementById("toggle-diagnostics-btn");
        this.toggleRemarksBtn = document.getElementById("toggle-remarks-btn");
        this.findingsList = document.getElementById("explorer-findings-list");
        this.findingsCount = document.getElementById("explorer-findings-count");
        this.selectionSummary = document.getElementById("explorer-selection-summary");
    }

    init() {
        this.initializeSplitLayout();
        this.setupEventListeners();
        this.renderEmptyState();
        this.updateInlineToggleButtons();
        this.updateActionButtons();
        this.renderIntegratedFindings();
    }

    initializeSplitLayout() {
        if (typeof Split === "undefined") {
            return;
        }
        if (window.matchMedia("(max-width: 1024px)").matches) {
            return;
        }
        Split(["#explorer-left-panel", "#explorer-right-panel"], {
            sizes: [50, 50],
            minSize: 200,
            gutterSize: 8,
            cursor: "col-resize",
        });
    }

    setupEventListeners() {
        this.fileSelector?.addEventListener("change", async (event) => {
            const nextFile = event.target.value;
            if (!isNonEmptyString(nextFile) || nextFile === this.currentFile) {
                return;
            }
            await this.selectFile(nextFile);
        });

        this.viewTypeSelector?.addEventListener("change", async (event) => {
            const nextViewType = event.target.value;
            if (
                !isNonEmptyString(nextViewType) ||
                nextViewType === this.currentViewType
            ) {
                return;
            }
            await this.changeViewType(nextViewType);
        });

        this.copyBtn?.addEventListener("click", () => this.copyToClipboard());
        this.downloadBtn?.addEventListener("click", () => this.downloadOutput());
        this.toggleDiagnosticsBtn?.addEventListener("click", (event) => {
            event.preventDefault();
            this.toggleInlineData("diagnostics");
        });
        this.toggleRemarksBtn?.addEventListener("click", (event) => {
            event.preventDefault();
            this.toggleInlineData("remarks");
        });
    }

    async onActivate() {
        this.isActive = true;
        await this.loadRepresentationCapabilities();
        await this.loadAvailableFiles();
    }

    loadRepresentationCapabilities() {
        return loadRepresentationCapabilities(this);
    }

    loadAvailableFiles() {
        return loadAvailableFiles(this);
    }

    updateFileSelector() {
        return updateFileSelector(this);
    }

    selectFile(filePath) {
        return selectFile(this, filePath);
    }

    changeViewType(viewType) {
        return changeViewType(this, viewType);
    }

    updateViewTypeSelector(file = null) {
        return updateViewTypeSelector(this, file);
    }

    updateRightPanelTitle() {
        return updateRightPanelTitle(this);
    }

    loadCurrentSelection() {
        return loadCurrentSelection(this);
    }

    loadCurrentOutput() {
        return loadCurrentOutput(this);
    }

    applySourceResponse(sourceData) {
        return applySourceResponse(this, sourceData);
    }

    applyOutputResponse(outputData) {
        return applyOutputResponse(this, outputData);
    }

    toggleInlineData(type) {
        return toggleInlineData(this, type);
    }

    updateInlineToggleButtons() {
        return updateInlineToggleButtons(this);
    }

    getInlineEntryCount(type) {
        return getInlineEntryCount(this, type);
    }

    updateActionButtons() {
        return updateActionButtons(this);
    }

    copyToClipboard() {
        return copyToClipboard(this);
    }

    downloadOutput() {
        return downloadOutput(this);
    }

    renderEmptyState(message) {
        return renderEmptyState(this, message);
    }

    renderSourceEmptyState(message) {
        return renderSourceEmptyState(this, message);
    }

    renderOutputEmptyState(message) {
        return renderOutputEmptyState(this, message);
    }

    showSourceLoading() {
        return showSourceLoading(this);
    }

    showOutputLoading() {
        return showOutputLoading(this);
    }

    showOutputError(message) {
        return showOutputError(this, message);
    }

    rebuildIntegratedFindings() {
        return rebuildIntegratedFindings(this);
    }

    findMappedOutputLine(sourceLine) {
        return findMappedOutputLine(this, sourceLine);
    }

    renderIntegratedFindings() {
        return renderIntegratedFindings(this);
    }

    formatFindingLocation(finding) {
        return formatFindingLocation(finding);
    }

    formatFindingContext(finding) {
        return formatFindingContext(finding);
    }

    activateFinding(finding) {
        return activateFinding(this, finding);
    }

    formatSelectionSummary(finding) {
        return formatSelectionSummary(finding);
    }

    handleSourceLineSelected(lineNumber, inlineData) {
        return handleSourceLineSelected(this, lineNumber, inlineData);
    }

    handleOutputLineSelected(lineNumber, inlineData) {
        return handleOutputLineSelected(this, lineNumber, inlineData);
    }

    findIntegratedFinding(predicate) {
        return findIntegratedFinding(this, predicate);
    }

    showError(message) {
        if (this.app && typeof this.app.showError === "function") {
            this.app.showError(message);
        }
    }

    setArtifactStatus(message, tone = "muted") {
        return setArtifactStatus(this, message, tone);
    }

    destroySourceViewer() {
        return destroySourceViewer(this);
    }

    destroyOutputViewers() {
        return destroyOutputViewers(this);
    }
}

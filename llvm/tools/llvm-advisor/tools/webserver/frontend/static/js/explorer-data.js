// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import {
    getDefaultViewTypeLabel,
    isNonEmptyString,
    normalizeArtifactList,
    normalizeFileKey,
    safeArray,
} from "./explorer-shared.js";

export async function loadRepresentationCapabilities(explorer) {
    try {
        const response = await explorer.apiClient.getRepresentationCapabilities();
        if (!response?.success) {
            return;
        }
        const capabilities = safeArray(response.data?.capabilities);
        explorer.representationCapabilities = new Map(
            capabilities
                .filter(
                    (capability) =>
                        capability &&
                        capability.category === "explorer" &&
                        isNonEmptyString(capability.kind),
                )
                .map((capability) => [capability.kind, capability]),
        );
    } catch {
        explorer.representationCapabilities = new Map();
    }
}

export async function loadAvailableFiles(explorer) {
    const currentUnitName = getCurrentUnitName(explorer);
    if (!currentUnitName) {
        setAvailableFiles(explorer, []);
        explorer.renderEmptyState("No compilation unit selected.");
        return;
    }

    try {
        const response = await explorer.apiClient.getSourceFiles(currentUnitName);
        if (!response.success) {
            throw new Error(response.error || "Failed to load available files");
        }

        const files = safeArray(response.data?.files);
        setAvailableFiles(explorer, files);
        updateFileSelector(explorer);

        if (files.length === 0) {
            setArtifactStatus(
                explorer,
                "No source files were collected for this compilation unit.",
                "warning",
            );
            explorer.renderEmptyState("No source files available for this unit.");
            return;
        }

        const preferredFile = resolvePreferredFile(explorer);
        if (preferredFile) {
            await selectFile(explorer, preferredFile.path || preferredFile.name);
        }
    } catch {
        setAvailableFiles(explorer, []);
        updateFileSelector(explorer);
        setArtifactStatus(explorer, "Failed to load collected source files.", "error");
        explorer.showError("Failed to load available files.");
    }
}

export function setAvailableFiles(explorer, files) {
    explorer.availableFiles = safeArray(files);
    explorer.availableFilesByPath = new Map(
        explorer.availableFiles.map((file) => [normalizeFileKey(file), file]),
    );
}

export function resolvePreferredFile(explorer) {
    if (
        explorer.currentFile &&
        explorer.availableFilesByPath.has(explorer.currentFile)
    ) {
        return explorer.availableFilesByPath.get(explorer.currentFile);
    }
    const primaryFile = explorer.availableFiles.find((file) => file?.is_primary);
    if (primaryFile) {
        return primaryFile;
    }
    return explorer.availableFiles[0] || null;
}

export function updateFileSelector(explorer) {
    if (!explorer.fileSelector) {
        return;
    }

    explorer.fileSelector.innerHTML = "";
    const placeholder = document.createElement("option");
    placeholder.value = "";
    placeholder.textContent =
        explorer.availableFiles.length > 0 ? "Select a file..." : "No files available";
    placeholder.disabled = true;
    placeholder.selected = !explorer.currentFile;
    explorer.fileSelector.appendChild(placeholder);

    explorer.availableFiles.forEach((file) => {
        const option = document.createElement("option");
        option.value = normalizeFileKey(file);
        option.textContent = file.display_name || file.name || option.value;
        option.selected = option.value === explorer.currentFile;
        explorer.fileSelector.appendChild(option);
    });

    if (
        explorer.currentFile &&
        explorer.availableFilesByPath.has(explorer.currentFile)
    ) {
        explorer.fileSelector.value = explorer.currentFile;
    }
}

export async function selectFile(explorer, filePath) {
    const file = explorer.availableFilesByPath.get(filePath);
    if (!file) {
        return;
    }

    explorer.currentFile = filePath;
    if (explorer.fileSelector) {
        explorer.fileSelector.value = filePath;
    }
    updateViewTypeSelector(explorer, file);
    await loadCurrentSelection(explorer);
}

export async function changeViewType(explorer, viewType) {
    explorer.currentViewType = viewType;
    updateRightPanelTitle(explorer);
    await loadCurrentOutput(explorer);
}

export function updateViewTypeSelector(explorer, file = null) {
    if (!explorer.viewTypeSelector) {
        return;
    }

    const selectedFile =
        file ||
        explorer.availableFilesByPath.get(explorer.currentFile) ||
        explorer.availableFiles[0] ||
        null;
    const availableArtifacts = new Set(normalizeArtifactList(selectedFile));
    if (availableArtifacts.has("ir") && availableArtifacts.has("optimized-ir")) {
        availableArtifacts.add("diff");
    }

    const orderedViewTypes = getRenderableViewTypes(explorer, availableArtifacts);
    explorer.viewTypeSelector.innerHTML = "";
    explorer.viewTypeSelector.disabled = orderedViewTypes.length === 0;

    orderedViewTypes.forEach((viewType) => {
        const option = document.createElement("option");
        option.value = viewType;
        option.textContent = getViewTypeLabel(explorer, viewType);
        explorer.viewTypeSelector.appendChild(option);
    });

    if (orderedViewTypes.length === 0) {
        const placeholder = document.createElement("option");
        placeholder.value = "";
        placeholder.textContent = "No representation views collected";
        explorer.viewTypeSelector.appendChild(placeholder);
        explorer.currentViewType = "";
        updateRightPanelTitle(explorer);
        setArtifactStatus(
            explorer,
            "This file has source text only. No renderable representation was collected for it.",
            "warning",
        );
        return;
    }

    if (!orderedViewTypes.includes(explorer.currentViewType)) {
        explorer.currentViewType = orderedViewTypes[0];
    }
    explorer.viewTypeSelector.value = explorer.currentViewType;
    updateRightPanelTitle(explorer);
    setArtifactStatus(
        explorer,
        `Collected views: ${orderedViewTypes
            .map((viewType) => getViewTypeLabel(explorer, viewType))
            .join(", ")}.`,
    );
}

export function updateRightPanelTitle(explorer) {
    if (!explorer.rightPanelTitle) {
        return;
    }
    explorer.rightPanelTitle.textContent =
        getViewTypeLabel(explorer, explorer.currentViewType) || "Representation Output";
}

export async function loadCurrentSelection(explorer) {
    if (!explorer.currentFile || explorer.isLoading) {
        return;
    }

    explorer.isLoading = true;
    const requestKey = `${getCurrentUnitName(explorer)}::${explorer.currentFile}`;
    explorer.lastLoadedRequestKey = requestKey;
    explorer.showSourceLoading();
    if (explorer.currentViewType) {
        explorer.showOutputLoading();
    } else {
        explorer.renderOutputEmptyState(
            "No representation view is available for the selected source file.",
        );
    }

    try {
        const requests = [fetchSourceCode(explorer, explorer.currentFile)];
        if (explorer.currentViewType) {
            requests.push(fetchOutput(explorer, explorer.currentFile, explorer.currentViewType));
        }
        const [sourceResponse, outputResponse = null] = await Promise.all(requests);
        if (explorer.lastLoadedRequestKey !== requestKey) {
            return;
        }
        explorer.applySourceResponse(sourceResponse);
        explorer.applyOutputResponse(outputResponse);
        explorer.updateActionButtons();
    } catch {
        explorer.showError("Failed to load file content.");
        explorer.showOutputError("Failed to load output.");
    } finally {
        if (explorer.lastLoadedRequestKey === requestKey) {
            explorer.isLoading = false;
        }
    }
}

export async function loadCurrentOutput(explorer) {
    if (!explorer.currentFile) {
        explorer.renderOutputEmptyState("No file selected.");
        return;
    }
    if (!explorer.currentViewType) {
        explorer.renderOutputEmptyState(
            "No representation view is available for the selected source file.",
        );
        return;
    }

    explorer.showOutputLoading();
    try {
        const outputResponse = await fetchOutput(
            explorer,
            explorer.currentFile,
            explorer.currentViewType,
        );
        explorer.applyOutputResponse(outputResponse);
        explorer.updateActionButtons();
    } catch {
        explorer.showOutputError("Failed to load output.");
    }
}

export async function fetchSourceCode(explorer, filePath) {
    const response = await explorer.apiClient.getSourceCode(
        filePath,
        getCurrentUnitName(explorer),
    );
    return response.success ? response.data : null;
}

export async function fetchOutput(explorer, filePath, viewType) {
    return fetchRepresentationOutput(explorer, filePath, viewType);
}

export async function fetchRepresentationOutput(explorer, filePath, viewType) {
    const unitName = getCurrentUnitName(explorer);
    if (!unitName || !viewType) {
        return null;
    }
    const response = await explorer.apiClient.requestRepresentations({
        action: "materialize",
        unit_name: unitName,
        source_ref: filePath,
        include_inline_data: true,
        requests: [{ kind: viewType }],
    });
    if (!response?.success) {
        return null;
    }
    const requestEntries = safeArray(response.data?.requests);
    const requestEntry = requestEntries.find(
        (entry) => entry && entry.kind === viewType,
    );
    if (!requestEntry || requestEntry.status !== "ready") {
        return null;
    }
    return requestEntry.payload || null;
}

export function getCurrentUnitName(explorer) {
    return (
        explorer.app?.currentUnit ||
        document.getElementById("unit-selector")?.value ||
        null
    );
}

export function getRenderableViewTypes(explorer, availableArtifacts) {
    const capabilityOrder = [...explorer.representationCapabilities.values()]
        .sort(
            (left, right) =>
                Number(left?.display_rank || 0) - Number(right?.display_rank || 0),
        )
        .map((capability) => capability.kind);
    const fallbackOrder = [
        "assembly",
        "ir",
        "optimized-ir",
        "diff",
        "ast-json",
        "object",
        "preprocessed",
        "macro-expansion",
    ];
    const orderedKinds = capabilityOrder.length > 0 ? capabilityOrder : fallbackOrder;
    const viewTypes = orderedKinds.filter((type) => availableArtifacts.has(type));

    if (
        availableArtifacts.has("ir") &&
        availableArtifacts.has("optimized-ir") &&
        !viewTypes.includes("diff")
    ) {
        const diffCapability = explorer.representationCapabilities.get("diff");
        const diffRank = Number(diffCapability?.display_rank || 40);
        const insertIndex = viewTypes.findIndex((kind) => {
            const capability = explorer.representationCapabilities.get(kind);
            return Number(capability?.display_rank || 999) > diffRank;
        });
        if (insertIndex >= 0) {
            viewTypes.splice(insertIndex, 0, "diff");
        } else {
            viewTypes.push("diff");
        }
    }
    return viewTypes;
}

export function getViewTypeLabel(explorer, viewType) {
    const capability = explorer.representationCapabilities.get(viewType);
    if (isNonEmptyString(capability?.label)) {
        return capability.label;
    }
    return getDefaultViewTypeLabel(viewType);
}

export function setArtifactStatus(explorer, message, tone = "muted") {
    if (!explorer.artifactStatus) {
        return;
    }
    explorer.artifactStatus.textContent = message;
    explorer.artifactStatus.dataset.tone = tone;
}

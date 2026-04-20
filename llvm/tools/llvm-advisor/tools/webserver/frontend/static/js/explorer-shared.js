// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { Utils } from "./utils.js";

export function isNonEmptyString(value) {
    return typeof value === "string" && value.trim().length > 0;
}

export function safeArray(value) {
    return Array.isArray(value) ? value : [];
}

export function normalizeFileKey(file) {
    return file?.path || file?.name || "";
}

export function normalizeArtifactList(file) {
    return safeArray(file?.available_representations).filter(isNonEmptyString);
}

export function escapeHtml(value) {
    const div = document.createElement("div");
    div.textContent = value ?? "";
    return div.innerHTML;
}

export function createLoadingMarkup(message) {
    return `
        <div class="flex items-center justify-center h-full text-gray-500">
            <div class="text-center">
                <div class="inline-block animate-spin rounded-full h-6 w-6 border-b-2 border-llvm-blue mb-2"></div>
                <p>${escapeHtml(message)}</p>
            </div>
        </div>
    `;
}

export function createCenteredMessageMarkup(
    message,
    colorClass = "text-gray-500",
) {
    return `
        <div class="flex items-center justify-center h-full ${colorClass}">
            <div class="text-center">
                <p>${escapeHtml(message)}</p>
            </div>
        </div>
    `;
}

export function getDefaultViewTypeLabel(viewType) {
    return Utils.capitalize(String(viewType || "").replace(/-/g, " "));
}

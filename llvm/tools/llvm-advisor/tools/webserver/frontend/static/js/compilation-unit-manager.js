// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Compilation Unit Manager
 * Handles compilation unit selection, ordering, metadata tracking,
 * and selector rendering.
 */

const RECENT_UNITS_STORAGE_KEY = "llvm_advisor_recent_units";
const MAX_RECENT_UNITS = 5;
const DEFAULT_SELECTOR_PLACEHOLDER = "Select a compilation unit...";
const EMPTY_SELECTOR_MESSAGE = "No compilation units found";

function isNonEmptyString(value) {
    return typeof value === "string" && value.trim().length > 0;
}

function safeArray(value) {
    return Array.isArray(value) ? value : [];
}

function toTimestampValue(unit) {
    const timestamp =
        unit?.metadata_timestamp || unit?.run_timestamp || unit?.timestamp;
    if (!isNonEmptyString(timestamp)) {
        return null;
    }

    const parsed = new Date(timestamp);
    return Number.isNaN(parsed.getTime()) ? timestamp : parsed.getTime();
}

function sortUnits(units) {
    return [...safeArray(units)].sort((left, right) => {
        const leftTimestamp = toTimestampValue(left);
        const rightTimestamp = toTimestampValue(right);

        if (
            typeof leftTimestamp === "number" &&
            typeof rightTimestamp === "number"
        ) {
            return rightTimestamp - leftTimestamp;
        }

        if (typeof leftTimestamp === "number") {
            return -1;
        }

        if (typeof rightTimestamp === "number") {
            return 1;
        }

        if (
            isNonEmptyString(leftTimestamp) &&
            isNonEmptyString(rightTimestamp)
        ) {
            return String(rightTimestamp).localeCompare(String(leftTimestamp));
        }

        return String(left?.name || "").localeCompare(
            String(right?.name || ""),
        );
    });
}

function cloneUnit(unit) {
    return {
        ...unit,
        representation_kinds: safeArray(unit?.representation_kinds),
        available_runs: safeArray(unit?.available_runs),
        metadata:
            unit?.metadata && typeof unit.metadata === "object"
                ? { ...unit.metadata }
                : {},
    };
}

export class CompilationUnitManager {
    constructor(apiClient, options = {}) {
        this.apiClient = apiClient;
        this.selectorId = options.selectorId || "unit-selector";

        this.selector = null;
        this.units = [];
        this.unitsByName = new Map();
        this.unitMetadata = new Map();
        this.recentUnitNames = this.loadRecentUnits();
        this.currentUnit = null;
        this.onUnitChangeCallback = null;
        this.emptyMessage = EMPTY_SELECTOR_MESSAGE;
    }

    async init(options = {}) {
        this.onUnitChangeCallback = options.onUnitChange || null;
        this.selector = document.getElementById(this.selectorId);

        if (!this.selector) {
            throw new Error("Unit selector element not found");
        }

        this.setupEventListeners();
    }

    setupEventListeners() {
        this.selector?.addEventListener("change", async (event) => {
            const nextUnitName = event.target.value;
            if (!nextUnitName || nextUnitName === this.currentUnit) {
                return;
            }

            await this.selectUnit(nextUnitName);
        });
    }

    async loadUnits() {
        try {
            const response = await this.apiClient.getUnits();
            if (!response?.success) {
                this.renderEmptySelector();
                return false;
            }

            this.updateUnits(response.data?.units || []);
            return true;
        } catch (error) {
            this.renderEmptySelector();
            return false;
        }
    }

    updateUnits(units) {
        const normalizedUnits = sortUnits(units).map((unit) => cloneUnit(unit));

        this.units = normalizedUnits;
        this.unitsByName = new Map(
            normalizedUnits.map((unit) => [unit.name, unit]),
        );
        this.emptyMessage = EMPTY_SELECTOR_MESSAGE;

        this.rebuildMetadata(normalizedUnits);
        this.renderSelector();

        if (this.currentUnit && this.unitsByName.has(this.currentUnit)) {
            this.syncSelectorValue(this.currentUnit);
            return;
        }

        const preferredUnit = this.getPreferredInitialUnit();
        if (preferredUnit) {
            this.syncSelectorValue(preferredUnit.name);
        } else {
            this.currentUnit = null;
            this.syncSelectorValue("");
        }
    }

    rebuildMetadata(units) {
        const previousMetadata = new Map(this.unitMetadata);
        this.unitMetadata = new Map();

        units.forEach((unit, index) => {
            const previous = previousMetadata.get(unit.name) || {};
            this.unitMetadata.set(unit.name, {
                ...unit,
                isRecent: this.isRecentUnit(unit, index),
                lastSelected: previous.lastSelected || null,
            });
        });
    }

    renderSelector() {
        if (!this.selector) {
            return;
        }

        this.selector.innerHTML = "";

        if (this.units.length === 0) {
            this.renderEmptySelector();
            return;
        }

        this.selector.appendChild(
            this.createOption({
                value: "",
                text: DEFAULT_SELECTOR_PLACEHOLDER,
                disabled: true,
                selected: !this.currentUnit,
            }),
        );

        this.units.forEach((unit, index) => {
            const option = this.createOption({
                value: unit.name,
                text: this.buildUnitDisplayText(unit, index),
                selected: unit.name === this.currentUnit,
            });

            option.title = this.buildUnitTooltip(unit.name);
            this.selector.appendChild(option);
        });
    }

    renderEmptySelector() {
        if (!this.selector) {
            return;
        }

        this.selector.innerHTML = "";
        this.selector.appendChild(
            this.createOption({
                value: "",
                text: this.emptyMessage,
                disabled: true,
                selected: true,
            }),
        );
    }

    setEmptyMessage(message) {
        this.emptyMessage =
            isNonEmptyString(message) ? message : EMPTY_SELECTOR_MESSAGE;
        if (this.units.length === 0) {
            this.renderEmptySelector();
        }
    }

    createOption({ value, text, disabled = false, selected = false }) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = text;
        option.disabled = disabled;
        option.selected = selected;
        return option;
    }

    buildUnitDisplayText(unit, index = -1) {
        const parts = [unit.name];

        if (unit.total_files) {
            parts.push(`(${unit.total_files} representations)`);
        }

        const formattedTimestamp = this.formatTimestamp(
            unit.run_timestamp || unit.metadata_timestamp || unit.timestamp,
        );
        if (formattedTimestamp) {
            parts.push(`- ${formattedTimestamp}`);
        }

        if (this.isRecentUnit(unit, index)) {
            parts.push("🔥");
        }

        return parts.join(" ");
    }

    buildUnitTooltip(unitName) {
        const unit = this.unitsByName.get(unitName);
        const metadata = this.unitMetadata.get(unitName);

        if (!unit) {
            return unitName;
        }

        const lines = [`Unit: ${unit.name}`];

        if (unit.total_files) {
            lines.push(`Artifacts: ${unit.total_files}`);
        }

        if (unit.path) {
            lines.push(`Path: ${unit.path}`);
        }

        const formattedTimestamp = this.formatTimestamp(
            unit.run_timestamp || unit.metadata_timestamp || unit.timestamp,
        );
        if (formattedTimestamp) {
            lines.push(`Run: ${formattedTimestamp}`);
        }

        if (safeArray(unit.available_runs).length > 1) {
            lines.push(`Available runs: ${unit.available_runs.length}`);
        }

        if (safeArray(unit.representation_kinds).length > 0) {
            lines.push(`Representations: ${unit.representation_kinds.join(", ")}`);
        }

        if (metadata?.lastSelected) {
            lines.push(
                `Last selected: ${new Date(metadata.lastSelected).toLocaleString()}`,
            );
        }

        return lines.join("\n");
    }

    async selectUnit(unitName) {
        if (!isNonEmptyString(unitName) || unitName === this.currentUnit) {
            return;
        }

        const nextUnit = this.unitsByName.get(unitName);
        if (!nextUnit) {
            return;
        }

        const previousUnit = this.currentUnit;

        try {
            this.currentUnit = unitName;
            this.syncSelectorValue(unitName);
            this.markUnitSelected(unitName);

            if (typeof this.onUnitChangeCallback === "function") {
                await this.onUnitChangeCallback(unitName, previousUnit);
            }

            this.updateRecentUnits(unitName);
            this.renderSelector();
        } catch (error) {
            this.currentUnit = previousUnit;
            this.syncSelectorValue(previousUnit || "");
            throw error;
        }
    }

    syncSelectorValue(unitName) {
        if (this.selector) {
            this.selector.value = unitName || "";
        }
    }

    markUnitSelected(unitName) {
        const metadata = this.unitMetadata.get(unitName);
        if (!metadata) {
            return;
        }

        metadata.lastSelected = new Date().toISOString();
        this.unitMetadata.set(unitName, metadata);
    }

    updateRecentUnits(unitName) {
        this.recentUnitNames = [
            unitName,
            ...this.recentUnitNames.filter((name) => name !== unitName),
        ].slice(0, MAX_RECENT_UNITS);

        try {
            localStorage.setItem(
                RECENT_UNITS_STORAGE_KEY,
                JSON.stringify(this.recentUnitNames),
            );
        } catch (error) {
            // Ignore persistence failures.
        }
    }

    loadRecentUnits() {
        try {
            const rawValue = localStorage.getItem(RECENT_UNITS_STORAGE_KEY);
            const parsed = JSON.parse(rawValue || "[]");
            return safeArray(parsed).filter((name) => isNonEmptyString(name));
        } catch (error) {
            return [];
        }
    }

    getPreferredInitialUnit() {
        for (const unitName of this.recentUnitNames) {
            const unit = this.unitsByName.get(unitName);
            if (unit) {
                return unit;
            }
        }

        return this.units[0] || null;
    }

    isRecentUnit(unit, index = -1) {
        if (!unit?.name) {
            return false;
        }

        if (this.recentUnitNames.includes(unit.name)) {
            return true;
        }

        return index >= 0 && index < 3;
    }

    getCurrentUnit() {
        return this.currentUnit;
    }

    getCurrentUnitInfo() {
        if (!this.currentUnit) {
            return null;
        }

        const unit = this.unitsByName.get(this.currentUnit);
        const metadata = this.unitMetadata.get(this.currentUnit);

        return unit
            ? {
                  ...unit,
                  metadata: metadata || null,
              }
            : null;
    }

    getAllUnits() {
        return this.units.map((unit) => ({
            ...unit,
            metadata: this.unitMetadata.get(unit.name) || null,
        }));
    }

    getRecentUnits() {
        return [...this.recentUnitNames];
    }

    formatTimestamp(timestamp) {
        if (!isNonEmptyString(timestamp)) {
            return "";
        }

        if (/^\d{8}_\d{6}$/.test(timestamp)) {
            try {
                const year = Number.parseInt(timestamp.slice(0, 4), 10);
                const month = Number.parseInt(timestamp.slice(4, 6), 10) - 1;
                const day = Number.parseInt(timestamp.slice(6, 8), 10);
                const hour = Number.parseInt(timestamp.slice(9, 11), 10);
                const minute = Number.parseInt(timestamp.slice(11, 13), 10);
                const second = Number.parseInt(timestamp.slice(13, 15), 10);

                return new Date(
                    year,
                    month,
                    day,
                    hour,
                    minute,
                    second,
                ).toLocaleString();
            } catch (error) {
                return timestamp;
            }
        }

        const date = new Date(timestamp);
        return Number.isNaN(date.getTime()) ? timestamp : date.toLocaleString();
    }
}

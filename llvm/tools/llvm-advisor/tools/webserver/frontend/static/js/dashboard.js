// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Dashboard
 * Main dashboard controller for data binding, chart updates, and
 * insight rendering.
 */

import { ChartComponents } from "./chart-components.js";
import { Utils } from "./utils.js";
import { DashboardCharts } from "./dashboard-charts.js";

function getElement(id) {
    return document.getElementById(id);
}

function getCurrentUnitName(data) {
    return (
        data?.unitDetail?.unit_name ||
        data?.unitDetail?.name ||
        getElement("unit-selector")?.value ||
        null
    );
}

function getNestedValue(object, path, fallback = null) {
    const value = path.reduce(
        (current, key) =>
            current && typeof current === "object" ? current[key] : undefined,
        object,
    );
    return value ?? fallback;
}

function getNumber(value, fallback = 0) {
    const numericValue = Number(value);
    return Number.isFinite(numericValue) ? numericValue : fallback;
}

function getDependencySourceCount(buildDependencies, currentUnitName) {
    if (!buildDependencies || !currentUnitName) {
        return 0;
    }

    const unit = buildDependencies.units?.[currentUnitName];
    if (!unit) {
        return 0;
    }

    const metadataCount = getNumber(unit.summary_stats?.unique_sources);
    if (metadataCount > 0) {
        return metadataCount;
    }

    const fallbackMetadataCount = getNumber(unit.metadata?.unique_sources);
    if (fallbackMetadataCount > 0) {
        return fallbackMetadataCount;
    }

    const files = Array.isArray(unit.files) ? unit.files : [];
    const uniqueSourceFiles = new Set();
    let discoveredCount = 0;

    files.forEach((file) => {
        const fileMetadataCount = getNumber(file?.metadata?.unique_sources);
        if (fileMetadataCount > discoveredCount) {
            discoveredCount = fileMetadataCount;
        }

        const filePath = file?.file_path;
        if (
            typeof filePath === "string" &&
            filePath.includes("/sources/") &&
            !filePath.endsWith("/")
        ) {
            const fileName = filePath.split("/").pop();
            if (fileName && !fileName.startsWith(".")) {
                uniqueSourceFiles.add(fileName);
            }
        }
    });

    return discoveredCount || uniqueSourceFiles.size;
}

export class Dashboard extends DashboardCharts {
    constructor(apiClient) {
        super();

        this.apiClient = apiClient;
        this.chartComponents = new ChartComponents();
        this.charts = {};
        this.currentData = null;
        this.isInitialized = false;
    }

    init() {
        this.chartComponents.init();
        this.isInitialized = true;
    }

    async updateData(data) {
        if (!this.isInitialized) {
            return;
        }

        this.currentData = data || null;

        try {
            this.updateMetricsCards(this.currentData);
            await this.updateCharts(this.currentData);
            this.updateInsights(this.currentData);
            this.clearDashboardError();
        } catch (error) {
            this.showDashboardError("Failed to update dashboard data");
        }
    }

    updateMetricsCards(data) {
        const summary = data?.summary || {};
        const diagnostics = data?.diagnostics || {};
        const buildDependencies = data?.buildDependencies || {};
        const compilationPhasesBindings = data?.compilationPhasesBindings || {};

        const currentUnitName = getCurrentUnitName(data);
        const totalSourceFiles = getDependencySourceCount(
            buildDependencies,
            currentUnitName,
        );
        const successRate = getNumber(summary.success_rate);
        const totalErrors =
            getNumber(summary.errors) ||
            getNumber(diagnostics.by_level?.error) ||
            getNumber(diagnostics.error);
        const totalBindings = getNumber(
            compilationPhasesBindings.summary?.total_bindings,
        );

        this.updateMetricCard(
            "metric-total-files",
            Utils.formatNumber(totalSourceFiles),
        );
        this.updateMetricCard(
            "metric-success-rate",
            `${successRate.toFixed(1)}%`,
        );
        this.updateMetricCard(
            "metric-total-errors",
            Utils.formatNumber(totalErrors),
        );
        this.updateMetricCard(
            "metric-timing-phases",
            Utils.formatNumber(totalBindings),
        );
    }

    updateMetricCard(elementId, value) {
        const element = getElement(elementId);
        if (!element) {
            return;
        }

        element.style.opacity = "0.6";
        window.setTimeout(() => {
            element.textContent = value;
            element.style.opacity = "1";
        }, 150);
    }

    showDashboardError(message) {
        const container = getElement("dashboard-content");
        if (!container) {
            return;
        }

        let errorBanner = getElement("dashboard-error-banner");
        if (!errorBanner) {
            errorBanner = document.createElement("div");
            errorBanner.id = "dashboard-error-banner";
            errorBanner.className =
                "mb-6 rounded-md border border-slate-300 bg-slate-50 px-4 py-3 text-sm text-slate-700";
            container.prepend(errorBanner);
        }

        errorBanner.textContent = message;
        errorBanner.classList.remove("hidden");
    }

    clearDashboardError() {
        const errorBanner = getElement("dashboard-error-banner");
        if (errorBanner) {
            errorBanner.classList.add("hidden");
        }
    }

    clearCharts() {
        Object.values(this.charts).forEach((chart) => {
            if (chart && typeof chart.destroy === "function") {
                chart.destroy();
            }
        });
        this.charts = {};
    }

    async refresh() {
        if (!this.currentData) {
            return;
        }

        await this.updateData(this.currentData);
    }

    getStats() {
        return {
            chartsActive: Object.keys(this.charts).length,
            lastUpdate: this.currentData ? new Date().toISOString() : null,
            isInitialized: this.isInitialized,
            currentUnit: getCurrentUnitName(this.currentData),
            totalFiles: getNestedValue(
                this.currentData,
                ["summary", "total_files"],
                0,
            ),
            totalErrors: getNestedValue(
                this.currentData,
                ["summary", "errors"],
                0,
            ),
        };
    }
}

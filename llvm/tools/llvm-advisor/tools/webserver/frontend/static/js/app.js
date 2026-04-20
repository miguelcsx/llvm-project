// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Main Application Controller
 * Orchestrates dashboard bootstrap, state transitions, and high-level data flow.
 */

import { ApiClient } from "./api-client.js";
import { CompareWorkspace } from "./compare.js";
import { CompilationUnitManager } from "./compilation-unit-manager.js";
import { Dashboard } from "./dashboard.js";
import { Explorer } from "./explorer.js";
import { TabManager } from "./tab-manager.js";

const REFRESH_INTERVAL_MS = 30_000;

const AppState = Object.freeze({
    IDLE: "idle",
    INITIALIZING: "initializing",
    READY: "ready",
    ERROR: "error",
});

function safeText(value, fallback = "") {
    return value === null || value === undefined ? fallback : String(value);
}

function hasCollectedUnits(snapshot) {
    return Number(snapshot?.summary?.unit_count || 0) > 0;
}

function buildEmptySnapshotMessage(snapshot) {
    const metadata =
        snapshot?.metadata && typeof snapshot.metadata === "object"
            ? snapshot.metadata
            : {};

    if (metadata.forwarded_invocation && Number(metadata.unit_count || 0) === 0) {
        return {
            selector: "No collected compilation units",
            banner: 'This run was forwarded without collection. Use a compile invocation such as "llvm-advisor view clang++ -c file.cpp" or build a target with source compilation work.',
        };
    }

    return {
        selector: "No compilation units found",
        banner: "No collected compilation units were found in this snapshot.",
    };
}

function createUnavailableResponse() {
    return { success: false, data: null, error: "Unavailable" };
}

class UIController {
    constructor() {
        this.loadingScreen = document.getElementById("loading-screen");
        this.appRoot = document.getElementById("app");
        this.alertBanner = document.getElementById("alert-banner");
        this.alertMessage = document.getElementById("alert-message");
        this.statusIndicator = document.getElementById("status-indicator");
        this.lastRefreshTime = document.getElementById("last-refresh-time");
        this.snapshotBadge = document.getElementById("snapshot-badge");
        this.profileBadge = document.getElementById("profile-badge");
        this.projectBadge = document.getElementById("project-badge");
        this.storeBadge = document.getElementById("store-badge");
    }

    showLoadingScreen() {
        this.loadingScreen?.classList.remove("hidden");
        this.appRoot?.classList.add("hidden");
    }

    hideLoadingScreen() {
        this.loadingScreen?.classList.add("hidden");
        this.appRoot?.classList.remove("hidden");
    }

    showAlert(message, variant = "error") {
        if (!this.alertBanner || !this.alertMessage) {
            return;
        }

        const palette = {
            error: {
                bannerAdd: ["bg-slate-50", "border-slate-300"],
                bannerRemove: ["bg-blue-50", "border-blue-300"],
                textAdd: ["text-slate-700"],
                textRemove: ["text-blue-800"],
            },
            info: {
                bannerAdd: ["bg-blue-50", "border-blue-300"],
                bannerRemove: ["bg-slate-50", "border-slate-300"],
                textAdd: ["text-blue-800"],
                textRemove: ["text-slate-700"],
            },
        };

        const selectedPalette = palette[variant] || palette.error;

        this.alertMessage.textContent = message;
        this.alertBanner.classList.remove("hidden");
        this.alertBanner.classList.remove(...selectedPalette.bannerRemove);
        this.alertBanner.classList.add(...selectedPalette.bannerAdd);

        const messageElement = this.alertBanner.querySelector("p");
        if (messageElement) {
            messageElement.classList.remove(...selectedPalette.textRemove);
            messageElement.classList.add(...selectedPalette.textAdd);
        }
    }

    hideAlert() {
        this.alertBanner?.classList.add("hidden");
    }

    showError(message) {
        this.showAlert(message, "error");
    }

    showInfo(message) {
        this.showAlert(message, "info");
    }

    updateConnectionStatus(isConnected) {
        if (!this.statusIndicator) {
            return;
        }

        const dot = this.statusIndicator.querySelector("div");
        const text = this.statusIndicator.querySelector("span");
        if (!dot || !text) {
            return;
        }

        if (isConnected) {
            dot.className = "h-2 w-2 bg-success rounded-full";
            text.textContent = "Connected";
            text.className = "text-sm text-gray-600";
            return;
        }

        dot.className = "h-2 w-2 bg-error rounded-full";
        text.textContent = "Disconnected";
        text.className = "text-sm text-gray-600";
    }

    updateLastRefreshTime() {
        if (this.lastRefreshTime) {
            this.lastRefreshTime.textContent = new Date().toLocaleTimeString();
        }
    }

    updateSnapshotContext(snapshot) {
        if (!snapshot) {
            return;
        }

        if (this.snapshotBadge) {
            const shortId = String(snapshot.snapshot_id || "-");
            this.snapshotBadge.textContent =
                shortId.length > 18 ? shortId.slice(0, 18) : shortId;
            this.snapshotBadge.title = shortId;
        }

        if (this.profileBadge) {
            this.profileBadge.textContent = safeText(snapshot.profile, "-");
        }

        if (this.projectBadge) {
            this.projectBadge.textContent = safeText(snapshot.project_id, "-");
            this.projectBadge.title = safeText(snapshot.project_id, "-");
        }

        if (this.storeBadge) {
            const storeLabel = safeText(snapshot.data_dir, "-");
            this.storeBadge.textContent =
                storeLabel.length > 28
                    ? `...${storeLabel.slice(-28)}`
                    : storeLabel;
            this.storeBadge.title = storeLabel;
        }
    }
}

class LLVMAdvisorApp {
    constructor({
        apiClient = new ApiClient(),
        tabManager = new TabManager(),
        dashboard = null,
        explorer = null,
        compareWorkspace = null,
        unitManager = null,
        ui = new UIController(),
    } = {}) {
        this.apiClient = apiClient;
        this.tabManager = tabManager;
        this.dashboard = dashboard || new Dashboard(this.apiClient);
        this.ui = ui;
        this.unitManager =
            unitManager || new CompilationUnitManager(this.apiClient);
        this.explorer = explorer || new Explorer(this.apiClient, this);
        this.compareWorkspace =
            compareWorkspace || new CompareWorkspace(this.apiClient, this);

        this.performanceManager = null;
        this.currentSnapshot = null;
        this.snapshots = [];
        this.currentUnit = null;
        this.appData = null;
        this.state = AppState.IDLE;
        this.refreshTimer = null;
    }

    async init() {
        if (this.state === AppState.INITIALIZING) {
            return;
        }

        this.state = AppState.INITIALIZING;
        this.ui.showLoadingScreen();

        try {
            await this.initializeComponents();
            await this.bootstrapInitialState();
            this.setupGlobalEventListeners();
            this.startAutoRefresh();
            this.ui.hideLoadingScreen();
            this.state = AppState.READY;
        } catch (error) {
            this.state = AppState.ERROR;
            this.ui.updateConnectionStatus(false);
            this.ui.showLoadingScreen();
            this.ui.showError(
                "Failed to initialize dashboard. Please check your connection and try again.",
            );
            throw error;
        }
    }

    async initializeComponents() {
        this.tabManager.init({
            onTabChange: (tabId) => this.handleTabChange(tabId),
        });

        await this.unitManager.init({
            onUnitChange: (unitName) => this.handleUnitChange(unitName),
        });

        this.dashboard.init();
        this.explorer.init();
        this.compareWorkspace.init();
        this.initializePerformanceManager();
    }

    initializePerformanceManager() {
        if (!window.PerformanceManager) {
            return;
        }

        this.performanceManager = new window.PerformanceManager(this.apiClient);
        window.performanceManager = this.performanceManager;
    }

    async bootstrapInitialState() {
        const healthResponse = await this.apiClient.getHealth();
        this.ui.updateConnectionStatus(Boolean(healthResponse.success));

        if (!healthResponse.success) {
            throw new Error(
                healthResponse.error || "API server is not responding",
            );
        }

        const snapshotsResponse = await this.apiClient.getSnapshots();
        if (!snapshotsResponse.success) {
            throw new Error(
                snapshotsResponse.error || "Failed to load snapshots",
            );
        }

        const snapshots = snapshotsResponse.data?.snapshots || [];
        this.snapshots = snapshots;
        if (snapshots.length === 0) {
            this.currentSnapshot = null;
            this.currentUnit = null;
            this.appData = null;
            this.unitManager.setEmptyMessage("No snapshots found");
            this.unitManager.updateUnits([]);
            this.ui.showInfo("No snapshots found.");
            return;
        }

        const snapshotsNewestFirst = [...snapshots].reverse();
        this.currentSnapshot =
            snapshotsNewestFirst.find((snapshot) => hasCollectedUnits(snapshot)) ||
            snapshotsNewestFirst[0];
        this.apiClient.setSnapshot(this.currentSnapshot.snapshot_id);
        this.ui.updateSnapshotContext(this.currentSnapshot);
        this.compareWorkspace.setContext({
            snapshots,
            currentSnapshotId: this.currentSnapshot.snapshot_id,
            currentUnitName: null,
        });

        const snapshotDetailResponse = await this.apiClient.getSnapshot(
            this.currentSnapshot.snapshot_id,
        );
        if (!snapshotDetailResponse.success) {
            throw new Error(
                snapshotDetailResponse.error || "Failed to load snapshot detail",
            );
        }

        const snapshotDetail = snapshotDetailResponse.data || {};
        const units = snapshotDetail.units || [];
        this.unitManager.updateUnits(units);

        if (units.length === 0) {
            this.currentUnit = null;
            this.appData = null;
            const emptyState = buildEmptySnapshotMessage(snapshotDetail);
            this.unitManager.setEmptyMessage(emptyState.selector);
            this.ui.showInfo(emptyState.banner);
            await this.loadDashboardData();
            return;
        }

        const initialUnit = units[0]?.name || null;
        if (!initialUnit) {
            return;
        }

        this.currentUnit = null;
        await this.unitManager.selectUnit(initialUnit);
        await this.preloadSupplementaryData(initialUnit);
        this.ui.updateLastRefreshTime();
    }

    setupGlobalEventListeners() {
        window.addEventListener("error", () => {
            this.ui.showError(
                "An unexpected error occurred. Please refresh the page.",
            );
        });

        window.addEventListener("online", () => {
            this.ui.updateConnectionStatus(true);
            this.ui.hideAlert();
        });

        window.addEventListener("offline", () => {
            this.ui.updateConnectionStatus(false);
            this.ui.showError(
                "You are currently offline. Some features may not work properly.",
            );
        });
    }

    startAutoRefresh() {
        this.stopAutoRefresh();
        this.refreshTimer = window.setInterval(() => {
            void this.refreshCurrentData();
        }, REFRESH_INTERVAL_MS);
    }

    stopAutoRefresh() {
        if (this.refreshTimer !== null) {
            window.clearInterval(this.refreshTimer);
            this.refreshTimer = null;
        }
    }

    async handleTabChange(tabId) {
        try {
            switch (tabId) {
                case "dashboard":
                    if (this.currentUnit && !this.appData) {
                        await this.loadDashboardData();
                    }
                    break;
                case "explorer":
                    await this.explorer.onActivate();
                    break;
                case "compare":
                    await this.compareWorkspace.onActivate();
                    break;
                case "performance":
                    if (this.performanceManager) {
                        await this.performanceManager.initialize();
                    }
                    break;
                default:
                    break;
            }
        } catch (error) {
            this.ui.showError(`Failed to load ${tabId} data.`);
        }
    }

    async handleUnitChange(unitName) {
        if (!unitName || unitName === this.currentUnit) {
            return;
        }

        this.currentUnit = unitName;
        this.appData = null;
        this.ui.updateLastRefreshTime();
        this.compareWorkspace.setContext({
            snapshots: this.snapshots,
            currentSnapshotId: this.currentSnapshot?.snapshot_id || null,
            currentUnitName: unitName,
        });

        const currentTab = this.tabManager.getCurrentTab();
        if (currentTab === "dashboard") {
            await this.loadDashboardData();
        } else if (currentTab === "explorer") {
            await this.explorer.loadAvailableFiles();
        } else if (currentTab === "compare") {
            await this.compareWorkspace.onActivate();
        }

        if (currentTab === "performance" && this.performanceManager) {
            this.performanceManager.onUnitChanged(unitName);
        }

        void this.preloadSupplementaryData(unitName, currentTab);
    }

    async loadDashboardData() {
        if (!this.currentSnapshot?.snapshot_id) {
            return;
        }

        try {
            const dashboardData = await this.loadDashboardState();
            this.dashboard.updateData(dashboardData);
            this.appData = dashboardData;
            this.ui.hideAlert();
        } catch (error) {
            this.ui.showError(
                "Failed to load dashboard data. Some sections may not be available.",
            );
        }
    }

    async loadDashboardState() {
        const queryPayload = {
            snapshot_id: this.currentSnapshot.snapshot_id,
            profile: this.currentSnapshot.profile || "balanced",
            scope: this.currentUnit
                ? { unit_names: [this.currentUnit] }
                : {},
            policy: { include_l2: true, max_cost_class: "expensive" },
        };

        const [
            snapshotResponse,
            queryResponse,
            summaryResponse,
            remarksOverviewResponse,
            remarksPassesResponse,
            diagnosticsOverviewResponse,
            compilationBindingsResponse,
            binarySizeResponse,
            unitDetailResponse,
            sourceFilesResponse,
        ] = await Promise.all([
            this.apiClient.getSnapshot(this.currentSnapshot.snapshot_id),
            this.apiClient.query(queryPayload),
            this.apiClient.getSummary(),
            this.apiClient.getRemarksOverview(),
            this.apiClient.getRemarksPasses(),
            this.apiClient.getDiagnosticsOverview(),
            this.apiClient.getCompilationPhasesBindings(),
            this.apiClient.getBinarySizeOverview(),
            this.currentUnit
                ? this.apiClient.getUnitDetail(this.currentUnit)
                : Promise.resolve(createUnavailableResponse()),
            this.currentUnit
                ? this.apiClient.getSourceFiles(this.currentUnit)
                : Promise.resolve(createUnavailableResponse()),
        ]);

        if (!snapshotResponse.success) {
            throw new Error(snapshotResponse.error || "Failed to load snapshot");
        }
        if (!queryResponse.success) {
            throw new Error(queryResponse.error || "Failed to query capabilities");
        }

        const snapshot = snapshotResponse.data || {};
        const summary = summaryResponse.success ? summaryResponse.data || {} : {};
        const remarksOverview = remarksOverviewResponse.success
            ? remarksOverviewResponse.data || {}
            : {};
        const remarksPasses = remarksPassesResponse.success
            ? remarksPassesResponse.data || {}
            : {};
        const diagnosticsOverview = diagnosticsOverviewResponse.success
            ? diagnosticsOverviewResponse.data || {}
            : {};
        const compilationBindings = compilationBindingsResponse.success
            ? compilationBindingsResponse.data || {}
            : {};
        const binarySize = binarySizeResponse.success
            ? binarySizeResponse.data || {}
            : {};
        const unitDetail = unitDetailResponse.success
            ? unitDetailResponse.data || {}
            : {};
        const sourceFiles = sourceFilesResponse.success
            ? sourceFilesResponse.data?.files || []
            : [];
        const resultEntries = new Map(
            (queryResponse.data?.results || []).map((entry) => [
                entry.capability_id,
                entry.data || {},
            ]),
        );

        const diagnosticsSummary =
            resultEntries.get("clang.diag.summary") || {};
        const astSummary = resultEntries.get("clang.ast.summary") || {};
        const remarksSummary = resultEntries.get("llvm.remarks.summary") || {};
        const passStats = resultEntries.get("llvm.pass.stats") || {};
        const commandSummary = resultEntries.get("build.command.meta") || {};
        const irSummary = resultEntries.get("llvm.ir.summary") || {};
        const objectSummary = resultEntries.get("llvm.obj.summary") || {};
        const debugSummary = resultEntries.get("llvm.debug.summary") || {};
        const totalSourceFiles = sourceFiles.length;
        const normalizedRemarksPasses = Object.fromEntries(
            Object.entries(remarksPasses.passes || {}).map(
                ([passName, passData]) => [
                    passName,
                    {
                        count: passData.remarks_count || 0,
                        unique_functions: passData.unique_functions || 0,
                        unique_files: passData.unique_files || 0,
                        examples: (passData.sample_messages || []).map(
                            (message) => ({ message }),
                        ),
                    },
                ],
            ),
        );

        return {
            summary: {
                total_files:
                    summary.total_files || totalSourceFiles || commandSummary.unit_count || 0,
                success_rate: summary.success_rate || 100,
                errors:
                    summary.errors ||
                    diagnosticsOverview.by_level?.error ||
                    diagnosticsSummary.error_count ||
                    0,
            },
            unitDetail: this.currentUnit
                ? {
                      unit_name: unitDetail.unit_name || this.currentUnit,
                      name: unitDetail.unit_name || this.currentUnit,
                      representation_kinds: unitDetail.representation_kinds || {},
                      summary: unitDetail.summary || {},
                  }
                : { unit_name: null, name: null, representation_kinds: {}, summary: {} },
            diagnostics: {
                totals: diagnosticsOverview.totals || {},
                by_level: diagnosticsOverview.by_level || {
                    error: diagnosticsSummary.error_count || 0,
                    warning: diagnosticsSummary.warning_count || 0,
                    note: diagnosticsSummary.note_count || 0,
                    info: diagnosticsSummary.info_count || 0,
                },
                top_files: diagnosticsOverview.top_files || {},
            },
            remarks: {
                totals:
                    remarksOverview.totals || {
                        remarks: remarksSummary.total || 0,
                        unique_passes: Object.keys(remarksSummary.top_passes || {}).length,
                        unique_functions: Object.keys(remarksSummary.top_functions || {}).length,
                        source_files: Object.keys(remarksSummary.top_files || {}).length,
                    },
                top_passes:
                    remarksOverview.top_passes || remarksSummary.top_passes || {},
                top_functions:
                    remarksOverview.top_functions ||
                    remarksSummary.top_functions ||
                    {},
                top_files:
                    remarksOverview.top_files || remarksSummary.top_files || {},
            },
            remarksPasses: {
                passes: normalizedRemarksPasses,
            },
            binarySize: binarySize,
            compilationPhases: summary,
            compilationPhasesBindings: compilationBindings,
            buildDependencies: {
                units: this.currentUnit
                    ? {
                          [this.currentUnit]: {
                              summary_stats: {
                                  unique_sources: totalSourceFiles,
                              },
                              files: sourceFiles,
                          },
                      }
                    : {},
            },
            versionInfo: {
                snapshot_id: snapshot.snapshot_id || null,
                profile: snapshot.profile || null,
                producer_version: snapshot.producer_version || null,
                source_revision: snapshot.source_revision || null,
                build_config: snapshot.build_config || null,
            },
            capabilities: {
                astSummary,
                passStats,
                objectSummary,
                debugSummary,
                irSummary,
                commandSummary,
            },
        };
    }

    async preloadSupplementaryData(unitName, currentTab = this.tabManager.getCurrentTab()) {
        const tasks = [];

        if (currentTab !== "dashboard" && this.currentSnapshot?.snapshot_id) {
            tasks.push(this.loadDashboardData());
        }

        if (this.explorer && currentTab !== "explorer") {
            tasks.push(this.explorer.loadAvailableFiles());
        }

        if (this.performanceManager) {
            tasks.push(
                currentTab === "performance"
                    ? this.performanceManager.initialize()
                    : this.performanceManager.preload(unitName),
            );
        }

        if (tasks.length > 0) {
            await Promise.allSettled(tasks);
        }
    }

    async resolveDashboardData(requests) {
        const entries = Object.entries(requests);
        const settled = await Promise.allSettled(
            entries.map(([, request]) => request()),
        );

        return entries.reduce((result, [key], index) => {
            result[key] = this.extractSuccessfulData(settled[index]);
            return result;
        }, {});
    }

    extractSuccessfulData(settledResult) {
        if (settledResult.status !== "fulfilled") {
            return null;
        }

        const response = settledResult.value || createUnavailableResponse();
        return response.success ? (response.data ?? null) : null;
    }

    async refreshCurrentData() {
        if (
            this.tabManager.getCurrentTab() !== "dashboard" ||
            !this.currentUnit
        ) {
            return;
        }

        try {
            await this.loadDashboardData();
            this.ui.updateLastRefreshTime();
        } catch (error) {
            // Ignore background refresh failures.
        }
    }

    getCurrentUnitName() {
        return this.currentUnit;
    }

    getCurrentSnapshotId() {
        return this.currentSnapshot?.snapshot_id || null;
    }

    getAppData() {
        return this.appData;
    }

    updateConnectionStatus(isConnected) {
        this.ui.updateConnectionStatus(isConnected);
    }

    showError(message) {
        this.ui.showError(safeText(message));
    }

    hideError() {
        this.ui.hideAlert();
    }

    showInfo(message) {
        this.ui.showInfo(safeText(message));
    }

    destroy() {
        this.stopAutoRefresh();
    }
}

function bootstrapApplication() {
    const app = new LLVMAdvisorApp();
    window.llvmAdvisorApp = app;
    void app.init();
}

document.addEventListener("DOMContentLoaded", bootstrapApplication);

export { LLVMAdvisorApp, AppState, UIController };

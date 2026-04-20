// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import { Utils } from "./utils.js";
import { DashboardInsights } from "./dashboard-insights.js";

export class DashboardCharts extends DashboardInsights {
    async updateCharts(data) {
        const diagnostics = data.diagnostics;
        const compilationPhases = data.compilationPhases;
        const binarySize = data.binarySize;
        const remarks = data.remarks;
        const remarksPasses = data.remarksPasses;
        const versionInfo = data.versionInfo;

        if (remarks || remarksPasses) {
            await this.updateRemarksDistributionChart(remarks, remarksPasses);
        }

        if (diagnostics?.by_level) {
            await this.updateDiagnosticLevelsChart(diagnostics.by_level);
        }

        if (compilationPhases || versionInfo) {
            this.updateCompilationInfoTable(compilationPhases, versionInfo);
        }

        if (binarySize?.section_breakdown) {
            await this.updateBinarySizeChart(binarySize.section_breakdown);
        }
    }

    async updateRemarksDistributionChart(remarksData, remarksPassesData) {
        const canvas = document.getElementById("remarks-distribution-chart");
        if (!canvas) return;

        if (remarksPassesData && remarksPassesData.passes) {
            const passesArray = Object.entries(remarksPassesData.passes)
                .sort(([, a], [, b]) => (b.count || 0) - (a.count || 0))
                .slice(0, 8);

            if (passesArray.length > 0) {
                const labels = passesArray.map(([name]) =>
                    name.length > 20 ? name.substring(0, 20) + "..." : name,
                );
                const counts = passesArray.map(([, data]) => data.count || 0);

                const colors = [
                    "#3b82f6",
                    "#1e40af",
                    "#1d4ed8",
                    "#2563eb",
                    "#60a5fa",
                    "#93c5fd",
                    "#dbeafe",
                    "#eff6ff",
                ];

                const config = {
                    type: "doughnut",
                    data: {
                        labels,
                        datasets: [
                            {
                                data: counts,
                                backgroundColor: colors.slice(0, labels.length),
                                borderWidth: 2,
                                borderColor: "#ffffff",
                            },
                        ],
                    },
                    options: {
                        responsive: true,
                        maintainAspectRatio: false,
                        plugins: {
                            legend: {
                                position: "bottom",
                                labels: {
                                    padding: 20,
                                    usePointStyle: true,
                                    font: { size: 11 },
                                },
                            },
                            tooltip: {
                                callbacks: {
                                    label: (context) => {
                                        const label = context.label;
                                        const value = context.parsed;
                                        const total =
                                            context.dataset.data.reduce(
                                                (a, b) => a + b,
                                                0,
                                            );
                                        const percentage = (
                                            (value / total) *
                                            100
                                        ).toFixed(1);
                                        return `${label}: ${value} remarks (${percentage}%)`;
                                    },
                                },
                            },
                        },
                    },
                };

                this.charts = this.charts || {};
                if (this.charts.remarksDistribution) {
                    this.charts.remarksDistribution.destroy();
                }

                this.charts.remarksDistribution = new Chart(canvas, config);
                return;
            }
        }

        this.showPlaceholderChart(
            "remarks-distribution-chart",
            "No Remarks Data Available",
        );
    }

    async updateDiagnosticLevelsChart(diagnosticData) {
        const canvas = document.getElementById("diagnostic-levels-chart");
        if (!canvas) return;

        const levels = ["error", "warning", "note", "info"];
        const colors = {
            error: "#ef4444",
            warning: "#f59e0b",
            note: "#3b82f6",
            info: "#10b981",
        };

        const data = levels.map((level) => {
            if (diagnosticData.by_level && diagnosticData.by_level[level]) {
                return diagnosticData.by_level[level];
            }
            return diagnosticData[level] || 0;
        });

        const config = {
            type: "bar",
            data: {
                labels: levels.map((level) => Utils.capitalize(level)),
                datasets: [
                    {
                        label: "Diagnostics",
                        data,
                        backgroundColor: levels.map((level) => colors[level]),
                        borderRadius: 4,
                        borderWidth: 0,
                    },
                ],
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                plugins: {
                    legend: { display: false },
                    tooltip: {
                        callbacks: {
                            label: (context) => {
                                return `${context.label}: ${context.parsed.y} issues`;
                            },
                        },
                    },
                },
                scales: {
                    y: { beginAtZero: true, ticks: { precision: 0 } },
                    x: { grid: { display: false } },
                },
            },
        };

        this.charts = this.charts || {};
        if (this.charts.diagnosticLevels) {
            this.charts.diagnosticLevels.destroy();
        }

        this.charts.diagnosticLevels = new Chart(canvas, config);
    }

    updateCompilationInfoTable(compilationData, versionInfo) {
        const container = document.getElementById("compilation-info-table");
        if (!container) return;

        if (!compilationData && !versionInfo) {
            container.innerHTML = `
                <div class="text-center py-4 text-gray-500">
                    <svg class="mx-auto h-8 w-8 mb-2" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z"></path>
                    </svg>
                    No compilation information available
                </div>
            `;
            return;
        }

        const rows = [];
        const currentUnitName =
            this.currentData?.unitDetail?.name ||
            document.getElementById("unit-selector")?.value;

        const addRow = (label, value) => {
            if (value === undefined || value === null || value === "") return;
            rows.push({ label, value });
        };

        if (versionInfo && typeof versionInfo === "object") {
            Object.entries(versionInfo).forEach(([key, value]) => {
                if (typeof value === "object") {
                    addRow(Utils.capitalize(key), JSON.stringify(value));
                } else {
                    addRow(Utils.capitalize(key), value);
                }
            });
        }

        if (compilationData && typeof compilationData === "object") {
            if (compilationData.totals) {
                Object.entries(compilationData.totals).forEach(([key, value]) =>
                    addRow(`Total ${Utils.capitalize(key)}`, value),
                );
            }

            if (compilationData.units && currentUnitName) {
                const unitInfo = compilationData.units[currentUnitName];
                if (unitInfo?.files?.length) {
                    addRow("Compilation Files", unitInfo.files.length);
                }
            }
        }

        if (rows.length === 0) {
            container.innerHTML = `
                <div class="text-center py-4 text-gray-500">
                    No compilation information available
                </div>
            `;
            return;
        }

        container.innerHTML = `
            <div class="overflow-x-auto">
                <table class="min-w-full text-sm">
                    <tbody class="divide-y divide-gray-200">
                        ${rows
                            .map(
                                (row) => `
                            <tr>
                                <td class="py-2 pr-4 text-gray-600 font-medium">${row.label}</td>
                                <td class="py-2 text-gray-900">${row.value}</td>
                            </tr>
                        `,
                            )
                            .join("")}
                    </tbody>
                </table>
            </div>
        `;
    }

    async updateBinarySizeChart(sizeData) {
        const canvas = document.getElementById("binary-size-chart");
        if (!canvas) return;

        if (!sizeData || Object.keys(sizeData).length === 0) {
            this.showPlaceholderChart(
                "binary-size-chart",
                "No Binary Size Data",
            );
            return;
        }

        const sections = Object.entries(sizeData)
            .filter(([, size]) => size > 0)
            .sort(([, a], [, b]) => b - a)
            .slice(0, 8);

        if (sections.length === 0) {
            this.showPlaceholderChart(
                "binary-size-chart",
                "No Binary Size Data",
            );
            return;
        }

        const labels = sections.map(([name]) =>
            Utils.formatSectionName ? Utils.formatSectionName(name) : name,
        );
        const sizes = sections.map(([, size]) => size);

        const colors = this.chartComponents?.generateColors
            ? this.chartComponents.generateColors(labels.length, "blue")
            : [
                  "#3b82f6",
                  "#1e40af",
                  "#1d4ed8",
                  "#2563eb",
                  "#60a5fa",
                  "#93c5fd",
                  "#dbeafe",
                  "#eff6ff",
              ];

        const config = {
            type: "pie",
            data: {
                labels,
                datasets: [
                    {
                        data: sizes,
                        backgroundColor: colors.slice(0, labels.length),
                        borderWidth: 2,
                        borderColor: "#ffffff",
                    },
                ],
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                plugins: {
                    legend: {
                        position: "bottom",
                        labels: { padding: 20, usePointStyle: true },
                    },
                    tooltip: {
                        callbacks: {
                            label: (context) => {
                                const label = context.label;
                                const value = context.parsed;
                                const total = context.dataset.data.reduce(
                                    (a, b) => a + b,
                                    0,
                                );
                                const percentage = (
                                    (value / total) *
                                    100
                                ).toFixed(1);
                                const formattedSize = this.formatBytes
                                    ? this.formatBytes(value)
                                    : `${value} bytes`;
                                return `${label}: ${formattedSize} (${percentage}%)`;
                            },
                        },
                    },
                },
            },
        };

        this.charts = this.charts || {};
        if (this.charts.binarySize) {
            this.charts.binarySize.destroy();
        }

        this.charts.binarySize = new Chart(canvas, config);
    }

    showPlaceholderChart(canvasId, message) {
        const canvas = document.getElementById(canvasId);
        if (!canvas) return;

        const chartKey = canvasId
            .replace("-chart", "")
            .replace(/-([a-z])/g, (g) => g[1].toUpperCase());

        const config = {
            type: "doughnut",
            data: {
                labels: [message],
                datasets: [
                    { data: [1], backgroundColor: ["#f3f4f6"], borderWidth: 0 },
                ],
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                plugins: {
                    legend: { display: false },
                    tooltip: { enabled: false },
                },
            },
        };

        this.charts = this.charts || {};
        if (this.charts[chartKey]) {
            this.charts[chartKey].destroy();
        }

        this.charts[chartKey] = new Chart(canvas, config);
    }

    formatBytes(bytes) {
        if (bytes === 0) return "0 B";
        const k = 1024;
        const sizes = ["B", "KB", "MB", "GB"];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + " " + sizes[i];
    }
}

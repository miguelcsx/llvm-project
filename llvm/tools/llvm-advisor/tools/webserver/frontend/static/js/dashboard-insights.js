// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

export class DashboardInsights {
    /**
     * Update insights and recommendations
     */
    updateInsights(data) {
        this.updateRemarksSummary(data);
        this.updateOptimizationPasses(data);
    }

    /**
     * Update remarks summary
     */
    updateRemarksSummary(data) {
        const container = document.getElementById("remarks-summary-list");
        if (!container) return;

        const remarks = data.remarks;
        container.innerHTML = "";

        if (!remarks || !remarks.totals) {
            container.innerHTML = `
                <div class="text-center py-4 text-gray-500">
                    <svg class="mx-auto h-8 w-8 mb-2" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"></path>
                    </svg>
                    No optimization remarks available
                </div>
            `;
            return;
        }

        const summaryItems = [
            {
                title: "Total Optimization Remarks",
                value: remarks.totals.remarks || 0,
                description: "Total number of optimization opportunities found",
                icon: "M13 10V3L4 14h7v7l9-11h-7z",
                color: "stack-list-item",
            },
            {
                title: "Unique Optimization Passes",
                value: remarks.totals.unique_passes || 0,
                description:
                    "Number of different optimization passes that generated remarks",
                icon: "M9 3v2m6-2v2M9 19v2m6-2v2M5 9H3m2 6H3m18-6h-2m2 6h-2M7 19h10a2 2 0 002-2V7a2 2 0 00-2-2H7a2 2 0 00-2 2v10a2 2 0 002 2zM9 9h6v6H9V9z",
                color: "stack-list-item",
            },
            {
                title: "Functions with Remarks",
                value: remarks.totals.unique_functions || 0,
                description: "Functions that have optimization remarks",
                icon: "M7 21a4 4 0 01-4-4V5a2 2 0 012-2h4a2 2 0 012 2v12a4 4 0 01-4 4zM21 5a2 2 0 012 2v12a4 4 0 01-4 4h-4a2 2 0 01-2-2V5a2 2 0 012-2h4z",
                color: "stack-list-item",
            },
        ];

        summaryItems.forEach((item) => {
            const itemElement = document.createElement("div");
            itemElement.className = `flex items-start space-x-3 p-4 ${item.color}`;

            itemElement.innerHTML = `
                <div class="flex-shrink-0">
                    <svg class="h-5 w-5 text-gray-600" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="${item.icon}"></path>
                    </svg>
                </div>
                <div class="flex-1">
                    <h4 class="text-sm font-medium text-gray-900">${item.title}</h4>
                    <p class="text-sm text-gray-600 mt-1">${item.description}</p>
                    <span class="soft-badge px-2 py-1 text-xs font-medium mt-2">${item.value}</span>
                </div>
            `;

            container.appendChild(itemElement);
        });
    }

    /**
     * Update optimization passes
     */
    updateOptimizationPasses(data) {
        const container = document.getElementById("optimization-passes-list");
        if (!container) return;

        const passesData = data.remarksPasses;
        container.innerHTML = "";

        if (!passesData || !passesData.passes) {
            container.innerHTML = `
                <div class="text-center py-4 text-gray-500">
                    <svg class="mx-auto h-8 w-8 mb-2" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 3v2m6-2v2M9 19v2m6-2v2M5 9H3m2 6H3m18-6h-2m2 6h-2M7 19h10a2 2 0 002-2V7a2 2 0 00-2-2H7a2 2 0 00-2 2v10a2 2 0 002 2zM9 9h6v6H9V9z"></path>
                    </svg>
                    No optimization passes data available
                </div>
            `;
            return;
        }

        // Get top optimization passes by remark count
        const topPasses = Object.entries(passesData.passes)
            .sort(([, a], [, b]) => (b.count || 0) - (a.count || 0))
            .slice(0, 5); // Top 5 passes

        topPasses.forEach(([passName, passInfo]) => {
            const passElement = document.createElement("div");
            passElement.className =
                "flex items-start space-x-3 p-4 stack-list-item";

            const description =
                passInfo.examples && passInfo.examples.length > 0
                    ? passInfo.examples[0].message ||
                      "Optimization pass execution"
                    : "Optimization pass with multiple improvements";

            passElement.innerHTML = `
                <div class="flex-shrink-0">
                    <svg class="h-5 w-5 text-gray-500" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 3v2m6-2v2M9 19v2m6-2v2M5 9H3m2 6H3m18-6h-2m2 6h-2M7 19h10a2 2 0 002-2V7a2 2 0 00-2-2H7a2 2 0 00-2 2v10a2 2 0 002 2zM9 9h6v6H9V9z"></path>
                    </svg>
                </div>
                <div class="flex-1">
                    <h4 class="text-sm font-medium text-gray-900">${passName
                        .replace(/-/g, " ")
                        .replace(/\b\w/g, (l) => l.toUpperCase())}</h4>
                    <p class="text-sm text-gray-600 mt-1">${
                        description.length > 80
                            ? description.substring(0, 80) + "..."
                            : description
                    }</p>
                    <div class="flex items-center space-x-4 mt-2">
                        <span class="soft-badge px-2 py-1 text-xs font-medium">${
                            passInfo.count || 0
                        } remarks</span>
                        <span class="soft-badge px-2 py-1 text-xs font-medium">${
                            passInfo.unique_functions || 0
                        } functions</span>
                    </div>
                </div>
            `;

            container.appendChild(passElement);
        });
    }

    /**
     * Generate top issues from data
     */
    generateTopIssues(data) {
        const issues = [];

        // Check for compilation errors
        if (data.summary?.errors > 0) {
            issues.push({
                severity: "error",
                title: "Compilation Errors",
                description: `${data.summary.errors} files failed to parse correctly`,
                count: data.summary.errors,
            });
        }

        // Check for high error rate in diagnostics
        if (data.diagnostics?.by_level?.error > 10) {
            issues.push({
                severity: "error",
                title: "High Error Count",
                description: "Large number of compilation errors detected",
                count: data.diagnostics.by_level.error,
            });
        }

        // Check for many warnings
        if (data.diagnostics?.by_level?.warning > 50) {
            issues.push({
                severity: "warning",
                title: "Many Warnings",
                description:
                    "High number of compiler warnings that should be addressed",
                count: data.diagnostics.by_level.warning,
            });
        }

        return issues.slice(0, 5); // Return top 5 issues
    }

    /**
     * Generate optimization recommendations
     */
    generateOptimizationRecommendations(data) {
        const recommendations = [];

        // Optimization remarks suggestions
        if (data.remarks?.totals?.remarks > 0) {
            recommendations.push({
                title: "Review Optimization Remarks",
                description: `${data.remarks.totals.remarks} optimization opportunities found in your code`,
                impact: "Potential performance improvement",
            });
        }

        // Binary size optimization
        if (data.binarySize?.size_statistics?.total_size > 10 * 1024 * 1024) {
            // > 10MB
            recommendations.push({
                title: "Consider Binary Size Optimization",
                description:
                    "Binary size is large, consider link-time optimization or unused code removal",
                impact: "Reduce binary size",
            });
        }

        // Warning cleanup
        if (data.diagnostics?.by_level?.warning > 20) {
            recommendations.push({
                title: "Clean Up Warnings",
                description:
                    "Addressing compiler warnings can improve code quality and catch potential bugs",
                impact: "Better code quality",
            });
        }

        return recommendations.slice(0, 4); // Return top 4 recommendations
    }

    /**
     * Get CSS class for issue severity
     */
    getIssueColorClass(severity) {
        switch (severity) {
            case "error":
                return "stack-list-item";
            case "warning":
                return "stack-list-item";
            case "info":
                return "stack-list-item";
            default:
                return "stack-list-item";
        }
    }

    /**
     * Get icon for issue severity
     */
    getIssueIcon(severity) {
        const iconClass =
            severity === "error"
                ? "text-red-500"
                : severity === "warning"
                  ? "text-yellow-500"
                : "text-gray-500";

        const iconPath =
            severity === "error"
                ? "M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L3.732 16.5c-.77.833.192 2.5 1.732 2.5z"
                : "M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z";

        return `
            <svg class="h-5 w-5 ${iconClass}" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="${iconPath}"></path>
            </svg>
        `;
    }
}

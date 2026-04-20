// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

export class PerformanceManager {
    constructor(apiClient = window.llvmAdvisorApp?.apiClient || null) {
        this.apiClient = apiClient;
        this.currentViewType = "time-order";
        this.currentUnit = null;
        this.timeTraceData = null;
        this.runtimeTraceData = null;
        this.traceBundles = {
            "time-trace": null,
            "runtime-trace": null,
        };
        this.loadedSnapshotId = null;
        this.loadedUnitKey = null;
        this.listenersInitialized = false;

        this.viewports = {
            timeTrace: { offsetX: 0, scaleX: 1, offsetY: 0, scaleY: 1 },
            runtimeTrace: { offsetX: 0, scaleX: 1, offsetY: 0, scaleY: 1 },
        };

        this.isDragging = false;
        this.lastMousePos = { x: 0, y: 0 };
        this.isZooming = false;

        this.searchQuery = "";
        this.searchResults = { timeTrace: [], runtimeTrace: [] };
        this.sandwichFunctions = [];

        this.FRAME_HEIGHT = 18;
        this.MINIMAP_HEIGHT = 50;
        this.MIN_FRAME_WIDTH_FOR_TEXT = 25;
        this.PADDING = 4;
        this.MIN_ZOOM = 0.1;
        this.MAX_ZOOM = 100;

        this.initializeEventListeners();
    }

    initializeEventListeners() {
        if (this.listenersInitialized) {
            return;
        }
        this.listenersInitialized = true;

        const viewTypeSelector = document.getElementById(
            "view-type-selector-perf",
        );
        if (viewTypeSelector) {
            viewTypeSelector.addEventListener("change", (e) => {
                this.currentViewType = e.target.value;
                this.syncActiveTraceData();
                this.renderBothViews();
                this.updateIndividualStats();
            });
        }

        const refreshBtn = document.getElementById("refresh-performance-btn");
        if (refreshBtn) {
            refreshBtn.addEventListener("click", () => {
                this.loadAllPerformanceData();
            });
        }

        this.initializeSearch();
    }

    initializeSearch() {
        const controlsDiv = document.querySelector(
            "#performance-content .performance-controls",
        );
        if (controlsDiv && !document.getElementById("perf-search")) {
            const searchDiv = document.createElement("div");
            searchDiv.className = "toolbar-group";
            searchDiv.innerHTML = `
                <label for="perf-search" class="toolbar-label">Search:</label>
                <input id="perf-search" type="text" placeholder="Function name..."
                       class="toolbar-select" style="min-width: 12rem;">
                <span id="search-results-count" class="text-sm text-gray-500"></span>
            `;
            controlsDiv.appendChild(searchDiv);

            const searchInput = document.getElementById("perf-search");
            searchInput.addEventListener("input", (e) => {
                this.searchQuery = e.target.value.toLowerCase();
                this.performSearch();
            });
        }
    }

    async initialize() {
        this.currentUnit =
            this.currentUnit ||
            document.getElementById("unit-selector")?.value ||
            null;
        await this.loadAllPerformanceData({ render: true });
    }

    onUnitChanged(unitName) {
        this.currentUnit = unitName;
        void this.loadAllPerformanceData({ render: true, force: true });
    }

    async preload(unitName = null) {
        if (unitName !== null) {
            this.currentUnit = unitName;
        }

        await this.loadAllPerformanceData({ render: false });
    }

    async loadAllPerformanceData({ render = true, force = false } = {}) {
        try {
            const snapshotId = window.llvmAdvisorApp?.getCurrentSnapshotId?.();
            if (!snapshotId) {
                return;
            }

            const unitKey = this.currentUnit || "__all__";
            const hasCachedData =
                this.traceBundles["time-trace"] &&
                this.traceBundles["runtime-trace"] &&
                this.loadedSnapshotId === snapshotId &&
                this.loadedUnitKey === unitKey;

            if (!force && hasCachedData) {
                this.syncActiveTraceData();
                if (render) {
                    this.resetViewports();
                    this.renderBothViews();
                    this.updateIndividualStats();
                }
                return;
            }

            const [timeTraceBundle, runtimeTraceBundle] = await Promise.all([
                this.loadTraceBundle("time-trace"),
                this.loadTraceBundle("runtime-trace"),
            ]);

            this.traceBundles["time-trace"] = timeTraceBundle;
            this.traceBundles["runtime-trace"] = runtimeTraceBundle;
            this.loadedSnapshotId = snapshotId;
            this.loadedUnitKey = unitKey;
            this.syncActiveTraceData();

            this.resetViewports();

            if (render) {
                this.renderBothViews();
                this.updateIndividualStats();
            }
        } catch (error) {
            this.showError(`Failed to load performance data: ${error.message}`);
        }
    }

    async loadTraceBundle(traceType) {
        const query = this.currentUnit ? { unit: this.currentUnit } : {};
        const endpoints = [
            ["flamegraph", traceType, "flamegraph", query],
            ["sandwich", traceType, "sandwich", query],
            ["overview", traceType, "overview", query],
            ["hotspots", traceType, "hotspots", query],
            ["categories", traceType, "categories", query],
            ["parallelism", traceType, "parallelism", query],
        ];

        const settledResults = await Promise.allSettled(
            endpoints.map(([, analysisType, subEndpoint, params]) =>
                this.apiClient?.getAnalysisData(analysisType, subEndpoint, params),
            ),
        );

        return endpoints.reduce((bundle, [key], index) => {
            bundle[key] = this.normalizeTraceResponse(
                settledResults[index],
                traceType,
            );
            return bundle;
        }, {});
    }

    normalizeTraceResponse(result, traceType) {
        if (result.status !== "fulfilled") {
            return null;
        }

        const response = result.value;
        if (!response?.success || !response.data) {
            return null;
        }

        const data = response.data;
        if (Array.isArray(data.samples)) {
            data.samples.forEach((sample) => {
                sample.source = traceType;
            });
        }
        if (Array.isArray(data.functions)) {
            data.functions.forEach((func) => {
                func.source = traceType;
            });
        }

        return data;
    }

    syncActiveTraceData() {
        const bundleKey =
            this.currentViewType === "sandwich" ? "sandwich" : "flamegraph";
        this.timeTraceData = this.traceBundles["time-trace"]?.[bundleKey] || null;
        this.runtimeTraceData =
            this.traceBundles["runtime-trace"]?.[bundleKey] || null;
    }

    renderBothViews() {
        switch (this.currentViewType) {
            case "time-order":
                this.renderTimeOrderViews();
                break;
            case "sandwich":
                this.renderSandwichViews();
                break;
        }
    }

    renderTimeOrderViews() {
        const container = document.getElementById("performance-visualization");
        container.innerHTML = `
            <div class="space-y-6">
                <div class="trace-shell">
                    <div class="trace-header">
                        <h3 class="trace-title text-lg font-semibold">
                            <svg class="w-5 h-5 text-gray-500" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z"/>
                            </svg>
                            Compilation Time Trace
                        </h3>
                        <div class="trace-meta text-sm font-normal">
                            <span id="time-trace-stats" class="trace-stat">Loading...</span>
                            <span id="time-trace-debug" class="trace-stat" title="Debug info">?</span>
                        </div>
                    </div>
                    <div id="time-trace-container" class="relative">
                        ${this.createInteractiveCanvasContainer("time-trace")}
                    </div>
                </div>

                <div class="trace-shell">
                    <div class="trace-header">
                        <h3 class="trace-title text-lg font-semibold">
                            <svg class="w-5 h-5 text-gray-500" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"/>
                            </svg>
                            Runtime Offloading Trace
                        </h3>
                        <div class="trace-meta text-sm font-normal">
                            <span id="runtime-trace-stats" class="trace-stat">Loading...</span>
                            <span id="runtime-trace-debug" class="trace-stat" title="Debug info">?</span>
                        </div>
                    </div>
                    <div id="runtime-trace-container" class="relative">
                        ${this.createInteractiveCanvasContainer("runtime-trace")}
                    </div>
                </div>
            </div>
        `;

        this.setupInteractiveCanvases();
    }

    redrawTimeOrderViews() {
        ["time-trace", "runtime-trace"].forEach((traceType) => {
            const mainCanvas = document.getElementById(`${traceType}-main`);
            const minimapCanvas = document.getElementById(
                `${traceType}-minimap`,
            );
            if (!mainCanvas || !minimapCanvas) return;

            const mainCtx = mainCanvas.getContext("2d");
            const minimapCtx = minimapCanvas.getContext("2d");
            if (!mainCtx || !minimapCtx) return;

            mainCtx.setTransform(1, 0, 0, 1, 0, 0);
            mainCtx.scale(window.devicePixelRatio, window.devicePixelRatio);
            minimapCtx.setTransform(1, 0, 0, 1, 0, 0);
            minimapCtx.scale(window.devicePixelRatio, window.devicePixelRatio);

            const data =
                traceType === "time-trace"
                    ? this.timeTraceData
                    : this.runtimeTraceData;
            this.renderSingleTimeOrder(
                traceType,
                data,
                mainCtx,
                minimapCtx,
                mainCanvas,
                minimapCanvas,
            );
        });
    }

    createInteractiveCanvasContainer(traceType) {
        return `
            <div class="w-full relative" style="min-height: 450px;">
                <div class="absolute top-2 right-2 flex items-center space-x-2 z-10">
                    <button id="${traceType}-reset-btn" class="px-2 py-1 text-xs surface-note" title="Reset zoom (Double-click frame to fit)">
                        Reset
                    </button>
                    <span id="${traceType}-zoom-level" class="trace-stat">100%</span>
                </div>

                <canvas id="${traceType}-minimap" width="800" height="${this.MINIMAP_HEIGHT}"
                        class="trace-canvas w-full border-b border-gray-200 cursor-pointer" title="Click and drag to navigate"></canvas>

                <canvas id="${traceType}-main" width="800" height="350"
                        class="trace-canvas w-full cursor-grab bg-white" title="Drag to pan, scroll to zoom, double-click frame to fit"></canvas>

                <div id="${traceType}-tooltip" class="absolute pointer-events-none bg-gray-900 text-white px-3 py-2 rounded-lg text-sm z-20 hidden shadow-lg">
                    <div class="arrow-down absolute -bottom-1 left-1/2 transform -translate-x-1/2 w-0 h-0 border-l-4 border-r-4 border-t-4 border-transparent border-t-gray-900"></div>
                </div>

                <div id="${traceType}-status" class="absolute bottom-2 left-2 trace-stat">
                    Loading...
                </div>

                <div class="absolute bottom-2 right-2 trace-stat" title="Keyboard: +/- to zoom, Arrow keys to pan, Escape to reset">
                    ? Keys
                </div>
            </div>
        `;
    }

    setupInteractiveCanvases() {
        ["time-trace", "runtime-trace"].forEach((traceType) => {
            this.setupSingleInteractiveCanvas(traceType);
        });

        document.addEventListener("keydown", this.handleKeyDown.bind(this));
        document.addEventListener("keyup", this.handleKeyUp.bind(this));
    }

    setupSingleInteractiveCanvas(traceType) {
        const mainCanvas = document.getElementById(`${traceType}-main`);
        const minimapCanvas = document.getElementById(`${traceType}-minimap`);
        const resetBtn = document.getElementById(`${traceType}-reset-btn`);

        if (!mainCanvas || !minimapCanvas) return;

        const container = mainCanvas.parentElement;
        const containerWidth = container.clientWidth;

        mainCanvas.width = containerWidth * window.devicePixelRatio;
        mainCanvas.height = 350 * window.devicePixelRatio;
        mainCanvas.style.width = `${containerWidth}px`;
        mainCanvas.style.height = "350px";

        const mainCtx = mainCanvas.getContext("2d");
        mainCtx.scale(window.devicePixelRatio, window.devicePixelRatio);

        minimapCanvas.width = containerWidth * window.devicePixelRatio;
        minimapCanvas.height = this.MINIMAP_HEIGHT * window.devicePixelRatio;
        minimapCanvas.style.width = `${containerWidth}px`;
        minimapCanvas.style.height = `${this.MINIMAP_HEIGHT}px`;

        const minimapCtx = minimapCanvas.getContext("2d");
        minimapCtx.scale(window.devicePixelRatio, window.devicePixelRatio);

        this.addCanvasInteractions(mainCanvas, minimapCanvas, traceType);

        if (resetBtn) {
            resetBtn.addEventListener("click", () => {
                this.resetViewport(traceType);
            });
        }

        const data =
            traceType === "time-trace"
                ? this.timeTraceData
                : this.runtimeTraceData;
        this.renderSingleTimeOrder(
            traceType,
            data,
            mainCtx,
            minimapCtx,
            mainCanvas,
            minimapCanvas,
        );
    }

    renderSingleTimeOrder(
        traceType,
        data,
        mainCtx,
        minimapCtx,
        mainCanvas,
        minimapCanvas,
    ) {
        const statusEl = document.getElementById(`${traceType}-status`);

        if (!data || !data.samples || data.samples.length === 0) {
            statusEl.textContent = "No data available";
            statusEl.className = statusEl.className.replace(
                "text-gray-500",
                "text-red-500",
            );

            const canvasWidth = mainCanvas.width / window.devicePixelRatio;
            const canvasHeight = mainCanvas.height / window.devicePixelRatio;

            mainCtx.clearRect(0, 0, canvasWidth, canvasHeight);
            mainCtx.fillStyle = "#f3f4f6";
            mainCtx.fillRect(0, 0, canvasWidth, canvasHeight);

            mainCtx.fillStyle = "#6b7280";
            mainCtx.font = "14px system-ui";
            mainCtx.textAlign = "center";
            mainCtx.fillText(
                "No trace data available",
                canvasWidth / 2,
                canvasHeight / 2,
            );

            minimapCtx.clearRect(
                0,
                0,
                minimapCanvas.width / window.devicePixelRatio,
                this.MINIMAP_HEIGHT,
            );
            return;
        }

        const key = traceType === "time-trace" ? "timeTrace" : "runtimeTrace";
        const viewport = this.viewports[key];

        statusEl.textContent = `${data.samples.length} events • Zoom: ${Math.round(viewport.scaleX * 100)}%`;
        statusEl.className = statusEl.className.replace(
            "text-red-500",
            "text-gray-500",
        );

        this.renderFlamechart(data.samples, mainCtx, mainCanvas, traceType);
        this.renderMinimap(data.samples, minimapCtx, minimapCanvas, traceType);
        this.updateZoomDisplay(traceType);
    }

    renderFlamechart(samples, ctx, canvas, traceType) {
        const canvasWidth = canvas.width / window.devicePixelRatio;
        const canvasHeight = canvas.height / window.devicePixelRatio;

        ctx.clearRect(0, 0, canvasWidth, canvasHeight);

        if (!samples || samples.length === 0) return;

        const key = traceType === "time-trace" ? "timeTrace" : "runtimeTrace";
        const viewport = this.viewports[key];

        const times = samples
            .map((s) => [s.timestamp, s.timestamp + s.duration])
            .flat();
        const minTime = Math.min(...times);
        const maxTime = Math.max(...times);
        const totalDuration = maxTime - minTime;

        if (totalDuration === 0) return;

        const layers = this.buildNonOverlappingLayers(samples);

        const viewportLeft = viewport.offsetX * totalDuration;
        const viewportWidth = totalDuration / viewport.scaleX;
        const visibleTimeStart = minTime + viewportLeft;
        const visibleTimeEnd = visibleTimeStart + viewportWidth;

        const timeScale = canvasWidth / viewportWidth;
        const maxVisibleLayers = Math.floor(canvasHeight / this.FRAME_HEIGHT);

        const layerOffset = Math.floor(viewport.offsetY * layers.length);
        const visibleLayers = layers.slice(
            layerOffset,
            layerOffset + maxVisibleLayers,
        );

        visibleLayers.forEach((layer, layerIndex) => {
            const y = layerIndex * this.FRAME_HEIGHT;

            layer.forEach((sample) => {
                const sampleStart = sample.timestamp;
                const sampleEnd = sample.timestamp + sample.duration;

                if (
                    sampleEnd >= visibleTimeStart &&
                    sampleStart <= visibleTimeEnd
                ) {
                    const x = (sampleStart - visibleTimeStart) * timeScale;
                    const width = Math.max(1, sample.duration * timeScale);

                    if (x < canvasWidth && x + width > 0) {
                        this.drawFrame(
                            ctx,
                            sample,
                            x,
                            y,
                            width,
                            this.FRAME_HEIGHT,
                        );
                    }
                }
            });
        });

        this.renderTimeAxis(
            ctx,
            visibleTimeStart,
            viewportWidth,
            canvasWidth,
            canvasHeight,
        );

        canvas.dataset.visibleTimeStart = visibleTimeStart;
        canvas.dataset.visibleTimeEnd = visibleTimeEnd;
        canvas.dataset.timeScale = timeScale;
        canvas.dataset.totalDuration = totalDuration;
        canvas.dataset.minTime = minTime;
        canvas.dataset.layers = layers.length.toString();
    }

    buildNonOverlappingLayers(samples) {
        const layers = [];
        const sortedSamples = [...samples].sort(
            (a, b) => a.timestamp - b.timestamp,
        );

        for (const sample of sortedSamples) {
            let placed = false;

            for (let i = 0; i < layers.length; i++) {
                const layer = layers[i];
                const lastInLayer = layer[layer.length - 1];

                if (
                    !lastInLayer ||
                    lastInLayer.timestamp + lastInLayer.duration <=
                        sample.timestamp
                ) {
                    layer.push(sample);
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                layers.push([sample]);
            }
        }

        return layers;
    }

    drawFrame(ctx, sample, x, y, width, height) {
        const isSearchMatch =
            this.searchQuery &&
            sample.name.toLowerCase().includes(this.searchQuery);

        let color = this.getCategoryColor(sample.category);
        if (this.searchQuery && !isSearchMatch) {
            color = this.fadeColor(color);
        }

        ctx.fillStyle = color;
        ctx.fillRect(x, y, width, height);

        ctx.strokeStyle = "rgba(255, 255, 255, 0.3)";
        ctx.lineWidth = 0.5;
        ctx.strokeRect(x, y, width, height);

        if (isSearchMatch) {
            ctx.strokeStyle = "#fbbf24";
            ctx.lineWidth = 2;
            ctx.strokeRect(x, y, width, height);
        }

        if (width > this.MIN_FRAME_WIDTH_FOR_TEXT) {
            ctx.fillStyle = this.getTextColor(color);
            ctx.font = "11px system-ui";
            ctx.textAlign = "left";
            ctx.textBaseline = "middle";

            const text = this.truncateText(
                sample.name,
                width - 2 * this.PADDING,
            );
            ctx.fillText(text, x + this.PADDING, y + height / 2);
        }
    }

    renderTimeAxis(ctx, minTime, totalDuration, canvasWidth, canvasHeight) {
        if (totalDuration === 0) return;

        const targetInterval = totalDuration / 8;
        const magnitude = Math.pow(10, Math.floor(Math.log10(targetInterval)));
        let interval = magnitude;

        if (targetInterval / interval > 5) interval *= 5;
        else if (targetInterval / interval > 2) interval *= 2;

        ctx.strokeStyle = "#e5e7eb";
        ctx.lineWidth = 1;
        ctx.fillStyle = "#6b7280";
        ctx.font = "10px system-ui";
        ctx.textAlign = "center";

        const timeScale = canvasWidth / totalDuration;

        for (
            let time = Math.ceil(minTime / interval) * interval;
            time <= minTime + totalDuration;
            time += interval
        ) {
            const x = (time - minTime) * timeScale;

            if (x >= 0 && x <= canvasWidth) {
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, canvasHeight - 20);
                ctx.stroke();

                const timeMs = (time / 1000).toFixed(1);
                ctx.fillText(`${timeMs}ms`, x, canvasHeight - 5);
            }
        }
    }

    renderMinimap(samples, ctx, canvas) {
        const canvasWidth = canvas.width / window.devicePixelRatio;
        const canvasHeight = canvas.height / window.devicePixelRatio;

        ctx.clearRect(0, 0, canvasWidth, canvasHeight);

        if (!samples || samples.length === 0) return;

        const times = samples
            .map((s) => [s.timestamp, s.timestamp + s.duration])
            .flat();
        const minTime = Math.min(...times);
        const maxTime = Math.max(...times);
        const totalDuration = maxTime - minTime;

        if (totalDuration === 0) return;

        const timeScale = canvasWidth / totalDuration;
        const barHeight = canvasHeight - 10;

        samples.forEach((sample) => {
            const x = (sample.timestamp - minTime) * timeScale;
            const width = Math.max(1, sample.duration * timeScale);
            const y = 5;

            ctx.fillStyle = this.getCategoryColor(sample.category);
            ctx.fillRect(x, y, width, barHeight);
        });
    }

    addCanvasInteractions(mainCanvas, minimapCanvas, traceType) {
        const key = traceType === "time-trace" ? "timeTrace" : "runtimeTrace";

        mainCanvas.addEventListener("mousedown", (e) => {
            this.isDragging = true;
            this.lastMousePos = { x: e.offsetX, y: e.offsetY };
        });

        mainCanvas.addEventListener("mousemove", (e) => {
            if (!this.isDragging) return;

            const viewport = this.viewports[key];
            const dx = e.offsetX - this.lastMousePos.x;
            const dy = e.offsetY - this.lastMousePos.y;

            const totalDuration = parseFloat(
                mainCanvas.dataset.totalDuration || "0",
            );
            if (totalDuration > 0) {
                const deltaX =
                    dx /
                    (mainCanvas.width / window.devicePixelRatio) /
                    viewport.scaleX;
                viewport.offsetX = this.clamp(
                    viewport.offsetX - deltaX,
                    0,
                    1 - 1 / viewport.scaleX,
                );
            }

            const totalLayers = parseInt(mainCanvas.dataset.layers || "1", 10);
            if (totalLayers > 0) {
                const deltaY =
                    dy /
                    (mainCanvas.height / window.devicePixelRatio) /
                    totalLayers;
                viewport.offsetY = this.clamp(viewport.offsetY - deltaY, 0, 1);
            }

            this.lastMousePos = { x: e.offsetX, y: e.offsetY };
            this.redrawTimeOrderViews();
        });

        mainCanvas.addEventListener("mouseup", () => {
            this.isDragging = false;
        });

        mainCanvas.addEventListener("mouseleave", () => {
            this.isDragging = false;
        });

        mainCanvas.addEventListener("wheel", (e) => {
            e.preventDefault();
            const viewport = this.viewports[key];
            const zoomFactor = e.deltaY > 0 ? 0.9 : 1.1;
            const newScale = this.clamp(
                viewport.scaleX * zoomFactor,
                this.MIN_ZOOM,
                this.MAX_ZOOM,
            );

            const canvasWidth = mainCanvas.width / window.devicePixelRatio;
            const mouseRatio = e.offsetX / canvasWidth;
            const center = viewport.offsetX + mouseRatio / viewport.scaleX;
            viewport.scaleX = newScale;
            viewport.offsetX = this.clamp(
                center - mouseRatio / newScale,
                0,
                1 - 1 / newScale,
            );

            this.redrawTimeOrderViews();
        });

        mainCanvas.addEventListener("dblclick", () => {
            this.resetViewport(traceType);
        });

        minimapCanvas.addEventListener("click", (e) => {
            const viewport = this.viewports[key];
            const canvasWidth = minimapCanvas.width / window.devicePixelRatio;
            const clickRatio = e.offsetX / canvasWidth;
            viewport.offsetX = this.clamp(
                clickRatio - 0.5 / viewport.scaleX,
                0,
                1 - 1 / viewport.scaleX,
            );
            this.redrawTimeOrderViews();
        });
    }

    resetViewports() {
        this.viewports.timeTrace = {
            offsetX: 0,
            scaleX: 1,
            offsetY: 0,
            scaleY: 1,
        };
        this.viewports.runtimeTrace = {
            offsetX: 0,
            scaleX: 1,
            offsetY: 0,
            scaleY: 1,
        };
    }

    resetViewport(traceType) {
        if (traceType === "time-trace") {
            this.viewports.timeTrace = {
                offsetX: 0,
                scaleX: 1,
                offsetY: 0,
                scaleY: 1,
            };
        } else {
            this.viewports.runtimeTrace = {
                offsetX: 0,
                scaleX: 1,
                offsetY: 0,
                scaleY: 1,
            };
        }
        this.redrawTimeOrderViews();
    }

    updateZoomDisplay(traceType) {
        const key = traceType === "time-trace" ? "timeTrace" : "runtimeTrace";
        const viewport = this.viewports[key];
        const zoomEl = document.getElementById(`${traceType}-zoom-level`);
        if (zoomEl) {
            zoomEl.textContent = `${Math.round(viewport.scaleX * 100)}%`;
        }
    }

    handleKeyDown(event) {
        if (this.isZooming) return;

        const step = 0.05;
        const zoomStep = 0.1;

        if (event.key === "+") {
            this.zoomAll(1 + zoomStep);
        } else if (event.key === "-") {
            this.zoomAll(1 - zoomStep);
        } else if (event.key === "ArrowLeft") {
            this.panAll(-step, 0);
        } else if (event.key === "ArrowRight") {
            this.panAll(step, 0);
        } else if (event.key === "ArrowUp") {
            this.panAll(0, -step);
        } else if (event.key === "ArrowDown") {
            this.panAll(0, step);
        } else if (event.key === "Escape") {
            this.resetViewports();
            this.redrawTimeOrderViews();
        }
    }

    handleKeyUp() {
        this.isZooming = false;
    }

    zoomAll(factor) {
        ["timeTrace", "runtimeTrace"].forEach((key) => {
            const viewport = this.viewports[key];
            viewport.scaleX = this.clamp(
                viewport.scaleX * factor,
                this.MIN_ZOOM,
                this.MAX_ZOOM,
            );
            viewport.offsetX = this.clamp(
                viewport.offsetX,
                0,
                1 - 1 / viewport.scaleX,
            );
        });
        this.redrawTimeOrderViews();
    }

    panAll(dx, dy) {
        ["timeTrace", "runtimeTrace"].forEach((key) => {
            const viewport = this.viewports[key];
            viewport.offsetX = this.clamp(
                viewport.offsetX + dx,
                0,
                1 - 1 / viewport.scaleX,
            );
            viewport.offsetY = this.clamp(viewport.offsetY + dy, 0, 1);
        });
        this.redrawTimeOrderViews();
    }

    renderSandwichViews() {
        const container = document.getElementById("performance-visualization");
        container.innerHTML = `
            <div class="performance-sandwich-layout flex h-full">
                <div class="performance-sandwich-list flex flex-col">
                    <div class="bg-gray-50 px-4 py-3 border-b border-gray-200">
                        <div class="flex items-center justify-between">
                            <h3 class="text-sm font-semibold text-gray-900">Functions</h3>
                            <div class="flex items-center space-x-3">
                                <select id="sandwich-sort" class="text-xs border border-gray-300 rounded px-2 py-1 bg-white">
                                    <option value="total">Total Time</option>
                                    <option value="self">Self Time</option>
                                    <option value="name">Name</option>
                                </select>
                                <span id="sandwich-count" class="text-xs text-gray-600">0 functions</span>
                            </div>
                        </div>
                    </div>
                    <div class="flex-1 overflow-hidden">
                        <div class="h-full overflow-y-auto">
                            <table class="min-w-full text-xs">
                                <thead class="bg-gray-50 sticky top-0">
                                    <tr class="border-b border-gray-200">
                                        <th class="px-3 py-2 text-left font-medium text-gray-700 cursor-pointer" data-sort="total">
                                            Total
                                        </th>
                                        <th class="px-3 py-2 text-left font-medium text-gray-700 cursor-pointer" data-sort="self">
                                            Self
                                        </th>
                                        <th class="px-3 py-2 text-left font-medium text-gray-700 cursor-pointer" data-sort="name">
                                            Function
                                        </th>
                                    </tr>
                                </thead>
                                <tbody id="sandwich-table-body" class="divide-y divide-gray-100">
                                </tbody>
                            </table>
                        </div>
                    </div>
                </div>

                <div class="performance-sandwich-detail flex-1 flex flex-col">
                    <div class="bg-gray-50 px-4 py-3 border-b border-gray-200">
                        <h3 class="text-sm font-semibold text-gray-900" id="sandwich-detail-title">Select a function to view details</h3>
                    </div>
                    <div class="flex-1 flex flex-col">
                        <div class="flex-1 border-b border-gray-200">
                            <div class="h-8 bg-gray-100 flex items-center px-4 border-b border-gray-200">
                                <span class="text-xs font-medium text-gray-700">Callers (functions that call this)</span>
                            </div>
                            <div id="callers-chart" class="h-full bg-gray-50 flex items-center justify-center text-gray-500 text-sm">
                                Select a function to see its callers
                            </div>
                        </div>

                        <div class="flex-1">
                            <div class="h-8 bg-gray-100 flex items-center px-4 border-b border-gray-200">
                                <span class="text-xs font-medium text-gray-700">Callees (functions called by this)</span>
                            </div>
                            <div id="callees-chart" class="h-full bg-gray-50 flex items-center justify-center text-gray-500 text-sm">
                                Select a function to see its callees
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        `;

        this.renderSpeedscopeSandwich();
    }

    buildMergedFunctions() {
        const allFunctions = [];

        if (this.timeTraceData && this.timeTraceData.functions) {
            this.timeTraceData.functions.forEach((func) => {
                allFunctions.push({
                    ...func,
                    source: "compilation",
                    color: this.getCategoryColor(func.category),
                });
            });
        }

        if (this.runtimeTraceData && this.runtimeTraceData.functions) {
            this.runtimeTraceData.functions.forEach((func) => {
                allFunctions.push({
                    ...func,
                    source: "runtime",
                    color: this.getCategoryColor(func.category),
                });
            });
        }

        const functionMap = new Map();
        allFunctions.forEach((func) => {
            const key = func.name;
            if (functionMap.has(key)) {
                const existing = functionMap.get(key);
                existing.total_time += func.total_time;
                existing.call_count += func.call_count;
                existing.sources = existing.sources || [existing.source];
                if (!existing.sources.includes(func.source)) {
                    existing.sources.push(func.source);
                }
            } else {
                functionMap.set(key, {
                    ...func,
                    self_time: func.total_time,
                    sources: [func.source],
                });
            }
        });

        return Array.from(functionMap.values());
    }

    renderSpeedscopeSandwich() {
        this.sandwichFunctions = this.buildMergedFunctions();
        this.sandwichFunctions.sort((a, b) => b.total_time - a.total_time);

        const totalTime = this.sandwichFunctions.reduce(
            (sum, f) => sum + f.total_time,
            0,
        );

        this.renderFunctionTable(this.sandwichFunctions, totalTime);
        this.setupSandwichInteractions(this.sandwichFunctions);

        const countEl = document.getElementById("sandwich-count");
        if (countEl) {
            countEl.textContent = `${this.sandwichFunctions.length} functions`;
        }
    }

    renderFunctionTable(functions, totalTime) {
        const tbody = document.getElementById("sandwich-table-body");
        if (!tbody) return;

        const filtered = this.searchQuery
            ? functions.filter((f) =>
                  f.name.toLowerCase().includes(this.searchQuery),
              )
            : functions;

        tbody.innerHTML = filtered
            .map((func, index) => {
                const totalPerc =
                    totalTime > 0 ? (func.total_time / totalTime) * 100 : 0;
                const selfPerc =
                    totalTime > 0 ? (func.self_time / totalTime) * 100 : 0;
                const isMatch =
                    this.searchQuery &&
                    func.name.toLowerCase().includes(this.searchQuery);

                return `
                <tr class="hover:bg-gray-50 cursor-pointer sandwich-row ${
                    isMatch ? "bg-yellow-50" : ""
                }" data-index="${index}">
                    <td class="px-3 py-2">
                        <div class="flex items-center">
                            <div class="w-full bg-gray-200 rounded-full h-1 mr-2" style="width: 60px;">
                                <div class="h-1 rounded-full" style="width: ${totalPerc.toFixed(
                                    1,
                                )}%; background-color: ${func.color};"></div>
                            </div>
                            <span class="text-xs font-mono font-medium">${(
                                func.total_time / 1000
                            ).toFixed(2)}ms</span>
                            <span class="text-xs text-gray-500 ml-1">(${totalPerc.toFixed(
                                1,
                            )}%)</span>
                        </div>
                    </td>
                    <td class="px-3 py-2">
                        <div class="flex items-center">
                            <div class="w-full bg-gray-200 rounded-full h-1 mr-2" style="width: 60px;">
                                <div class="h-1 rounded-full" style="width: ${selfPerc.toFixed(
                                    1,
                                )}%; background-color: ${func.color};"></div>
                            </div>
                            <span class="text-xs font-mono font-medium">${(
                                func.self_time / 1000
                            ).toFixed(2)}ms</span>
                            <span class="text-xs text-gray-500 ml-1">(${selfPerc.toFixed(
                                1,
                            )}%)</span>
                        </div>
                    </td>
                    <td class="px-3 py-2">
                        <div class="flex items-center">
                            <div class="w-2 h-2 rounded mr-2" style="background-color: ${func.color};"></div>
                            <div class="flex-1 min-w-0">
                                <div class="text-xs font-medium text-gray-900 truncate" title="${this.escapeHtml(
                                    func.name,
                                )}">
                                    ${this.highlightSearchInText(func.name)}
                                </div>
                                <div class="text-xs text-gray-500">
                                    ${func.call_count.toLocaleString()} calls
                                    ${func.sources ? " • " + func.sources.join(", ") : ""}
                                </div>
                            </div>
                        </div>
                    </td>
                </tr>
            `;
            })
            .join("");
    }

    setupSandwichInteractions(functions) {
        const rows = document.querySelectorAll(".sandwich-row");
        rows.forEach((row) => {
            row.addEventListener("click", () => {
                document
                    .querySelectorAll(".sandwich-row")
                    .forEach((r) => r.classList.remove("bg-slate-50"));

                row.classList.add("bg-slate-50");

                const index = parseInt(row.dataset.index, 10);
                const selectedFunction = functions[index];

                this.showFunctionDetails(selectedFunction);
            });
        });

        const sortSelect = document.getElementById("sandwich-sort");
        if (sortSelect) {
            sortSelect.addEventListener("change", () => {
                this.sortFunctions(functions, sortSelect.value);
            });
        }

        const headers = document.querySelectorAll("th[data-sort]");
        headers.forEach((header) => {
            header.addEventListener("click", () => {
                const sortBy = header.dataset.sort;
                this.sortFunctions(functions, sortBy);
            });
        });
    }

    sortFunctions(functions, sortBy) {
        switch (sortBy) {
            case "total":
                functions.sort((a, b) => b.total_time - a.total_time);
                break;
            case "self":
                functions.sort((a, b) => b.self_time - a.self_time);
                break;
            case "name":
                functions.sort((a, b) => a.name.localeCompare(b.name));
                break;
        }

        const totalTime = functions.reduce((sum, f) => sum + f.total_time, 0);
        this.renderFunctionTable(functions, totalTime);
        this.setupSandwichInteractions(functions);
    }

    showFunctionDetails(func) {
        const titleEl = document.getElementById("sandwich-detail-title");
        const callersEl = document.getElementById("callers-chart");
        const calleesEl = document.getElementById("callees-chart");

        if (titleEl) {
            titleEl.innerHTML = `
                <div class="flex items-center">
                    <div class="w-3 h-3 rounded mr-2" style="background-color: ${func.color};"></div>
                    <span class="font-semibold">${this.escapeHtml(func.name)}</span>
                    <span class="ml-2 text-xs text-gray-600">${(
                        func.total_time / 1000
                    ).toFixed(2)}ms total</span>
                </div>
            `;
        }

        if (callersEl) {
            callersEl.innerHTML = `
                <div class="p-4 text-center">
                    <div class="text-sm text-gray-600 mb-2">Callers for <strong>${this.escapeHtml(
                        func.name,
                    )}</strong></div>
                    <div class="text-xs text-gray-500">Callers graph not yet implemented.</div>
                    <div class="mt-2 text-xs">
                        <div class="bg-gray-200 rounded p-2">
                            Total calls: ${func.call_count.toLocaleString()}<br>
                            Sources: ${func.sources ? func.sources.join(", ") : "unknown"}
                        </div>
                    </div>
                </div>
            `;
        }

        if (calleesEl) {
            calleesEl.innerHTML = `
                <div class="p-4 text-center">
                    <div class="text-sm text-gray-600 mb-2">Callees for <strong>${this.escapeHtml(
                        func.name,
                    )}</strong></div>
                    <div class="text-xs text-gray-500">Callees graph not yet implemented.</div>
                    <div class="mt-2 text-xs">
                        <div class="bg-gray-200 rounded p-2">
                            Self time: ${(func.self_time / 1000).toFixed(
                                2,
                            )}ms<br>
                            Category: ${func.category || "Unknown"}
                        </div>
                    </div>
                </div>
            `;
        }
    }

    performSearch() {
        if (this.currentViewType === "sandwich") {
            this.renderSandwichTables();
        } else {
            this.redrawTimeOrderViews();
        }
        this.updateSearchResultsDisplay();
    }

    renderSandwichTables() {
        if (!this.sandwichFunctions || this.sandwichFunctions.length === 0) {
            this.sandwichFunctions = this.buildMergedFunctions();
        }
        const totalTime = this.sandwichFunctions.reduce(
            (sum, f) => sum + f.total_time,
            0,
        );
        this.renderFunctionTable(this.sandwichFunctions, totalTime);
        this.setupSandwichInteractions(this.sandwichFunctions);
    }

    updateSearchResultsDisplay() {
        const countEl = document.getElementById("search-results-count");
        if (!countEl) return;

        let totalResults = 0;

        if (this.timeTraceData && this.timeTraceData.functions) {
            totalResults += this.timeTraceData.functions.filter((f) =>
                this.searchQuery
                    ? f.name.toLowerCase().includes(this.searchQuery)
                    : true,
            ).length;
        }

        if (this.runtimeTraceData && this.runtimeTraceData.functions) {
            totalResults += this.runtimeTraceData.functions.filter((f) =>
                this.searchQuery
                    ? f.name.toLowerCase().includes(this.searchQuery)
                    : true,
            ).length;
        }

        countEl.textContent = this.searchQuery ? `${totalResults} results` : "";
    }

    updateIndividualStats() {
        this.updateTraceStats("time-trace", this.timeTraceData);
        this.updateTraceStats("runtime-trace", this.runtimeTraceData);

        const totalEventsEl = document.getElementById("perf-total-events");
        const totalDurationEl = document.getElementById("perf-total-duration");
        const avgDurationEl = document.getElementById("perf-avg-duration");
        const viewModeEl = document.getElementById("perf-view-mode");

        if (totalEventsEl || totalDurationEl || avgDurationEl || viewModeEl) {
            let totalEvents = 0;
            let totalDuration = 0;

            ["time-trace", "runtime-trace"].forEach((traceType) => {
                const overview = this.traceBundles[traceType]?.overview;
                const activeData = this.traceBundles[traceType]?.[
                    this.currentViewType === "sandwich"
                        ? "sandwich"
                        : "flamegraph"
                ];

                if (overview?.totals) {
                    totalEvents += Number(
                        overview.totals.total_events ||
                            overview.totals.events ||
                            0,
                    );
                    totalDuration += Number(
                        overview.totals.total_duration ||
                            overview.totals.duration ||
                            0,
                    );
                    return;
                }

                if (activeData?.samples) {
                    totalEvents += activeData.samples.length;
                    totalDuration += activeData.samples.reduce(
                        (sum, sample) => sum + (sample.duration || 0),
                        0,
                    );
                    return;
                }

                if (activeData?.functions) {
                    totalEvents += activeData.functions.reduce(
                        (sum, func) => sum + (func.call_count || 0),
                        0,
                    );
                    totalDuration += activeData.functions.reduce(
                        (sum, func) => sum + (func.total_time || 0),
                        0,
                    );
                }
            });

            if (totalEventsEl)
                totalEventsEl.textContent = totalEvents.toLocaleString();
            if (totalDurationEl)
                totalDurationEl.textContent = `${(totalDuration / 1000).toFixed(
                    2,
                )} ms`;
            if (avgDurationEl) {
                const avg = totalEvents > 0 ? totalDuration / totalEvents : 0;
                avgDurationEl.textContent = `${(avg / 1000).toFixed(2)} ms`;
            }
            if (viewModeEl) {
                viewModeEl.textContent =
                    this.currentViewType.charAt(0).toUpperCase() +
                    this.currentViewType.slice(1);
            }
            const zoomLevelEl = document.getElementById("perf-zoom-level");
            if (zoomLevelEl) {
                zoomLevelEl.textContent = `${this.viewports.timeTrace.scaleX.toFixed(
                    1,
                )}x`;
            }
        }
    }

    updateTraceStats(traceType, data) {
        const statsEl = document.getElementById(`${traceType}-stats`);
        if (!statsEl) return;

        if (!data) {
            statsEl.textContent = "No data";
            return;
        }

        let events = 0;
        let duration = 0;
        let sources = new Set();

        if (data.samples) {
            events = data.samples.length;
            duration = data.samples.reduce(
                (sum, s) => sum + (s.duration || 0),
                0,
            );
            data.samples.forEach((s) => {
                if (s.source) sources.add(s.source);
            });
        } else if (data.functions) {
            events = data.functions.reduce((sum, f) => sum + f.call_count, 0);
            duration = data.functions.reduce((sum, f) => sum + f.total_time, 0);
            data.functions.forEach((f) => {
                if (f.source) sources.add(f.source);
            });
        }

        const sourceInfo =
            sources.size > 0 ? ` • ${Array.from(sources).join(", ")}` : "";
        statsEl.textContent = `${events.toLocaleString()} events • ${(duration / 1000).toFixed(2)}ms${sourceInfo}`;

        const debugEl = document.getElementById(`${traceType}-debug`);
        if (debugEl) {
            const sourceList = Array.from(sources);
            const debugInfo =
                sourceList.length > 0 ? sourceList.join(",") : "unknown";
            debugEl.textContent = debugInfo;
            debugEl.title = `Data source: ${debugInfo}\nSamples: ${
                data.samples ? data.samples.length : 0
            }\nFunctions: ${data.functions ? data.functions.length : 0}`;
        }

        if (traceType === "runtime-trace" && window.timeTraceDataHash) {
            const currentHash = this.hashData(data);
            if (currentHash === window.timeTraceDataHash) {
                if (debugEl) {
                    debugEl.style.backgroundColor = "#ef4444";
                    debugEl.style.color = "white";
                    debugEl.textContent = "SAME";
                    debugEl.title =
                        "WARNING: This data appears identical to time-trace data";
                }
            }
        } else if (traceType === "time-trace") {
            window.timeTraceDataHash = this.hashData(data);
        }
    }

    showError(message) {
        const container = document.getElementById("performance-visualization");
        if (container) {
            container.innerHTML = `
                <div class="p-6 text-center text-red-600">${this.escapeHtml(
                    message,
                )}</div>
            `;
        }
    }

    getCategoryColor(category) {
        const colors = {
            frontend: "#3b82f6",
            backend: "#10b981",
            codegen: "#f59e0b",
            optimization: "#8b5cf6",
            linking: "#ef4444",
            parsing: "#06b6d4",
            analysis: "#6366f1",
        };
        if (category && colors[category]) return colors[category];
        const hash = this.hashString(category || "default");
        const palette = [
            "#3b82f6",
            "#10b981",
            "#f59e0b",
            "#ef4444",
            "#8b5cf6",
            "#06b6d4",
            "#84cc16",
            "#f97316",
            "#ec4899",
            "#6366f1",
        ];
        return palette[hash % palette.length];
    }

    getTextColor(hexColor) {
        const c = hexColor.replace("#", "");
        const r = parseInt(c.substr(0, 2), 16);
        const g = parseInt(c.substr(2, 2), 16);
        const b = parseInt(c.substr(4, 2), 16);
        const luminance = (0.299 * r + 0.587 * g + 0.114 * b) / 255;
        return luminance > 0.5 ? "#000000" : "#ffffff";
    }

    fadeColor(hexColor) {
        const c = hexColor.replace("#", "");
        const r = parseInt(c.substr(0, 2), 16);
        const g = parseInt(c.substr(2, 2), 16);
        const b = parseInt(c.substr(4, 2), 16);
        return `rgba(${r}, ${g}, ${b}, 0.3)`;
    }

    truncateText(text, maxWidthPx) {
        if (!text) return "";
        const approxCharWidth = 7;
        const maxChars = Math.max(1, Math.floor(maxWidthPx / approxCharWidth));
        if (text.length <= maxChars) return text;
        return text.slice(0, Math.max(1, maxChars - 3)) + "...";
    }

    highlightSearchInText(text) {
        if (!this.searchQuery) return this.escapeHtml(text);
        const escaped = this.escapeHtml(text);
        const regex = new RegExp(`(${this.searchQuery})`, "gi");
        return escaped.replace(regex, `<mark class="bg-yellow-200">$1</mark>`);
    }

    hashData(data) {
        try {
            return this.hashString(JSON.stringify(data));
        } catch (error) {
            return 0;
        }
    }

    hashString(str) {
        let hash = 0;
        if (!str || str.length === 0) return hash;
        for (let i = 0; i < str.length; i++) {
            const char = str.charCodeAt(i);
            hash = (hash << 5) - hash + char;
            hash = hash & hash;
        }
        return Math.abs(hash);
    }

    escapeHtml(text) {
        const div = document.createElement("div");
        div.textContent = text;
        return div.innerHTML;
    }

    clamp(value, min, max) {
        return Math.max(min, Math.min(max, value));
    }
}

window.PerformanceManager = PerformanceManager;

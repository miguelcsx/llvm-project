// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * API Client
 * Handles communication with the LLVM Advisor API backend with
 * consistent request processing, caching, and response normalization.
 */

const DEFAULT_CACHE_TTL_MS = 5 * 60 * 1000;
const DEFAULT_HEADERS = Object.freeze({
    Accept: "application/json",
});

const CACHEABLE_METHODS = new Set(["GET"]);

function isPlainObject(value) {
    return value !== null && typeof value === "object" && !Array.isArray(value);
}

function normalizeBaseUrl(baseUrl = "") {
    return String(baseUrl).replace(/\/+$/, "");
}

function normalizeMethod(method) {
    return String(method || "GET").toUpperCase();
}

function buildQueryString(params = {}) {
    const searchParams = new URLSearchParams();

    Object.entries(params).forEach(([key, value]) => {
        if (value === undefined || value === null || value === "") {
            return;
        }

        if (Array.isArray(value)) {
            value.forEach((item) => {
                if (item !== undefined && item !== null && item !== "") {
                    searchParams.append(key, String(item));
                }
            });
            return;
        }

        searchParams.append(key, String(value));
    });

    const query = searchParams.toString();
    return query ? `?${query}` : "";
}

function stableStringify(value) {
    if (Array.isArray(value)) {
        return `[${value.map((item) => stableStringify(item)).join(",")}]`;
    }

    if (isPlainObject(value)) {
        const entries = Object.keys(value)
            .sort()
            .map(
                (key) =>
                    `${JSON.stringify(key)}:${stableStringify(value[key])}`,
            );
        return `{${entries.join(",")}}`;
    }

    return JSON.stringify(value);
}

function cloneResponseData(data) {
    if (typeof structuredClone === "function") {
        return structuredClone(data);
    }

    return JSON.parse(JSON.stringify(data));
}

function createErrorResponse(error, status = 500) {
    return {
        success: false,
        error: error instanceof Error ? error.message : String(error),
        status,
        data: null,
    };
}

export class ApiClient {
    constructor(options = {}) {
        const normalizedOptions = isPlainObject(options)
            ? options
            : { baseUrl: options };

        this.baseUrl = normalizeBaseUrl(normalizedOptions.baseUrl || "");
        this.cacheTtlMs =
            normalizedOptions.cacheTimeout ?? DEFAULT_CACHE_TTL_MS;
        this.cache = new Map();
        this.snapshotId = normalizedOptions.snapshotId || null;
    }

    async request(endpoint, options = {}) {
        const requestConfig = this._buildRequestConfig(endpoint, options);
        const cacheKey = this._buildCacheKey(requestConfig);

        if (this._isCacheable(requestConfig.method)) {
            const cachedResponse = this._getCachedResponse(cacheKey);
            if (cachedResponse) {
                return cachedResponse;
            }
        }

        try {
            const response = await fetch(
                requestConfig.url,
                requestConfig.fetchOptions,
            );
            const normalizedResponse = await this._normalizeResponse(response);

            if (
                this._isCacheable(requestConfig.method) &&
                normalizedResponse.success
            ) {
                this._setCachedResponse(cacheKey, normalizedResponse);
            }

            return normalizedResponse;
        } catch (error) {
            return createErrorResponse(error);
        }
    }

    clearCache() {
        this.cache.clear();
    }

    invalidateCache(predicate = null) {
        if (typeof predicate !== "function") {
            this.clearCache();
            return;
        }

        for (const key of this.cache.keys()) {
            if (predicate(key)) {
                this.cache.delete(key);
            }
        }
    }

    getCacheStats() {
        return {
            size: this.cache.size,
            keys: Array.from(this.cache.keys()),
        };
    }

    async batchRequests(requests) {
        const settledResults = await Promise.allSettled(
            requests.map((request) => this._executeBatchRequest(request)),
        );

        return settledResults.map((result, index) => {
            if (result.status === "fulfilled") {
                return {
                    request: requests[index],
                    success: Boolean(result.value?.success),
                    data: result.value?.data ?? null,
                    error: result.value?.error ?? null,
                    status: result.value?.status ?? 200,
                };
            }

            return {
                request: requests[index],
                success: false,
                data: null,
                error:
                    result.reason instanceof Error
                        ? result.reason.message
                        : String(result.reason),
                status: 500,
            };
        });
    }

    async isApiAvailable() {
        const health = await this.getHealth();
        return health.success && health.data?.status === "healthy";
    }

    async getHealth() {
        if (this.snapshotId) {
            return this.request(this._joinPath("snapshots", this.snapshotId, "health"));
        }
        return this.request("health");
    }

    async getCapabilities() {
        return this.request("capabilities");
    }

    async getRepresentationCapabilities() {
        return this.request("representation-capabilities");
    }

    async getSnapshots() {
        return this.request("snapshots");
    }

    async getSnapshot(snapshotId) {
        return this.request(this._joinPath("snapshots", snapshotId));
    }

    async createSnapshot(payload) {
        return this.request("snapshots", {
            method: "POST",
            body: payload,
        });
    }

    async query(payload) {
        return this.request("query", {
            method: "POST",
            body: payload,
        });
    }

    async compare(payload) {
        return this.request("compare", {
            method: "POST",
            body: payload,
        });
    }

    async getJobs() {
        return this.request("jobs");
    }

    async getJob(jobId) {
        return this.request(this._joinPath("jobs", jobId));
    }

    async createJob(payload) {
        return this.request("jobs", {
            method: "POST",
            body: payload,
        });
    }

    async cancelJob(jobId) {
        return this.request(this._joinPath("jobs", jobId, "cancel"), {
            method: "POST",
        });
    }

    async getUnits() {
        return this.request(
            this._joinPath("snapshots", this._requireSnapshotId(), "units"),
        );
    }

    async getUnitDetail(unitName) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "units",
                unitName,
            ),
        );
    }

    async getSummary() {
        return this.request(
            this._joinPath("snapshots", this._requireSnapshotId(), "summary"),
        );
    }

    async getRepresentationTypes() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "representations",
            ),
        );
    }

    async getRepresentationData(representationKind) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "representations",
                representationKind,
            ),
        );
    }

    async queryRepresentationCatalog({
        unitName = null,
        sourceRef = null,
        representationKind = null,
    } = {}) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "representation-query",
            ),
            {
                query: {
                    unit: unitName || undefined,
                    source: sourceRef || undefined,
                    kind: representationKind || undefined,
                },
            },
        );
    }

    async requestRepresentations(payload, snapshotId = null) {
        const targetSnapshotId = snapshotId || this._requireSnapshotId();
        return this.request(
            this._joinPath(
                "snapshots",
                targetSnapshotId,
                "representation-requests",
            ),
            {
                method: "POST",
                body: payload,
            },
        );
    }

    async getBuildDependencies() {
        return this.getRepresentationData("dependencies");
    }

    async getFileContent(unitName, fileType, fileName, full = false) {
        void full;
        switch (fileType) {
            case "assembly":
                return this.getAssembly(fileName, unitName);
            case "ir":
                return this.getLLVMIR(fileName, unitName);
            case "optimized-ir":
                return this.getOptimizedIR(fileName, unitName);
            case "object":
                return this.getObjectCode(fileName, unitName);
            case "ast-json":
                return this.getASTJSON(fileName, unitName);
            case "preprocessed":
                return this.getPreprocessed(fileName, unitName);
            case "macro-expansion":
                return this.getMacroExpansion(fileName, unitName);
            default:
                return createErrorResponse(
                    new Error(`Unsupported representation kind '${fileType}'`),
                    400,
                );
        }
    }

    async getRemarksOverview() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "remarks",
                "overview",
            ),
        );
    }

    async getRemarksPasses() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "remarks",
                "passes",
            ),
        );
    }

    async getRemarksFunctions() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "remarks",
                "functions",
            ),
        );
    }

    async getRemarksHotspots() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "remarks",
                "hotspots",
            ),
        );
    }

    async getDiagnosticsOverview() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "diagnostics",
                "overview",
            ),
        );
    }

    async getDiagnosticsByLevel() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "diagnostics",
                "by-level",
            ),
        );
    }

    async getDiagnosticsFiles() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "diagnostics",
                "files",
            ),
        );
    }

    async getDiagnosticsPatterns() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "diagnostics",
                "patterns",
            ),
        );
    }

    async getFTimeReport() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "representations",
                "ftime-report",
            ),
        );
    }

    async getVersionInfo() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "representations",
                "version-info",
            ),
        );
    }

    async getCompilationPhasesBindings() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "compilation-phases",
                "bindings",
            ),
        );
    }

    async getTimeTraceOverview() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "time-trace",
                "overview",
            ),
        );
    }

    async getTimeTraceTimeline(limit = 1000) {
        return this.request(this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "time-trace",
                "timeline",
            ), {
            query: { limit },
        });
    }

    async getTimeTraceHotspots() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "time-trace",
                "hotspots",
            ),
        );
    }

    async getTimeTraceCategories() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "time-trace",
                "categories",
            ),
        );
    }

    async getTimeTraceParallelism() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "time-trace",
                "parallelism",
            ),
        );
    }

    async getBinarySizeOverview() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "binary-size",
                "overview",
            ),
        );
    }

    async getBinarySizeSections() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "binary-size",
                "sections",
            ),
        );
    }

    async getBinarySizeOptimization() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "binary-size",
                "optimization",
            ),
        );
    }

    async getBinarySizeComparison() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "binary-size",
                "comparison",
            ),
        );
    }

    async getAnalysisData(analysisType, subEndpoint = "overview", query = {}) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                analysisType,
                subEndpoint,
            ),
            { query },
        );
    }

    async getRuntimeTraceOverview() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "runtime-trace",
                "overview",
            ),
        );
    }

    async getRuntimeTraceTimeline(limit = 1000) {
        return this.request(this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "runtime-trace",
                "timeline",
            ), {
            query: { limit },
        });
    }

    async getRuntimeTraceHotspots() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "runtime-trace",
                "hotspots",
            ),
        );
    }

    async getRuntimeTraceCategories() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "runtime-trace",
                "categories",
            ),
        );
    }

    async getRuntimeTraceParallelism() {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "analysis",
                "runtime-trace",
                "parallelism",
            ),
        );
    }

    async getSourceFiles(unitName = null) {
        return this.request(this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "files",
            ), {
            query: { unit: unitName || undefined },
        });
    }

    async getSourceCode(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "source",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    async getAssembly(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "explorer",
                "assembly",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    async getLLVMIR(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "explorer",
                "ir",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    async getOptimizedIR(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "explorer",
                "optimized-ir",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    async getObjectCode(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "explorer",
                "object",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    async getASTJSON(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "explorer",
                "ast-json",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    async getPreprocessed(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "explorer",
                "preprocessed",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    async getMacroExpansion(filePath, unitName = null) {
        return this.request(
            this._joinPath(
                "snapshots",
                this._requireSnapshotId(),
                "explorer",
                "macro-expansion",
                filePath,
            ),
            {
                query: { unit: unitName || undefined },
            },
        );
    }

    setSnapshot(snapshotId) {
        this.snapshotId = snapshotId || null;
        this.clearCache();
    }

    _buildRequestConfig(endpoint, options) {
        const method = normalizeMethod(options.method);
        const query = buildQueryString(options.query);
        const normalizedEndpoint = String(endpoint || "").replace(/^\/+/, "");
        const url = `${this.baseUrl}/api/${normalizedEndpoint}${query}`;

        const headers = {
            ...DEFAULT_HEADERS,
            ...(options.headers || {}),
        };

        const fetchOptions = {
            method,
            headers,
            signal: options.signal,
        };

        if (options.body !== undefined) {
            if (isPlainObject(options.body) || Array.isArray(options.body)) {
                fetchOptions.body = JSON.stringify(options.body);
                if (!fetchOptions.headers["Content-Type"]) {
                    fetchOptions.headers["Content-Type"] = "application/json";
                }
            } else {
                fetchOptions.body = options.body;
            }
        }

        return {
            url,
            method,
            query: options.query || null,
            body: options.body,
            fetchOptions,
        };
    }

    _buildCacheKey(requestConfig) {
        return stableStringify({
            method: requestConfig.method,
            url: requestConfig.url,
            body: requestConfig.body ?? null,
        });
    }

    _isCacheable(method) {
        return CACHEABLE_METHODS.has(method);
    }

    _getCachedResponse(cacheKey) {
        const cachedEntry = this.cache.get(cacheKey);
        if (!cachedEntry) {
            return null;
        }

        if (Date.now() > cachedEntry.expiresAt) {
            this.cache.delete(cacheKey);
            return null;
        }

        return cloneResponseData(cachedEntry.data);
    }

    _setCachedResponse(cacheKey, data) {
        this.cache.set(cacheKey, {
            data: cloneResponseData(data),
            expiresAt: Date.now() + this.cacheTtlMs,
        });
    }

    async _normalizeResponse(response) {
        const contentType = response.headers.get("content-type") || "";
        const isJson = contentType.includes("application/json");

        let payload = null;
        try {
            payload = isJson ? await response.json() : await response.text();
        } catch (error) {
            payload = null;
        }

        if (!response.ok) {
            const errorMessage =
                (isPlainObject(payload) &&
                    (typeof payload.error === "string"
                        ? payload.error
                        : payload.error?.message)) ||
                response.statusText ||
                `HTTP ${response.status}`;

            return {
                success: false,
                error: errorMessage,
                status: response.status,
                data: isPlainObject(payload) ? (payload.data ?? null) : payload,
            };
        }

        if (isPlainObject(payload) && typeof payload.success === "boolean") {
            return {
                success: payload.success,
                error:
                    typeof payload.error === "string"
                        ? payload.error
                        : payload.error?.message ?? null,
                status: payload.status ?? response.status,
                data: payload.data ?? null,
            };
        }

        return {
            success: true,
            error: null,
            status: response.status,
            data: payload,
        };
    }

    _joinPath(...segments) {
        return segments
            .filter(
                (segment) =>
                    segment !== undefined && segment !== null && segment !== "",
            )
            .map((segment) => encodeURIComponent(String(segment)))
            .join("/");
    }

    _executeBatchRequest(request) {
        if (typeof request === "string") {
            return this.request(request);
        }

        return this.request(request.endpoint, request.options);
    }

    _requireSnapshotId() {
        if (!this.snapshotId) {
            throw new Error("Snapshot is not selected");
        }

        return this.snapshotId;
    }
}

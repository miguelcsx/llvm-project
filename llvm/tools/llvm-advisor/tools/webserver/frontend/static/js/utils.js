// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Utility Functions
 * Shared formatting and data helpers for the LLVM Advisor frontend.
 */

const EMPTY_TEXT = "";
const UNKNOWN_LABEL = "Unknown";
const DEFAULT_PERCENT_DECIMALS = 1;
const DEFAULT_TRUNCATE_LENGTH = 50;
const BYTE_UNITS = ["B", "KB", "MB", "GB", "TB"];
const BYTE_BASE = 1024;

const FILE_TYPE_LABELS = Object.freeze({
    opt_record: "Optimization Records",
    opt_remarks: "Optimization Remarks",
    time_trace: "Time Trace",
    runtime_trace: "Runtime Trace",
    binary_size: "Binary Size",
    compilation_units: "Compilation Units",
    diagnostics: "Diagnostics",
    clang_diagnostics: "Clang Diagnostics",
    coverage_report: "Coverage Report",
    profile_data: "Profile Data",
    ast_dump: "AST Dump",
    ir_code: "IR Code",
    assembly_code: "Assembly Code",
    debug_info: "Debug Info",
    static_analysis: "Static Analysis",
    memory_usage: "Memory Usage",
    compilation_commands: "Compilation Commands",
    build_log: "Build Log",
    link_map: "Link Map",
    symbol_table: "Symbol Table",
});

const PHASE_LABELS = Object.freeze({
    frontend: "Frontend",
    backend: "Backend",
    codegen: "Code Generation",
    optimization: "Optimization",
    linking: "Linking",
    parsing: "Parsing",
    semantic: "Semantic Analysis",
    irgen: "IR Generation",
    opt: "Optimization",
    asm: "Assembly Generation",
    obj: "Object Generation",
});

const SECTION_LABELS = Object.freeze({
    ".text": "Code (.text)",
    ".data": "Data (.data)",
    ".bss": "BSS (.bss)",
    ".rodata": "Read-Only Data (.rodata)",
    ".debug": "Debug Info (.debug)",
    ".symtab": "Symbol Table (.symtab)",
    ".strtab": "String Table (.strtab)",
    ".rela": "Relocations (.rela)",
    ".dynamic": "Dynamic (.dynamic)",
    ".interp": "Interpreter (.interp)",
    ".note": "Notes (.note)",
    ".comment": "Comments (.comment)",
    ".plt": "PLT (.plt)",
    ".got": "GOT (.got)",
});

const DIAGNOSTIC_LEVEL_LABELS = Object.freeze({
    error: "Error",
    warning: "Warning",
    note: "Note",
    info: "Info",
    fatal: "Fatal Error",
    remark: "Remark",
});

const DEFAULT_COLORS = Object.freeze([
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
    "#14b8a6",
    "#f59e0b",
]);

function isNil(value) {
    return value === null || value === undefined;
}

function isObject(value) {
    return value !== null && typeof value === "object";
}

function toInteger(value, fallback = 0) {
    const number = Number.parseInt(value, 10);
    return Number.isNaN(number) ? fallback : number;
}

function toFloat(value, fallback = 0) {
    const number = Number.parseFloat(value);
    return Number.isNaN(number) ? fallback : number;
}

function withFallback(value, fallback) {
    return isNil(value) ? fallback : value;
}

function formatCompactNumber(value) {
    if (value >= 1_000_000) {
        return `${(value / 1_000_000).toFixed(1)}M`;
    }

    if (value >= 1_000) {
        return `${(value / 1_000).toFixed(1)}K`;
    }

    return value.toLocaleString();
}

function normalizeWords(value) {
    return String(value).replace(/[_-]+/g, " ").trim();
}

function titleize(value) {
    return normalizeWords(value).replace(/\b\w/g, (letter) =>
        letter.toUpperCase(),
    );
}

function applyKnownAcronyms(value) {
    return value
        .replace(/\bIr\b/g, "IR")
        .replace(/\bLlvm\b/g, "LLVM")
        .replace(/\bCpp\b/g, "C++")
        .replace(/\bAst\b/g, "AST");
}

function formatDurationParts(timeMs) {
    if (timeMs < 1_000) {
        return `${timeMs.toFixed(0)}ms`;
    }

    if (timeMs < 60_000) {
        return `${(timeMs / 1_000).toFixed(2)}s`;
    }

    if (timeMs < 3_600_000) {
        const minutes = Math.floor(timeMs / 60_000);
        const seconds = ((timeMs % 60_000) / 1_000).toFixed(0);
        return `${minutes}m ${seconds}s`;
    }

    const hours = Math.floor(timeMs / 3_600_000);
    const minutes = Math.floor((timeMs % 3_600_000) / 60_000);
    return `${hours}h ${minutes}m`;
}

function formatByteUnits(size) {
    if (size < BYTE_BASE) {
        return `${size} B`;
    }

    let value = size;
    let unitIndex = 0;

    while (value >= BYTE_BASE && unitIndex < BYTE_UNITS.length - 1) {
        value /= BYTE_BASE;
        unitIndex += 1;
    }

    return `${value.toFixed(1)} ${BYTE_UNITS[unitIndex]}`;
}

function cloneValue(value) {
    if (!isObject(value)) {
        return value;
    }

    if (value instanceof Date) {
        return new Date(value.getTime());
    }

    if (Array.isArray(value)) {
        return value.map((item) => cloneValue(item));
    }

    return Object.keys(value).reduce((result, key) => {
        result[key] = cloneValue(value[key]);
        return result;
    }, {});
}

function arraysEqual(left, right) {
    if (left.length !== right.length) {
        return false;
    }

    return left.every((value, index) => Utils.isEqual(value, right[index]));
}

function objectsEqual(left, right) {
    const leftKeys = Object.keys(left);
    const rightKeys = Object.keys(right);

    if (leftKeys.length !== rightKeys.length) {
        return false;
    }

    return leftKeys.every(
        (key) =>
            rightKeys.includes(key) && Utils.isEqual(left[key], right[key]),
    );
}

function createDelayedInvoker(callback, wait, context, mode) {
    let timeoutId = null;
    let isThrottled = false;

    return function wrappedFunction(...args) {
        const invoke = () => callback.apply(context || this, args);

        if (mode === "debounce") {
            window.clearTimeout(timeoutId);
            timeoutId = window.setTimeout(invoke, wait);
            return;
        }

        if (isThrottled) {
            return;
        }

        invoke();
        isThrottled = true;
        timeoutId = window.setTimeout(() => {
            isThrottled = false;
        }, wait);
    };
}

export class Utils {
    static formatNumber(value) {
        if (isNil(value)) {
            return "0";
        }

        const number = toInteger(value, 0);
        return formatCompactNumber(number);
    }

    static formatFileType(type) {
        if (!type) {
            return UNKNOWN_LABEL;
        }

        return FILE_TYPE_LABELS[type] || titleize(type);
    }

    static capitalize(value) {
        if (!value) {
            return EMPTY_TEXT;
        }

        return value.charAt(0).toUpperCase() + value.slice(1).toLowerCase();
    }

    static formatPhaseName(name) {
        if (!name) {
            return "Unknown Phase";
        }

        const normalizedName = String(name).toLowerCase();
        const knownLabel = PHASE_LABELS[normalizedName];
        if (knownLabel) {
            return knownLabel;
        }

        return applyKnownAcronyms(titleize(name));
    }

    static formatTime(timeMs) {
        if (isNil(timeMs)) {
            return "0ms";
        }

        return formatDurationParts(toFloat(timeMs, 0));
    }

    static formatBytes(bytes) {
        if (isNil(bytes)) {
            return "0 B";
        }

        const size = toInteger(bytes, 0);
        if (size <= 0) {
            return "0 B";
        }

        return formatByteUnits(size);
    }

    static formatSectionName(name) {
        if (!name) {
            return "Unknown Section";
        }

        if (SECTION_LABELS[name]) {
            return SECTION_LABELS[name];
        }

        if (name.startsWith(".")) {
            return `${Utils.capitalize(name.slice(1))} (${name})`;
        }

        return Utils.capitalize(name);
    }

    static formatPercentage(value, decimals = DEFAULT_PERCENT_DECIMALS) {
        if (isNil(value)) {
            return "0%";
        }

        return `${toFloat(value, 0).toFixed(decimals)}%`;
    }

    static formatDiagnosticLevel(level) {
        const normalizedLevel = String(
            withFallback(level, "unknown"),
        ).toLowerCase();
        return (
            DIAGNOSTIC_LEVEL_LABELS[normalizedLevel] ||
            Utils.capitalize(normalizedLevel)
        );
    }

    static truncateText(text, maxLength = DEFAULT_TRUNCATE_LENGTH) {
        if (!text) {
            return EMPTY_TEXT;
        }

        if (text.length <= maxLength) {
            return text;
        }

        return `${text.substring(0, maxLength - 3)}...`;
    }

    static debounce(callback, wait) {
        return createDelayedInvoker(callback, wait, null, "debounce");
    }

    static throttle(callback, wait) {
        return createDelayedInvoker(callback, wait, null, "throttle");
    }

    static deepClone(value) {
        return cloneValue(value);
    }

    static isEqual(left, right) {
        if (left === right) {
            return true;
        }

        if (isNil(left) || isNil(right)) {
            return false;
        }

        if (typeof left !== typeof right) {
            return false;
        }

        if (Array.isArray(left) && Array.isArray(right)) {
            return arraysEqual(left, right);
        }

        if (isObject(left) && isObject(right)) {
            return objectsEqual(left, right);
        }

        return false;
    }

    static getRandomColor() {
        return DEFAULT_COLORS[
            Math.floor(Math.random() * DEFAULT_COLORS.length)
        ];
    }
}

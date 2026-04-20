// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Code Viewer Component
 * Read-only code editor with Prism.js syntax highlighting and selection
 */

export class CodeViewer {
    constructor(container, options = {}) {
        this.container = container;
        this.options = {
            language: "text",
            showLineNumbers: true,
            readOnly: true,
            ...options,
        };

        this.content = "";
        this.inlineData = null;
        this.inlineDataVisible = { diagnostics: false, remarks: false };

        this.lineNumbersDiv = null;
        this.codeContentDiv = null;
        this.lineElements = new Map();
        this.lineNumberElements = new Map();
        this.activeLine = null;
    }

    render(content, language = null, inlineData = null) {
        if (!this.container) return;

        this.content = content || "";
        this.language = language || this.options.language;
        this.inlineData = inlineData;

        if (!this.content) {
            this.renderEmpty();
            return;
        }

        this.renderEditor();
    }

    renderEditor() {
        const lines = this.content.split("\n");
        const maxLineDigits = lines.length.toString().length;

        const MAX_LINES = 5000;
        const isLargeFile = lines.length > MAX_LINES;
        const displayLines = isLargeFile ? lines.slice(0, MAX_LINES) : lines;

        const editorDiv = document.createElement("div");
        editorDiv.className =
            "code-editor code-editor-shell h-full border rounded-lg overflow-hidden";
        this.lineElements = new Map();
        this.lineNumberElements = new Map();

        if (isLargeFile) {
            const warningDiv = document.createElement("div");
            warningDiv.className =
                "bg-yellow-50 border-b border-yellow-200 px-4 py-2 text-sm text-yellow-800";
            warningDiv.innerHTML = ` Large file detected. Showing first ${MAX_LINES} lines of ${lines.length} total lines.`;
            editorDiv.appendChild(warningDiv);
        }

        const flexContainer = document.createElement("div");
        flexContainer.className = "code-editor-body flex h-full min-h-0 min-w-0";

        if (this.options.showLineNumbers) {
            const lineNumbersDiv = document.createElement("div");
            lineNumbersDiv.className =
                "line-numbers code-gutter px-3 py-2 select-none flex-shrink-0 text-right font-mono text-sm text-gray-500";
            lineNumbersDiv.style.minWidth = `${maxLineDigits * 10 + 24}px`;
            lineNumbersDiv.style.cssText +=
                "font-size: 13px; line-height: 1.5; padding-top: 8px;";

            displayLines.forEach((_, index) => {
                const lineNumber = index + 1;
                const lineInlineData = this.getInlineDataForLine(lineNumber);

                const lineNumWrapper = document.createElement("div");
                lineNumWrapper.className = "line-number-wrapper";

                const lineNumDiv = document.createElement("div");
                lineNumDiv.className = "line-number-content";
                lineNumDiv.style.cssText =
                    "min-height: 21px; line-height: 1.5; padding-top: 0; padding-bottom: 0;";
                lineNumDiv.textContent = lineNumber.toString();
                lineNumDiv.dataset.lineNumber = String(lineNumber);
                lineNumDiv.addEventListener("click", () =>
                    this.selectLine(lineNumber),
                );

                lineNumWrapper.appendChild(lineNumDiv);
                this.lineNumberElements.set(lineNumber, lineNumWrapper);

                if (this.shouldShowInlineData(lineInlineData)) {
                    const spacerDiv = document.createElement("div");
                    spacerDiv.className = "line-number-spacer";
                    lineNumWrapper.appendChild(spacerDiv);
                }

                lineNumbersDiv.appendChild(lineNumWrapper);
            });

            flexContainer.appendChild(lineNumbersDiv);
            this.lineNumbersDiv = lineNumbersDiv;
        }

        const codeContentDiv = document.createElement("div");
        codeContentDiv.className = "code-content flex-1 overflow-auto";

        const linesContainer = document.createElement("div");
        linesContainer.className = "code-lines font-mono text-sm";
        linesContainer.style.cssText =
            "padding: 8px; font-size: 13px; line-height: 1.5; min-width: max-content;";

        displayLines.forEach((line, index) => {
            const lineNumber = index + 1;
            const lineInlineData = this.getInlineDataForLine(lineNumber);

            const lineWrapper = document.createElement("div");
            lineWrapper.className = "code-line-wrapper";

            const codeLine = document.createElement("div");
            codeLine.className = "code-line";
            codeLine.style.cssText = "min-height: 21px; line-height: 1.5;";
            codeLine.dataset.lineNumber = String(lineNumber);
            codeLine.addEventListener("click", () => this.selectLine(lineNumber));

            const tempPre = document.createElement("pre");
            tempPre.className =
                "language-" + this.getPrismLanguage(this.language);
            tempPre.style.cssText =
                "margin: 0; padding: 0; background: transparent; display: inline-block; min-width: 100%;";

            const tempCode = document.createElement("code");
            tempCode.className =
                "language-" + this.getPrismLanguage(this.language);
            tempCode.textContent = line;

            tempPre.appendChild(tempCode);

            if (window.Prism) {
                try {
                    window.Prism.highlightElement(tempCode);
                } catch (e) {
                    tempCode.textContent = line;
                }
            }

            codeLine.appendChild(tempPre);
            lineWrapper.appendChild(codeLine);
            this.lineElements.set(lineNumber, codeLine);

            if (this.shouldShowInlineData(lineInlineData)) {
                const inlineDataContainer = document.createElement("div");
                inlineDataContainer.className = "inline-data-container";
                inlineDataContainer.innerHTML =
                    this.renderInlineDataForLine(lineInlineData);
                lineWrapper.appendChild(inlineDataContainer);
            }

            linesContainer.appendChild(lineWrapper);
        });

        codeContentDiv.appendChild(linesContainer);

        if (this.options.showLineNumbers) {
            this.synchronizeLineHeights();
        }

        if (this.options.showLineNumbers && this.lineNumbersDiv) {
            codeContentDiv.addEventListener("scroll", () => {
                if (this.lineNumbersDiv) {
                    this.lineNumbersDiv.scrollTop = codeContentDiv.scrollTop;
                }
            });
        }

        flexContainer.appendChild(codeContentDiv);
        editorDiv.appendChild(flexContainer);

        this.codeContentDiv = codeContentDiv;

        this.container.innerHTML = "";
        this.container.appendChild(editorDiv);
        this.applyActiveLineState();
    }

    renderEmpty() {
        this.container.innerHTML = `
            <div class="h-full flex items-center justify-center code-editor-shell text-gray-500 border border-gray-200 rounded-lg">
                <div class="text-center">
                    <svg class="mx-auto h-12 w-12 mb-4 text-gray-400" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M10 20l4-16m4 4l4 4-4 4M6 16l-4-4 4-4" />
                    </svg>
                    <p class="text-sm">No code to display</p>
                </div>
            </div>
        `;
    }

    toggleInlineData(type, visible) {
        this.inlineDataVisible[type] = visible;

        if (this.content) {
            this.renderEditor();
        }
    }

    updateContent(content, language = null, inlineData = null) {
        this.render(content, language, inlineData);
    }

    setLanguage(language) {
        this.language = language;
        if (this.content) {
            this.renderEditor();
        }
    }

    clear() {
        this.content = "";
        this.inlineData = null;
        this.activeLine = null;
        this.lineElements.clear();
        this.lineNumberElements.clear();
        this.renderEmpty();
    }

    selectLine(lineNumber) {
        this.setActiveLine(lineNumber);

        if (typeof this.options.onLineSelect === "function") {
            this.options.onLineSelect({
                lineNumber,
                inlineData: this.getInlineDataForLine(lineNumber),
            });
        }
    }

    setActiveLine(lineNumber) {
        this.activeLine =
            Number.isInteger(lineNumber) && lineNumber > 0 ? lineNumber : null;
        this.applyActiveLineState();
    }

    scrollToLine(lineNumber) {
        if (!this.codeContentDiv) {
            return;
        }
        const codeLine = this.lineElements.get(lineNumber);
        if (!codeLine) {
            return;
        }

        const top = codeLine.offsetTop - this.codeContentDiv.clientHeight / 3;
        this.codeContentDiv.scrollTop = Math.max(0, top);
    }

    focusLine(lineNumber) {
        this.setActiveLine(lineNumber);
        this.scrollToLine(lineNumber);
    }

    getPrismLanguage(language) {
        const languageMap = {
            c: "c",
            cpp: "cpp",
            assembly: "nasm",
            "llvm-ir": "llvm",
            json: "json",
            python: "python",
            rust: "rust",
            javascript: "javascript",
            go: "go",
            java: "java",
        };
        return languageMap[language] || "plaintext";
    }

    shouldShowInlineData(lineInlineData) {
        const hasDiagnostics =
            this.inlineDataVisible.diagnostics &&
            lineInlineData.diagnostics.length > 0;
        const hasRemarks =
            this.inlineDataVisible.remarks && lineInlineData.remarks.length > 0;
        return hasDiagnostics || hasRemarks;
    }

    getInlineDataForLine(lineNumber) {
        if (!this.inlineData) {
            return { diagnostics: [], remarks: [] };
        }

        let diagnostics = [];
        let remarks = [];

        if (Array.isArray(this.inlineData.diagnostics)) {
            diagnostics = this.inlineData.diagnostics.filter(
                (d) => d.line === lineNumber,
            );
        }

        if (Array.isArray(this.inlineData.remarks)) {
            remarks = this.inlineData.remarks.filter(
                (r) => r.line === lineNumber,
            );
        }

        return { diagnostics, remarks };
    }

    renderInlineDataForLine(lineInlineData) {
        let html = "";

        if (
            this.inlineDataVisible.diagnostics &&
            lineInlineData.diagnostics.length > 0
        ) {
            lineInlineData.diagnostics.forEach((diagnostic) => {
                const levelClass = this.getDiagnosticLevelClass(
                    diagnostic.level,
                );
                const icon = this.getDiagnosticIcon(diagnostic.level);

                html += `
                    <div class="inline-diagnostic inline-message ${levelClass} text-xs transition-all duration-200">
                        <div class="flex items-start space-x-2">
                            <span class="flex-shrink-0">${icon}</span>
                            <div class="flex-1">
                                <div class="font-medium">${this.escapeHtml(
                                    diagnostic.message,
                                )}</div>
                                ${
                                    diagnostic.column
                                        ? `<div class="text-xs mt-1 opacity-75">Column ${diagnostic.column}</div>`
                                        : ""
                                }
                            </div>
                        </div>
                    </div>
                `;
            });
        }

        if (
            this.inlineDataVisible.remarks &&
            lineInlineData.remarks.length > 0
        ) {
            lineInlineData.remarks.forEach((remark) => {
                html += `
                    <div class="inline-remark inline-message inline-message-remark text-xs transition-all duration-200">
                        <div class="flex items-start space-x-2">
                            <span class="flex-shrink-0 text-gray-500"></span>
                            <div class="flex-1">
                                <div class="font-medium text-gray-900">${this.escapeHtml(
                                    remark.message,
                                )}</div>
                                ${
                                    remark.pass_name
                                        ? `<div class="text-gray-500 text-xs mt-1 italic">[${this.escapeHtml(
                                              remark.pass_name,
                                          )}]</div>`
                                        : ""
                                }
                                ${
                                    remark.column
                                        ? `<div class="text-xs mt-1 opacity-75">Column ${remark.column}</div>`
                                        : ""
                                }
                                ${
                                    remark.source_line
                                        ? `<div class="text-xs mt-1 opacity-75">Source line ${this.escapeHtml(
                                              remark.source_line,
                                          )}</div>`
                                        : ""
                                }
                            </div>
                        </div>
                    </div>
                `;
            });
        }

        return html;
    }

    getDiagnosticLevelClass(level) {
        const levelMap = {
            error: "inline-message-error",
            warning: "inline-message-warning",
            note: "inline-message-note",
            info: "inline-message-info",
        };
        return levelMap[level] || levelMap["info"];
    }

    getDiagnosticIcon(level) {
        const iconMap = { error: "", warning: "", note: "", info: "" };
        return iconMap[level] || iconMap["info"];
    }

    escapeHtml(text) {
        const div = document.createElement("div");
        div.textContent = text;
        return div.innerHTML;
    }

    applyActiveLineState() {
        this.lineElements.forEach((element, lineNumber) => {
            element.classList.toggle("code-line-active", lineNumber === this.activeLine);
        });

        this.lineNumberElements.forEach((element, lineNumber) => {
            element.classList.toggle(
                "line-number-active",
                lineNumber === this.activeLine,
            );
        });
    }

    synchronizeLineHeights() {
        if (!this.lineNumbersDiv) return;

        setTimeout(() => {
            const lineWrappers =
                this.container.querySelectorAll(".code-line-wrapper");
            const lineNumberWrappers = this.lineNumbersDiv.querySelectorAll(
                ".line-number-wrapper",
            );

            lineWrappers.forEach((codeLineWrapper, index) => {
                const lineNumberWrapper = lineNumberWrappers[index];
                if (!lineNumberWrapper) return;

                const inlineDataContainer = codeLineWrapper.querySelector(
                    ".inline-data-container",
                );
                const lineNumberSpacer = lineNumberWrapper.querySelector(
                    ".line-number-spacer",
                );

                if (inlineDataContainer && lineNumberSpacer) {
                    const inlineDataHeight = inlineDataContainer.offsetHeight;
                    lineNumberSpacer.style.height = `${inlineDataHeight}px`;
                } else if (lineNumberSpacer) {
                    lineNumberSpacer.style.height = "0px";
                }
            });
        }, 0);
    }
}

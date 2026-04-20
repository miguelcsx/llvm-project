// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

export class DiffViewer {
    constructor(container, options = {}) {
        this.container = container;
        this.options = {
            language: "llvm",
            theme: "llvm-dark",
            readOnly: true,
            ...options
        };
        
        this.container.innerHTML = "";
        this.editorContainer = document.createElement("div");
        this.editorContainer.className = "w-full h-full";
        this.editorContainer.style.minHeight = "400px";
        this.container.appendChild(this.editorContainer);
        
        this.diffEditor = null;
        this.editorReady = this.initEditor();
    }
    
    async initEditor() {
        if (typeof window !== "undefined" && window.monacoReady) {
            try {
                await window.monacoReady;
            } catch (_error) {
                this.editorContainer.innerHTML = "<div class=\"p-4 text-error\">Monaco Editor failed to load.</div>";
                return;
            }
        }

        if (typeof monaco === "undefined") {
            this.editorContainer.innerHTML = "<div class=\"p-4 text-error\">Monaco Editor not loaded.</div>";
            return;
        }
        
        this.diffEditor = monaco.editor.createDiffEditor(this.editorContainer, {
            theme: this.options.theme,
            readOnly: this.options.readOnly,
            automaticLayout: true,
            renderSideBySide: true,
            minimap: { enabled: false },
            ignoreTrimWhitespace: false,
            fontFamily: '"JetBrains Mono", "Fira Code", "Consolas", "Monaco", monospace',
            fontSize: 13,
        });
    }
    
    async render(originalContent, modifiedContent, language = "llvm") {
        await this.editorReady;
        if (!this.diffEditor || typeof monaco === "undefined") return;
        
        const lang = language === "ir" || language === "optimized-ir" ? "llvm" : language;
        
        const originalModel = monaco.editor.createModel(originalContent || "", lang);
        const modifiedModel = monaco.editor.createModel(modifiedContent || "", lang);
        
        this.diffEditor.setModel({
            original: originalModel,
            modified: modifiedModel
        });
    }
    
    showLoading() {
        if (!this.diffEditor) return;
        this.diffEditor.setModel(null);
    }

    showError(msg) {
        if (!this.diffEditor) return;
        this.diffEditor.setModel(null);
    }
    
    destroy() {
        if (this.diffEditor) {
            this.diffEditor.dispose();
            this.diffEditor = null;
        }
    }
}

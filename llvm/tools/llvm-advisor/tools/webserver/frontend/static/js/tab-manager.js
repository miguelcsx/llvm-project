// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/**
 * Tab Manager
 * Provides deterministic tab registration, navigation, UI state updates,
 * and URL hash synchronization for the LLVM Advisor frontend.
 */

const DEFAULT_TAB_ID = "dashboard";
const TAB_BUTTON_SELECTOR = ".tab-button";
const TAB_CONTENT_SELECTOR = ".tab-content";
const ACTIVE_BUTTON_CLASSES = ["active", "border-llvm-blue", "text-llvm-blue"];
const INACTIVE_BUTTON_CLASSES = [
    "border-transparent",
    "text-gray-500",
    "hover:text-gray-700",
    "hover:border-gray-300",
];
const HIDDEN_CONTENT_CLASS = "hidden";
const CONTENT_TRANSITION_CLASS = "tab-transition";

function normalizeTabId(value) {
    return typeof value === "string" ? value.trim() : "";
}

function getHashTabId() {
    return normalizeTabId(window.location.hash.replace(/^#/, ""));
}

function createTabRecord(button, content, isActive = false) {
    return {
        button,
        content,
        isLoaded: isActive,
        title: button.textContent.trim(),
    };
}

export class TabManager {
    constructor(options = {}) {
        this.defaultTabId =
            normalizeTabId(options.defaultTabId) || DEFAULT_TAB_ID;
        this.buttonSelector = options.buttonSelector || TAB_BUTTON_SELECTOR;
        this.contentSelector = options.contentSelector || TAB_CONTENT_SELECTOR;

        this.tabs = new Map();
        this.tabOrder = [];
        this.currentTab = this.defaultTabId;
        this.onTabChangeCallback = null;
        this.isSwitching = false;
    }

    init(options = {}) {
        this.onTabChangeCallback = options.onTabChange || null;

        this.registerTabs();
        this.setupEventListeners();

        const initialTabId = this.resolveInitialTabId();
        this.currentTab = initialTabId;
        this.setActiveTab(initialTabId);
        this.syncHash(initialTabId, true);
    }

    registerTabs() {
        this.tabs.clear();
        this.tabOrder = [];

        const tabButtons = document.querySelectorAll(this.buttonSelector);
        tabButtons.forEach((button) => {
            const tabId = normalizeTabId(button.dataset.tab);
            if (!tabId) {
                return;
            }

            const content = document.getElementById(`${tabId}-content`);
            if (!content) {
                return;
            }

            const isDefault = tabId === this.defaultTabId;
            this.tabs.set(tabId, createTabRecord(button, content, isDefault));
            this.tabOrder.push(tabId);
        });

        if (!this.tabs.has(this.defaultTabId) && this.tabOrder.length > 0) {
            this.currentTab = this.tabOrder[0];
        }
    }

    setupEventListeners() {
        document.addEventListener("click", this.handleDocumentClick);
        document.addEventListener("keydown", this.handleKeyDown);
        window.addEventListener("hashchange", this.handleHashChange);
    }

    destroy() {
        document.removeEventListener("click", this.handleDocumentClick);
        document.removeEventListener("keydown", this.handleKeyDown);
        window.removeEventListener("hashchange", this.handleHashChange);
    }

    handleDocumentClick = (event) => {
        const button = event.target.closest(this.buttonSelector);
        if (!button) {
            return;
        }

        const tabId = normalizeTabId(button.dataset.tab);
        if (!this.tabs.has(tabId)) {
            return;
        }

        event.preventDefault();
        void this.switchTab(tabId);
    };

    handleKeyDown = (event) => {
        if (!event.ctrlKey || event.key !== "Tab") {
            return;
        }

        event.preventDefault();
        if (event.shiftKey) {
            void this.switchToPreviousTab();
            return;
        }

        void this.switchToNextTab();
    };

    handleHashChange = () => {
        const hashTabId = getHashTabId();
        if (
            !hashTabId ||
            hashTabId === this.currentTab ||
            !this.tabs.has(hashTabId)
        ) {
            return;
        }

        void this.switchTab(hashTabId, { updateHash: false });
    };

    resolveInitialTabId() {
        const hashTabId = getHashTabId();
        if (hashTabId && this.tabs.has(hashTabId)) {
            return hashTabId;
        }

        if (this.tabs.has(this.defaultTabId)) {
            return this.defaultTabId;
        }

        return this.tabOrder[0] || this.defaultTabId;
    }

    async switchTab(tabId, options = {}) {
        const normalizedTabId = normalizeTabId(tabId);
        if (!this.tabs.has(normalizedTabId)) {
            return false;
        }

        if (this.isSwitching || normalizedTabId === this.currentTab) {
            return false;
        }

        const previousTabId = this.currentTab;
        this.isSwitching = true;

        try {
            this.currentTab = normalizedTabId;
            this.setActiveTab(normalizedTabId);

            if (options.updateHash !== false) {
                this.syncHash(normalizedTabId);
            }

            await this.notifyTabChange(normalizedTabId, previousTabId);
            this.markTabAsLoaded(normalizedTabId);
            this.trackTabSwitch(normalizedTabId, previousTabId);

            return true;
        } catch (error) {
            this.currentTab = previousTabId;
            this.setActiveTab(previousTabId);

            if (options.updateHash !== false) {
                this.syncHash(previousTabId);
            }

            this.showTabSwitchError(normalizedTabId, error);
            return false;
        } finally {
            this.isSwitching = false;
        }
    }

    async switchToNextTab() {
        const nextTabId = this.getAdjacentTabId(1);
        if (nextTabId) {
            await this.switchTab(nextTabId);
        }
    }

    async switchToPreviousTab() {
        const previousTabId = this.getAdjacentTabId(-1);
        if (previousTabId) {
            await this.switchTab(previousTabId);
        }
    }

    getAdjacentTabId(direction) {
        if (this.tabOrder.length === 0) {
            return null;
        }

        const currentIndex = this.tabOrder.indexOf(this.currentTab);
        if (currentIndex === -1) {
            return this.tabOrder[0];
        }

        const nextIndex =
            (currentIndex + direction + this.tabOrder.length) %
            this.tabOrder.length;
        return this.tabOrder[nextIndex];
    }

    setActiveTab(tabId) {
        this.tabs.forEach((tab, id) => {
            const isActive = id === tabId;
            this.updateButtonState(tab.button, isActive);
            this.updateContentState(tab.content, isActive);
        });
    }

    updateButtonState(button, isActive) {
        if (isActive) {
            button.classList.add(...ACTIVE_BUTTON_CLASSES);
            button.classList.remove(...INACTIVE_BUTTON_CLASSES);
            button.setAttribute("aria-selected", "true");
            button.setAttribute("tabindex", "0");
            return;
        }

        button.classList.remove(...ACTIVE_BUTTON_CLASSES);
        button.classList.add(...INACTIVE_BUTTON_CLASSES);
        button.setAttribute("aria-selected", "false");
        button.setAttribute("tabindex", "-1");
    }

    updateContentState(content, isActive) {
        content.classList.toggle(HIDDEN_CONTENT_CLASS, !isActive);
        content.classList.toggle(CONTENT_TRANSITION_CLASS, isActive);
    }

    syncHash(tabId, replace = false) {
        const nextHash = `#${tabId}`;
        if (window.location.hash === nextHash) {
            return;
        }

        if (replace) {
            history.replaceState(null, "", nextHash);
            return;
        }

        history.replaceState(null, "", nextHash);
    }

    async notifyTabChange(nextTabId, previousTabId) {
        if (typeof this.onTabChangeCallback !== "function") {
            return;
        }

        await this.onTabChangeCallback(nextTabId, previousTabId);
    }

    markTabAsLoaded(tabId) {
        const tab = this.tabs.get(tabId);
        if (tab) {
            tab.isLoaded = true;
        }
    }

    trackTabSwitch(nextTabId, previousTabId) {
        void nextTabId;
        void previousTabId;
    }

    showTabSwitchError(tabId, error) {
        void tabId;
        void error;
    }

    getCurrentTab() {
        return this.currentTab;
    }

    getTabInfo(tabId) {
        return this.tabs.get(normalizeTabId(tabId)) || null;
    }

    getAllTabs() {
        return this.tabOrder.reduce((result, tabId) => {
            const tab = this.tabs.get(tabId);
            if (!tab) {
                return result;
            }

            result[tabId] = {
                title: tab.title,
                isLoaded: tab.isLoaded,
                isActive: tabId === this.currentTab,
            };
            return result;
        }, {});
    }

    isTabLoaded(tabId) {
        const tab = this.tabs.get(normalizeTabId(tabId));
        return Boolean(tab?.isLoaded);
    }

    showTabLoading(tabId) {
        const tab = this.tabs.get(normalizeTabId(tabId));
        if (!tab?.content) {
            return;
        }

        tab.content.innerHTML = `
            <div class="flex items-center justify-center h-64">
                <div class="text-center">
                    <div class="inline-block animate-spin rounded-full h-8 w-8 border-b-2 border-llvm-blue"></div>
                    <p class="mt-2 text-gray-500">Loading ${tab.title}...</p>
                </div>
            </div>
        `;
    }
}

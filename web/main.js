/**
 * Try Turmeric - Web-based REPL
 * Main JavaScript file for the Turmeric WASM REPL
 */

import { createLspClient } from './lsp-client.js';
import { encodeShareCode, decodeShareCode } from './share-codec.js';
import { STORAGE_KEYS, safeRead, safeWrite, isTutorialMode, debounce, resetWorkspace } from './storage.js';
import { EXAMPLES } from './examples.js';
import { escapeHtml } from './html.js';
import { consoleRuns, renderConsoleRuns } from './console-runs.js';
import { provideEditor } from './editor-ref.js';
import { syncMinimapMenuItem, toggleMinimap, initMinimap } from './minimap.js';
import { initSplitHandle } from './split-handle.js';
import { initInstallAffordances } from './pwa-install.js';
import { initSymbolsMenu } from './outline.js';
import { setHashParam, getHashParam } from './hash-params.js';
import { configureDocsPane, docsPaneState, loadDocsIndex, docsEntryFor, parseDocsRef, showDocsPage, openDocsPane, closeDocsPane, docsPaneIsOpen, initDocsPane, syncDocsPaneFromHash } from './docs-pane.js';
import { configureTrace, traceState, traceCode, traceClose, traceSeek, traceInstallHandlers, traceInstallGutterHandler } from './trace.js';
import { monacoThemeName } from './editor-theme.js';
import { configureMonaco } from './editor-language.js';
import { buildZip, MAX_ZIP_BYTES, readStoreZip } from './zip.js';
import { initHScrollDrag } from './hscroll-drag.js';
import { initConsoleHover } from './console-hover.js';
import { configureEvalClient, WASM_STATE, wasmState, setWasmState, hasWorker, startEvalWorker, send, terminateWorker, pendingStats } from './eval-client.js';
import { parseLangDirective, fetchLangRegistry, reconcileLangPicker, initLangPicker } from './lang-picker.js';
import { refreshBuildStamp, forceUpdatePWA } from './pwa-update.js';
import { docNames, fetchDocNames, wasmDocLookup, wasmTypeOf, wasmExplain, showDocPanel, hideDocPanel, maybeShowDoc } from './doc-panel.js';

// ============================================================================
// WASM Module State
// ============================================================================

// The eval Worker, its state and its outstanding requests live in
// eval-client.js; these are its hooks back into the console and the Run queue.
configureEvalClient({
    onPrint: (text) => appendToConsole(`<span class="console-output">${escapeHtml(text)}</span>`),
    onPrintErr: (text) => appendToConsole(`<span class="console-error">${escapeHtml(text)}</span>`),
    onEvalTime: (ms) => updateExecTime(ms),
    nextEval: () => processQueue(),
    onPending: () => armEvalWatchdog(),
});
let editor = null;
let monaco = null;
let executionQueue = [];
let isExecuting = false;
let replHistory = [];
let replHistoryIndex = -1;
let lspClient = null;            // language server adapter; booted lazily
// Modules split out of this file read the editor through these getters.
provideEditor(() => editor, () => monaco);

// ============================================================================
// Configuration
// ============================================================================

const CONFIG = {
    DEFAULT_CODE: `#lang turmeric/sweet

println "Hello, Turmeric!"
`,
    // How long one request may keep the eval Worker busy before the watchdog
    // stops it and starts a fresh session (security-audit-plan W-3). Generous,
    // because a playground program that legitimately computes for a while is
    // not the problem; a program that never returns is.
    EXECUTION_TIMEOUT: 30000,
    MAX_OUTPUT_LENGTH: 10000,
};

// ============================================================================
// Local State Persistence
// ============================================================================

// Multi-tab editor state. Each tab carries its persisted record plus a
// non-persisted Monaco ITextModel created on demand. The model holds the
// per-tab undo stack, so swapping models (rather than re-using one and
// calling setValue) preserves undo across tab switches.
let tabs = [];          // [{id, name, content, cursor, scrollTop, createdAt, _model}]
let activeId = null;
let tabsHydrating = false;  // suppress persist during initial hydration / tutorial
/* PS5 (playground-session-hygiene-plan): what each tab's last successful Run
 * put in the session, keyed by tab id.  The session is shared by every tab -- a
 * Run is how a tab's definitions become callable at the prompt, and from other
 * tabs -- so rewinding it for one tab's Run must bring the others back
 * (rewindSessionForRun).  In memory only: a reload starts a fresh session in
 * which no tab has run. */
const lastRunByTab = new Map();

const MAX_CONSOLE_LINES = 200;

let consoleLog = [];

// The console transcript is stored as data; see console-runs.js.

function hydrateConsole() {
    // The HTML-era transcript is dropped, not rendered: there is no safe way
    // to read markup back, and a console history is not worth a parser.
    try { localStorage.removeItem(STORAGE_KEYS.legacyConsole); } catch {}
    const saved = safeRead(STORAGE_KEYS.consol);
    if (!Array.isArray(saved) || saved.length === 0) return;
    const consoleEl = document.getElementById('console');
    if (!consoleEl) return;
    const lines = saved.filter(Array.isArray).slice(-MAX_CONSOLE_LINES);
    if (lines.length === 0) return;
    consoleEl.textContent = '';
    for (const line of lines) {
        renderConsoleRuns(line, consoleEl);
        consoleEl.appendChild(document.createElement('br'));
    }
    consoleLog = lines;
    consoleEl.scrollTop = consoleEl.scrollHeight;
}

// ============================================================================
// Multi-tab editor state
// ============================================================================

function genTabId() {
    // Stable, non-cryptographic id. Tab ids are local to a single browser
    // workspace -- collision risk is purely cosmetic.
    return 'tab-' + Math.random().toString(36).slice(2, 10) + '-' + Date.now().toString(36);
}

function tabsSnapshot() {
    return tabs.map(t => ({
        id: t.id,
        name: t.name,
        content: t._model ? t._model.getValue() : t.content,
        cursor: t.cursor || { lineNumber: 1, column: 1 },
        scrollTop: t.scrollTop || 0,
        createdAt: t.createdAt,
        // Read-only stdlib buffers opened by go-to-definition (M4). Carried on
        // the snapshot so the test surface can see them; every *storage* path
        // filters them back out, and each of those filters is written at its
        // own call site rather than hidden here, because "which tabs count"
        // has a different answer for a zip than for a reload.
        readOnly: !!t.readOnly,
        sourceUri: t.sourceUri || undefined,
    }));
}

// A stdlib buffer is not part of the user's workspace: they did not create it,
// they cannot edit it, and restoring it on reload would present a file they
// never opened deliberately as one of their own.
const persistTabs = debounce(() => {
    if (tabsHydrating) return;
    safeWrite(STORAGE_KEYS.tabs, tabsSnapshot().filter(t => !t.readOnly));
}, 250);

function persistActiveId() {
    if (tabsHydrating) return;
    safeWrite(STORAGE_KEYS.activeTab, activeId);
}

function findTab(id) {
    return tabs.find(t => t.id === id) || null;
}

function currentTab() {
    return findTab(activeId);
}

function uniqueUntitledName() {
    const used = new Set(tabs.map(t => t.name));
    for (let n = 1; n < 10000; n++) {
        const candidate = `untitled-${n}.tur`;
        if (!used.has(candidate)) return candidate;
    }
    return `untitled-${Date.now()}.tur`;
}

function ensureModel(tab) {
    if (tab._model && !tab._model.isDisposed()) return tab._model;
    tab._model = monaco.editor.createModel(tab.content || '', 'turmeric');
    return tab._model;
}

function captureActiveTabState() {
    const t = currentTab();
    if (!t || !editor) return;
    t.content = editor.getValue();
    const pos = editor.getPosition();
    if (pos) t.cursor = { lineNumber: pos.lineNumber, column: pos.column };
    t.scrollTop = editor.getScrollTop();
}

function switchTab(id) {
    if (!editor) return;
    const next = findTab(id);
    if (!next) return;
    if (id === activeId) {
        renderTabs();
        return;
    }
    captureActiveTabState();
    activeId = id;
    editor.setModel(ensureModel(next));
    // Read-only is a property of the tab, not of the editor, so it is applied
    // on every switch rather than once. Missing this in one direction is the
    // bug that lets someone type into the stdlib; missing it in the other
    // locks them out of their own file.
    applyReadOnlyState(next);
    try {
        if (next.cursor) editor.setPosition(next.cursor);
        if (typeof next.scrollTop === 'number') editor.setScrollTop(next.scrollTop);
    } catch {}
    editor.focus();
    renderTabs();
    persistActiveId();
    // The picker reflects the active tab; dialect is per-file (§3.5).
    reconcileLangPicker();
}

function createTab({ name, content = '', activate = true } = {}) {
    const tab = {
        id: genTabId(),
        name: name || uniqueUntitledName(),
        content,
        cursor: { lineNumber: 1, column: 1 },
        scrollTop: 0,
        createdAt: Date.now(),
        _model: null,
    };
    tabs.push(tab);
    if (activate) switchTab(tab.id);
    else renderTabs();
    persistTabs();
    notifyTabsChanged();
    return tab;
}

function closeTab(id) {
    if (tabs.length <= 1) return;  // always keep at least one tab open
    const idx = tabs.findIndex(t => t.id === id);
    if (idx < 0) return;
    lastRunByTab.delete(id);   // PS5: a closed tab is not replayed
    const closing = tabs[idx];
    tabs.splice(idx, 1);
    if (closing._model && !closing._model.isDisposed()) {
        try { closing._model.dispose(); } catch {}
    }
    if (id === activeId) {
        const neighbor = tabs[idx] || tabs[idx - 1] || tabs[0];
        activeId = neighbor.id;
        editor.setModel(ensureModel(neighbor));
        applyReadOnlyState(neighbor);
        try {
            if (neighbor.cursor) editor.setPosition(neighbor.cursor);
            if (typeof neighbor.scrollTop === 'number') editor.setScrollTop(neighbor.scrollTop);
        } catch {}
    }
    renderTabs();
    persistTabs();
    persistActiveId();
    notifyTabsChanged();
    reconcileLangPicker();
}

function sanitizeTabName(raw) {
    let name = (raw || '').trim();
    if (!name) return null;
    // strip path separators / shell-unfriendly characters
    name = name.replace(/[\/\\<>:"|?*\x00-\x1f]/g, '_');
    if (!/\.tur$/i.test(name)) name += '.tur';
    return name;
}

function renameTab(id, rawName) {
    const tab = findTab(id);
    if (!tab) return;
    const cleaned = sanitizeTabName(rawName);
    if (!cleaned || cleaned === tab.name) { renderTabs(); return; }
    // disambiguate on collision: append -2, -3, ...
    let final = cleaned;
    const used = new Set(tabs.filter(t => t.id !== id).map(t => t.name));
    if (used.has(final)) {
        const base = final.replace(/\.tur$/i, '');
        for (let n = 2; n < 10000; n++) {
            const candidate = `${base}-${n}.tur`;
            if (!used.has(candidate)) { final = candidate; break; }
        }
    }
    tab.name = final;
    renderTabs();
    persistTabs();
    // A rename changes the document uri, so the server has to be told: the
    // client closes the old one and opens the new.
    notifyTabsChanged();
}

// ============================================================================
// Read-only buffers + jump-back (try-turmeric-navigation-and-minimap-plan, M4)
// ============================================================================

/** Put the editor in or out of read-only for the tab it is showing. */
function applyReadOnlyState(tab) {
    if (!editor) return;
    try {
        editor.updateOptions({ readOnly: !!(tab && tab.readOnly) });
    } catch { /* editor disposed mid-switch */ }
}

/** `file:///stdlib/list.tur` -> `list.tur`; used for the tab label. */
function basenameFromUri(uri) {
    const path = String(uri || '').replace(/^file:\/\//, '');
    const last = path.split('/').filter(Boolean).pop();
    return last || 'source.tur';
}

/**
 * Open a definition that lives outside the workspace -- in practice, a stdlib
 * source file the WASM bundle carries at /stdlib.
 *
 * Returns the tab (so the caller can convert a range against its model), or
 * null when there is nothing to show: no server, no reader export in this
 * bundle, or a path the export refuses. Null is the pre-M4 behaviour, which is
 * the right thing to degrade to.
 *
 * Opening the same file twice focuses the tab that is already there rather
 * than stacking copies of list.tur across a reading session.
 */
async function openReadOnlyTab(uri) {
    if (!lspClient || !lspClient.isAvailable()) return null;

    const existing = tabs.find(t => t.readOnly && t.sourceUri === uri);
    if (existing) { ensureModel(existing); return existing; }

    const path = String(uri || '').replace(/^file:\/\//, '');
    let text = null;
    try {
        text = await lspClient.readFile(path);
    } catch {
        return null;
    }
    if (typeof text !== 'string') return null;

    // Not createTab(): that activates, persists, and tells the server about a
    // new document. None of the three is wanted here -- the caller navigates
    // through onNavigate, storage excludes read-only tabs by design, and the
    // server already knows this file as its own source. Announcing it a second
    // time under a file:///project/ uri would have it analysed as if the user
    // had pasted the stdlib into their project.
    const tab = {
        id: genTabId(),
        name: basenameFromUri(uri),
        content: text,
        cursor: { lineNumber: 1, column: 1 },
        scrollTop: 0,
        createdAt: Date.now(),
        readOnly: true,
        sourceUri: uri,
        _model: null,
    };
    tabs.push(tab);
    ensureModel(tab);
    renderTabs();
    return tab;
}

// Where a jump came from, so it can be undone. Monaco standalone registers
// `editor.action.revealDefinition` but ships no navigation history service --
// that is a VS Code workbench feature -- so this is ours.
const JUMP_STACK_MAX = 20;
let jumpStack = [];

function pushJumpOrigin(tabId, position) {
    if (!tabId || !position) return;
    const top = jumpStack[jumpStack.length - 1];
    // Two definition requests from the same spot are one origin, not two.
    if (top && top.tabId === tabId &&
        top.position.lineNumber === position.lineNumber &&
        top.position.column === position.column) {
        return;
    }
    jumpStack.push({ tabId, position });
    if (jumpStack.length > JUMP_STACK_MAX) jumpStack.shift();
    renderJumpBack();
}

/** Pop back to the most recent origin whose tab still exists. */
function jumpBack() {
    while (jumpStack.length > 0) {
        const entry = jumpStack.pop();
        const tab = findTab(entry.tabId);
        // A closed tab is not an error, it is just not somewhere to go back
        // to. Keep popping rather than making the user press it twice.
        if (!tab) continue;
        switchTab(tab.id);
        try {
            editor.revealPositionInCenterIfOutsideViewport(entry.position);
            editor.setPosition(entry.position);
            editor.focus();
        } catch {}
        break;
    }
    renderJumpBack();
}

/** Show the Back button only while there is somewhere to go back to. */
function renderJumpBack() {
    const btn = document.getElementById('jump-back-btn');
    if (btn) btn.hidden = jumpStack.length === 0;
}

// ============================================================================
// Language server (try-turmeric-lsp-plan)
// ============================================================================

/**
 * Every tab is an open document as far as the server is concerned -- which is
 * what makes workspace/symbol and cross-tab go-to-definition work here without
 * the filesystem crawl a native client would need. That requires a Monaco
 * model per tab, including tabs the user has not activated yet; models are
 * otherwise created lazily on first switch.
 */
function ensureAllTabModels() {
    for (const tab of tabs) ensureModel(tab);
}

/**
 * Tell the server the tab set moved. Driven from the mutation sites rather
 * than by polling, but the client re-derives the whole open set from `tabs`
 * each time, so a missed call costs one stale moment rather than a permanently
 * desynchronised server.
 */
function notifyTabsChanged() {
    if (!lspClient || !lspClient.isAvailable()) return;
    ensureAllTabModels();
    lspClient.sync();
}

function setLspStatus(state) {
    const wrap = document.getElementById('lsp-status');
    const dot = document.getElementById('lsp-status-dot');
    const text = document.getElementById('lsp-status-text');
    if (!wrap || !dot || !text) return;

    if (state === 'unavailable') {
        // Nothing to say. The playground worked without analysis for its whole
        // life and still does; a permanent red dot would read as a fault.
        wrap.hidden = true;
        return;
    }
    wrap.hidden = false;
    if (state === 'booting') {
        dot.className = 'dot loading';
        text.textContent = 'Starting analysis...';
    } else if (state === 'analyzing') {
        dot.className = 'dot loading';
        text.textContent = 'Analyzing...';
    } else {
        dot.className = 'dot online';
        text.textContent = 'Analysis ready';
    }
}

/**
 * Boot the language server.
 *
 * Lazy on purpose: this is a second WASM instance, which roughly doubles the
 * playground's wasm memory. It is the same /turmeric.js URL so it is a cache
 * hit rather than a second download, but the memory is real -- and a visitor
 * who lands on the page to read a snippet should not pay for an analysis they
 * never asked a question of. First editor focus is the signal that they are
 * going to.
 */
function startLspClient() {
    if (lspClient) return lspClient.start();

    // The hover fallback reads doc-names.json, which otherwise only arrives
    // after a successful WASM boot -- so it was absent for the whole window
    // where it is most wanted, and permanently absent when that boot failed.
    // Idempotent and not awaited: a hover that arrives first simply misses,
    // which is the same as having no fallback, which is where we started.
    fetchDocNames();

    lspClient = createLspClient({
        monaco,
        languageId: 'turmeric',
        // Read-only stdlib buffers are deliberately not part of the document
        // set (M4). They are reference material the user cannot edit, and
        // announcing one under a file:///project/ uri would have the server
        // analyse the stdlib as if it had been pasted into the project --
        // publishing diagnostics against code nobody here can fix.
        getTabs: () => tabs.filter(t => !t.readOnly),
        onStatus: setLspStatus,
        // Hover fallback (M3). Same table the docs pane searches, already
        // fetched by the WASM boot -- no new asset, and no fetch on the hover
        // path: an unloaded table is an empty array, which is a miss.
        lookupDoc: (name) => (docNames || []).find(d => d.name === name) || null,
        onNavigate: (tab, range) => {
            // Monaco's standalone editor cannot switch models on its own, so a
            // definition in another tab is a tab switch we perform.
            switchTab(tab.id);
            try {
                editor.revealRangeInCenterIfOutsideViewport(range);
                editor.setPosition({
                    lineNumber: range.startLineNumber,
                    column: range.startColumn,
                });
            } catch {}
        },
        // A definition in the stdlib (M4).
        onOpenExternal: (uri) => openReadOnlyTab(uri),
        // Record where a jump starts. Fired from inside the definition
        // provider, so F12, Cmd+click and the context menu all push -- there
        // is no keybinding here that could miss one of them.
        onBeforeNavigate: (model, position) => {
            const origin = tabs.find(t => t._model === model);
            if (origin) pushJumpOrigin(origin.id, position);
        },
    });

    // Test surface: lets a spec await the server instead of sleeping on it.
    window._turiLsp = {
        ready: lspClient.start(),
        isAvailable: () => lspClient.isAvailable(),
        isBusy: () => lspClient.isBusy(),
        openDocuments: () => lspClient._openDocumentCount(),
        sync: () => notifyTabsChanged(),
        // Monaco is a module-scoped import, not a global, so a spec has no
        // other way to read the markers the adapter set.
        highlights: () => lspClient.documentHighlights(
            editor.getModel(), editor.getPosition()),
        markers: () => monaco.editor.getModelMarkers({ owner: 'turmeric' })
            .map(m => ({
                message: m.message,
                severity: m.severity,
                startLineNumber: m.startLineNumber,
                startColumn: m.startColumn,
                endColumn: m.endColumn,
            })),
    };

    return lspClient.start().then((ok) => {
        if (ok) notifyTabsChanged();
        return ok;
    });
}

function renderTabs() {
    const strip = document.querySelector('.editor-tabs');
    if (!strip) return;
    strip.innerHTML = '';
    const canClose = tabs.length > 1;
    for (const tab of tabs) {
        const btn = document.createElement('button');
        btn.className = 'tab-button' + (tab.id === activeId ? ' active' : '')
                                     + (tab.readOnly ? ' read-only' : '');
        btn.dataset.tabId = tab.id;
        btn.title = tab.readOnly
            ? `${tab.name} (read-only, from the standard library)`
            : tab.name;
        if (tab.readOnly) {
            // A padlock, so a tab that will not accept typing says so before
            // the user tries. Read-only is otherwise invisible until a
            // keystroke does nothing, which reads as a broken editor.
            const lock = document.createElement('span');
            lock.className = 'tab-lock';
            lock.textContent = '\u{1F512}';
            lock.setAttribute('aria-hidden', 'true');
            btn.appendChild(lock);
        }
        const label = document.createElement('span');
        label.className = 'tab-label';
        label.textContent = tab.name;
        btn.appendChild(label);
        if (canClose) {
            const x = document.createElement('span');
            x.className = 'tab-close';
            x.textContent = '×';
            x.setAttribute('aria-label', `Close ${tab.name}`);
            x.addEventListener('mousedown', (e) => { e.stopPropagation(); });
            x.addEventListener('click', (e) => {
                e.stopPropagation();
                closeTab(tab.id);
            });
            btn.appendChild(x);
        }
        btn.addEventListener('click', () => {
            if (btn.dataset.suppressClick === '1') {
                delete btn.dataset.suppressClick;
                return;
            }
            switchTab(tab.id);
            requestAnimationFrame(() => btn.scrollIntoView({ inline: 'nearest', block: 'nearest' }));
        });
        btn.addEventListener('dblclick', (e) => {
            e.preventDefault();
            // A stdlib buffer's name is its identity, not a label: it is how
            // the tab is matched on a second jump to the same file.
            if (tab.readOnly) return;
            beginRename(tab.id, btn, label);
        });
        setupTabDrag(btn, tab);
        strip.appendChild(btn);
    }
    const newBtn = document.createElement('button');
    newBtn.className = 'tab-button tab-new';
    newBtn.dataset.tab = 'new';
    newBtn.textContent = '+ New';
    newBtn.title = 'New tab';
    newBtn.addEventListener('click', () => createTab());
    strip.appendChild(newBtn);
}

// Pointer-driven horizontal reorder. Mirrors the 6px-threshold + pointer-
// capture pattern from initHScrollDrag(); during a drag we don't mutate
// tabs[] (renderTabs() would tear down the captured element), we just paint
// a drop indicator on the target neighbor and commit on release.
function setupTabDrag(btn, tab) {
    let pointerId = null;
    let startX = 0;
    let dragging = false;
    let dropBefore = null;  // tab id where dragging tab will land, or null = end

    const clearDropMarkers = () => {
        const strip = btn.parentNode;
        if (!strip) return;
        for (const el of strip.querySelectorAll('.tab-button')) {
            delete el.dataset.dropBefore;
        }
    };

    const computeDropTarget = (clientX) => {
        const strip = btn.parentNode;
        if (!strip) return null;
        for (const el of strip.querySelectorAll('.tab-button[data-tab-id]')) {
            if (el === btn) continue;
            const rect = el.getBoundingClientRect();
            if (clientX < rect.left + rect.width / 2) {
                return el.dataset.tabId;
            }
        }
        return null;  // past the last tab → drop at end
    };

    btn.addEventListener('pointerdown', (e) => {
        // Don't start a drag on the × close or on a rename input.
        if (e.target.closest('.tab-close, .tab-rename')) return;
        if (e.button !== 0) return;
        pointerId = e.pointerId;
        startX = e.clientX;
        dragging = false;
        dropBefore = null;
    });

    btn.addEventListener('pointermove', (e) => {
        if (pointerId !== e.pointerId) return;
        const dx = e.clientX - startX;
        if (!dragging && Math.abs(dx) > 6) {
            dragging = true;
            btn.dataset.dragging = '1';
            try { btn.setPointerCapture(pointerId); } catch {}
        }
        if (!dragging) return;
        e.preventDefault();
        const target = computeDropTarget(e.clientX);
        if (target !== dropBefore) {
            clearDropMarkers();
            dropBefore = target;
            if (target) {
                const el = btn.parentNode.querySelector(`.tab-button[data-tab-id="${target}"]`);
                if (el) el.dataset.dropBefore = '1';
            } else {
                // "drop at end" — mark the trailing + New button so the user
                // sees a consistent visual cue.
                const newBtn = btn.parentNode.querySelector('.tab-new');
                if (newBtn) newBtn.dataset.dropBefore = '1';
            }
        }
    });

    const endDrag = (e) => {
        if (pointerId !== e.pointerId) return;
        try { btn.releasePointerCapture(pointerId); } catch {}
        pointerId = null;
        if (!dragging) return;
        dragging = false;
        delete btn.dataset.dragging;
        clearDropMarkers();
        btn.dataset.suppressClick = '1';  // consumed by the click listener
        // Commit the reorder.
        const fromIdx = tabs.findIndex(t => t.id === tab.id);
        if (fromIdx < 0) return;
        let toIdx = dropBefore == null
            ? tabs.length
            : tabs.findIndex(t => t.id === dropBefore);
        if (toIdx < 0) toIdx = tabs.length;
        // Remove first, then compute the insertion index in the now-shorter
        // array so the math works whether moving left or right.
        const [moved] = tabs.splice(fromIdx, 1);
        if (toIdx > fromIdx) toIdx -= 1;
        if (toIdx === fromIdx) {
            // No actual movement -- just re-insert.
            tabs.splice(fromIdx, 0, moved);
            return;
        }
        tabs.splice(toIdx, 0, moved);
        renderTabs();
        persistTabs();
    };
    btn.addEventListener('pointerup', endDrag);
    btn.addEventListener('pointercancel', (e) => {
        if (pointerId !== e.pointerId) return;
        try { btn.releasePointerCapture(pointerId); } catch {}
        pointerId = null;
        dragging = false;
        delete btn.dataset.dragging;
        clearDropMarkers();
    });
}

function beginRename(id, btn, labelEl) {
    const tab = findTab(id);
    if (!tab) return;
    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'tab-rename';
    input.value = tab.name;
    input.spellcheck = false;
    const commit = (apply) => {
        if (input.parentNode) input.replaceWith(labelEl);
        if (apply) renameTab(id, input.value);
        else renderTabs();
    };
    input.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); commit(true); }
        else if (e.key === 'Escape') { e.preventDefault(); commit(false); }
    });
    input.addEventListener('blur', () => commit(true));
    labelEl.replaceWith(input);
    input.focus();
    input.select();
}

function hydrateTabs(defaultContent) {
    tabsHydrating = true;
    try {
        const stored = safeRead(STORAGE_KEYS.tabs);
        if (Array.isArray(stored) && stored.length > 0) {
            tabs = stored.map(t => ({
                id: typeof t.id === 'string' ? t.id : genTabId(),
                name: sanitizeTabName(t.name) || uniqueUntitledName(),
                content: typeof t.content === 'string' ? t.content : '',
                cursor: t.cursor && typeof t.cursor.lineNumber === 'number'
                    ? { lineNumber: t.cursor.lineNumber, column: t.cursor.column || 1 }
                    : { lineNumber: 1, column: 1 },
                scrollTop: typeof t.scrollTop === 'number' ? t.scrollTop : 0,
                createdAt: typeof t.createdAt === 'number' ? t.createdAt : Date.now(),
                _model: null,
            }));
            const storedActive = safeRead(STORAGE_KEYS.activeTab);
            activeId = (typeof storedActive === 'string' && findTab(storedActive))
                ? storedActive : tabs[0].id;
            return;
        }
        // Migration from legacy single-buffer keys.
        const legacyBuffer = safeRead(STORAGE_KEYS.buffer);
        if (typeof legacyBuffer === 'string') {
            const legacyCursor = safeRead(STORAGE_KEYS.cursor);
            const legacyScroll = safeRead(STORAGE_KEYS.scroll);
            tabs = [{
                id: genTabId(),
                name: 'main.tur',
                content: legacyBuffer,
                cursor: legacyCursor && typeof legacyCursor.lineNumber === 'number'
                    ? { lineNumber: legacyCursor.lineNumber, column: legacyCursor.column || 1 }
                    : { lineNumber: 1, column: 1 },
                scrollTop: typeof legacyScroll === 'number' ? legacyScroll : 0,
                createdAt: Date.now(),
                _model: null,
            }];
            activeId = tabs[0].id;
            // Write new keys before deleting legacy so a crash mid-migration
            // leaves the legacy data intact.
            safeWrite(STORAGE_KEYS.tabs, tabsSnapshot());
            safeWrite(STORAGE_KEYS.activeTab, activeId);
            try {
                localStorage.removeItem(STORAGE_KEYS.buffer);
                localStorage.removeItem(STORAGE_KEYS.cursor);
                localStorage.removeItem(STORAGE_KEYS.scroll);
            } catch {}
            return;
        }
        // Fresh workspace -- seed with the default Hello World tab.
        tabs = [{
            id: genTabId(),
            name: 'main.tur',
            content: defaultContent,
            cursor: { lineNumber: 1, column: 1 },
            scrollTop: 0,
            createdAt: Date.now(),
            _model: null,
        }];
        activeId = tabs[0].id;
    } finally {
        tabsHydrating = false;
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Append text to the console
 */
function appendToConsole(html) {
    // `html` is this file's own markup, with every dynamic part escaped at the
    // call site. It is parsed once, into a template, so the line that is shown
    // and the record that is persisted (consoleRuns) are the same nodes.
    const consoleEl = document.getElementById('console');
    const tpl = document.createElement('template');
    tpl.innerHTML = html;
    const runs = consoleRuns(tpl.content.childNodes);
    consoleEl.appendChild(tpl.content);
    consoleEl.appendChild(document.createElement('br'));
    consoleEl.scrollTop = consoleEl.scrollHeight;
    consoleLog.push(runs);
    if (consoleLog.length > MAX_CONSOLE_LINES) {
        consoleLog.splice(0, consoleLog.length - MAX_CONSOLE_LINES);
    }
    schedulePersistConsole();
}

const schedulePersistConsole = debounce(() => {
    safeWrite(STORAGE_KEYS.consol, consoleLog);
}, 500);

/**
 * Clear the console
 */
function clearConsole() {
    const consoleEl = document.getElementById('console');
    consoleEl.innerHTML = '';
    consoleLog = [];
    safeWrite(STORAGE_KEYS.consol, []);
}

/**
 * Copy console content to clipboard
 */
async function copyConsole() {
    const consoleEl = document.getElementById('console');
    const text = consoleEl.textContent;
    try {
        await navigator.clipboard.writeText(text);
        showStatus('Console copied to clipboard!', 'success');
    } catch (e) {
        showStatus('Failed to copy console', 'error');
    }
}

/**
 * Show status message
 */
function showStatus(message, type = 'info') {
    const statusEl = document.getElementById('wasm-status-text');
    const dotEl = document.querySelector('.dot');
    
    statusEl.textContent = message;
    
    if (dotEl) {
        dotEl.className = 'dot';
        if (type === 'success') dotEl.classList.add('online');
        else if (type === 'error') dotEl.classList.add('offline');
        else dotEl.classList.add('loading');
    }
    
    // Reset after a delay
    setTimeout(() => {
        if (wasmState() === WASM_STATE.READY) {
            statusEl.textContent = 'Ready';
            if (dotEl) {
                dotEl.className = 'dot online';
            }
        }
    }, 3000);
}

/**
 * Update cursor position display
 */
function updateCursorPosition() {
    if (!editor) return;
    const cursor = editor.getPosition();
    const posEl = document.getElementById('cursor-pos');
    if (posEl) {
        posEl.textContent = `Line ${cursor.lineNumber}, Column ${cursor.column}`;
    }
}

/**
 * Update execution time display
 */
function updateExecTime(timeMs) {
    const timeEl = document.getElementById('exec-time');
    if (timeEl) {
        timeEl.textContent = timeMs < 1 ? 'Ready' : `${timeMs.toFixed(0)}ms`;
    }
}

/**
 * Update URL hash with current code
 */
let urlHashSeq = 0;
async function updateUrlHash() {
    if (!editor) return;
    // Encoding is async; a slower, older encode must not land after a newer one.
    const seq = ++urlHashSeq;
    const encoded = await encodeShareCode(editor.getValue());
    if (encoded && seq === urlHashSeq) {
        // Merge rather than assign: the docs pane keeps a `doc=` key in the
        // same hash, and clobbering it would close the pane mid-read.
        setHashParam('code', encoded);
    }
}

/**
 * Load code from URL hash
 */
async function loadFromUrlHash() {
    const encoded = getHashParam('code');
    if (encoded) {
        const code = await decodeShareCode(encoded);
        if (code && editor && code !== editor.getValue()) {
            editor.setValue(code);
        }
    }
}

// ============================================================================
// WASM Module Loading
// ============================================================================

/**
 * Initialize the WASM module via the eval Worker.
 * All WASM calls (eval, format, doc lookup, reset) run inside the Worker so
 * that Atomics.wait is permitted and blocking select cannot freeze the tab.
 */
async function initWasm() {
    if (wasmState() !== WASM_STATE.INITIALIZING) return;

    setWasmState(WASM_STATE.LOADING);
    // performance-audit-plan WP5: boot marks, ms since navigation start. The
    // worker adds its own stage times on 'ready' (startEvalWorker).
    window._turiBoot = Object.assign(window._turiBoot || {}, { initWasmAt: performance.now() });

    const loadingOverlay = document.getElementById('loading-overlay');
    const wasmStatus = document.getElementById('wasm-status-text');
    if (loadingOverlay) loadingOverlay.style.display = 'flex';
    if (wasmStatus) wasmStatus.textContent = 'Loading WASM module...';

    try {
        await startEvalWorker();

        console.log('Turmeric WASM runtime initialized');
        setWasmState(WASM_STATE.READY);

        promptSetEnabled(true);

        await fetchDocNames();
        fetchLangRegistry();  // re-renders the picker from the C-side tables
        showStatus('Ready', 'success');
        window._turiBoot = Object.assign(window._turiBoot || {}, { statusReadyAt: performance.now() });
        loadFromUrlHash();

        if (loadingOverlay) loadingOverlay.style.display = 'none';

    } catch (error) {
        console.error('Failed to initialize WASM:', error);
        setWasmState(WASM_STATE.ERROR);
        if (loadingOverlay) loadingOverlay.style.display = 'none';

        if (error.message === 'shared-array-buffer-unavailable') {
            showStatus('Browser not supported', 'error');
            appendToConsole(
                '<span class="console-error">The Try Turmeric playground requires a browser with ' +
                '<a href="https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer#browser_compatibility" ' +
                'target="_blank" rel="noopener" style="color:inherit">SharedArrayBuffer</a> support.<br>' +
                'On iOS, please update to iOS 15.4 or later and use Safari.<br>' +
                'On desktop, Chrome, Firefox, and Edge all work.</span>'
            );
        } else {
            showStatus('Failed to load WASM', 'error');
            appendToConsole('<span class="console-error">Error: Failed to load WASM module. Please refresh the page.</span>');
        }
    }
}

/**
 * Evaluate Turmeric code via the eval Worker.
 */
function evaluateCode(code, { quiet = false } = {}) {
    return new Promise((resolve, reject) => {
        executionQueue.push({ code, quiet, resolve, reject });
        if (!isExecuting) processQueue();
    });
}

/**
 * Process the execution queue, sending one eval at a time to the Worker.
 * The Worker message handler calls setTimeout(processQueue, 0) when a result
 * arrives so the next queued item is dispatched on the next tick.
 */
function processQueue() {
    if (executionQueue.length === 0) {
        isExecuting = false;
        return;
    }

    isExecuting = true;

    const { code, quiet, resolve, reject } = executionQueue.shift();

    if (wasmState() !== WASM_STATE.READY) {
        reject(new Error('WASM not ready'));
        updateExecTime(0);
        setTimeout(processQueue, 0);
        return;
    }

    // turi_eval_typed detects and strips an inline #lang directive itself, so
    // pass the raw source through, and forward the base to the Worker as a hint
    // for runtimes that export _turi_wasm_set_lang. The base IS the whole
    // directive tail now -- set_lang rejects anything after it.
    const { lang } = parseLangDirective(code);

    send({ type: 'eval', input: code, lang, quiet }, { resolve, reject, isEval: true });
}

// ============================================================================
// Eval watchdog and Stop (security-audit-plan W-3)
//
// The interpreter runs in a Worker, so a runaway program cannot freeze the
// page -- but the Worker handles one request at a time, so it freezes the REPL:
// every later Run, :doc and Format queues behind it for good. A Worker cannot
// be interrupted from outside, only terminated, so that is what Stop does:
// terminate it, fail everything that was waiting on it, and boot a fresh one.
// The session -- definitions from earlier runs -- goes with the old Worker.
//
// The Worker is serial, so the oldest outstanding request is the one it is
// running, and that request's age is the watchdog's clock: past
// EVAL_STOP_OFFER_MS the Stop button is shown, past CONFIG.EXECUTION_TIMEOUT
// the Worker is stopped without asking.
// ============================================================================

const EVAL_STOP_OFFER_MS = 1000;
const EVAL_WATCHDOG_TICK_MS = 250;
let evalWatchdogTimer = null;
let evalWatchdogShowedPanel = false;
let evalStopping = false;

function armEvalWatchdog() {
    if (evalWatchdogTimer === null) {
        evalWatchdogTimer = setInterval(evalWatchdogTick, EVAL_WATCHDOG_TICK_MS);
    }
}

function evalWatchdogTick() {
    const panel = document.getElementById('console-loading');
    const stopBtn = document.getElementById('stop-btn');
    const { count, oldestStart } = pendingStats();
    if (count === 0) {
        clearInterval(evalWatchdogTimer);
        evalWatchdogTimer = null;
        if (stopBtn) stopBtn.hidden = true;
        // Only a panel the watchdog opened; a Run's own spinner is executeCode's.
        if (evalWatchdogShowedPanel && panel) panel.style.display = 'none';
        evalWatchdogShowedPanel = false;
        return;
    }
    const age = performance.now() - oldestStart;
    if (age >= CONFIG.EXECUTION_TIMEOUT) {
        stopEvalWorker(`Stopped: still running after ${CONFIG.EXECUTION_TIMEOUT / 1000} s`);
    } else if (age >= EVAL_STOP_OFFER_MS) {
        if (stopBtn) stopBtn.hidden = false;
        if (panel && panel.style.display === 'none') {
            panel.style.display = 'flex';
            evalWatchdogShowedPanel = true;
        }
    }
}

/**
 * Terminate the eval Worker and start a fresh one. Every request it owed an
 * answer -- and every Run still queued behind them -- is rejected with
 * `reason`, which is what the console shows for the Run that was stopped.
 */
function stopEvalWorker(reason) {
    if (!hasWorker() || evalStopping) return;
    evalStopping = true;
    const owed = terminateWorker();
    setWasmState(WASM_STATE.LOADING);
    promptSetEnabled(false);

    const queued = executionQueue.splice(0);
    isExecuting = false;

    // What the old session had accepted is gone with it.
    replSessionReset();
    lastRunByTab.clear();

    const err = new Error(reason);
    for (const call of owed) call.reject(err);
    for (const run of queued) run.reject(err);
    evalWatchdogTick();   // nothing is pending now: hides Stop and its panel
    showStatus('Restarting...', 'info');

    startEvalWorker().then(() => {
        setWasmState(WASM_STATE.READY);
        promptSetEnabled(true);
        appendToConsole('<span class="console-output">Started a fresh session '
                        + '-- definitions from earlier runs are gone.</span>');
        showStatus('Ready', 'success');
    }, (e) => {
        setWasmState(WASM_STATE.ERROR);
        console.error('Failed to restart the WASM worker:', e);
        showStatus('Failed to load WASM', 'error');
        appendToConsole('<span class="console-error">Error: could not restart the '
                        + 'interpreter. Please refresh the page.</span>');
    }).finally(() => {
        evalStopping = false;
    });
}

/**
 * Reset the WASM environment via the eval Worker.
 */
function resetWasm() {
    if (wasmState() !== WASM_STATE.READY) return;

    send({ type: 'reset' }, {
        resolve: () => {
            /* The session forgot everything it had accepted, so the prompt's
             * document has to forget it too -- otherwise completion keeps
             * offering names `:reset` just destroyed. */
            replSessionReset();
            lastRunByTab.clear();
            clearConsole();
            showStatus('Environment reset', 'success');
        },
        reject:  (err) => { console.error('Reset error:', err); showStatus('Failed to reset', 'error'); },
    });
}

/**
 * Initialize Monaco Editor
 */
async function initEditor() {
    // Hide loading spinner
    const editorLoading = document.getElementById('editor-loading');
    if (editorLoading) editorLoading.style.display = 'none';
    
    // Create editor
    editor = monaco.editor.create(document.getElementById('editor'), {
        value: CONFIG.DEFAULT_CODE,
        language: 'turmeric',
        theme: monacoThemeName(),
        automaticLayout: true,
        tabSize: 2,
        insertSpaces: true,
        detectIndentation: false,
        minimap: {
            // Resolved for real by applyMinimapPreference() below, once the
            // editor exists and its width can be measured. Starting false and
            // turning it on is the cheap direction: an editor that renders the
            // strip and then yanks it away on the first layout pass flickers.
            enabled: false,
            // Blocks, not glyphs. c2mp reached the same conclusion the hard
            // way (minimap.js:8-13): no browser hints glyphs at 2px, so
            // rendered characters are platform-dependent mush. Blocks are
            // legible and cheap.
            renderCharacters: false,
            showSlider: 'mouseover',
            size: 'proportional',
            maxColumn: 80,
        },
        scrollbar: {
            vertical: 'auto',
            horizontal: 'auto',
            verticalScrollbarSize: 8,
            horizontalScrollbarSize: 8
        },
        lineNumbers: 'on',
        lineDecorationsWidth: 10,
        folding: true,
        wordWrap: 'on',
        autoClosingBrackets: 'beforeWhitespace',
        autoClosingQuotes: 'beforeWhitespace',
        autoSurround: 'never',
        bracketMatching: true,
        bracketPairColorization: {
            enabled: true,
            independentColorPoolPerBracketType: false
        },
        guides: {
            bracketPairs: true,
            bracketPairsHorizontal: 'active',
            highlightActiveBracketPair: true
        },
        colorDecorators: true,
        contextmenu: true,
        cursorBlinking: 'blink',
        cursorStyle: 'line',
        disableLayerHinting: true,
        disableMonospaceOptimizations: false,
        dragAndDrop: true,
        find: {
            seedSearchStringFromSelection: true
        },
        hover: {
            enabled: true
        },
        links: true,
        mouseWheelZoom: false,
        multiCursorModifier: 'ctrlCmd',
        multiCursorPaste: 'full',
        occurrencesHighlight: true,
        overviewRulerBorder: false,
        // Three lanes is what puts error and warning ticks in the right-hand
        // strip -- the affordance c2mp builds by hand (minimap.js RULER_W).
        // Unlike the minimap this is not width-gated: it is a few pixels wide
        // at any pane size, and a diagnostic you can find by looking is worth
        // those pixels on a phone too.
        overviewRulerLanes: 3,
        quickSuggestions: {
            other: true,
            comments: false,
            strings: false
        },
        readOnly: false,
        roundedSelection: true,
        scrollBeyondLastColumn: 5,
        scrollBeyondLastLine: true,
        selectOnLineNumbers: true,
        selectionClipboard: true,
        selectionHighlight: true,
        showFoldingControls: 'mouseover',
        smoothScrolling: true,
        suggest: {
            showWords: true,
            showSnippets: true,
            showFiles: false
        },
        suggestOnTriggerCharacters: true,
        wordBasedSuggestions: true,
        fontFamily: "'Iosevka', 'SF Mono', 'Fira Code', 'Fira Mono', Consolas, monospace",
        fontSize: 13,
        fontLigatures: true,
        fontWeight: '400'
    });
    
    // Expose editor for smoke tests
    window._turiEditor = editor;
    // T3: line-number clicks jump the timeline while a recording is open.
    traceInstallGutterHandler(editor);
    // Timeline test surface, alongside _turiTabs below.
    // Eval watchdog test surface (W-3): the limit is settable so a test can
    // watch the automatic stop without waiting out the real 30 s.
    window._turiEval = {
        get timeoutMs() { return CONFIG.EXECUTION_TIMEOUT; },
        set timeoutMs(ms) { CONFIG.EXECUTION_TIMEOUT = ms; },
    };
    window._turiTrace = {
        state:   () => ({ active: traceState.active, steps: traceState.steps,
                          index: traceState.index, baseLine: traceState.baseLine,
                          frames: traceState.frames }),
        run:     () => traceCode(),
        seek:    (i) => traceSeek(i),
        close:   () => traceClose(),
    };
    // Multi-tab test surface. Read-only `tabs()` snapshot keeps tests from
    // accidentally mutating module state.
    window._turiTabs = {
        tabs: () => tabsSnapshot(),
        activeId: () => activeId,
        create: (opts) => createTab(opts),
        close:  (id) => closeTab(id),
        switch: (id) => switchTab(id),
        rename: (id, name) => renameTab(id, name),
        reorder: (id, toIdx) => {
            const fromIdx = tabs.findIndex(t => t.id === id);
            if (fromIdx < 0 || toIdx < 0 || toIdx > tabs.length - 1) return;
            const [m] = tabs.splice(fromIdx, 1);
            tabs.splice(toIdx, 0, m);
            renderTabs();
            persistTabs();
        },
        // Phase 2 surface: lets specs assert the logical project layout
        // without intercepting an actual browser download.
        projectEntries: () => buildProjectEntries().entries,
        // Same data as projectEntries(), but encoded into the zip bytes the
        // user would actually download. The test surface returns a plain
        // Array<number> because Playwright's evaluate() does not transfer
        // Uint8Array cleanly across the wire.
        projectZipBytes: async () => {
            const { entries } = buildProjectEntries();
            const blob = buildZip(entries);
            return Array.from(new Uint8Array(await blob.arrayBuffer()));
        },
        // Phase 3 surface: parse + load entry points for specs that need
        // to drive a fake zip into the app without juggling File objects.
        // `loadBytes` is fire-and-forget from the test's perspective; the
        // confirm() dialog is auto-accepted via page.on('dialog').
        loadBytes: async (bytes) => {
            const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
            return loadProjectFromBytes(u8, 'spec.zip');
        },
        parseBytes: (bytes) => {
            const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
            return parseProjectZip(u8);
        },
    };

    // Navigation test surface (M4): read-only buffers and the jump-back stack.
    window._turiNav = {
        readOnlyTabs: () => tabs.filter(t => t.readOnly)
            .map(t => ({ id: t.id, name: t.name, sourceUri: t.sourceUri })),
        stackDepth: () => jumpStack.length,
        back: () => jumpBack(),
        // Editor-level truth, not the tab record: what a spec cares about is
        // whether typing would do anything.
        isReadOnly: () => {
            try { return !!editor.getOption(monaco.editor.EditorOption.readOnly); }
            catch { return false; }
        },
    };

    // Hydrate the tab set. Priority order:
    //   1. URL hash share-link (handled later in loadFromUrlHash; it overwrites
    //      the active tab's content).
    //   2. localStorage multi-tab set (or migration from legacy single-buffer).
    //   3. Default Hello World tab.
    // Tutorial mode owns a single ephemeral tab and bypasses persistence.
    if (isTutorialMode()) {
        tabs = [{
            id: genTabId(),
            name: 'tutorial.tur',
            content: editor.getValue(),
            cursor: { lineNumber: 1, column: 1 },
            scrollTop: 0,
            createdAt: Date.now(),
            _model: null,
        }];
        activeId = tabs[0].id;
        tabsHydrating = true;  // permanently suppress persistence in tutorial mode
    } else {
        hydrateTabs(CONFIG.DEFAULT_CODE);
    }
    // Attach the active tab's model to the editor. The initial editor.create
    // call gave us a throwaway model with CONFIG.DEFAULT_CODE; swapping to the
    // active tab's model replaces it so all subsequent reads/writes go to the
    // tab record.
    const active = currentTab();
    if (active) {
        const orphan = editor.getModel();
        editor.setModel(ensureModel(active));
        if (orphan && !orphan.isDisposed()) {
            try { orphan.dispose(); } catch {}
        }
        try {
            if (active.cursor) editor.setPosition(active.cursor);
            if (typeof active.scrollTop === 'number') editor.setScrollTop(active.scrollTop);
        } catch {}
    }
    renderTabs();
    hydrateConsole();

    // Update cursor position display + per-tab cursor persistence.
    const persistCursor = debounce(() => {
        const t = currentTab();
        if (!t) return;
        const pos = editor.getPosition();
        if (pos) t.cursor = { lineNumber: pos.lineNumber, column: pos.column };
        persistTabs();
    }, 500);
    editor.onDidChangeCursorPosition(() => { updateCursorPosition(); persistCursor(); });

    // Per-tab scroll persistence.
    const persistScroll = debounce(() => {
        const t = currentTab();
        if (!t) return;
        t.scrollTop = editor.getScrollTop();
        persistTabs();
    }, 500);
    editor.onDidScrollChange(persistScroll);

    // Update URL hash on content change (debounced) + persist active tab.
    const persistContent = debounce(() => {
        const t = currentTab();
        if (!t) return;
        t.content = editor.getValue();
        persistTabs();
    }, 250);
    let urlHashTimer;
    editor.onDidChangeModelContent(() => {
        persistContent();
        clearTimeout(urlHashTimer);
        urlHashTimer = setTimeout(updateUrlHash, 1000);
        // Round-trip the picker from the text: typing the header by hand and
        // using the picker are the same operation.  Reads line 1 only.
        reconcileLangPicker();
    });
    
    // Handle Ctrl+Enter to run code
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter,
        () => runCode()
    );
    
    // Handle Ctrl+S to save (update URL hash)
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS,
        (e) => {
            e.preventDefault();
            updateUrlHash();
            showStatus('Code saved to URL', 'success');
        }
    );

    // Handle Alt+Shift+F to format
    editor.addCommand(
        monaco.KeyMod.Alt | monaco.KeyMod.Shift | monaco.KeyCode.KeyF,
        () => formatCode()
    );

    // F12 -> go to definition. Cmd/Ctrl+click already works through the
    // provider; F12 is the binding people reach for without a mouse, and
    // Monaco standalone does not bind it by default.
    editor.addCommand(monaco.KeyCode.F12, () => {
        try { editor.getAction('editor.action.revealDefinition')?.run(); } catch {}
    });

    // Ctrl/Cmd+Alt+- -> back to where the last jump started. Monaco standalone
    // has no navigation history service, so this drives our own stack.
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyMod.Alt | monaco.KeyCode.Minus,
        () => jumpBack()
    );

    // Register as Monaco document formatter so "Format Document" also works
    monaco.languages.registerDocumentFormattingEditProvider('turmeric', {
        provideDocumentFormattingEdits() {
            return formatCode().then(() => []);
        }
    });
    
    // Boot the language server the first time the user puts a cursor in the
    // editor. Reading code costs nothing; asking the editor a question is what
    // pays for the second WASM instance.
    const bootLsp = editor.onDidFocusEditorText(() => {
        bootLsp.dispose();
        startLspClient();
    });

    // Initialize cursor position display
    updateCursorPosition();
    
    // Store editor reference for global access
    window.turmericEditor = editor;
}

// ============================================================================
// Event Handlers
// ============================================================================

/**
 * Shared eval + output path used by both the Run button and the REPL input.
 * @param {string} code           Source to evaluate
 * @param {string} promptHtml     HTML for the prompt prefix (e.g. '<span ...>></span>')
 * @param {boolean} showTiming    Whether to call updateExecTime and show the loading indicator
 * @param {boolean} echoSource    Whether to echo the source to the console
 * @param {boolean} suppressResult Whether to suppress the result line (errors still shown).
 *                                 Used by the Run button to hide the bare `#<fn main>`
 *                                 closure while it defines `main`, before invoking it.
 * @returns {{ isError: boolean }} Whether the evaluation reported an error.
 */
async function executeCode(source, promptHtml, showTiming = false, echoSource = true, suppressResult = false) {
    const consoleLoading = showTiming ? document.getElementById('console-loading') : null;
    if (consoleLoading) consoleLoading.style.display = 'flex';

    if (echoSource) {
        appendToConsole(`${promptHtml} ${escapeHtml(source)}`);
    }

    try {
        const startTime = performance.now();
        const { result, isError } = await evaluateCode(source);
        const execTime = performance.now() - startTime;

        if (consoleLoading) consoleLoading.style.display = 'none';

        if (isError) {
            appendToConsole(`<span class="console-error">${escapeHtml(result)}</span>`);
        } else if (!suppressResult && result && result !== 'nil') {
            appendToConsole(`<span class="console-result">${escapeHtml(result)}</span>`);
        }

        if (showTiming) updateExecTime(execTime);
        maybeShowDoc(source.trim());

        return { isError };

    } catch (err) {
        if (consoleLoading) consoleLoading.style.display = 'none';
        appendToConsole(`<span class="console-error">Error: ${escapeHtml(err.message)}</span>`);
        if (showTiming) updateExecTime(0);
        return { isError: true };
    }
}

/**
 * Detect whether a program defines a top-level `main` function, in either
 * s-expression (`(defn main ...)`) or sweet-exp (`defn main ...`) form. Used to
 * mirror `tur run`: a program that defines `main` should have it invoked as the
 * entry point, rather than the REPL leaving the bare `#<fn main>` closure as the
 * last top-level value.
 */
function definesMainEntry(code) {
    return /(^|\n)[ \t]*\(?defn\s+main\b/.test(code);
}

/**
 * Run the current editor contents
 */
async function runCode() {
    if (wasmState() !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = editor.getValue();
    if (!code.trim()) {
        appendToConsole('<span class="console-error">Error: No code to evaluate</span>');
        return;
    }

    /* PS5: Run runs THIS program -- not this program appended to whatever
     * the session already holds.  Rewind to the stdlib, bring back the other
     * tabs' last successful runs, then run the buffer. */
    const tabId = activeId;
    await rewindSessionForRun(tabId, code);

    // Mirror `tur run`: a program that defines a top-level `main` uses it as its
    // entry point. Evaluate the program's top-level forms (which define `main`),
    // suppressing the bare `#<fn main>` closure result, then invoke `(main)` and
    // show ITS output/return -- so running e.g. `(defn main [] : int (+ 1 1))`
    // shows `2`, not `#<fn main>`.
    if (definesMainEntry(code)) {
        const { isError } = await executeCode(code, '', true, false, true);
        recordRun(tabId, code, isError);
        if (!isError) await executeCode('(main)', '', false, false);
        return;
    }

    const { isError } = await executeCode(code, '', true, false);
    recordRun(tabId, code, isError);
}

/**
 * Note the outcome of a tab's Run.  W2: a Run is how a tab's definitions become
 * callable at the prompt, so it is also how they become offerable there. A Run
 * that failed defined nothing the next rewind can bring back.
 */
function recordRun(tabId, code, isError) {
    if (isError) {
        lastRunByTab.delete(tabId);
        return;
    }
    lastRunByTab.set(tabId, code);
    replSessionAccept(code);
}

/**
 * Rewind the session to the preloaded stdlib and replay the other tabs' last
 * runs, so the tab about to run starts from exactly those definitions.
 *
 * Replayed runs are muted: their output was already shown when they ran, and
 * a replay is bookkeeping, not a run. Only tabs in the running tab's dialect
 * are replayed -- a `#lang` switch rewinds the session itself, so a tab in
 * another dialect would wipe everything replayed before it. Definitions typed
 * at the prompt are not replayed: Run means "run this program", and the
 * program is the buffer.
 */
async function rewindSessionForRun(tabId, code) {
    await rewindWasm();
    replSessionReset();
    const dialect = parseLangDirective(code).lang;
    for (const t of tabs) {
        if (t.id === tabId) continue;
        const src = lastRunByTab.get(t.id);
        if (src === undefined || parseLangDirective(src).lang !== dialect) continue;
        let isError = true;
        try {
            ({ isError } = await evaluateCode(src, { quiet: true }));
        } catch (_) { /* treated as a failed replay */ }
        recordRun(t.id, src, isError);
    }
}

/**
 * Rewind the WASM session to the preloaded stdlib (PS5). Unlike resetWasm, the
 * env and its stdlib stay; only what turns added since is dropped.
 */
function rewindWasm() {
    return new Promise((resolve) => {
        send({ type: 'rewind' }, {
            resolve: () => resolve(true),
            reject: () => resolve(false),
        });
    });
}

/**
 * Format the editor contents via the eval Worker.
 */
async function formatCode() {
    if (wasmState() !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = editor.getValue();
    if (!code.trim()) return;

    try {
        const formatted = await new Promise((resolve, reject) => {
            send({ type: 'format', input: code }, { resolve, reject });
        });
        editor.setValue(formatted);
        showStatus('Formatted', 'success');
    } catch (err) {
        console.error('Format error:', err);
        showStatus('Format failed', 'error');
    }
}

/**
 * Clear the editor
 */
function clearEditor() {
    if (editor) {
        editor.setValue('');
    }
}

/**
 * Load an example
 */
function loadExample(name) {
    if (EXAMPLES[name]) {
        if (editor) {
            editor.setValue(EXAMPLES[name]);
        }
        updateUrlHash();
        showStatus(`Loaded example: ${name}`, 'success');
    }
}

// ============================================================================
// Project download (Phase 2 of try-turmeric-multi-tab-and-projects-plan)
// ============================================================================

// Slugify a tab name into something tur build will accept as a source
// filename. Strips path separators and shell-unfriendly characters,
// guarantees the .tur extension, and disambiguates collisions.
function slugifyTabName(raw, used) {
    let name = (raw || 'untitled.tur').trim().replace(/[\/\\<>:"|?*\x00-\x1f]/g, '_');
    if (!/\.tur$/i.test(name)) name += '.tur';
    if (!used.has(name)) return name;
    const base = name.replace(/\.tur$/i, '');
    for (let n = 2; n < 10000; n++) {
        const candidate = `${base}-${n}.tur`;
        if (!used.has(candidate)) return candidate;
    }
    return `untitled-${Date.now()}.tur`;
}

// Build the project zip's logical entries from the current tab set. Exposed
// (via a wrapper below) so Playwright can drive it without intercepting the
// browser download.
function buildProjectEntries() {
    captureActiveTabState();
    // Read-only stdlib buffers are not the user's files. A downloaded project
    // zip that carried copies of list.tur would build differently from the
    // workspace it came from -- the real stdlib is already on the path.
    const snapshot = tabsSnapshot().filter(t => !t.readOnly);
    const used = new Set();
    const files = snapshot.map(t => {
        const fileName = slugifyTabName(t.name, used);
        used.add(fileName);
        return { fileName, tab: t };
    });
    // Pick the main: prefer main.tur if present, else the first tab.
    const mainEntry = files.find(f => f.fileName === 'main.tur') || files[0];

    const buildTur = [
        '(project',
        '  :name "try-turmeric-project"',
        `  :main "${mainEntry.fileName}"`,
        `  :src [${files.map(f => `"${f.fileName}"`).join(' ')}]`,
        ')',
        '',
    ].join('\n');

    const workspaceJson = JSON.stringify({
        version: 1,
        activeId: activeId,
        tabs: files.map(f => ({
            id: f.tab.id,
            name: f.tab.name,
            file: `src/${f.fileName}`,
            cursor: f.tab.cursor,
            scrollTop: f.tab.scrollTop,
            createdAt: f.tab.createdAt,
        })),
    }, null, 2);

    const entries = [
        { path: 'build.tur', content: buildTur },
        ...files.map(f => ({ path: `src/${f.fileName}`, content: f.tab.content })),
        { path: '.turmeric/workspace.json', content: workspaceJson },
    ];
    return { entries, mainEntry };
}

function downloadProject() {
    const { entries } = buildProjectEntries();
    const blob = buildZip(entries);
    const ts = new Date().toISOString().replace(/[:.]/g, '-').replace(/-\d{3}Z$/, 'Z');
    const filename = `try-turmeric-${ts}.zip`;
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    // Free the blob URL after the click has had a chance to start the
    // download -- some browsers will revoke too eagerly otherwise.
    setTimeout(() => URL.revokeObjectURL(url), 0);
    showStatus(`Downloaded ${filename}`, 'success');
}

// ============================================================================
// Project load (Phase 3 of try-turmeric-multi-tab-and-projects-plan)
// ============================================================================

// Take the raw entry map and shape it into a Tab[] (no Monaco models yet --
// those get created when the project actually applies). Returns
// `{ tabs, activeId }` or throws a user-readable error.
function parseProjectZip(bytes) {
    const dec = new TextDecoder('utf-8');
    const raw = readStoreZip(bytes);
    // Pull workspace.json first (optional) -- a missing one is fine.
    let ws = null;
    if (raw['.turmeric/workspace.json']) {
        try { ws = JSON.parse(dec.decode(raw['.turmeric/workspace.json'])); }
        catch { ws = null; }
    }
    // Collect *.tur entries under src/.
    const srcFiles = {};
    for (const path of Object.keys(raw)) {
        if (!path.startsWith('src/')) continue;
        if (!/\.tur$/i.test(path)) continue;
        if (path.length === 'src/'.length) continue;
        srcFiles[path] = dec.decode(raw[path]);
    }
    // Reject the zip only when there's nothing meaningful to load -- per
    // the plan, "a zip whose src/ is empty OR whose only .tur files are
    // 0 bytes is rejected." Individual zero-byte tabs in a mixed zip are
    // kept; the user gets a blank tab to fill in.
    const fileNames = Object.keys(srcFiles);
    if (fileNames.length === 0) {
        throw new Error('ZIP contains no .tur source files under src/');
    }
    if (!fileNames.some(f => srcFiles[f].length > 0)) {
        throw new Error('ZIP src/ contains only empty .tur files');
    }
    // Build the tab list. When workspace.json is present we honor its order
    // and metadata for matching files; any extra src/ entries get appended
    // alphabetically so nothing is silently dropped.
    const seen = new Set();
    const tabsOut = [];
    if (ws && Array.isArray(ws.tabs)) {
        for (const t of ws.tabs) {
            const file = typeof t.file === 'string' ? t.file : null;
            if (!file || !(file in srcFiles)) continue;  // referenced file missing
            const content = srcFiles[file];
            tabsOut.push({
                id: typeof t.id === 'string' ? t.id : genTabId(),
                name: sanitizeTabName(t.name || file.replace(/^src\//, '')) || 'main.tur',
                content,
                cursor: t.cursor && typeof t.cursor.lineNumber === 'number'
                    ? { lineNumber: t.cursor.lineNumber, column: t.cursor.column || 1 }
                    : { lineNumber: 1, column: 1 },
                scrollTop: typeof t.scrollTop === 'number' ? t.scrollTop : 0,
                createdAt: typeof t.createdAt === 'number' ? t.createdAt : Date.now(),
                _model: null,
            });
            seen.add(file);
        }
    }
    const leftovers = Object.keys(srcFiles)
        .filter(f => !seen.has(f))
        .sort();
    for (const file of leftovers) {
        tabsOut.push({
            id: genTabId(),
            name: sanitizeTabName(file.replace(/^src\//, '')) || 'untitled.tur',
            content: srcFiles[file],
            cursor: { lineNumber: 1, column: 1 },
            scrollTop: 0,
            createdAt: Date.now(),
            _model: null,
        });
    }
    // Pick the active tab. workspace.json's activeId wins if it points at a
    // tab we kept; else main.tur; else alphabetical first.
    let activeIdOut = null;
    if (ws && typeof ws.activeId === 'string') {
        if (tabsOut.find(t => t.id === ws.activeId)) activeIdOut = ws.activeId;
    }
    if (!activeIdOut) {
        const main = tabsOut.find(t => t.name === 'main.tur');
        activeIdOut = main ? main.id
            : [...tabsOut].sort((a, b) => a.name.localeCompare(b.name))[0].id;
    }
    return { tabs: tabsOut, activeId: activeIdOut };
}

// Replace the current tab set with `parsed`. Persistence and UI re-render
// happen synchronously; callers (drag-drop, file picker) are responsible
// for the user confirmation step before invoking this.
function applyProjectLoad(parsed) {
    // Dispose old models so we don't leak them into Monaco's global model
    // store.
    for (const t of tabs) {
        if (t._model && !t._model.isDisposed()) {
            try { t._model.dispose(); } catch {}
        }
    }
    tabs = parsed.tabs;
    activeId = parsed.activeId;
    // Every origin the stack held belonged to the workspace that just went
    // away. jumpBack() would skip them one by one; clearing says so up front,
    // and takes the Back button down with it.
    jumpStack = [];
    renderJumpBack();
    const active = currentTab();
    if (active && editor) {
        editor.setModel(ensureModel(active));
        applyReadOnlyState(active);
        try {
            if (active.cursor) editor.setPosition(active.cursor);
            if (typeof active.scrollTop === 'number') editor.setScrollTop(active.scrollTop);
        } catch {}
    }
    renderTabs();
    safeWrite(STORAGE_KEYS.tabs, tabsSnapshot());
    safeWrite(STORAGE_KEYS.activeTab, activeId);
    // A wholesale replacement, not an edit. Re-opening the new tabs into a
    // session that still holds the old project's documents would leave
    // workspace/symbol answering with names from a workspace the user closed.
    if (lspClient && lspClient.isAvailable()) {
        ensureAllTabModels();
        lspClient.resetWorkspace();
    }
    showStatus(`Loaded ${tabs.length} tab${tabs.length === 1 ? '' : 's'}`, 'success');
}

// Common entry point: take bytes (Uint8Array) from a File / drop / picker,
// validate, prompt to replace, apply. Resolves when the operation is done.
async function loadProjectFromBytes(bytes, sourceLabel = 'project') {
    if (bytes.length > MAX_ZIP_BYTES) {
        showStatus(`${sourceLabel} is too large (${(bytes.length / 1024 / 1024).toFixed(1)} MB; limit 5 MB)`, 'error');
        return false;
    }
    let parsed;
    try {
        parsed = parseProjectZip(bytes);
    } catch (e) {
        showStatus(`${sourceLabel}: ${e.message}`, 'error');
        return false;
    }
    // Replace, not merge. Implementation note from the plan: don't write
    // the new tab set to localStorage until the user confirms -- a reject
    // leaves the existing tab set intact.
    const ok = confirm(
        `Loading this project will replace your current ${tabs.length} ` +
        `tab${tabs.length === 1 ? '' : 's'}. The current workspace is in ` +
        `your browser's localStorage and can be recovered by reloading ` +
        `without confirming.`);
    if (!ok) return false;
    applyProjectLoad(parsed);
    return true;
}

async function loadProjectFromFile(file) {
    if (!file) return false;
    const bytes = new Uint8Array(await file.arrayBuffer());
    return loadProjectFromBytes(bytes, file.name || 'file');
}

function openProjectFile() {
    const input = document.getElementById('open-project-input');
    if (input) {
        input.value = '';  // allow re-selecting the same file
        input.click();
    }
}

// Drag-and-drop overlay on the editor pane. dragenter / dragover are the
// signals; the visible overlay sits inside .editor-pane and toggles via a
// .drag-over class. dragleave fires too eagerly on child enter/leave, so
// we count enters / leaves and only clear the overlay when the count hits
// zero -- the standard pattern for whole-element drop targets.
function initProjectDrop() {
    const pane = document.querySelector('.editor-pane');
    if (!pane) return;
    let depth = 0;
    const isZipDrag = (e) => Array.from(e.dataTransfer?.types || []).includes('Files');
    pane.addEventListener('dragenter', (e) => {
        if (!isZipDrag(e)) return;
        e.preventDefault();
        depth++;
        pane.classList.add('drag-over');
    });
    pane.addEventListener('dragover', (e) => {
        if (!isZipDrag(e)) return;
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
    });
    pane.addEventListener('dragleave', (e) => {
        if (!isZipDrag(e)) return;
        depth = Math.max(0, depth - 1);
        if (depth === 0) pane.classList.remove('drag-over');
    });
    pane.addEventListener('drop', async (e) => {
        if (!isZipDrag(e)) return;
        e.preventDefault();
        depth = 0;
        pane.classList.remove('drag-over');
        const file = e.dataTransfer?.files?.[0];
        if (!file) return;
        if (!/\.zip$/i.test(file.name) && file.type !== 'application/zip') {
            showStatus(`${file.name}: not a .zip file`, 'error');
            return;
        }
        await loadProjectFromFile(file);
    });
}

/**
 * Share the current code
 */
async function shareCode() {
    if (!editor) return;
    
    const code = editor.getValue();
    const encoded = await encodeShareCode(code);
    
    if (encoded) {
        const url = `${window.location.origin}${window.location.pathname}#code=${encoded}`;
        
        // Copy to clipboard
        navigator.clipboard.writeText(url).then(() => {
            showStatus('URL copied to clipboard!', 'success');
        }).catch(() => {
            // Fallback: show the URL
            prompt('Copy this URL:', url);
        });
    } else {
        showStatus('Failed to encode code', 'error');
    }
}

/* ---------------------------------------------------------------------------
 * W1/W2: the REPL prompt
 *
 * The prompt was a bare <input> with Enter-to-submit and ArrowUp/ArrowDown
 * history, and nothing the language server knows reached it -- no completion,
 * no hover, no signature help, in the one place a beginner types the most.
 *
 * The fix is not a completion widget built over the <input>. It is a
 * single-line Monaco editor, because then completion, hover and signature help
 * at the prompt are *the providers that already exist* rather than a second
 * copy of the state machine that drives them.
 * ------------------------------------------------------------------------ */

/** The prompt's Monaco editor, or null before init. */
let promptEditor = null;

/** The synthetic document the prompt is analysed as (W2). */
const REPL_DOC_URI = 'file:///project/repl.tur';
let promptDoc = null;

/**
 * Source the interpreter session has actually accepted -- previous prompt
 * lines and Runs that evaluated without error.
 *
 * This, and not the active tab, is what the prompt may offer. The editor's
 * index is built from whatever file is open; the prompt evaluates against the
 * wasm session, and a `defn` typed in a tab that has never been Run is not
 * callable here. An offered name the session cannot resolve is not merely
 * unhelpful: accepting it produces an expression that fails to evaluate.
 */
let replSessionSource = '';

function replSessionAccept(source) {
    const text = String(source || '').trim();
    if (!text) return;
    replSessionSource = replSessionSource ? replSessionSource + '\n' + text : text;
    // Push it now rather than on the debounce, so the very next keystroke's
    // completion already knows about the name that was just defined.
    if (promptDoc) promptDoc.refresh();
}

function replSessionReset() {
    replSessionSource = '';
    if (promptDoc) promptDoc.refresh();
}

/** Is the suggest widget open? Asked once per keystroke, by one handler. */
function promptSuggestOpen() {
    if (!promptEditor) return false;
    const c = promptEditor.getContribution('editor.contrib.suggestController');
    // `model.state` is 0 when idle; the widget's own visibility flag is not
    // public, and the controller is.
    return !!(c && c.model && c.model.state !== 0);
}

function promptValue() {
    return promptEditor ? promptEditor.getValue() : '';
}

function promptSetValue(text) {
    if (!promptEditor) return;
    promptEditor.setValue(text);
    const line = promptEditor.getModel().getLineCount();
    promptEditor.setPosition({
        lineNumber: line,
        column: promptEditor.getModel().getLineMaxColumn(line),
    });
}

function promptSetEnabled(on) {
    /* The host class is set whether or not the editor exists yet: the WASM
     * boot and the prompt's construction are independent, and whichever wins
     * the race must not leave the row looking disabled forever. The editor's
     * own initial readOnly is read from wasmState in initReplInput. */
    const host = document.getElementById('repl-input');
    if (host) host.classList.toggle('repl-input-disabled', !on);
    if (promptEditor) promptEditor.updateOptions({ readOnly: !on });
}

async function promptSubmit() {
    const code = promptValue().trim();
    if (!code || wasmState() !== WASM_STATE.READY) return;

    replHistory.unshift(code);
    replHistoryIndex = -1;
    promptSetValue('');

    if (code.startsWith(':')) {
        appendToConsole(`<span class="console-prompt">turi&gt;</span> ${escapeHtml(code)}`);
        await dispatchReplMetaCommand(code);
    } else {
        const res = await executeCode(code, '<span class="console-prompt">turi&gt;</span>');
        // Only a line the session actually accepted joins the prompt's
        // document. A line that failed defined nothing, and offering its
        // names would be the exact lie W2 exists to prevent.
        if (!res || !res.isError) replSessionAccept(code);
    }

    const consoleEl = document.getElementById('console');
    if (consoleEl) consoleEl.scrollTop = consoleEl.scrollHeight;
}

function initReplInput() {
    const host = document.getElementById('repl-input');
    if (!host) return;

    /* The suggest widget has to escape a 22px-tall, overflow:hidden row, so it
     * renders into a body-level node instead of inside the editor.
     *
     * `monaco-editor` on that node is mandatory, not decorative. Monaco ships
     * every widget rule scoped to a `.monaco-editor` ancestor
     * (`.monaco-editor .suggest-widget { ... }`) and declares the whole
     * `--vscode-*` color set on `.monaco-editor` itself. Hoisted out of the
     * editor without the class, the popup matches none of it: transparent
     * background, no border, unsized rows -- suggestions painted straight over
     * the prompt with nothing behind them. */
    let overlay = document.getElementById('repl-suggest-overlay');
    if (!overlay) {
        overlay = document.createElement('div');
        overlay.id = 'repl-suggest-overlay';
        overlay.className = 'repl-suggest-overlay monaco-editor';
        document.body.appendChild(overlay);
    }
    // An overlay left over from a previous init predates the class.
    overlay.classList.add('monaco-editor');

    promptEditor = monaco.editor.create(host, {
        value: '',
        language: 'turmeric',
        theme: monacoThemeName(),
        automaticLayout: true,
        lineNumbers: 'off',
        glyphMargin: false,
        folding: false,
        lineDecorationsWidth: 0,
        lineNumbersMinChars: 0,
        minimap: { enabled: false },
        overviewRulerLanes: 0,
        overviewRulerBorder: false,
        hideCursorInOverviewRuler: true,
        renderLineHighlight: 'none',
        scrollBeyondLastLine: false,
        scrollBeyondLastColumn: 0,
        wordWrap: 'off',
        contextmenu: false,
        fontFamily: 'var(--font-mono)',
        fontSize: 12.5,
        lineHeight: 20,
        padding: { top: 1, bottom: 1 },
        scrollbar: {
            vertical: 'hidden',
            horizontal: 'hidden',
            handleMouseWheel: false,
            alwaysConsumeMouseWheel: false,
        },
        overflowWidgetsDomNode: overlay,
        fixedOverflowWidgets: true,
        readOnly: wasmState() !== WASM_STATE.READY,
        quickSuggestions: { other: true, comments: false, strings: false },
        suggestOnTriggerCharacters: true,
        acceptSuggestionOnEnter: 'on',
        tabCompletion: 'off',
        parameterHints: { enabled: true },
    });

    host.classList.toggle('repl-input-disabled', wasmState() !== WASM_STATE.READY);
    host.classList.add('repl-input-empty');

    const model = promptEditor.getModel();

    promptEditor.onDidChangeModelContent(() => {
        const v = model.getValue();
        host.classList.toggle('repl-input-empty', v.length === 0);
        // One line, always. A paste can carry newlines and Monaco will happily
        // take them; the prompt submits a line, so it keeps one.
        if (v.indexOf('\n') >= 0) {
            const flat = v.replace(/\s*\n\s*/g, ' ');
            promptEditor.setValue(flat);
            promptEditor.setPosition({
                lineNumber: 1,
                column: model.getLineMaxColumn(1),
            });
        }
    });

    /* One handler, not several listeners.
     *
     * Enter submits, ArrowUp/ArrowDown walk history, and the suggest widget
     * wants all three. With two listeners the behaviour depends on
     * registration order, which a reader has to infer rather than read -- so
     * every key that is contested is decided here, in one place, by asking
     * whether the widget is open.
     */
    promptEditor.onKeyDown((e) => {
        const widget = promptSuggestOpen();

        if (e.keyCode === monaco.KeyCode.Enter && !e.shiftKey) {
            if (widget) return;          // accept the highlighted suggestion
            e.preventDefault();
            e.stopPropagation();
            promptSubmit();
            return;
        }
        if (e.keyCode === monaco.KeyCode.UpArrow) {
            if (widget) return;          // move within the list
            e.preventDefault();
            e.stopPropagation();
            if (replHistoryIndex < replHistory.length - 1) {
                replHistoryIndex++;
                promptSetValue(replHistory[replHistoryIndex]);
            }
            return;
        }
        if (e.keyCode === monaco.KeyCode.DownArrow) {
            if (widget) return;
            e.preventDefault();
            e.stopPropagation();
            if (replHistoryIndex > 0) {
                replHistoryIndex--;
                promptSetValue(replHistory[replHistoryIndex]);
            } else {
                replHistoryIndex = -1;
                promptSetValue('');
            }
            return;
        }
        if (e.keyCode === monaco.KeyCode.Escape) {
            /* Dismissing a popup must not also close the docs pane or a menu.
             * The page has document-level Escape handlers; Monaco's own
             * dismissal happens on this same event, so the key is left to it
             * and only the propagation is stopped. */
            e.stopPropagation();
            return;
        }
    });

    /* W4: meta-command completion, on a `:` at the start of a line and nowhere
     * else. A `:foo` mid-expression is a keyword literal and a map key
     * (`#map{:name name}`), and offering the REPL's vocabulary there would be
     * noise on top of the language completions that already fire. */
    const metaProvider = {
        triggerCharacters: [':', ' '],
        provideCompletionItems(m, position) {
            if (m !== model) return { suggestions: [] };
            const upto = m.getLineContent(position.lineNumber)
                          .slice(0, position.column - 1);

            const typing = /^\s*(:[a-z-]*)$/.exec(upto);
            if (typing) {
                const word = m.getWordUntilPosition(position);
                const range = {
                    startLineNumber: position.lineNumber,
                    endLineNumber: position.lineNumber,
                    // The `:` is not a word character, so getWordUntilPosition
                    // stops after it; the replacement has to start on it or
                    // accepting a suggestion leaves `::doc`.
                    startColumn: position.column - typing[1].length,
                    endColumn: word.endColumn,
                };
                return {
                    suggestions: REPL_META_COMMANDS.map(c => ({
                        label: c.usage ? `${c.name} ${c.usage}` : c.name,
                        kind: monaco.languages.CompletionItemKind.Keyword,
                        insertText: c.arg ? c.name + ' ' : c.name,
                        detail: c.summary,
                        filterText: c.name,
                        range,
                    })),
                };
            }

            /* Second context: `:doc ` and `:type ` take a symbol, so
             * completion after them offers symbols rather than commands. One
             * line of context test, reusing the doc table the panel already
             * loaded. */
            const withArg = /^\s*(:[a-z-]+)\s+(\S*)$/.exec(upto);
            if (withArg) {
                const spec = REPL_META_COMMANDS.find(c => c.name === withArg[1]);
                if (!spec || (spec.arg !== 'sym' && spec.arg !== 'expr'))
                    return { suggestions: [] };
                const word = m.getWordUntilPosition(position);
                const range = {
                    startLineNumber: position.lineNumber,
                    endLineNumber: position.lineNumber,
                    startColumn: word.startColumn,
                    endColumn: word.endColumn,
                };
                return {
                    suggestions: (docNames || []).slice(0, 500).map(d => ({
                        label: d.name,
                        kind: monaco.languages.CompletionItemKind.Function,
                        insertText: d.name,
                        detail: d.summary ? String(d.summary).split('\n')[0] : undefined,
                        range,
                    })),
                };
            }
            return { suggestions: [] };
        },
    };
    monaco.languages.registerCompletionItemProvider('turmeric', metaProvider);

    /* Test surface. A spec cannot type into Monaco through `fill()` and should
     * not have to read the suggest widget's DOM to find out what was offered,
     * so the prompt's own state and its providers' answers are reachable
     * directly -- while Enter and the arrows still go through the real
     * onKeyDown, which is the part worth covering. */
    window._turiRepl = {
        focus: () => promptEditor && promptEditor.focus(),
        value: () => promptValue(),
        setValue: (t) => promptSetValue(t),
        submit: () => promptSubmit(),
        suggestOpen: () => promptSuggestOpen(),
        sessionSource: () => replSessionSource,
        metaCompletions: (line) => {
            promptSetValue(line);
            const r = metaProvider.provideCompletionItems(model, {
                lineNumber: 1,
                column: model.getLineMaxColumn(1),
            });
            return (r && r.suggestions ? r.suggestions : []).map(x => x.label);
        },
        completions: async (line) => {
            promptSetValue(line);
            if (!lspClient) return [];
            const r = await lspClient.completions(model, {
                lineNumber: 1,
                column: model.getLineMaxColumn(1),
            });
            return (r && r.suggestions ? r.suggestions : []).map(x => x.label);
        },
    };

    /* W2: the prompt's own document. It is opened against the same session the
     * editor's tabs are, which needs nothing from the server --
     * lsp_session_handle is already document-keyed and already holds several
     * documents at once. */
    const attachPromptDoc = () => {
        if (promptDoc || !lspClient) return;
        promptDoc = lspClient.attachDocument({
            model,
            uri: REPL_DOC_URI,
            getPrefix: () => replSessionSource,
        });
    };
    promptEditor.onDidFocusEditorText(() => {
        startLspClient().then(attachPromptDoc).catch(() => {});
    });
    if (lspClient) attachPromptDoc();
}

/**
 * Initialize event listeners
 */
function initEventListeners() {
    // Run button
    document.getElementById('run-btn')?.addEventListener('click', runCode);

    // Stop: shown by the eval watchdog while a request runs long (W-3).
    document.getElementById('stop-btn')?.addEventListener('click',
        () => stopEvalWorker('Stopped'));

    // Clear button (Shift+click = reset full workspace)
    document.getElementById('clear-btn')?.addEventListener('click', (e) => {
        if (e.shiftKey) {
            resetWorkspace();
        } else {
            clearEditor();
        }
    });

    // Back-from-definition button (M4). Hidden until the stack is non-empty.
    document.getElementById('jump-back-btn')?.addEventListener('click', jumpBack);
    renderJumpBack();

    // Time-travel timeline (try-turmeric-tracer-plan T3).
    traceInstallHandlers();

    // Format button
    document.getElementById('format-btn')?.addEventListener('click', formatCode);
    
    // Share button
    document.getElementById('share-btn')?.addEventListener('click', shareCode);

    // Download-project-as-zip button (Phase 2 of the multi-tab plan).
    document.getElementById('download-btn')?.addEventListener('click', downloadProject);

    // Open-project-zip (Phase 3): button + hidden <input type=file> + the
    // drag-drop overlay on the editor pane.
    document.getElementById('open-btn')?.addEventListener('click', openProjectFile);
    document.getElementById('open-project-input')?.addEventListener('change', async (e) => {
        const file = e.target.files?.[0];
        if (file) await loadProjectFromFile(file);
        e.target.value = '';
    });
    initProjectDrop();
    
    // Clear console button
    document.getElementById('clear-console-btn')?.addEventListener('click', clearConsole);
    
    // Copy console button
    document.getElementById('copy-console-btn')?.addEventListener('click', copyConsole);
    
    // Examples dropdown (hamburger button + popover). Mirrors the ⋯ overflow
    // menu below: reparent to <body> so it escapes ancestor overflow/stacking
    // contexts, anchor with position:fixed, close on outside-click / Escape.
    const examplesBtn = document.getElementById('examples-btn');
    const examplesMenu = document.getElementById('examples-menu');
    if (examplesBtn && examplesMenu) {
        if (examplesMenu.parentElement !== document.body) {
            document.body.appendChild(examplesMenu);
        }
        const closeExamples = () => {
            examplesMenu.hidden = true;
            examplesBtn.setAttribute('aria-expanded', 'false');
        };
        const openExamples = () => {
            examplesMenu.hidden = false;
            examplesBtn.setAttribute('aria-expanded', 'true');
            const r = examplesBtn.getBoundingClientRect();
            examplesMenu.style.top = `${r.bottom + 4}px`;
            examplesMenu.style.right = `${Math.max(4, window.innerWidth - r.right)}px`;
        };
        examplesBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            examplesMenu.hidden ? openExamples() : closeExamples();
        });
        examplesMenu.addEventListener('click', (e) => {
            const item = e.target.closest('.more-item');
            if (!item || !item.dataset.example) return;
            loadExample(item.dataset.example);
            closeExamples();
        });
        document.addEventListener('click', (e) => {
            if (!examplesMenu.hidden && !examplesMenu.contains(e.target) && e.target !== examplesBtn) {
                closeExamples();
            }
        });
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && !examplesMenu.hidden) closeExamples();
        });
    }

    // Symbols dropdown (the outline)
    initSymbolsMenu({ currentTab, getLspClient: () => lspClient });

    // Language picker (dialect radio group + layer checkboxes)
    initLangPicker();

    // Mobile overflow menu (⋯) — forwards each menu item to the underlying
    // button or the examples <select>, so the existing wiring stays the
    // single source of truth.
    const moreBtn = document.getElementById('more-btn');
    const moreMenu = document.getElementById('more-menu');
    if (moreBtn && moreMenu) {
        // Reparent to <body> so the popover escapes any ancestor
        // overflow:hidden / stacking-context (Monaco, editor-pane, etc.).
        if (moreMenu.parentElement !== document.body) {
            document.body.appendChild(moreMenu);
        }
        const closeMenu = () => {
            moreMenu.hidden = true;
            moreBtn.setAttribute('aria-expanded', 'false');
        };
        const openMenu = () => {
            // The minimap row's label *is* its state, so it has to be right
            // before the menu is painted, not after the click that changes it.
            syncMinimapMenuItem();
            refreshBuildStamp();
            moreMenu.hidden = false;
            moreBtn.setAttribute('aria-expanded', 'true');
            // Anchor below the button. position:fixed so the menu escapes
            // .editor-header's overflow:hidden clip on mobile.
            const r = moreBtn.getBoundingClientRect();
            moreMenu.style.top = `${r.bottom + 4}px`;
            moreMenu.style.right = `${Math.max(4, window.innerWidth - r.right)}px`;
        };
        moreBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            moreMenu.hidden ? openMenu() : closeMenu();
        });
        moreMenu.addEventListener('click', (e) => {
            const item = e.target.closest('.more-item');
            if (!item) return;
            // Stop here rather than letting the click reach document. Some
            // rows forward to a button that opens another popover (Symbols),
            // and the other popovers close themselves on any document click
            // whose target is outside them -- which this one is. Without this,
            // opening Symbols from the ⋯ menu opened and closed it in the same
            // event. Nothing else needs the click at document level: the only
            // listeners there close menus, and closeMenu() below does that
            // for this one.
            e.stopPropagation();
            const cmd = item.dataset.cmd;
            const example = item.dataset.example;
            const action = item.dataset.action;
            if (cmd) {
                document.getElementById(cmd)?.click();
            } else if (example) {
                loadExample(example);
            } else if (action === 'force-update') {
                showStatus('Updating...', 'info');
                forceUpdatePWA();
            } else if (action === 'toggle-minimap') {
                toggleMinimap();
            }
            closeMenu();
        });
        document.addEventListener('click', (e) => {
            if (!moreMenu.hidden && !moreMenu.contains(e.target) && e.target !== moreBtn) {
                closeMenu();
            }
        });
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && !moreMenu.hidden) closeMenu();
        });
    }

    // Window resize
    window.addEventListener('resize', () => {
        if (editor) {
            editor.layout();
        }
    });
    
    // Handle hash changes (for sharing, and for #doc= docs deep links)
    window.addEventListener('hashchange', () => {
        loadFromUrlHash();
        syncDocsPaneFromHash();
    });

    // REPL input
    initReplInput();
    initConsoleHover({ getDocNames: () => docNames, wasmDocLookup });

    // Horizontal drag-to-scroll on the editor header (mobile / narrow widths)
    initHScrollDrag();

    // Draggable editor/console split (horizontal on desktop, vertical on mobile)
    initSplitHandle();

    // Minimap width gate + user toggle (M1). After initSplitHandle(), which
    // restores the persisted split and therefore the editor's real width.
    initMinimap();

    // PWA install affordances
    initInstallAffordances();
}

// ============================================================================
// Main Initialization
// ============================================================================

/**
 * Main initialization function
 */
async function init() {
    console.log('Initializing Try Turmeric...');
    
    // Load Monaco Editor
    try {
        // Configure Monaco workers BEFORE importing Monaco so workers are
        // available when Monaco initializes during the import itself.
        window.MonacoEnvironment = {
            getWorker: function (_moduleId, label) {
                if (label === 'json') {
                    return new Worker(new URL('monaco-editor/language/json/json.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'css' || label === 'scss' || label === 'less') {
                    return new Worker(new URL('monaco-editor/language/css/css.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'html' || label === 'handlebars' || label === 'razor') {
                    return new Worker(new URL('monaco-editor/language/html/html.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'typescript' || label === 'javascript') {
                    return new Worker(new URL('monaco-editor/language/typescript/ts.worker.js', import.meta.url), { type: 'module' });
                }
                return new Worker(new URL('monaco-editor/editor/editor.worker.js', import.meta.url), { type: 'module' });
            }
        };

        monaco = await import('monaco-editor');

        // Configure Monaco (must happen after import)
        configureMonaco();

        // Initialize editor
        await initEditor();
        
        // Initialize event listeners
        initEventListeners();
        
        // Initialize WASM
        await initWasm();
        
        console.log('Try Turmeric initialized successfully');
        
    } catch (error) {
        console.error('Failed to initialize Try Turmeric:', error);
        appendToConsole('<span class="console-error">Error: Failed to initialize editor. Please refresh the page.</span>');
    }
}

// Start initialization when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
} else {
    init();
}

// The REPL's `:` commands (the doc panel itself is doc-panel.js).

/* ---------------------------------------------------------------------------
 * W4: one table, three consumers
 *
 * The meta-command vocabulary was about to exist in three places -- the
 * dispatch chain, the hand-written `:help` text, and (once the prompt could
 * complete) a suggestion list. Three copies is how the help text and the
 * switch quietly stop agreeing about what the REPL can do.
 *
 * `arg` says what a command takes, which is also the completion context: a
 * command taking 'sym' offers symbols after the space, one taking nothing
 * offers nothing.
 * ------------------------------------------------------------------------ */
const REPL_META_COMMANDS = [
    { name: ':help',    arg: null,     usage: '',        summary: 'show this help text' },
    { name: ':type',    arg: 'expr',   usage: '<expr>',  summary: 'print inferred type without evaluating' },
    { name: ':doc',     arg: 'sym',    usage: '<sym>',   summary: 'print builtin/standard operator documentation' },
    { name: ':docs',    arg: 'page',   usage: '[page]',  summary: 'open the docs browser (guides + API), offline-ready' },
    { name: ':reset',   arg: null,     usage: '',        summary: 'clear session and start fresh' },
    { name: ':explain', arg: 'code',   usage: '[code]',  summary: 'explain the most recent error, or a TUR-E#### code' },
    { name: ':trace',   arg: null,     usage: '',        summary: 'record this program and open the time-travel timeline' },
];

/* Rendered from the table, aligned on the widest label -- so adding a command
 * cannot leave the column crooked, which is the failure mode a hand-written
 * block has and a generated one cannot. */
function replHelpText() {
    const labels = REPL_META_COMMANDS.map(c =>
        c.usage ? `${c.name} ${c.usage}` : c.name);
    const width = labels.reduce((w, l) => Math.max(w, l.length), 0);
    const rows = REPL_META_COMMANDS.map((c, i) =>
        `  ${labels[i].padEnd(width)}  ${c.summary}`);
    return 'Turmeric REPL Help\n' +
           '------------------\n' +
           rows.join('\n') + '\n' +
           '\n' +
           'Multi-line input: keep typing when parentheses are open;\n' +
           '  an empty line to cancel an incomplete expression.';
}

/**
 * Dispatch web REPL meta-commands locally.
 */
async function dispatchReplMetaCommand(line) {
    const parts = line.trim().split(/\s+/);
    const cmd = parts[0].toLowerCase();
    const arg = parts.slice(1).join(' ').trim();

    if (cmd === ':help') {
        appendToConsole(
            `<pre class="console-output console-flush">${escapeHtml(replHelpText())}</pre>`);

    } else if (cmd === ':trace') {
        await traceCode();

    } else if (cmd === ':doc') {
        if (!arg) {
            appendToConsole('<span class="console-error">:doc requires a symbol name</span>');
            return;
        }
        const entry = docNames.find(d => d.name === arg);
        if (entry && entry.summary) {
            appendToConsole(`<pre class="console-output console-flush">${escapeHtml(entry.summary)}</pre>`);
        } else {
            const docText = await wasmDocLookup(arg);
            if (docText) {
                appendToConsole(`<pre class="console-output console-flush">${escapeHtml(docText)}</pre>`);
            } else {
                appendToConsole(`<span class="console-output">no documentation for \'${escapeHtml(arg)}\'</span>`);
            }
        }

    } else if (cmd === ':docs') {
        // `:docs` opens the browser at its last location; `:docs <page>` jumps
        // straight to one. A bare slug is resolved against guides first, then
        // API modules, so `:docs hkt-guide` and `:docs tur-list` both work
        // without the reader knowing the pack's section layout.
        await loadDocsIndex();
        let ref;
        if (arg) {
            const candidates = arg.includes('/')
                ? [arg]
                : [`guides/${arg}`, `guides/${arg}-guide`, `api/${arg}`, `spices/${arg}`];
            ref = candidates.find(c => docsEntryFor(parseDocsRef(c).ref));
            if (!ref) {
                appendToConsole(
                    `<span class="console-error">no documentation page '${escapeHtml(arg)}'`
                    + ' -- open :docs and search</span>');
                return;
            }
        }
        openDocsPane(ref);

    } else if (cmd === ':type') {
        if (!arg) {
            appendToConsole('<span class="console-error">:type requires an expression</span>');
            return;
        }
        const typeText = await wasmTypeOf(arg);
        if (typeText.startsWith('error:')) {
            appendToConsole(`<span class="console-error">${escapeHtml(typeText)}</span>`);
        } else {
            appendToConsole(`<span class="console-output">: ${escapeHtml(typeText)}</span>`);
        }

    } else if (cmd === ':explain') {
        const explainText = await wasmExplain(arg);
        appendToConsole(`<pre class="console-output console-flush">${escapeHtml(explainText)}</pre>`);

    } else if (cmd === ':reset') {
        resetWasm();

    } else if (cmd === ':quit' || cmd === ':q') {
        appendToConsole('<span class="console-error">:quit is not supported in the browser REPL</span>');

    } else {
        appendToConsole(`<span class="console-error">unknown meta-command \'${escapeHtml(cmd)}\' -- try :help</span>`);
    }
}

configureDocsPane({
    showStatus,
    getDocNames: () => docNames,
    fetchDocNames,
    wasmDocLookup,
    showDocPanel,
});
document.addEventListener('DOMContentLoaded', initDocsPane);

// Export for debugging
window.turmericApp = {
    runCode,
    clearEditor,
    clearConsole,
    formatCode,
    loadExample,
    shareCode,
    resetWasm,
    resetWorkspace,
    showDocPanel,
    hideDocPanel,
    wasmDocLookup,
    openDocsPane,
    closeDocsPane,
    showDocsPage,
    loadDocsIndex,
    getState: () => ({
        wasmState: wasmState(),
        hasEditor: !!editor,
        hasWasm: hasWorker() && wasmState() === WASM_STATE.READY,
        docsOpen: docsPaneIsOpen(),
        docsRef: docsPaneState().ref,
        docsGuideView: docsPaneState().guideView,
    })
};

// The trace timeline (trace.js) asks the worker for each trace-* request
// through this, and resets the session for a recording through traceResetEnv.
function traceWorkerCall(message) {
    if (!hasWorker() || wasmState() !== WASM_STATE.READY) return Promise.resolve(null);
    return new Promise((resolve, reject) => {
        send(message, { resolve, reject });
    });
}

/**
 * Reset the interpreter env for a recording, without the console-clearing and
 * status message the user-facing `:reset` does.
 *
 * A trace is a run of a whole program from the start, which is what `tur trace
 * <file>` means and what makes two recordings of the same program comparable.
 * The browser env is a REPL session that accumulates every eval, so without
 * this the SECOND Trace re-evaluates the program on top of the first one's
 * definitions and dies on `'Ask' is already defined` -- and even where the
 * program has no top-level definitions to collide, the recording would be of a
 * program running in an env polluted by its own previous run.
 */
function traceResetEnv() {
    return new Promise((resolve) => {
        send({ type: 'reset' }, {
            resolve: () => {
                // The session forgot what it had accepted, so the prompt's
                // document has to forget it too.
                replSessionReset();
                lastRunByTab.clear();
                resolve(true);
            },
            reject: () => resolve(false),
        });
    });
}

configureTrace({
    isReady: () => wasmState() === WASM_STATE.READY,
    workerCall: traceWorkerCall,
    resetSession: traceResetEnv,
    getConsoleLog: () => consoleLog,
    setConsoleLog: (log) => { consoleLog = log; },
    showStatus,
    appendToConsole,
    updateExecTime,
    parseLangDirective,
    definesMainEntry,
});


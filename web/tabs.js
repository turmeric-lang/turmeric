// tabs.js -- the editor's tabs: the tab set and which one is active, each
// tab's Monaco model, persistence to localStorage, the tab bar (rename, drag,
// close), read-only stdlib buffers with the jump-back stack
// (try-turmeric-navigation-and-minimap-plan, M4), and hydrating the set at
// startup.
//
// main.js reads `tabs`, `activeId`, `tabsHydrating` and `jumpStack` through
// this module's live exports. It replaces the set wholesale only through
// useTutorialTab() and replaceTabs(); everything else edits it through the
// functions here.

import { STORAGE_KEYS, safeRead, safeWrite, debounce } from './storage.js';
import { getEditor, getMonaco } from './editor-ref.js';
import { reconcileLangPicker } from './lang-picker.js';
import { lspClient } from './lsp-ref.js';

// Multi-tab editor state. Each tab carries its persisted record plus a
// non-persisted Monaco ITextModel created on demand. The model holds the
// per-tab undo stack, so swapping models (rather than re-using one and
// calling setValue) preserves undo across tab switches.
export let tabs = [];          // [{id, name, content, cursor, scrollTop, createdAt, _model}]
export let activeId = null;
export let tabsHydrating = false;  // suppress persist during initial hydration / tutorial
/* PS5 (playground-session-hygiene-plan): what each tab's last successful Run
 * put in the session, keyed by tab id.  The session is shared by every tab -- a
 * Run is how a tab's definitions become callable at the prompt, and from other
 * tabs -- so rewinding it for one tab's Run must bring the others back
 * (rewindSessionForRun).  In memory only: a reload starts a fresh session in
 * which no tab has run. */
export const lastRunByTab = new Map();

export function genTabId() {
    // Stable, non-cryptographic id. Tab ids are local to a single browser
    // workspace -- collision risk is purely cosmetic.
    return 'tab-' + Math.random().toString(36).slice(2, 10) + '-' + Date.now().toString(36);
}

export function tabsSnapshot() {
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
export const persistTabs = debounce(() => {
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

export function currentTab() {
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

export function ensureModel(tab) {
    const monaco = getMonaco();
    if (tab._model && !tab._model.isDisposed()) return tab._model;
    tab._model = monaco.editor.createModel(tab.content || '', 'turmeric');
    return tab._model;
}

export function captureActiveTabState() {
    const editor = getEditor();
    const t = currentTab();
    if (!t || !editor) return;
    t.content = editor.getValue();
    const pos = editor.getPosition();
    if (pos) t.cursor = { lineNumber: pos.lineNumber, column: pos.column };
    t.scrollTop = editor.getScrollTop();
}

export function switchTab(id) {
    const editor = getEditor();
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

export function createTab({ name, content = '', activate = true } = {}) {
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

export function closeTab(id) {
    const editor = getEditor();
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

export function sanitizeTabName(raw) {
    let name = (raw || '').trim();
    if (!name) return null;
    // strip path separators / shell-unfriendly characters
    name = name.replace(/[\/\\<>:"|?*\x00-\x1f]/g, '_');
    if (!/\.tur$/i.test(name)) name += '.tur';
    return name;
}

export function renameTab(id, rawName) {
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

/** Put the editor in or out of read-only for the tab it is showing. */
export function applyReadOnlyState(tab) {
    const editor = getEditor();
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
export async function openReadOnlyTab(uri) {
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
export let jumpStack = [];

export function pushJumpOrigin(tabId, position) {
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
export function jumpBack() {
    const editor = getEditor();
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
export function renderJumpBack() {
    const btn = document.getElementById('jump-back-btn');
    if (btn) btn.hidden = jumpStack.length === 0;
}

/**
 * Every tab is an open document as far as the server is concerned -- which is
 * what makes workspace/symbol and cross-tab go-to-definition work here without
 * the filesystem crawl a native client would need. That requires a Monaco
 * model per tab, including tabs the user has not activated yet; models are
 * otherwise created lazily on first switch.
 */
export function ensureAllTabModels() {
    for (const tab of tabs) ensureModel(tab);
}

/**
 * Tell the server the tab set moved. Driven from the mutation sites rather
 * than by polling, but the client re-derives the whole open set from `tabs`
 * each time, so a missed call costs one stale moment rather than a permanently
 * desynchronised server.
 */
export function notifyTabsChanged() {
    if (!lspClient || !lspClient.isAvailable()) return;
    ensureAllTabModels();
    lspClient.sync();
}

export function renderTabs() {
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

export function hydrateTabs(defaultContent) {
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

/**
 * The single ephemeral tab tutorial mode (`?tutorial`) works in. Persistence
 * stays off for the whole session, so a tutorial never overwrites the
 * reader's saved tabs.
 */
export function useTutorialTab(content) {
    tabs = [{
        id: genTabId(),
        name: 'tutorial.tur',
        content,
        cursor: { lineNumber: 1, column: 1 },
        scrollTop: 0,
        createdAt: Date.now(),
        _model: null,
    }];
    activeId = tabs[0].id;
    tabsHydrating = true;  // permanently suppress persistence in tutorial mode
}

/**
 * Replace the whole tab set (a loaded project). The caller disposes the old
 * models first and re-renders after.
 */
export function replaceTabs(newTabs, newActiveId) {
    tabs = newTabs;
    activeId = newActiveId;
    // Every origin the stack held belonged to the workspace that just went
    // away. jumpBack() would skip them one by one; clearing says so up front,
    // and takes the Back button down with it.
    jumpStack = [];
}

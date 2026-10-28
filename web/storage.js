// storage.js -- Try Turmeric's localStorage helpers and URL-mode checks.
//
// Every read and write goes through safeRead/safeWrite: storage can be absent
// (a private window, blocked site data) or full, and the playground has to keep
// working either way. A quota error turns persistence off for the session
// rather than failing every later write.

export const STORAGE_KEYS = {
    // Legacy single-buffer keys -- read only for migration.
    buffer:    'tur.try.buffer.v1',
    cursor:    'tur.try.cursor.v1',
    scroll:    'tur.try.scroll.v1',
    // The console transcript used to be stored as HTML and re-inserted as HTML
    // on load. It is data now (see consoleRuns); v1 is deleted unread.
    legacyConsole: 'tur.try.console.v1',
    consol:    'tur.try.console.v2',
    // Multi-tab keys (Phase 1 of try-turmeric-multi-tab-and-projects-plan).
    tabs:      'tur.try.tabs.v1',
    activeTab: 'tur.try.activeTab.v1',
    // Minimap: true / false when the user has chosen, absent when they have
    // not. The absence is load-bearing -- see minimapPreference() (M1).
    minimap:   'tur.try.minimap.v1',
    // Where the docs pane was when the app last went away: {ref, scrollTop},
    // or null once it is closed. See persistDocsState().
    docs:      'tur.try.docs.v1',
    // How the docs nav groups the Guides section: 'category' or 'tag'. Absent
    // until the reader has chosen, which reads as 'category'.
    docsView:  'tur.try.docs-view.v1',
};

let storageDisabled = false;

export function safeRead(key) {
    if (storageDisabled) return null;
    try {
        const raw = localStorage.getItem(key);
        return raw == null ? null : JSON.parse(raw);
    } catch {
        return null;
    }
}

export function safeWrite(key, value) {
    if (storageDisabled) return;
    try {
        localStorage.setItem(key, JSON.stringify(value));
    } catch (e) {
        if (e && (e.name === 'QuotaExceededError' || e.code === 22)) {
            storageDisabled = true;
            console.warn('Try Turmeric: localStorage quota exceeded; persistence disabled for this session.');
        }
    }
}

export function isTutorialMode() {
    try {
        return new URLSearchParams(window.location.search).has('tutorial');
    } catch {
        return false;
    }
}

export function hasUrlHashCode() {
    try {
        const hash = window.location.hash.slice(1);
        return new URLSearchParams(hash).has('code');
    } catch {
        return false;
    }
}

export function debounce(fn, ms) {
    let t = null;
    return (...args) => {
        clearTimeout(t);
        t = setTimeout(() => fn(...args), ms);
    };
}

export function resetWorkspace() {
    if (!confirm('Reset workspace? This clears the editor buffer, console, and saved layout.')) return;
    try {
        for (const key of Object.values(STORAGE_KEYS)) localStorage.removeItem(key);
        localStorage.removeItem('tur.try.split.h.v1');
        localStorage.removeItem('tur.try.split.v.v1');
    } catch {}
    window.location.hash = '';
    window.location.reload();
}

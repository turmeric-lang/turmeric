// minimap.js -- the editor minimap (try-turmeric-navigation-and-minimap-plan, M1).
//
//
// Two inputs decide whether the strip is showing: an explicit user choice,
// and how wide the editor actually is. The user's choice wins outright; the
// width gate only speaks when they have not made one.
//
// The gate is on *measured editor width*, not a media query, because the
// split handle can make the editor 300px wide on a 1920px desktop -- and a
// minimap on a 300px editor is a fifth of the code column showing three
// characters of shape.

import { STORAGE_KEYS, safeRead, safeWrite } from './storage.js';
import { getEditor, getMonaco } from './editor-ref.js';

// Below this many pixels of editor width the strip costs more column than it
// pays back. Roughly: 80 columns of Iosevka at 13px plus the gutter is around
// 600px, so this is "the code no longer fits comfortably beside it".
const MINIMAP_MIN_WIDTH = 600;

/** The user's explicit choice, or null when they have never made one. */
function minimapPreference() {
    const stored = safeRead(STORAGE_KEYS.minimap);
    return typeof stored === 'boolean' ? stored : null;
}

/** What the strip should be doing right now, given preference and width. */
function minimapShouldBeOn() {
    const editor = getEditor();
    const pref = minimapPreference();
    if (pref !== null) return pref;
    if (!editor) return false;
    let width = 0;
    try {
        width = editor.getLayoutInfo().width;
    } catch {
        return false;
    }
    // A layout that has not happened yet reports 0. Treat that as "not narrow"
    // rather than "narrow": the next layout pass re-runs this, and starting
    // wide-then-narrowing is less jarring than the reverse.
    return width === 0 || width >= MINIMAP_MIN_WIDTH;
}

/**
 * Push the current decision into the editor.
 *
 * Everything this touches is wrapped: the failure contract from c2mp
 * (minimap.js:342) is that a decoration which fails removes itself rather
 * than degrading the editor. Monaco's minimap is a supported component and is
 * not expected to throw, but the gate, the toggle, and the theme extension are
 * ours -- and if any of them throws, the playground must keep working.
 */
export function applyMinimapPreference() {
    const editor = getEditor();
    if (!editor) return;
    try {
        editor.updateOptions({ minimap: { enabled: minimapShouldBeOn() } });
    } catch (err) {
        console.warn('Try Turmeric: minimap update failed', err);
    }
    syncMinimapMenuItem();
}

/** Reflect the current state in the More menu's toggle row. */
export function syncMinimapMenuItem() {
    const editor = getEditor();
    const monaco = getMonaco();
    const item = document.querySelector('[data-action="toggle-minimap"]');
    if (!item) return;
    let on = false;
    try {
        on = !!editor && !!editor.getOption(monaco.editor.EditorOption.minimap).enabled;
    } catch {
        on = minimapShouldBeOn();
    }
    item.setAttribute('aria-checked', on ? 'true' : 'false');
    item.textContent = on ? 'Hide minimap' : 'Show minimap';
}

/** Flip the strip and remember the choice, which from now on beats the gate. */
export function toggleMinimap() {
    const editor = getEditor();
    const monaco = getMonaco();
    if (!editor) return;
    let on = false;
    try {
        on = !!editor.getOption(monaco.editor.EditorOption.minimap).enabled;
    } catch {
        on = minimapShouldBeOn();
    }
    safeWrite(STORAGE_KEYS.minimap, !on);
    applyMinimapPreference();
}

export function initMinimap() {
    applyMinimapPreference();
    // Re-evaluate wherever the editor can change width. The split handle calls
    // applyMinimapPreference() from applySplit(); this covers the window and
    // anything else that resizes the pane (the docs overlay, the on-screen
    // keyboard, a rotated phone).
    if (typeof ResizeObserver !== 'undefined') {
        const host = document.getElementById('editor');
        if (host) {
            try {
                new ResizeObserver(() => applyMinimapPreference()).observe(host);
            } catch { /* older engine; the resize listener below still fires */ }
        }
    }
    window.addEventListener('resize', () => applyMinimapPreference());

    // Test surface: a spec should assert what the editor is actually doing,
    // not re-derive it from localStorage.
    window._turiMinimap = {
        enabled: () => {
            try {
                return !!getEditor().getOption(getMonaco().editor.EditorOption.minimap).enabled;
            } catch { return false; }
        },
        preference: () => minimapPreference(),
        toggle: () => toggleMinimap(),
    };
}

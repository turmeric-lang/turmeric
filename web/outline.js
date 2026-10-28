// outline.js -- the Symbols outline popover (try-turmeric-navigation-and-minimap-plan, M2).
//
//
// Monaco standalone has no outline pane and no breadcrumbs -- those are VS
// Code workbench features -- so the documentSymbol provider the adapter
// registers has answered correctly since it landed with nothing reading it.
// This is the surface.
//
// Two ways in, and they are not redundant. Ctrl/Cmd+Shift+O runs Monaco's own
// quick-outline, which is a filterable palette and is already registered
// (`editor.all.js` pulls standaloneGotoSymbolQuickAccess in); the button opens
// this popover, which is a list you can look at without typing. The button is
// also the only one of the two that is discoverable.

import { getEditor } from './editor-ref.js';
import { currentTab } from './tabs.js';
import { lspClient } from './lsp-ref.js';

// What to call each SymbolKind out loud. c2mp's KIND_LABEL (symbols.js:752)
// is the phrasing being copied: what you would say about the entry, not the
// LSP enum name. Kinds the server does not currently emit are listed anyway,
// because M5 widens the mapping and a missing row here would silently render
// as the bare enum name.
const SYMBOL_KIND_LABELS = {
    Function:  'function',
    Method:    'function',
    Constructor: 'constructor',
    Variable:  'value',
    Constant:  'constant',
    Struct:    'type',
    Class:     'type',
    Enum:      'type',
    Interface: 'class',
    Object:    'instance',
    Operator:  'macro',
    Module:    'module',
    Namespace: 'module',
    Field:     'field',
    Property:  'field',
    EnumMember: 'variant',
    TypeParameter: 'type param',
};

function symbolKindLabel(kindName) {
    return SYMBOL_KIND_LABELS[kindName] || (kindName || '').toLowerCase();
}

/**
 * Which entry contains the caret.
 *
 * Ported from c2mp (symbols.js:775-795), rule and reasoning intact: the name
 * wins when the caret is actually on one, otherwise the smallest containing
 * range wins. Someone editing the middle of a function is not sitting on its
 * name, and "where am I" is the entire point of the marker -- a rule that only
 * matched names would go blank the moment you started typing.
 *
 * Returns an index into `symbols`, or -1.
 */
function currentSymbolIndex(symbols, position) {
    if (!position) return -1;
    const within = (r) => {
        if (position.lineNumber < r.startLineNumber) return false;
        if (position.lineNumber > r.endLineNumber) return false;
        if (position.lineNumber === r.startLineNumber &&
            position.column < r.startColumn) return false;
        if (position.lineNumber === r.endLineNumber &&
            position.column > r.endColumn) return false;
        return true;
    };
    for (let i = 0; i < symbols.length; i++) {
        if (within(symbols[i].selectionRange || symbols[i].range)) return i;
    }
    let best = -1;
    let bestSpan = Infinity;
    for (let i = 0; i < symbols.length; i++) {
        const r = symbols[i].range;
        if (!within(r)) continue;
        const span = (r.endLineNumber - r.startLineNumber) * 100000 +
                     (r.endColumn - r.startColumn);
        if (span < bestSpan) { bestSpan = span; best = i; }
    }
    return best;
}

/** Move the caret to a symbol, with the same two calls onNavigate makes. */
function jumpToSymbol(sym) {
    const editor = getEditor();
    if (!editor || !sym) return;
    const r = sym.selectionRange || sym.range;
    try {
        editor.revealRangeInCenterIfOutsideViewport(r);
        editor.setPosition({ lineNumber: r.startLineNumber, column: r.startColumn });
        editor.focus();
    } catch { /* model swapped underneath; nothing to navigate to */ }
}

/** Paint the list. `state` is 'ok' | 'empty' | 'unavailable' | 'loading'. */
function renderSymbolsMenu(list, symbols, state) {
    list.innerHTML = '';
    if (state !== 'ok') {
        const msg = document.createElement('div');
        msg.className = 'symbols-empty';
        // Three distinct sentences on purpose. An empty outline shown because
        // analysis is down would say "this file defines nothing", which is a
        // claim about the user's code made from a fact about our worker.
        msg.textContent =
            state === 'loading'      ? 'Reading symbols...' :
            state === 'unavailable'  ? 'Analysis is unavailable.' :
            state === 'read-only'    ? 'No outline for a stdlib buffer.' :
                                       'Nothing defined yet';
        list.appendChild(msg);
        return;
    }

    const editor = getEditor();
    const current = currentSymbolIndex(symbols, editor && editor.getPosition());
    symbols.forEach((sym, i) => {
        const row = document.createElement('button');
        row.className = 'symbol-item' + (i === current ? ' is-current' : '');
        row.setAttribute('role', 'menuitem');
        if (i === current) row.setAttribute('aria-current', 'true');
        const name = document.createElement('span');
        name.className = 'symbol-item-name';
        name.textContent = sym.name;
        const kind = document.createElement('span');
        kind.className = 'symbol-item-kind';
        kind.textContent = symbolKindLabel(sym.kindName);
        row.appendChild(name);
        row.appendChild(kind);
        row.addEventListener('click', () => {
            jumpToSymbol(sym);
            closeSymbolsMenu();
        });
        list.appendChild(row);
    });
}

let closeSymbolsMenu = () => {};

export function initSymbolsMenu() {
    const btn = document.getElementById('symbols-btn');
    const menu = document.getElementById('symbols-menu');
    const list = document.getElementById('symbols-list');
    if (!btn || !menu || !list) return;

    // Same reparent/anchor/outside-click pattern as Examples and the ⋯ menu.
    if (menu.parentElement !== document.body) document.body.appendChild(menu);

    const close = () => {
        menu.hidden = true;
        btn.setAttribute('aria-expanded', 'false');
    };
    closeSymbolsMenu = close;

    const anchor = () => {
        // Below 600px the toolbar collapses and #symbols-btn is display:none,
        // so its rect is all zeros and the popover would land in the corner.
        // Anchor to whichever button the user actually pressed -- the ⋯ menu
        // forwards to this one via .click().
        const more = document.getElementById('more-btn');
        const el = btn.getBoundingClientRect().width > 0 ? btn : (more || btn);
        const r = el.getBoundingClientRect();
        menu.style.top = `${r.bottom + 4}px`;
        menu.style.right = `${Math.max(4, window.innerWidth - r.right)}px`;
    };

    const open = async () => {
        menu.hidden = false;
        btn.setAttribute('aria-expanded', 'true');
        anchor();

        // Filled on open, never kept in sync. c2mp's reason (main.js:427)
        // holds exactly: the buffer changes on every keystroke and this list
        // is read once in a while.
        if (!lspClient || !lspClient.isAvailable()) {
            renderSymbolsMenu(list, [], 'unavailable');
            return;
        }
        // A stdlib buffer is not an open document as far as the server is
        // concerned (see getTabs in startLspClient), so documentSymbol would
        // come back empty -- and "Nothing defined yet" about list.tur is a
        // false statement, not a degraded one.
        const active = currentTab();
        if (active && active.readOnly) {
            renderSymbolsMenu(list, [], 'read-only');
            return;
        }
        renderSymbolsMenu(list, [], 'loading');
        let symbols = [];
        try {
            symbols = await lspClient.documentSymbols(getEditor().getModel()) || [];
        } catch {
            renderSymbolsMenu(list, [], 'unavailable');
            return;
        }
        if (menu.hidden) return;   // closed while the request was in flight
        // Position order, not collection order: an outline that does not match
        // the file it describes is harder to use than no outline.
        symbols.sort((a, b) =>
            (a.range.startLineNumber - b.range.startLineNumber) ||
            (a.range.startColumn - b.range.startColumn));
        renderSymbolsMenu(list, symbols, symbols.length ? 'ok' : 'empty');
    };

    btn.addEventListener('click', (e) => {
        e.stopPropagation();
        if (menu.hidden) open(); else close();
    });

    // Hand off to Monaco's filterable palette. The action has been in the
    // bundle since the page started importing the full `editor.api` entry;
    // nothing ever pointed at it. If a future Monaco drops it, the row goes
    // away rather than becoming a button that does nothing.
    const filterRow = document.getElementById('symbols-filter');
    if (filterRow) {
        const action = () => (getEditor() ? getEditor().getAction('editor.action.quickOutline') : null);
        if (!action()) {
            filterRow.remove();
        } else {
            filterRow.addEventListener('click', (e) => {
                e.stopPropagation();
                close();
                try {
                    getEditor().focus();
                    action().run();
                } catch { /* palette unavailable; the list above still works */ }
            });
        }
    }

    document.addEventListener('click', (e) => {
        if (!menu.hidden && !menu.contains(e.target) && !btn.contains(e.target)) {
            close();
        }
    });
    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && !menu.hidden) close();
    });

    // Test surface: a spec should read the rendered rows, but asserting on the
    // caret rule wants the rule's own answer rather than a DOM scrape.
    window._turiOutline = {
        open: () => open(),
        close: () => close(),
        rows: () => Array.from(list.querySelectorAll('.symbol-item')).map(el => ({
            name: el.querySelector('.symbol-item-name')?.textContent || '',
            kind: el.querySelector('.symbol-item-kind')?.textContent || '',
            current: el.classList.contains('is-current'),
        })),
        state: () => (list.querySelector('.symbols-empty')?.textContent || 'ok'),
    };
}

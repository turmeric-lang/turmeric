// lang-picker.js -- the #lang picker (try-turmeric-lang-toggle-plan), and
// parseLangDirective(), which reads the #lang line it edits.
//
//
// A dialect radio group that edits the #lang line in place.  The #lang line
// in the buffer stays the source of truth: the picker is a text edit, not a
// hidden mode, and everything round-trips -- paste a file with a #lang header
// and the picker updates; flip the picker and the header updates.  Nothing is
// stored in UI state that is not also in the source.
//
// One control, because `#lang` has one axis: the layer checkboxes that used
// to sit beside the radios are gone with the axis itself.  A registry object
// from a cached older wasm build may still carry a stale `layers` key; it is
// simply never read, which is what ignoring an unknown key already does --
// the point is not to assert against it.

import { WASM_STATE, wasmState, send } from './eval-client.js';
import { escapeHtml, escapeAttr } from './html.js';
import { getEditor, getMonaco } from './editor-ref.js';

/**
 * Strip a leading #lang directive from code and return the detected language
 * plus the remaining source.  The #lang line must be the first non-blank
 * line (leading spaces/tabs are allowed but not newlines).
 *
 * Returns { lang: string|null, body: string, line: string|null }
 *   lang   -- the base dialect name (e.g. "turmeric/sweet") or null if no
 *             directive found
 *   body   -- source text with the #lang line removed
 *   line   -- the full directive line text (no trailing newline), or null
 *
 * `#lang` takes a base and nothing else, so a trailing token is malformed
 * input from the editor buffer rather than a second axis.  The whole line is
 * still PARSED (so `body` stays correct and a pasted file does not choke the
 * playground); the wasm side rejects the directive with a status message.
 */
export function parseLangDirective(code) {
    const m = code.match(/^[ \t]*#lang[ \t]+([^\r\n]*?)[ \t]*(\r?\n|$)/);
    if (!m) return { lang: null, body: code, line: null };
    const toks = m[1].split(/[ \t]+/).filter(Boolean);
    if (!toks.length) return { lang: null, body: code, line: null };
    return {
        lang: toks[0],
        body: code.slice(m[0].length),
        line: m[0].replace(/\r?\n$/, ''),
    };
}

// Fallback for older deployed WASM builds that don't export
// _turi_wasm_lang_registry.
//
// Deliberately still the four Turmeric bases: a build old enough to be
// missing the registry export is also old enough to reject `#lang saffron`
// outright, so offering the Saffron bases here would hand that build a base
// it cannot read.  A current build never uses this list.
const LANG_REGISTRY_FALLBACK = {
    bases: [
        { name: 'turmeric',             label: 'S-expression',     language: 'turmeric', reader: 's-expr' },
        { name: 'turmeric/curly-infix', label: 'Curly-infix',      language: 'turmeric', reader: 'curly-infix' },
        { name: 'turmeric/neoteric',    label: 'Neoteric',         language: 'turmeric', reader: 'neoteric' },
        { name: 'turmeric/sweet',       label: 'Sweet-expression', language: 'turmeric', reader: 'sweet' },
    ],
};

const LANG_DEFAULT_BASE = 'turmeric';

// The readers the picker offers.  Curly-infix and neoteric are deliberately
// left out: `{a + b}` is enabled in EVERY dialect and neoteric `f(x)` is one of
// sweet-exp's three tools, so a file almost never wants either as its whole
// reader -- listing them made four Turmeric rows out of two real choices.  Both
// remain spellable in a `#lang` line, and a buffer that names one still gets its
// row (renderLangMenu's `keep`).
const LANG_READERS_SHOWN = ['s-expr', 'sweet', 'scheme'];

// Heading per language.  `r7rs` is the base token and the EXPERIMENTS[] name,
// but "Scheme" is what the language is called.
const LANG_GROUP_NAMES = {
    turmeric: 'Turmeric',
    saffron:  'Saffron',
    r7rs:     'Scheme',
};

function langGroupName(lang) {
    return LANG_GROUP_NAMES[lang] ||
        `${lang.charAt(0).toUpperCase()}${lang.slice(1)}`;
}

// The reader half of a base.  Registry rows carry it; derive it from the base
// token for an older cached WASM whose registry JSON predates the key.
function baseReader(b) {
    if (b.reader) return b.reader;
    const slash = b.name.indexOf('/');
    if (slash >= 0) return b.name.slice(slash + 1);
    return b.name === 'r7rs' ? 'scheme' : 's-expr';
}

// The language half of a base token: `saffron/sweet` -> `saffron`, and a bare
// base names its own language (`r7rs`).
function baseLanguage(name) {
    const slash = name.indexOf('/');
    return slash >= 0 ? name.slice(0, slash) : name;
}

// Registry fetched from the WASM module (null until it arrives).
let langRegistry = null;

function langMenuRegistry() {
    return langRegistry || LANG_REGISTRY_FALLBACK;
}

// The legacy alias is accepted on input but never generated; normalize it so
// the picker treats `#lang sweet-exp` as the turmeric/sweet selection.
function normalizeLangBase(name) {
    return name === 'sweet-exp' ? 'turmeric/sweet' : name;
}

// Short dialect tag for the header button (the full labels live in the
// popover, from the registry).
function baseShortLabel(base) {
    switch (base) {
        case 'turmeric':             return 's-expr';
        case 'turmeric/curly-infix': return 'curly';
        case 'turmeric/neoteric':    return 'neoteric';
        case 'turmeric/sweet':       return 'sweet';
        // The language is the interesting half of a Saffron base -- the
        // reader is what the Turmeric rows already say.
        case 'saffron':              return 'saffron';
        case 'saffron/curly-infix':  return 'saffron curly';
        case 'saffron/neoteric':     return 'saffron neoteric';
        case 'saffron/sweet':        return 'saffron sweet';
        // r7rs brings its own readers, so the base token says it all.
        case 'r7rs':                 return 'r7rs';
        case 'r7rs/sweet':           return 'r7rs sweet';
        default:                     return base || 's-expr';
    }
}

/**
 * Fetch the #lang registry (the base dialects) from the WASM module and
 * re-render the picker.  No-op fallback when the export is absent.
 */
export function fetchLangRegistry() {
    if (wasmState() !== WASM_STATE.READY) return Promise.resolve();
    return new Promise((resolve) => {
        send({ type: 'lang-registry' }, {
            resolve,
            reject: () => resolve(null),
        });
    }).then((json) => {
        if (!json) return;
        try {
            const parsed = JSON.parse(json);
            if (parsed && Array.isArray(parsed.bases)) {
                langRegistry = parsed;
                renderLangMenu();
            }
        } catch (err) {
            console.error('Bad lang registry JSON:', err);
        }
    });
}

/**
 * The active tab's current selection, read from line 1 of its model.  The
 * picker is per-tab: dialect is per-file in Turmeric, so nothing about it is
 * persisted outside the tab's own text.
 */
function currentLangSelection() {
    const editor = getEditor();
    const model = editor && editor.getModel();
    if (!model) return { base: LANG_DEFAULT_BASE };
    const parsed = parseLangDirective(model.getLineContent(1));
    if (parsed.lang === null) return { base: LANG_DEFAULT_BASE };
    return { base: normalizeLangBase(parsed.lang) };
}

// Tracks models whose #lang insert also added the blank separator line, so
// removal only eats a blank line this code created (never one the user
// typed).  WeakMap so disposed models don't pin the flag.
const langInsertAddedBlank = new WeakMap();

/**
 * The single writer of the #lang line (§3.4 of the plan).  All paths go
 * through one pushEditOperations call, so one Ctrl+Z undoes a language
 * switch.
 *
 * - No #lang line + default selection: write nothing (don't decorate a
 *   plain file with a redundant header).
 * - No #lang line + non-default selection: insert `#lang ...` as line 1,
 *   followed by a blank line.
 * - Has a #lang line: replace exactly that line, preserving everything
 *   after it.
 * - Selection returns to the default: remove the line (and the blank line
 *   after it if the insert above added one).
 */
function setLangDirective(model, { base }) {
    const monaco = getMonaco();
    if (!model || !monaco) return;
    const parsed = parseLangDirective(model.getLineContent(1));
    const isDefault = base === LANG_DEFAULT_BASE;
    const directive = `#lang ${base}`;

    let edits;
    if (parsed.lang === null) {
        if (isDefault) return;
        edits = [{ range: new monaco.Range(1, 1, 1, 1), text: directive + '\n\n' }];
        langInsertAddedBlank.set(model, true);
    } else if (isDefault) {
        const lineCount = model.getLineCount();
        const removeBlank = langInsertAddedBlank.get(model) === true &&
                            lineCount >= 2 &&
                            model.getLineContent(2).trim() === '';
        const lastRemoved = removeBlank ? 2 : 1;
        const range = lineCount > lastRemoved
            ? new monaco.Range(1, 1, lastRemoved + 1, 1)
            : new monaco.Range(1, 1, lineCount, model.getLineMaxColumn(lineCount));
        edits = [{ range, text: '' }];
        langInsertAddedBlank.delete(model);
    } else {
        if (parsed.line === directive) return;   // nothing to do
        edits = [{
            range: new monaco.Range(1, 1, 1, model.getLineMaxColumn(1)),
            text: directive,
        }];
    }
    // Delimit the undo stack on both sides so the switch is exactly one
    // Ctrl+Z -- neither merged with preceding typing nor split in two.
    model.pushStackElement();
    model.pushEditOperations([], edits, () => null);
    model.pushStackElement();
}

/**
 * Render the popover's radio group from the registry.  The form of the
 * control mirrors the form of the syntax: one mutually exclusive base, and
 * nothing else.
 */
function renderLangMenu(rebuilt) {
    const basesEl = document.getElementById('lang-bases');
    if (!basesEl) return;
    const reg = langMenuRegistry();

    // A base names a (language, reader) PAIR, so the two axes become the two
    // levels of the list: a heading per LANGUAGE, and one row per READER under
    // it.  A flat list had to repeat the language in every row name
    // ("Saffron -- Sweet-expression"), which read as eight unrelated dialects
    // rather than two languages with the same choice of syntax.
    //
    // A row is labelled by the `#lang` line it writes, and nothing else.  The
    // prose reader name ("Sweet-expression") was a second vocabulary for the
    // token sitting right beside it, and the token is the thing the user ends
    // up typing and reading at the top of the file.
    //
    // Only the readers people actually pick per file get a row -- see
    // LANG_READERS_SHOWN.  The one exception is the base the buffer already
    // names: hiding that would leave the picker silently unchecked and make a
    // switch look like a no-op.
    const keep = currentLangSelection().base;
    const rows = reg.bases.filter(b =>
        LANG_READERS_SHOWN.includes(baseReader(b)) || b.name === keep);
    // The buffer can name a base this registry does not list at all: the
    // fallback list is Turmeric-only, and it is what the picker renders from
    // until the WASM registry arrives.  Synthesize the row rather than leave
    // the picker contradicting line 1 of the file.
    if (keep && !rows.some(b => b.name === keep)) {
        rows.push({ name: keep, label: keep, language: baseLanguage(keep) });
    }

    // Group in registry order, which is the order `tur dialects` prints.
    const groups = [];
    rows.forEach(b => {
        const lang = b.language || 'turmeric';
        let g = groups.find(x => x.lang === lang);
        if (!g) groups.push(g = { lang, rows: [] });
        g.rows.push(b);
    });

    basesEl.innerHTML = groups.map(g => `
        <div class="lang-group" role="group" aria-label="${escapeAttr(langGroupName(g.lang))}">
            <div class="lang-group-name">${escapeHtml(langGroupName(g.lang))}</div>
            ${g.rows.map(b => `
            <label class="lang-row" title="#lang ${escapeAttr(b.name)}">
                <input type="radio" name="lang-base" value="${escapeAttr(b.name)}">
                <span class="lang-row-name">#lang ${escapeHtml(b.name)}</span>${
                    b.experiment
                        ? '<span class="lang-chip">experimental</span>'
                        : ''
                }
            </label>`).join('')}
        </div>`).join('');

    basesEl.querySelectorAll('input[type=radio]').forEach(r =>
        r.addEventListener('change', onLangControlChange));

    reconcileLangPicker(!!rebuilt);
}

/** A picker control changed: write the new selection into the buffer. */
function onLangControlChange() {
    const editor = getEditor();
    const model = editor && editor.getModel();
    if (!model) return;
    const checkedBase = document.querySelector('#lang-bases input[type=radio]:checked');
    const base = checkedBase ? checkedBase.value : LANG_DEFAULT_BASE;
    setLangDirective(model, { base });
    reconcileLangPicker();
}

/**
 * Reconcile the picker with the active tab's line 1.  Called on model
 * content change (typing the header by hand and using the picker are the
 * same operation) and on tab switch (the picker follows the tab).
 */
export function reconcileLangPicker(rebuilt) {
    const sel = currentLangSelection();
    const btnLabel = document.getElementById('lang-btn-label');
    if (btnLabel) btnLabel.textContent = baseShortLabel(sel.base);
    const radios = document.querySelectorAll('#lang-bases input[type=radio]');
    let found = false;
    radios.forEach(r => {
        r.checked = (r.value === sel.base);
        if (r.checked) found = true;
    });
    // The rendered rows stop matching the selection in two ways, and both are
    // the hidden bases (curly-infix / neoteric): the buffer names one and it
    // has no row yet, or it has moved off one whose row is still sitting there.
    // renderLangMenu always produces a row for the current base and only for
    // the current base, so this settles in one pass -- `rebuilt` is a stop, not
    // a retry budget.
    const stale = Array.from(radios).some(r =>
        r.value !== sel.base && !LANG_READERS_SHOWN.includes(baseReader({ name: r.value })));
    if ((!found || stale) && radios.length && !rebuilt) renderLangMenu(true);
}

/**
 * Wire the header button + popover.  Mirrors the Examples popover: reparent
 * to <body> to escape ancestor overflow/stacking contexts, anchor with
 * position:fixed, close on outside-click / Escape.
 */
export function initLangPicker() {
    const langBtn = document.getElementById('lang-btn');
    const langMenu = document.getElementById('lang-menu');
    if (!langBtn || !langMenu) return;
    if (langMenu.parentElement !== document.body) {
        document.body.appendChild(langMenu);
    }
    const closeLang = () => {
        langMenu.hidden = true;
        langBtn.setAttribute('aria-expanded', 'false');
    };
    const openLangAt = (anchor) => {
        langMenu.hidden = false;
        langBtn.setAttribute('aria-expanded', 'true');
        const r = anchor.getBoundingClientRect();
        langMenu.style.top = `${r.bottom + 4}px`;
        langMenu.style.right = `${Math.max(4, window.innerWidth - r.right)}px`;
        reconcileLangPicker();
    };
    langBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        langMenu.hidden ? openLangAt(langBtn) : closeLang();
    });
    document.addEventListener('click', (e) => {
        if (!langMenu.hidden && !langMenu.contains(e.target) && e.target !== langBtn) {
            closeLang();
        }
    });
    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && !langMenu.hidden) closeLang();
    });
    // Mobile: the "Language..." item in the ⋯ overflow opens the same
    // popover, anchored to the overflow button.  stopPropagation so the
    // click never reaches the document-level outside-click closer (which
    // would immediately re-hide the menu); that also bypasses the overflow
    // menu's own close-on-click delegate, so close it here.
    const moreBtn = document.getElementById('more-btn');
    document.querySelectorAll('[data-action="language"]').forEach(item => {
        item.addEventListener('click', (e) => {
            e.stopPropagation();
            const moreMenu = document.getElementById('more-menu');
            if (moreMenu) moreMenu.hidden = true;
            moreBtn?.setAttribute('aria-expanded', 'false');
            openLangAt(moreBtn || langBtn);
        });
    });
    renderLangMenu();
}

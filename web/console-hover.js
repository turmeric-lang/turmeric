// console-hover.js -- W3, doc hover in the console transcript.
//
//
// This needs no markup at all, which is the whole reason it is cheap.
// appendToConsole inserts HTML it built itself and console lines are ordinary
// text nodes, so caretPositionFromPoint lands on the right node without a span
// around every identifier. Wrapping them would put an escaping surface exactly
// where "interpreted stdout stays inert" lives.
//
// WHICH lines answer is a judgement, not a capability. The echoed `turi> ...`
// lines and error lines do; program stdout does not. Text a program happened
// to print is not a symbol reference, and a hover card about `println` over a
// program that printed the word "println" is a lie about what that text is.

import { debounce } from './storage.js';

// The documented names and the doc lookup, from main.js. Set by
// initConsoleHover().
let deps = { getDocNames: () => [], wasmDocLookup: () => Promise.resolve(null) };

/**
 * REPL input at the bottom of the console
 */
const CONSOLE_IDENT = /[A-Za-z0-9\-?!*/><+=_]/;

/** The identifier under `offset` in `text`, or ''. Same character class the
 *  server's occurrence scanner uses, so a hover lands on what a hover means. */
function identifierAt(text, offset) {
    if (!text || offset < 0 || offset > text.length) return '';
    let i = offset;
    if ((i >= text.length || !CONSOLE_IDENT.test(text[i])) &&
        i > 0 && CONSOLE_IDENT.test(text[i - 1])) i--;
    if (i >= text.length || !CONSOLE_IDENT.test(text[i])) return '';
    let start = i, end = i;
    while (start > 0 && CONSOLE_IDENT.test(text[start - 1])) start--;
    while (end < text.length && CONSOLE_IDENT.test(text[end])) end++;
    const word = text.slice(start, end);
    // A bare number is not a symbol; neither is a lone operator run that the
    // index has never heard of, but that one the lookup answers for itself.
    return /^[0-9]+$/.test(word) ? '' : word;
}

/** Does this text node belong to a line a hover may answer on? */
function consoleLineAnswers(node) {
    if (!node) return false;
    const parent = node.parentElement;
    if (!parent) return false;
    // An error line: the whole span carries the class.
    if (parent.classList && parent.classList.contains('console-error')) return true;
    // An echoed prompt line: `<span class="console-prompt">turi&gt;</span> code`,
    // so the code is a bare text node sitting right after that span.
    let prev = node.previousSibling;
    while (prev && prev.nodeType === Node.TEXT_NODE && !prev.data.trim()) {
        prev = prev.previousSibling;
    }
    return !!(prev && prev.nodeType === Node.ELEMENT_NODE &&
              prev.classList && prev.classList.contains('console-prompt'));
}

/** The (node, offset) under a viewport point, across the two browser spellings. */
function caretAt(x, y) {
    if (document.caretPositionFromPoint) {
        const p = document.caretPositionFromPoint(x, y);
        return p ? { node: p.offsetNode, offset: p.offset } : null;
    }
    if (document.caretRangeFromPoint) {
        const r = document.caretRangeFromPoint(x, y);
        return r ? { node: r.startContainer, offset: r.startOffset } : null;
    }
    return null;
}

/** Wire the transcript hover. `getDocNames` and `wasmDocLookup` come from main.js. */
export function initConsoleHover({ getDocNames, wasmDocLookup }) {
    deps = { getDocNames, wasmDocLookup };
    const consoleEl = document.getElementById('console');
    if (!consoleEl) return;

    let card = document.getElementById('console-hover-card');
    if (!card) {
        card = document.createElement('div');
        card.id = 'console-hover-card';
        card.className = 'console-hover-card';
        card.hidden = true;
        document.body.appendChild(card);
    }

    let shownFor = null;
    let token = 0;

    function hide() {
        shownFor = null;
        card.hidden = true;
    }

    async function show(name, x, y) {
        if (name === shownFor) return;
        shownFor = name;
        const mine = ++token;

        let summary = null;
        const entry = (deps.getDocNames() || []).find(d => d.name === name);
        if (entry && entry.summary) summary = entry.summary;
        else summary = await deps.wasmDocLookup(name).catch(() => null);
        // A later hover already won; do not paint over it.
        if (mine !== token) return;
        if (!summary) { hide(); return; }

        card.textContent = summary;
        card.hidden = false;
        // Place above the cursor when there is room, below when there is not.
        const rect = card.getBoundingClientRect();
        const top = (y - rect.height - 10 > 8) ? y - rect.height - 10 : y + 16;
        card.style.left = Math.max(8, Math.min(x, window.innerWidth - rect.width - 8)) + 'px';
        card.style.top = top + 'px';
    }

    const onMove = debounce((x, y) => {
        const caret = caretAt(x, y);
        if (!caret || caret.node.nodeType !== Node.TEXT_NODE) { hide(); return; }
        if (!consoleEl.contains(caret.node)) { hide(); return; }
        if (!consoleLineAnswers(caret.node)) { hide(); return; }
        const name = identifierAt(caret.node.data, caret.offset);
        if (!name) { hide(); return; }
        show(name, x, y);
    }, 120);

    consoleEl.addEventListener('mousemove', (e) => onMove(e.clientX, e.clientY));
    consoleEl.addEventListener('mouseleave', hide);
    consoleEl.addEventListener('scroll', hide);
}

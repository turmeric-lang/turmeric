// doc-panel.js -- the doc panel (D5: autodoc integration): the documented-names
// list from /doc-names.json, the search bar over it, the panel that shows one
// entry, and the worker's doc, type-of and explain lookups behind them.

import { WASM_STATE, wasmState, hasWorker, send } from './eval-client.js';
import { escapeHtml, escapeAttr } from './html.js';

// All documented names loaded from /doc-names.json on startup.
export let docNames = [];

/**
 * Fetch the doc name list and set up the search bar.
 */
let docNamesPromise = null;

export async function fetchDocNames() {
    // Idempotent. The WASM boot calls this, and so does the docs pane -- which
    // needs the symbol list for its search but has no reason to wait on (or be
    // lost to) a WASM boot that is slow or never completes. doc-names.json is
    // a static file; nothing about it depends on the runtime.
    if (docNamesPromise) return docNamesPromise;
    docNamesPromise = (async () => {
        try {
            const res = await fetch('/doc-names.json');
            if (!res.ok) {
                console.error('Failed to fetch doc-names.json:', res.status, res.statusText);
                docNamesPromise = null;
                return;
            }
            docNames = await res.json();
            console.log('docNames loaded successfully. Length:', docNames.length);
            initDocSearch();
        } catch (err) {
            console.error('Error fetching doc-names.json:', err);
            docNamesPromise = null;
            // Non-fatal — search bar stays empty
        }
    })();
    return docNamesPromise;
}

/**
 * Wire up the doc search input to filter docNames and show results.
 */
function initDocSearch() {
    const input = document.getElementById('doc-search');
    const results = document.getElementById('doc-search-results');
    if (!input || !results) return;

    let activeIndex = -1;

    function renderResults(items) {
        activeIndex = -1;
        if (!items.length) {
            results.innerHTML = '<div class="doc-no-results">No matches</div>';
            results.style.display = 'block';
            return;
        }
        results.innerHTML = items.slice(0, 40).map((item, i) => {
            const shortSummary = item.summary.replace(/^[\w/\-!?*+<>]+\s+--\s+/, '');
            const spiceTag = item.spice
                ? `<span class="doc-result-spice">tur-${escapeHtml(item.spice)}</span>`
                : '';
            return `<div class="doc-result-item" data-name="${escapeAttr(item.name)}" data-index="${i}">
                <span class="doc-result-name">${escapeHtml(item.name)}</span>
                <span class="doc-result-kind">${escapeHtml(item.kind)}</span>
                ${spiceTag}
                <span class="doc-result-summary">${escapeHtml(shortSummary)}</span>
            </div>`;
        }).join('');
        results.style.display = 'block';

        results.querySelectorAll('.doc-result-item').forEach(el => {
            el.addEventListener('mousedown', (e) => {
                e.preventDefault(); // keep focus on input
                selectResult(el.dataset.name);
            });
        });
    }

    function hideResults() {
        results.style.display = 'none';
        results.innerHTML = '';
        activeIndex = -1;
    }

    function selectResult(name) {
        input.value = '';
        hideResults();
        const entry = docNames.find(d => d.name === name) || null;
        wasmDocLookup(name).then(docText => showDocPanel(name, docText, entry));
    }

    function highlightActive() {
        const items = results.querySelectorAll('.doc-result-item');
        items.forEach((el, i) => el.classList.toggle('active', i === activeIndex));
        if (activeIndex >= 0 && items[activeIndex]) {
            items[activeIndex].scrollIntoView({ block: 'nearest' });
        }
    }

    let searchTimer;
    input.addEventListener('input', () => {
        clearTimeout(searchTimer);
        searchTimer = setTimeout(() => {
            const q = input.value.trim().toLowerCase();
            if (!q) { hideResults(); return; }
            const matches = docNames.filter(d =>
                d.name.toLowerCase().includes(q) ||
                d.summary.toLowerCase().includes(q)
            );
            renderResults(matches);
        }, 150);
    });

    input.addEventListener('keydown', (e) => {
        const items = results.querySelectorAll('.doc-result-item');
        if (e.key === 'ArrowDown') {
            e.preventDefault();
            activeIndex = Math.min(activeIndex + 1, items.length - 1);
            highlightActive();
        } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            activeIndex = Math.max(activeIndex - 1, -1);
            highlightActive();
        } else if (e.key === 'Enter') {
            if (activeIndex >= 0 && items[activeIndex]) {
                selectResult(items[activeIndex].dataset.name);
            } else if (input.value.trim()) {
                // Exact lookup
                selectResult(input.value.trim());
            }
        } else if (e.key === 'Escape') {
            hideResults();
            input.blur();
        }
    });

    input.addEventListener('blur', () => {
        // Small delay so mousedown on a result fires first
        setTimeout(hideResults, 150);
    });

    // Open the doc panel when the user starts typing in the search bar
    input.addEventListener('focus', () => {
        const pane = document.getElementById('doc-pane');
        if (pane && pane.style.display === 'none') {
            const container = document.querySelector('.repl-container');
            pane.style.display = 'flex';
            container?.classList.add('doc-open');
        }
        if (input.value.trim()) {
            input.dispatchEvent(new Event('input'));
        }
    });
}

/**
 * Look up documentation for `name` via the eval Worker.
 * Returns a Promise resolving to the doc string, or null if not found.
 */
export function wasmDocLookup(name) {
    if (!hasWorker() || wasmState() !== WASM_STATE.READY) return Promise.resolve(null);
    return new Promise((resolve) => {
        send({ type: 'doc', name }, {
            resolve,
            reject: () => resolve(null),
        });
    });
}

/**
 * Look up type for `expr` via the eval Worker.
 */
export function wasmTypeOf(expr) {
    if (!hasWorker() || wasmState() !== WASM_STATE.READY) return Promise.resolve('');
    return new Promise((resolve) => {
        send({ type: 'type-of', expr }, {
            resolve,
            reject: (err) => resolve(`error: ${err.message}`),
        });
    });
}

/**
 * Get detailed explanation for `code` via the eval Worker.
 */
export function wasmExplain(code) {
    if (!hasWorker() || wasmState() !== WASM_STATE.READY) return Promise.resolve('');
    return new Promise((resolve) => {
        send({ type: 'explain', code }, {
            resolve,
            reject: (err) => resolve(`Error: ${err.message}`),
        });
    });
}

/**
 * Show the doc panel with content for `name`.
 */
export function showDocPanel(name, docText, entry) {
    const pane = document.getElementById('doc-pane');
    const body = document.getElementById('doc-body');
    const title = document.getElementById('doc-title');
    const link = document.getElementById('doc-full-link');
    const container = document.querySelector('.repl-container');

    if (!pane || !body) return;

    // Fall back to entry if the caller didn't supply one (e.g. (doc name) eval)
    if (entry === undefined) {
        entry = (typeof docNames !== 'undefined')
            ? docNames.find(d => d.name === name) || null
            : null;
    }

    title.textContent = name || 'Documentation';

    if (docText) {
        body.textContent = docText;
    } else if (entry && entry.summary) {
        // wasm doc-lookup table only carries stdlib entries; for spice symbols
        // surface the JSON summary so the panel still has useful content.
        const tag = entry.spice
            ? `<p class="doc-placeholder">From <code>tur-${escapeHtml(entry.spice)}</code>.</p>`
            : '';
        body.innerHTML = tag +
            `<pre class="doc-summary">${escapeHtml(entry.summary)}</pre>`;
    } else {
        body.innerHTML = `<p class="doc-placeholder">No documentation found for <code>${escapeHtml(name)}</code>.</p>`;
    }

    // Link to the per-spice API page for spice symbols, stdlib index otherwise.
    if (entry && entry.spice) {
        link.href = `/docs/html/spices/${entry.spice}/api/`;
        link.textContent = `Open tur-${entry.spice} API \u2197`;
    } else {
        link.href = '/docs/html/api/index.html';
        link.textContent = 'Open full docs \u2197';
    }

    pane.style.display = 'flex';
    container?.classList.add('doc-open');
}

/**
 * Hide the doc panel.
 */
export function hideDocPanel() {
    const pane = document.getElementById('doc-pane');
    const container = document.querySelector('.repl-container');
    if (pane) pane.style.display = 'none';
    container?.classList.remove('doc-open');
}

/**
 * If the evaluated code looks like `(doc name)`, trigger the doc panel.
 * Called after each REPL eval / Run.
 */
export function maybeShowDoc(code) {
    const m = code.trim().match(/^\(\s*doc\s+([\w/\-!?*+]+)\s*\)$/);
    if (!m) return false;
    const name = m[1];
    wasmDocLookup(name).then(docText => showDocPanel(name, docText));
    return true;
}

// Wire doc panel close button
document.addEventListener('DOMContentLoaded', () => {
    document.getElementById('close-doc-btn')?.addEventListener('click', hideDocPanel);
});

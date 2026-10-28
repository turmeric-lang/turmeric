// console.js -- the console pane's transcript: appending to it, clearing and
// copying it, and keeping it in localStorage as data (console-runs.js).

import { STORAGE_KEYS, safeRead, safeWrite, debounce } from './storage.js';
import { consoleRuns, renderConsoleRuns } from './console-runs.js';
import { showStatus } from './status.js';

export const MAX_CONSOLE_LINES = 200;

export let consoleLog = [];

export function hydrateConsole() {
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

/**
 * Append text to the console
 */
export function appendToConsole(html) {
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

export const schedulePersistConsole = debounce(() => {
    safeWrite(STORAGE_KEYS.consol, consoleLog);
}, 500);

/**
 * Clear the console
 */
export function clearConsole() {
    const consoleEl = document.getElementById('console');
    consoleEl.innerHTML = '';
    consoleLog = [];
    safeWrite(STORAGE_KEYS.consol, []);
}

/**
 * Copy console content to clipboard
 */
export async function copyConsole() {
    const consoleEl = document.getElementById('console');
    const text = consoleEl.textContent;
    try {
        await navigator.clipboard.writeText(text);
        showStatus('Console copied to clipboard!', 'success');
    } catch (e) {
        showStatus('Failed to copy console', 'error');
    }
}

/** Put back a saved transcript (the trace timeline restores the console). */
export function setConsoleLog(log) {
    consoleLog = log;
}

/**
 * Try Turmeric - Web-based REPL
 * Main JavaScript file for the Turmeric WASM REPL
 */

import { TUTORIAL_STEPS } from './tutorials.js';
import { createLspClient } from './lsp-client.js';
import { encodeShareCode, decodeShareCode } from './share-codec.js';

// ============================================================================
// WASM Module State
// ============================================================================

const WASM_STATE = {
    INITIALIZING: 'initializing',
    LOADING: 'loading',
    READY: 'ready',
    ERROR: 'error'
};

let wasmState = WASM_STATE.INITIALIZING;
let evalWorker = null;
let evalCallId = 0;
// Every request awaiting the eval Worker, by id. Adding one arms the watchdog
// (see stopEvalWorker), so no call site has to remember to.
const pendingCalls = new (class extends Map {
    set(id, call) {
        super.set(id, call);
        armEvalWatchdog();
        return this;
    }
})();
let editor = null;
let monaco = null;
let consoleOutput = [];
let executionQueue = [];
let isExecuting = false;
let replHistory = [];
let replHistoryIndex = -1;
let currentLangMode = 'turmeric'; // tracks active #lang mode
let lspClient = null;            // language server adapter; booted lazily

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
// Code Examples
// ============================================================================

const EXAMPLES = {
    hello: `(println "Hello, World!")`,
    math: `;; Basic arithmetic
(+ 1 2 3)
(- 10 5)
(* 2 3 4)
(/ 100 4)`,
    factorial: `;; Factorial function
(defn factorial [n :int] :int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))

(factorial 10)`,
    fibonacci: `;; Fibonacci sequence
(defn fib [n :int] :int
  (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))

(fib 10)`,
    closure: `;; Closures example
(defn make-adder [x :int] (fn [y :int] (+ x y)))

(let [add5 (make-adder 5)]
  (println (add5 10)))

(let [add10 (make-adder 10)]
  (println (add10 20)))`,
    effects: `(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))`,
    sweet: `#lang turmeric/sweet
;; Sweet-exp syntax: indentation, curly-infix, neoteric, $

defn square [x : int] : int
  {x * x}

defn sum-squares [a : int b : int] : int
  +(square(a) square(b))

println $ sum-squares 3 4
`,
    'adt': `;; Algebraic data types and pattern matching
(defdata Shape :copy
  (Circle :float)
  (Rect   :float :float))

(defn area [s : Shape] : float
  (match s
    (Circle r) (* 3.14159 (* r r))
    (Rect w h) (* w h)))

(println (area (Circle 2.5)))
(println (area (Rect 3.5 4.0)))
`,
    'result': `;; Option and Result: no nulls, no exceptions
(defn safe-div [a : int b : int] : (Result int cstr)
  (if (= b 0)
    (err "division by zero")
    (ok (/ a b))))

(defn show [r : (Result int cstr)] : void
  (match r
    (Ok v)  (println v)
    (Err e) (println e)))

(show (safe-div 84 2))
(show (safe-div 1 0))
`,
    'literals': `;; Data literals: vectors, maps and sets
(let [v [10 20 30]
      m #map{:name "Ada" :lang "Turmeric"}
      s #set{1 1 2 3 3}]
  (println (vec-len v))
  (println (vec-get v 1))
  (println (map-get m :name))
  (println (set-count s)))   ; duplicates collapse
`,
    'saffron': `#lang saffron
;; Saffron: Turmeric with the types made optional.
;; Unannotated parameters and returns are any.

(defn add [a b] (+ a b))
(defn kind-of [x] (type-of x))
(defn describe [x] (if x "truthy" "falsy"))

(println (add 1 2))         ; ints
(println (add 1.5 2.25))    ; floats, same function
(println (kind-of "hi"))
(println (describe 0))      ; only nil and false are falsy
(println (describe false))
`,
    'saffron-hof': `#lang saffron
;; Higher-order Saffron: closures and vectors, no annotations
(defn make-adder [n] (fn [x] (+ x n)))
(defn apply-twice [f x] (f (f x)))
(defn sum [v] (vec-fold v 0 (fn [acc x] (+ acc x))))

(println (apply-twice (make-adder 7) 28))
(println (sum [1 2 3 4]))
(println (vec-len (vec-filter [1 2 3 4] (fn [x] (> x 2)))))
(println (vec-len [1 "two" 3.5]))   ; heterogeneous
`,
    'saffron-adt': `#lang saffron
;; Saffron ADTs: annotate the data's shape, leave the code dynamic
(defdata Shape :copy
  (Circle :float)
  (Rect   :float :float))

(defn area [s]
  (match s
    (Circle r) (* 3.14159 (* r r))
    (Rect w h) (* w h)))

(defn classify [n]
  (cond (> n 10) "big"
        (> n 0)  "small"
        else     "none"))

(println (area (Circle 2.5)))
(println (classify (area (Rect 3.5 4.0))))
(println (classify (area (Rect 0.5 1.5))))
`,
    'saffron-sweet': `#lang saffron/sweet
;; Saffron in sweet-expression syntax

defn abs [n]
  if {n < 0}
    {0 - n}
    n

defn countdown [n]
  when {n > 0}
    do
      println(n)
      countdown({n - 1})

println $ abs -7.1
countdown(3)
`,
    'scheme': `#lang r7rs
;; R7RS Scheme on the Turmeric runtime
(import (scheme base) (scheme write))

(define (fact n)
  (if (= n 0) 1 (* n (fact (- n 1)))))

(display (fact 25))     ; exact integers never overflow
(newline)
(write (map (lambda (x) (* x x)) (list 1 2 3 4)))
(newline)
(write (list (/ 7 2) (+ 1/2 1/3) (sqrt -4)))
(newline)
`,
    'scheme-macros': `#lang r7rs
;; Hygienic macros, named let, and call/cc
(import (scheme base) (scheme write))

(define-syntax swap!
  (syntax-rules ()
    ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))

(let ((p 1) (q 2))
  (swap! p q)
  (write (list p q))
  (newline))

(let loop ((i 0) (acc '()))
  (if (< i 5)
      (loop (+ i 1) (cons i acc))
      (begin (write acc) (newline))))

(define (find-first pred xs)
  (call-with-current-continuation
    (lambda (return)
      (for-each (lambda (x) (if (pred x) (return x))) xs)
      #f)))

(write (find-first even? '(1 3 4 5 6)))
(newline)
`,
    'scheme-records': `#lang r7rs
;; Records and error handling with guard
(import (scheme base) (scheme write))

(define-record-type point
  (make-point x y)
  point?
  (x point-x)
  (y point-y))

(define (dist2 p)
  (+ (* (point-x p) (point-x p)) (* (point-y p) (point-y p))))

(write (dist2 (make-point 3 4)))
(newline)

(define (checked-sqrt n)
  (if (< n 0)
      (raise 'negative)
      (sqrt n)))

(write (guard (e ((symbol? e) (list 'caught e)))
  (checked-sqrt -1)))
(newline)
(write (checked-sqrt 16))
(newline)
`,
    'scheme-sweet': `#lang r7rs/sweet
;; Scheme written with sweet-expressions (SRFI-110)
import (scheme base) (scheme write)

define (fib n)
  if {n < 2}
    n
    {fib{n - 1} + fib{n - 2}}

display $ fib 20
newline()
write $ map fib '(1 2 3 4 5 6 7 8)
newline()
`
};

// ============================================================================
// Local State Persistence
// ============================================================================

const STORAGE_KEYS = {
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
};

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

let storageDisabled = false;
let consoleLog = [];

function safeRead(key) {
    if (storageDisabled) return null;
    try {
        const raw = localStorage.getItem(key);
        return raw == null ? null : JSON.parse(raw);
    } catch {
        return null;
    }
}

function safeWrite(key, value) {
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

function isTutorialMode() {
    try {
        return new URLSearchParams(window.location.search).has('tutorial');
    } catch {
        return false;
    }
}

function hasUrlHashCode() {
    try {
        const hash = window.location.hash.slice(1);
        return new URLSearchParams(hash).has('code');
    } catch {
        return false;
    }
}

function debounce(fn, ms) {
    let t = null;
    return (...args) => {
        clearTimeout(t);
        t = setTimeout(() => fn(...args), ms);
    };
}

// ----------------------------------------------------------------------------
// Console transcript persistence (security-audit-plan W-2)
//
// The transcript is stored as DATA and rebuilt with createElement/textContent,
// so nothing read back out of localStorage is ever parsed as markup. It used
// to be the lines' HTML, re-inserted with insertAdjacentHTML on load -- which
// made any one-time injection into the console a permanent one.
//
// A line is a list of runs. A run is a string (text), or [tag, className, runs]
// for a tag in CONSOLE_TAGS; `br` carries no runs. Anything else in the markup
// appendToConsole was handed (a link in an error message, say) is kept as its
// text. Attributes other than class are not kept, which is why the flush <pre>
// blocks are a class (console-flush) rather than an inline style.
// ----------------------------------------------------------------------------

const CONSOLE_TAGS = new Set(['span', 'pre', 'strong', 'em', 'code', 'br']);

// Deeper than anything appendToConsole writes (two levels today); what the
// renderer does with a record nested past it is flatten it to text.
const CONSOLE_MAX_DEPTH = 8;

/** The runs of a parsed console line. */
function consoleRuns(nodes) {
    const runs = [];
    for (const n of nodes) {
        if (n.nodeType === Node.TEXT_NODE) {
            runs.push(n.data);
        } else if (n.nodeType === Node.ELEMENT_NODE) {
            const tag = n.localName;
            if (tag === 'br') runs.push(['br', '', []]);
            else if (CONSOLE_TAGS.has(tag)) runs.push([tag, n.className, consoleRuns(n.childNodes)]);
            else runs.push(n.textContent);
        }
    }
    return runs;
}

/** Text of a run list, for a record nested too deep to rebuild. */
function consoleRunsText(runs) {
    if (typeof runs === 'string') return runs;
    if (!Array.isArray(runs)) return '';
    return runs.map((r) => (typeof r === 'string' ? r
        : Array.isArray(r) ? consoleRunsText(r[2]) : '')).join('');
}

/**
 * Append a stored run list under `parent`. The record came out of storage, so
 * it is validated as it is walked rather than trusted: an unknown tag or a
 * malformed run contributes nothing, and nothing is ever parsed as HTML.
 */
function renderConsoleRuns(runs, parent, depth = 0) {
    if (!Array.isArray(runs)) return;
    for (const r of runs) {
        if (typeof r === 'string') {
            parent.appendChild(document.createTextNode(r));
        } else if (Array.isArray(r) && typeof r[0] === 'string' && CONSOLE_TAGS.has(r[0])) {
            if (depth >= CONSOLE_MAX_DEPTH) {
                parent.appendChild(document.createTextNode(consoleRunsText(r[2])));
                continue;
            }
            const el = document.createElement(r[0]);
            if (typeof r[1] === 'string' && r[1]) el.className = r[1];
            if (r[0] !== 'br') renderConsoleRuns(r[2], el, depth + 1);
            parent.appendChild(el);
        }
    }
}

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

function resetWorkspace() {
    if (!confirm('Reset workspace? This clears the editor buffer, console, and saved layout.')) return;
    try {
        for (const key of Object.values(STORAGE_KEYS)) localStorage.removeItem(key);
        localStorage.removeItem('tur.try.split.h.v1');
        localStorage.removeItem('tur.try.split.v.v1');
    } catch {}
    window.location.hash = '';
    window.location.reload();
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

const HTML_ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };

/**
 * Escape text for HTML -- element content or a quoted attribute value alike.
 *
 * It used to go through textContent -> innerHTML, which escapes `&`, `<` and
 * `>` but leaves both quotes alone, so the same helper was unsafe inside an
 * attribute. It was used there: a `#lang` line naming a base with a `"` in it
 * closed the language picker's value="..." and added its own attributes --
 * onfocus, autofocus -- to the radio button (security-audit-plan W-2). Quotes
 * are escaped now, so no caller has to know which context it is in.
 */
function escapeHtml(text) {
    return (text == null ? '' : String(text)).replace(/[&<>"']/g, (c) => HTML_ESCAPES[c]);
}

/**
 * Escape text that is going inside a quoted HTML attribute. The same as
 * escapeHtml now that that escapes quotes; kept as its own name so an
 * attribute site says what it is.
 */
function escapeAttr(text) {
    return escapeHtml(text);
}


/**
 * Format a console line with prompt and result
 */
function formatConsoleLine(input, result, isError = false) {
    const prompt = '<span class="console-prompt">></span>';
    const escapedInput = escapeHtml(input);
    const escapedResult = escapeHtml(result);
    
    if (isError) {
        return `${prompt} <span class="console-error">${escapedInput}\n${escapedResult}</span>`;
    }
    return `${prompt} ${escapedInput}\n<span class="console-result">${escapedResult}</span>`;
}

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
    consoleOutput = [];
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
        if (wasmState === WASM_STATE.READY) {
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
 * Start a fresh eval Worker and resolve once its wasm module is ready.
 *
 * Used at boot and again by stopEvalWorker, which is why it does nothing but
 * the worker: the one-time steps (doc names, the #lang registry, a share
 * link's code) stay in initWasm.
 */
function startEvalWorker() {
    return new Promise((resolve, reject) => {
        const worker = new Worker('/eval-worker.js');
        evalWorker = worker;

        worker.addEventListener('message', (e) => {
            // A stopped worker's last words -- a result or a print already in
            // flight when it was terminated -- belong to a session that no
            // longer exists.
            if (worker !== evalWorker) return;
            const msg = e.data;

            if (msg.type === 'ready') {
                resolve();
                return;
            }
            if (msg.type === 'init-error') {
                reject(new Error(msg.error));
                return;
            }

            // Console output forwarded from WASM print/printErr callbacks.
            if (msg.type === 'print') {
                appendToConsole(`<span class="console-output">${escapeHtml(msg.text)}</span>`);
                return;
            }
            if (msg.type === 'printErr') {
                appendToConsole(`<span class="console-error">${escapeHtml(msg.text)}</span>`);
                return;
            }

            // Resolve pending call promises (eval, format, doc, reset).
            const pending = pendingCalls.get(msg.id);
            if (!pending) return;
            pendingCalls.delete(msg.id);

            if (msg.type === 'eval-result') {
                const execTime = performance.now() - pending.startTime;
                updateExecTime(execTime);
                const isError = msg.result.startsWith('Error:') || msg.result.includes('error');
                pending.resolve({ result: msg.result, isError, execTime });
                setTimeout(processQueue, 0);
            } else if (msg.type === 'format-result') {
                pending.resolve(msg.result);
            } else if (msg.type === 'doc-result') {
                pending.resolve(msg.result);
            } else if (msg.type === 'type-of-result') {
                pending.resolve(msg.result);
            } else if (msg.type === 'explain-result') {
                pending.resolve(msg.result);
            } else if (msg.type === 'lang-registry-result') {
                pending.resolve(msg.result);
            } else if (msg.type === 'trace-run-result') {
                pending.resolve({ steps: msg.steps, stats: msg.stats, error: msg.error });
            } else if (msg.type === 'trace-state') {
                pending.resolve(msg.state);
            } else if (msg.type === 'trace-sites') {
                pending.resolve(msg.sites);
            } else if (msg.type === 'trace-found') {
                pending.resolve(msg.found);
            } else if (msg.type === 'trace-bytes') {
                pending.resolve(msg.bytes);
            } else if (msg.type === 'trace-released') {
                pending.resolve();
            } else if (msg.type === 'reset-done') {
                pending.resolve();
            } else if (msg.type === 'error') {
                pending.reject(new Error(msg.error));
                if (pending.isEval) setTimeout(processQueue, 0);
            }
        });

        worker.addEventListener('error', (e) => {
            reject(new Error(String(e.message || e)));
        });

        worker.postMessage({ type: 'init' });
    });
}

/**
 * Initialize the WASM module via the eval Worker.
 * All WASM calls (eval, format, doc lookup, reset) run inside the Worker so
 * that Atomics.wait is permitted and blocking select cannot freeze the tab.
 */
async function initWasm() {
    if (wasmState !== WASM_STATE.INITIALIZING) return;

    wasmState = WASM_STATE.LOADING;

    const loadingOverlay = document.getElementById('loading-overlay');
    const wasmStatus = document.getElementById('wasm-status-text');
    if (loadingOverlay) loadingOverlay.style.display = 'flex';
    if (wasmStatus) wasmStatus.textContent = 'Loading WASM module...';

    try {
        await startEvalWorker();

        console.log('Turmeric WASM runtime initialized');
        wasmState = WASM_STATE.READY;

        promptSetEnabled(true);

        await fetchDocNames();
        fetchLangRegistry();  // re-renders the picker from the C-side tables
        showStatus('Ready', 'success');
        loadFromUrlHash();

        if (loadingOverlay) loadingOverlay.style.display = 'none';

    } catch (error) {
        console.error('Failed to initialize WASM:', error);
        wasmState = WASM_STATE.ERROR;
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
function parseLangDirective(code) {
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

// ============================================================================
// Language picker (try-turmeric-lang-toggle-plan)
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
// ============================================================================

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
function fetchLangRegistry() {
    if (wasmState !== WASM_STATE.READY) return Promise.resolve();
    return new Promise((resolve) => {
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve,
            reject: () => resolve(null),
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ type: 'lang-registry', id });
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
function reconcileLangPicker(rebuilt) {
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
function initLangPicker() {
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

    if (wasmState !== WASM_STATE.READY) {
        reject(new Error('WASM not ready'));
        updateExecTime(0);
        setTimeout(processQueue, 0);
        return;
    }

    // turi_eval_typed detects and strips an inline #lang directive itself, so
    // pass the raw source through. Still parse it locally to keep the UI's
    // mode indicator (currentLangMode) in sync, and forward the base to the
    // Worker as a hint for runtimes that export _turi_wasm_set_lang. The base
    // IS the whole directive tail now -- set_lang rejects anything after it.
    const { lang } = parseLangDirective(code);
    if (lang !== null) currentLangMode = lang;
    const langDirective = lang;

    const id = ++evalCallId;
    pendingCalls.set(id, { resolve, reject, startTime: performance.now(), isEval: true });
    evalWorker.postMessage({ type: 'eval', id, input: code, lang: langDirective, quiet });
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
    if (pendingCalls.size === 0) {
        clearInterval(evalWatchdogTimer);
        evalWatchdogTimer = null;
        if (stopBtn) stopBtn.hidden = true;
        // Only a panel the watchdog opened; a Run's own spinner is executeCode's.
        if (evalWatchdogShowedPanel && panel) panel.style.display = 'none';
        evalWatchdogShowedPanel = false;
        return;
    }
    let oldest = Infinity;
    for (const call of pendingCalls.values()) oldest = Math.min(oldest, call.startTime);
    const age = performance.now() - oldest;
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
    if (!evalWorker || evalStopping) return;
    evalStopping = true;
    evalWorker.terminate();
    evalWorker = null;
    wasmState = WASM_STATE.LOADING;
    promptSetEnabled(false);

    const owed = [...pendingCalls.values()];
    pendingCalls.clear();
    const queued = executionQueue.splice(0);
    isExecuting = false;

    // What the old session had accepted is gone with it.
    currentLangMode = 'turmeric';
    replSessionReset();
    lastRunByTab.clear();

    const err = new Error(reason);
    for (const call of owed) call.reject(err);
    for (const run of queued) run.reject(err);
    evalWatchdogTick();   // nothing is pending now: hides Stop and its panel
    showStatus('Restarting...', 'info');

    startEvalWorker().then(() => {
        wasmState = WASM_STATE.READY;
        promptSetEnabled(true);
        appendToConsole('<span class="console-output">Started a fresh session '
                        + '-- definitions from earlier runs are gone.</span>');
        showStatus('Ready', 'success');
    }, (e) => {
        wasmState = WASM_STATE.ERROR;
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
    if (wasmState !== WASM_STATE.READY) return;

    const id = ++evalCallId;
    pendingCalls.set(id, {
        resolve: () => {
            currentLangMode = 'turmeric'; // turi_env_new() always starts in default mode
            /* The session forgot everything it had accepted, so the prompt's
             * document has to forget it too -- otherwise completion keeps
             * offering names `:reset` just destroyed. */
            replSessionReset();
            lastRunByTab.clear();
            clearConsole();
            showStatus('Environment reset', 'success');
        },
        reject:  (err) => { console.error('Reset error:', err); showStatus('Failed to reset', 'error'); },
        startTime: performance.now(),
        isEval: false,
    });
    evalWorker.postMessage({ type: 'reset', id });
}

// ============================================================================
// Monaco Editor Setup
// ============================================================================

/**
 * Read a CSS custom property and return it only if Monaco can parse it.
 *
 * Monaco runs theme colors through Color.fromHex(), which returns opaque RED
 * on any parse failure -- so an `rgba()` or a `var()` chain that did not
 * resolve does not degrade, it repaints the strip bright red. Anything that is
 * not a literal #RGB / #RRGGBB / #RRGGBBAA is refused here and the caller's
 * fallback is used instead, which is the same color the stylesheet declares.
 */
function cssHex(varName, fallback) {
    try {
        const raw = getComputedStyle(document.documentElement)
            .getPropertyValue(varName).trim();
        if (/^#(?:[0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.test(raw)) return raw;
    } catch {
        /* no document, or a browser that refuses computed styles here */
    }
    return fallback;
}

/** Replace (or append) the alpha byte of a #RRGGBB / #RRGGBBAA color. */
function withAlpha(hex, alphaByte) {
    const base = /^#[0-9a-f]{8}$/i.test(hex) ? hex.slice(0, 7) : hex;
    return /^#[0-9a-f]{6}$/i.test(base) ? base + alphaByte : base;
}

/**
 * Configure Monaco Editor for Turmeric
 */
function configureMonaco() {
    // Define Turmeric language
    monaco.languages.register({
        id: 'turmeric',
        extensions: ['.tur'],
        aliases: ['Turmeric', 'tur'],
        mimetypes: ['text/x-turmeric']
    });
    
    // Define syntax highlighting rules
    monaco.languages.setMonarchTokensProvider('turmeric', {
        tokenizer: {
            root: [
                // Comments
                [/;.*/, 'comment'],
                
                // Strings
                [/"([^"\\]|\\.)*"/, 'string'],
                
                // Numbers
                [/\b\d+\b/, 'number'],
                [/\b\d+\.\d+\b/, 'number.float'],
                
                // Keywords
                [/\b(defn|def|fn|let|if|when|unless|loop|for|while|do|return|break|continue)\b/, 'keyword'],
                [/\b(println|print|read|eval|apply|map|filter|reduce|fold)\b/, 'keyword.operator'],
                
                // Built-in types and functions
                [/\b(int|float|bool|str|char|nil|true|false)\b/, 'type'],
                [/\b(rc\/of|rc\/clone|rc\/drop|rc\/strong-count)\b/, 'keyword.operator'],
                [/\b(defeffect|perform|handle|resume|cont)\b/, 'keyword.operator'],
                [/\b(try|catch|throw|raise)\b/, 'keyword.operator'],
                
                // Symbols
                [/[\+\-\*\/\=\<\>\!\&\|\%\^~]/, 'operator'],
                [/[\(\)\[\]\{\}]/, 'delimiter'],
                
                // Identifiers
                [/[a-zA-Z_][a-zA-Z0-9_\-]*/, 'identifier']
            ]
        }
    });

    // Language configuration — teaches Monaco which characters form bracket
    // pairs so its native bracket-pair colorization (rainbow parens, matching
    // the trowel editor) can depth-color them. `colorizedBracketPairs` lists
    // the pairs that participate in the rainbow cycle.
    monaco.languages.setLanguageConfiguration('turmeric', {
        brackets: [
            ['(', ')'],
            ['[', ']'],
            ['{', '}'],
        ],
        colorizedBracketPairs: [
            ['(', ')'],
            ['[', ']'],
            ['{', '}'],
        ],
        autoClosingPairs: [
            { open: '(', close: ')' },
            { open: '[', close: ']' },
            { open: '{', close: '}' },
            { open: '"', close: '"' },
        ],
        surroundingPairs: [
            { open: '(', close: ')' },
            { open: '[', close: ']' },
            { open: '{', close: '}' },
            { open: '"', close: '"' },
        ],
        comments: { lineComment: ';' },
        // Monaco's default word pattern is C-shaped: it breaks on `-`, `?`,
        // `!`, `/` and the comparison characters, so `nil-value` is three
        // words and `vec-push!` is four. That is wrong everywhere it is used
        // here -- double-click selection, occurrence highlight, the range a
        // completion replaces, and the name the hover fallback looks up.
        //
        // This is exactly the character class the server's own tokenizer
        // accepts (is_ident_char, src/lsp/lsp_util.c:6). Keeping the two in
        // step is what makes a client-side word and a server-side word the
        // same word.
        wordPattern: /[A-Za-z0-9\-?!*/><+=_]+/g,
    });

    // Rainbow-bracket palette, mirrored from trowel's turmeric-dark theme
    // (resources/turmeric-dark.theme.json rainbow0..6 + bracketError). Monaco
    // supports six depth colors before the cycle repeats, so we take six of
    // trowel's seven levels spanning the spectrum; the unexpected-bracket color
    // is trowel's BracketError red.
    const rainbowBrackets = {
        'editorBracketHighlight.foreground1':               '#EFA030', // orange
        'editorBracketHighlight.foreground2':               '#D7C94A', // yellow
        'editorBracketHighlight.foreground3':               '#A8C98A', // green
        'editorBracketHighlight.foreground4':               '#7AC4B8', // teal
        'editorBracketHighlight.foreground5':               '#8AB0E8', // blue
        'editorBracketHighlight.foreground6':               '#C4A0E8', // purple
        'editorBracketHighlight.unexpectedBracket.foreground': '#FF5C57', // error red
    };

    // Minimap + overview ruler (try-turmeric-navigation-and-minimap-plan M1).
    //
    // Colors are resolved out of the stylesheet rather than written here as
    // literals. The strip sits directly against the editor canvas and paints
    // marks in the same semantic colors the console and the status dots use;
    // a hardcoded hex would silently stop matching the first time one of those
    // tokens moves. c2mp reaches into CSS custom properties for the same
    // reason (minimap.js:204-209).
    //
    // Monaco parses these with Color.fromHex() and falls back to opaque RED on
    // any parse failure, so cssHex() refuses anything that is not #RGB /
    // #RRGGBB / #RRGGBBAA and hands back the literal fallback instead. A theme
    // token that fails to resolve must look like the old theme, never like an
    // error.
    const minimapColors = {
        // Canvas: the strip is part of the editor, not a panel beside it.
        'minimap.background':                   cssHex('--bg-base', '#0C0A08'),
        'minimapSlider.background':             withAlpha(cssHex('--text-dim', '#453F39'), '59'),
        'minimapSlider.hoverBackground':        withAlpha(cssHex('--text-sec', '#88796C'), '59'),
        'minimapSlider.activeBackground':       withAlpha(cssHex('--text-sec', '#88796C'), '8C'),

        // Marks inside the strip. Diagnostics keep full opacity -- they are
        // the reason to look at it; selection and find are washes, because a
        // mark you cannot see past is a mark that hides the shape of the file.
        // c2mp's rule (minimap.js §"marks as washes"), same conclusion.
        'minimap.errorHighlight':               cssHex('--error-color', '#D9735A'),
        'minimap.warningHighlight':             cssHex('--warning-color', '#EFA030'),
        'minimap.findMatchHighlight':           withAlpha(cssHex('--gold', '#D48B1C'), '99'),
        'minimap.selectionHighlight':           withAlpha(cssHex('--text-dim', '#453F39'), '80'),
        'minimap.selectionOccurrenceHighlight': withAlpha(cssHex('--text-sec', '#88796C'), '66'),
        'minimapGutter.addedBackground':        withAlpha(cssHex('--success-color', '#A8C98A'), 'B3'),
        'minimapGutter.modifiedBackground':     withAlpha(cssHex('--gold', '#D48B1C'), 'B3'),
        'minimapGutter.deletedBackground':      withAlpha(cssHex('--error-color', '#D9735A'), 'B3'),

        // The outboard ruler. This is the affordance c2mp builds by hand
        // (minimap.js RULER_W): a diagnostic gets a tick here because it must
        // stay findable when the minimap is scrolled past it; an occurrence
        // does not, because there can be fifty of them and they are noise at
        // this width.
        'editorOverviewRuler.background':       cssHex('--bg-base', '#0C0A08'),
        'editorOverviewRuler.border':           '#00000000',
        'editorOverviewRuler.errorForeground':  cssHex('--error-color', '#D9735A'),
        'editorOverviewRuler.warningForeground': cssHex('--warning-color', '#EFA030'),
        'editorOverviewRuler.infoForeground':   cssHex('--info-color', '#7AC4B8'),
        'editorOverviewRuler.findMatchForeground': withAlpha(cssHex('--gold', '#D48B1C'), 'CC'),
        'editorOverviewRuler.selectionHighlightForeground':
            withAlpha(cssHex('--text-sec', '#88796C'), '80'),
        'editorOverviewRuler.bracketMatchForeground': '#00000000',
    };

    // Define theme
    monaco.editor.defineTheme('turmeric-light', {
        base: 'vs',
        inherit: true,
        rules: [
            { token: 'comment', foreground: '8292a2' },
            { token: 'string', foreground: '2aa198' },
            { token: 'number', foreground: 'd16969' },
            { token: 'keyword', foreground: '20bbb4', fontStyle: 'bold' },
            { token: 'keyword.operator', foreground: '20bbb4' },
            { token: 'type', foreground: 'dc9656' },
            { token: 'operator', foreground: '89ddff' },
            { token: 'delimiter', foreground: '24292e' },
            { token: 'identifier', foreground: '24292e' }
        ],
        colors: {
            'editor.background': '#ffffff',
            'editor.foreground': '#24292e',
            'editorCursor.foreground': '#0366d6',
            'editor.lineHighlightBackground': '#f8f8f8',
            'editorLineNumber.foreground': '#959da5',
            // NOTE: Monaco parses theme colors with Color.fromHex() and falls
            // back to opaque RED on any parse failure -- rgba() strings are NOT
            // supported here. Always use #RRGGBB or #RRGGBBAA.
            'editor.selectionBackground': '#0366D64D',
            'editor.inactiveSelectionBackground': '#0366D61A',
            'editorIndentGuide.background': '#e1e4e8',
            'editorIndentGuide.activeBackground': '#959da5',
            ...rainbowBrackets,
            // The light theme is not currently selectable (setTheme below
            // pins turmeric-dark), but a half-themed minimap is exactly the
            // kind of thing that ships the day someone makes it selectable.
            ...minimapColors,
            'minimap.background':             '#ffffff',
            'editorOverviewRuler.background': '#ffffff',
            'minimapSlider.background':       '#959da54D',
            'minimapSlider.hoverBackground':  '#959da580',
            'minimapSlider.activeBackground': '#959da5A6',
        }
    });

    monaco.editor.defineTheme('turmeric-dark', {
        base: 'vs-dark',
        inherit: true,
        rules: [
            // Dark Spice Market syntax colors — keywords (amber), types (teal),
            // builtins (violet), strings (coral), comments (dark italic),
            // numbers (sage green) per design-notes.md
            { token: 'comment',          foreground: '48433D', fontStyle: 'italic' },
            { token: 'string',           foreground: 'D9735A' },
            { token: 'number',           foreground: 'A8C98A' },
            { token: 'number.float',     foreground: 'A8C98A' },
            { token: 'keyword',          foreground: 'EFA030', fontStyle: 'bold' },
            { token: 'keyword.operator', foreground: 'C4A0E8' },
            { token: 'type',             foreground: '7AC4B8' },
            { token: 'operator',         foreground: '6A5F58' },
            { token: 'delimiter',        foreground: '5A5448' },
            { token: 'identifier',       foreground: 'EAE0D2' },
        ],
        colors: {
            // Editor canvas
            'editor.background':                    '#0C0A08',
            'editor.foreground':                    '#EAE0D2',
            'editorGutter.background':              '#0C0A08',
            'editorLineNumber.foreground':          '#453F39',
            'editorLineNumber.activeForeground':    '#88796C',

            // Cursor & selection -- neutral medium gray, never a warning color.
            // Selection/bracket-match/scrollbar are structural chrome; red is
            // reserved for diagnostics.
            'editorCursor.foreground':              '#D48B1C',
            'editor.selectionBackground':           '#5A544C59',
            'editor.inactiveSelectionBackground':   '#5A544C33',
            'editor.selectionHighlightBackground':  '#5A544C2E',
            'editor.wordHighlightBackground':       '#5A544C2E',
            'editor.wordHighlightStrongBackground': '#5A544C40',

            // Line highlight
            'editor.lineHighlightBackground':       '#111009',
            'editor.lineHighlightBorder':           '#00000000',

            // Indent guides
            'editorIndentGuide.background1':        '#252119',
            'editorIndentGuide.activeBackground1':  '#3E3830',

            // Bracket matching — medium gray, so a matched delimiter never
            // reads as an error highlight
            'editorBracketMatch.background':        '#6A625926',
            'editorBracketMatch.border':            '#8C847A99',

            // Find matches
            'editorFindMatch.background':           '#D48B1C47',
            'editorFindMatch.border':               '#D48B1CA6',
            'editorFindMatchHighlight.background':  '#D48B1C1F',

            // Autocomplete / hover / suggest widgets
            'editorWidget.background':                       '#181512',
            'editorWidget.border':                           '#302B24',
            'editorWidget.foreground':                       '#EAE0D2',
            'editorSuggestWidget.background':                '#181512',
            'editorSuggestWidget.border':                    '#302B24',
            'editorSuggestWidget.foreground':                '#EAE0D2',
            'editorSuggestWidget.selectedBackground':        '#D48B1C26',
            'editorSuggestWidget.selectedForeground':        '#EAE0D2',
            'editorSuggestWidget.highlightForeground':       '#EFA030',
            'editorHoverWidget.background':                  '#181512',
            'editorHoverWidget.border':                      '#302B24',
            'editorHoverWidget.foreground':                  '#EAE0D2',

            // Scrollbars — medium gray, brightening on hover/drag
            'scrollbar.shadow':                     '#00000000',
            'scrollbarSlider.background':           '#5A544C8C',
            'scrollbarSlider.hoverBackground':      '#7A736ABF',
            'scrollbarSlider.activeBackground':     '#8C847ACC',

            // Focus ring — gold instead of VS Code blue
            'focusBorder':                          '#D48B1C66',

            // Rainbow brackets — depth-colored, matching the trowel editor
            ...rainbowBrackets,

            // Minimap + overview ruler, resolved from the stylesheet above
            ...minimapColors
        }
    });

    // Always use dark theme to match Dark Spice Market design
    monaco.editor.setTheme('turmeric-dark');
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
        theme: 'turmeric-dark',
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
    if (wasmState !== WASM_STATE.READY) {
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
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve: () => resolve(true),
            reject: () => resolve(false),
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ type: 'rewind', id });
    });
}

/**
 * Format the editor contents via the eval Worker.
 */
async function formatCode() {
    if (wasmState !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = editor.getValue();
    if (!code.trim()) return;

    try {
        const formatted = await new Promise((resolve, reject) => {
            const id = ++evalCallId;
            pendingCalls.set(id, { resolve, reject, startTime: performance.now(), isEval: false });
            evalWorker.postMessage({ type: 'format', id, input: code });
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

// CRC-32 table (one-time init), used by both the local headers and the
// central directory for store-only entries.
const CRC32_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
        t[n] = c >>> 0;
    }
    return t;
})();

function crc32(bytes) {
    let c = 0xffffffff;
    for (let i = 0; i < bytes.length; i++) {
        c = CRC32_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
    }
    return (c ^ 0xffffffff) >>> 0;
}

// Hand-rolled store-only ZIP writer. No deflate -- text files compress
// poorly, the dep surface stays flat, and Phase 3's unzipper can do the
// same. Each entry is `{ path: string, content: string }`. Returns a Blob.
function buildZip(entries) {
    const enc = new TextEncoder();
    const localChunks = [];      // bytes for the local file part
    const centralChunks = [];    // bytes for the central directory part
    let offset = 0;

    for (const entry of entries) {
        const nameBytes = enc.encode(entry.path);
        const dataBytes = enc.encode(entry.content);
        const c = crc32(dataBytes);
        const size = dataBytes.length;

        // Local file header (PK\x03\x04). All multi-byte fields little-endian.
        const local = new Uint8Array(30 + nameBytes.length);
        const lv = new DataView(local.buffer);
        lv.setUint32(0,  0x04034b50, true);   // signature
        lv.setUint16(4,  20,         true);   // version needed
        lv.setUint16(6,  0,          true);   // flags
        lv.setUint16(8,  0,          true);   // method: 0 = store
        lv.setUint16(10, 0,          true);   // mod time (unset)
        lv.setUint16(12, 0x21,       true);   // mod date (jan 1 1980)
        lv.setUint32(14, c,          true);   // crc-32
        lv.setUint32(18, size,       true);   // compressed size
        lv.setUint32(22, size,       true);   // uncompressed size
        lv.setUint16(26, nameBytes.length, true);
        lv.setUint16(28, 0,          true);   // extra field length
        local.set(nameBytes, 30);

        localChunks.push(local, dataBytes);

        // Central directory entry (PK\x01\x02).
        const central = new Uint8Array(46 + nameBytes.length);
        const cv = new DataView(central.buffer);
        cv.setUint32(0,  0x02014b50, true);
        cv.setUint16(4,  20,         true);   // version made by
        cv.setUint16(6,  20,         true);   // version needed
        cv.setUint16(8,  0,          true);
        cv.setUint16(10, 0,          true);
        cv.setUint16(12, 0,          true);
        cv.setUint16(14, 0x21,       true);
        cv.setUint32(16, c,          true);
        cv.setUint32(20, size,       true);
        cv.setUint32(24, size,       true);
        cv.setUint16(28, nameBytes.length, true);
        cv.setUint16(30, 0,          true);   // extra field length
        cv.setUint16(32, 0,          true);   // comment length
        cv.setUint16(34, 0,          true);   // disk number
        cv.setUint16(36, 0,          true);   // internal attrs
        cv.setUint32(38, 0,          true);   // external attrs
        cv.setUint32(42, offset,     true);   // local header offset
        central.set(nameBytes, 46);
        centralChunks.push(central);

        offset += local.length + dataBytes.length;
    }

    const localSize   = localChunks.reduce((s, a) => s + a.length, 0);
    const centralSize = centralChunks.reduce((s, a) => s + a.length, 0);

    // End of central directory record (PK\x05\x06).
    const eocd = new Uint8Array(22);
    const ev = new DataView(eocd.buffer);
    ev.setUint32(0,  0x06054b50,     true);
    ev.setUint16(4,  0,              true);   // disk
    ev.setUint16(6,  0,              true);
    ev.setUint16(8,  entries.length, true);
    ev.setUint16(10, entries.length, true);
    ev.setUint32(12, centralSize,    true);
    ev.setUint32(16, localSize,      true);
    ev.setUint16(20, 0,              true);   // comment length

    return new Blob([...localChunks, ...centralChunks, eocd],
                    { type: 'application/zip' });
}

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

const MAX_ZIP_BYTES   = 5  * 1024 * 1024;   // user-facing size guard
const PARSE_ZIP_LIMIT = 50 * 1024 * 1024;   // safety belt against OOM

// Store-only ZIP reader. Mirrors buildZip(): we only accept method 0
// because that's all we write. Anything else throws and the caller
// surfaces a toast. Returns `{ [path]: Uint8Array }`.
function readStoreZip(bytes) {
    if (bytes.length > PARSE_ZIP_LIMIT) {
        throw new Error('ZIP exceeds parse limit');
    }
    if (bytes.length < 22 ||
        bytes[0] !== 0x50 || bytes[1] !== 0x4b ||
        bytes[2] !== 0x03 || bytes[3] !== 0x04) {
        throw new Error('Not a ZIP file (bad magic bytes)');
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    // Scan back from the end for the EOCD signature 0x06054b50.
    let eocd = -1;
    for (let i = bytes.length - 22; i >= 0 && i > bytes.length - 65557; i--) {
        if (view.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) throw new Error('EOCD not found -- truncated or not a ZIP');
    const total    = view.getUint16(eocd + 10, true);
    const cdSize   = view.getUint32(eocd + 12, true);
    const cdOffset = view.getUint32(eocd + 16, true);
    const dec = new TextDecoder('utf-8');
    const out = {};
    let p = cdOffset;
    for (let i = 0; i < total; i++) {
        if (view.getUint32(p, true) !== 0x02014b50) {
            throw new Error(`Bad central directory entry at byte ${p}`);
        }
        const method   = view.getUint16(p + 10, true);
        const compSize = view.getUint32(p + 20, true);
        const nameLen  = view.getUint16(p + 28, true);
        const extraLen = view.getUint16(p + 30, true);
        const cmtLen   = view.getUint16(p + 32, true);
        const localOff = view.getUint32(p + 42, true);
        const name = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen));
        if (method !== 0) {
            throw new Error(`Unsupported compression method ${method} for ${name}`);
        }
        const lNameLen  = view.getUint16(localOff + 26, true);
        const lExtraLen = view.getUint16(localOff + 28, true);
        const dataOff = localOff + 30 + lNameLen + lExtraLen;
        out[name] = bytes.subarray(dataOff, dataOff + compSize);
        p += 46 + nameLen + extraLen + cmtLen;
    }
    if (p - cdOffset !== cdSize) throw new Error('Central directory size mismatch');
    return out;
}

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

/**
 * REPL input at the bottom of the console
 */
/* ---------------------------------------------------------------------------
 * W3: hover in the transcript
 *
 * This needs no markup at all, which is the whole reason it is cheap.
 * appendToConsole inserts HTML it built itself and console lines are ordinary
 * text nodes, so caretPositionFromPoint lands on the right node without a span
 * around every identifier. Wrapping them would put an escaping surface exactly
 * where "interpreted stdout stays inert" lives.
 *
 * WHICH lines answer is a judgement, not a capability. The echoed `turi> ...`
 * lines and error lines do; program stdout does not. Text a program happened
 * to print is not a symbol reference, and a hover card about `println` over a
 * program that printed the word "println" is a lie about what that text is.
 * ------------------------------------------------------------------------ */

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

function initConsoleHover() {
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
        const entry = (docNames || []).find(d => d.name === name);
        if (entry && entry.summary) summary = entry.summary;
        else summary = await wasmDocLookup(name).catch(() => null);
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
    if (!code || wasmState !== WASM_STATE.READY) return;

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
        theme: 'turmeric-dark',
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
        readOnly: wasmState !== WASM_STATE.READY,
        quickSuggestions: { other: true, comments: false, strings: false },
        suggestOnTriggerCharacters: true,
        acceptSuggestionOnEnter: 'on',
        tabCompletion: 'off',
        parameterHints: { enabled: true },
    });

    host.classList.toggle('repl-input-disabled', wasmState !== WASM_STATE.READY);
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
    initSymbolsMenu();

    // Language picker (dialect radio group + layer checkboxes)
    initLangPicker();

    // Solve button
    document.getElementById('solve-btn')?.addEventListener('click', solveStep);

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
            // Mirror visibility of the Solve menu item to the original button.
            const solveItem = moreMenu.querySelector('[data-cmd="solve-btn"]');
            const solveBtn = document.getElementById('solve-btn');
            if (solveItem && solveBtn) {
                solveItem.hidden = solveBtn.style.display === 'none';
            }
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

    // Tutorial close button
    document.getElementById('tutorial-close')?.addEventListener('click', () => {
        document.getElementById('tutorial-overlay')?.style.setProperty('display', 'none');
    });
    
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
    initConsoleHover();

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

function initHScrollDrag() {
    const rows = document.querySelectorAll('.editor-tabs, .editor-actions');
    rows.forEach(initHScrollDragRow);
}

function initHScrollDragRow(el) {
    if (!el) return;

    const updateOverflow = () => {
        el.classList.toggle('has-overflow', el.scrollWidth > el.clientWidth + 1);
    };
    updateOverflow();
    if (typeof ResizeObserver !== 'undefined') {
        // The row's own box never changes when a button appears -- its width
        // comes from the pane -- so observing only `el` misses the overflow
        // that Solve or Back-from-definition unhiding creates. Watch the
        // children too, and re-attach as the child list changes (tabs are
        // re-rendered wholesale by renderTabs()).
        const ro = new ResizeObserver(updateOverflow);
        const observeChildren = () => {
            ro.disconnect();
            ro.observe(el);
            for (const child of el.children) ro.observe(child);
        };
        observeChildren();
        if (typeof MutationObserver !== 'undefined') {
            new MutationObserver(() => {
                observeChildren();
                updateOverflow();
            }).observe(el, { childList: true, attributes: true, subtree: true,
                             attributeFilter: ['hidden', 'style'] });
        }
    }
    window.addEventListener('resize', updateOverflow);

    let startX = 0;
    let startScroll = 0;
    let dragging = false;
    let pointerId = null;

    const isInteractive = (target) => {
        return !!target.closest('button, select, input, a, [role="button"]');
    };

    el.addEventListener('pointerdown', (e) => {
        if (isInteractive(e.target)) return;
        startX = e.clientX;
        startScroll = el.scrollLeft;
        pointerId = e.pointerId;
        dragging = false;
        try { el.setPointerCapture(pointerId); } catch {}
    });

    el.addEventListener('pointermove', (e) => {
        if (pointerId !== e.pointerId) return;
        const dx = e.clientX - startX;
        if (!dragging && Math.abs(dx) > 6) {
            dragging = true;
            el.dataset.dragging = '1';
        }
        if (dragging) {
            el.scrollLeft = startScroll - dx;
            e.preventDefault();
        }
    });

    const endDrag = (e) => {
        if (pointerId !== null) {
            try { el.releasePointerCapture(pointerId); } catch {}
        }
        pointerId = null;
        if (dragging) {
            const swallow = (ev) => {
                ev.stopPropagation();
                ev.preventDefault();
            };
            el.addEventListener('click', swallow, { capture: true, once: true });
            setTimeout(() => { delete el.dataset.dragging; }, 0);
        }
        dragging = false;
    };
    el.addEventListener('pointerup', endDrag);
    el.addEventListener('pointercancel', endDrag);

    // Tab-button click handlers (switch + scroll-into-view) are attached by
    // renderTabs() in the multi-tab state module.
}

// ============================================================================
// Symbols outline (try-turmeric-navigation-and-minimap-plan, M2)
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
// ============================================================================

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

function initSymbolsMenu() {
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
            symbols = await lspClient.documentSymbols(editor.getModel()) || [];
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
        const action = () => (editor ? editor.getAction('editor.action.quickOutline') : null);
        if (!action()) {
            filterRow.remove();
        } else {
            filterRow.addEventListener('click', (e) => {
                e.stopPropagation();
                close();
                try {
                    editor.focus();
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

// ============================================================================
// Minimap (try-turmeric-navigation-and-minimap-plan, M1)
//
// Two inputs decide whether the strip is showing: an explicit user choice,
// and how wide the editor actually is. The user's choice wins outright; the
// width gate only speaks when they have not made one.
//
// The gate is on *measured editor width*, not a media query, because the
// split handle can make the editor 300px wide on a 1920px desktop -- and a
// minimap on a 300px editor is a fifth of the code column showing three
// characters of shape.
// ============================================================================

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
function applyMinimapPreference() {
    if (!editor) return;
    try {
        editor.updateOptions({ minimap: { enabled: minimapShouldBeOn() } });
    } catch (err) {
        console.warn('Try Turmeric: minimap update failed', err);
    }
    syncMinimapMenuItem();
}

/** Reflect the current state in the More menu's toggle row. */
function syncMinimapMenuItem() {
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
function toggleMinimap() {
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

function initMinimap() {
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
                return !!editor.getOption(monaco.editor.EditorOption.minimap).enabled;
            } catch { return false; }
        },
        preference: () => minimapPreference(),
        toggle: () => toggleMinimap(),
    };
}

// ============================================================================
// Draggable Split Handle (editor / console)
// ============================================================================

const SPLIT_KEY_H = 'tur.try.split.h.v1';
const SPLIT_KEY_V = 'tur.try.split.v.v1';
const SPLIT_MIN = 0.15;
const SPLIT_MAX = 0.85;

const isMobileSplit = () =>
    window.matchMedia && window.matchMedia('(max-width: 1024px)').matches;

function applySplit(fraction, vertical) {
    const container = document.querySelector('.repl-container');
    if (!container) return;
    const f = Math.min(SPLIT_MAX, Math.max(SPLIT_MIN, fraction));
    container.style.setProperty(vertical ? '--split-v' : '--split-h', String(f));
    if (editor) {
        requestAnimationFrame(() => {
            try { editor.layout(); } catch {}
            // The split is the one thing that can make the editor narrow on a
            // desktop, so the width gate is re-evaluated from here as well as
            // from the ResizeObserver -- layout() is synchronous, the observer
            // is not, and a gate that lags a drag by a frame reads as a bug.
            applyMinimapPreference();
        });
    }
}

function hydrateSplit() {
    const h = safeRead(SPLIT_KEY_H);
    if (typeof h === 'number' && isFinite(h)) applySplit(h, false);
    const v = safeRead(SPLIT_KEY_V);
    if (typeof v === 'number' && isFinite(v)) applySplit(v, true);
}

function initSplitHandle() {
    const handle = document.getElementById('split-handle');
    const container = document.querySelector('.repl-container');
    if (!handle || !container) return;

    hydrateSplit();

    const refreshAria = () => {
        handle.setAttribute('aria-orientation',
            isMobileSplit() ? 'horizontal' : 'vertical');
    };
    refreshAria();
    window.addEventListener('resize', refreshAria);

    let pointerId = null;
    let vertical = false;
    let rect = null;

    handle.addEventListener('pointerdown', (e) => {
        if (e.button !== undefined && e.button !== 0) return;
        pointerId = e.pointerId;
        vertical = isMobileSplit();
        rect = container.getBoundingClientRect();
        try { handle.setPointerCapture(pointerId); } catch {}
        handle.classList.add('is-dragging');
        document.body.classList.add('split-dragging');
        document.body.style.cursor = vertical ? 'row-resize' : 'col-resize';
        e.preventDefault();
    });

    handle.addEventListener('pointermove', (e) => {
        if (pointerId !== e.pointerId) return;
        const size = vertical ? rect.height : rect.width;
        if (size <= 0) return;
        const offset = vertical ? (e.clientY - rect.top) : (e.clientX - rect.left);
        const fraction = offset / size;
        applySplit(fraction, vertical);
        e.preventDefault();
    });

    const persistSplit = debounce(() => {
        const cs = getComputedStyle(container);
        const h = parseFloat(cs.getPropertyValue('--split-h'));
        const v = parseFloat(cs.getPropertyValue('--split-v'));
        if (isFinite(h)) safeWrite(SPLIT_KEY_H, h);
        if (isFinite(v)) safeWrite(SPLIT_KEY_V, v);
    }, 100);

    const endDrag = (e) => {
        if (pointerId === null) return;
        try { handle.releasePointerCapture(pointerId); } catch {}
        pointerId = null;
        handle.classList.remove('is-dragging');
        document.body.classList.remove('split-dragging');
        document.body.style.cursor = '';
        persistSplit();
    };
    handle.addEventListener('pointerup', endDrag);
    handle.addEventListener('pointercancel', endDrag);

    // Keyboard: arrows nudge by 2%, Home/End jump to bounds
    handle.addEventListener('keydown', (e) => {
        const mobile = isMobileSplit();
        const key = e.key;
        const axisKeys = mobile
            ? { dec: 'ArrowUp', inc: 'ArrowDown' }
            : { dec: 'ArrowLeft', inc: 'ArrowRight' };
        const cs = getComputedStyle(container);
        let current = parseFloat(
            cs.getPropertyValue(mobile ? '--split-v' : '--split-h')
        );
        if (!isFinite(current)) current = 0.5;
        let next = current;
        if (key === axisKeys.dec) next = current - 0.02;
        else if (key === axisKeys.inc) next = current + 0.02;
        else if (key === 'Home') next = SPLIT_MIN;
        else if (key === 'End') next = SPLIT_MAX;
        else return;
        e.preventDefault();
        applySplit(next, mobile);
        persistSplit();
    });

    // Double-click resets to 50/50
    handle.addEventListener('dblclick', () => {
        const mobile = isMobileSplit();
        applySplit(0.5, mobile);
        persistSplit();
    });
}

// ============================================================================
// PWA Install Affordances
// ============================================================================

let deferredInstallPrompt = null;

function initInstallAffordances() {
    // Capture the install prompt for browsers that support it (Chrome/Edge/Android).
    window.addEventListener('beforeinstallprompt', (e) => {
        e.preventDefault();
        deferredInstallPrompt = e;
        showInstallButton();
    });

    window.addEventListener('appinstalled', () => {
        deferredInstallPrompt = null;
        hideInstallButton();
        try { localStorage.setItem('tur.try.installed.v1', '1'); } catch {}
    });

    if (window.matchMedia && window.matchMedia('(display-mode: standalone)').matches) {
        return;
    }

    // iOS Safari has no beforeinstallprompt -- show a one-time Add to Home Screen hint.
    const ua = navigator.userAgent || '';
    const isIOS = /iPad|iPhone|iPod/.test(ua) && !window.MSStream;
    const isSafari = /^((?!chrome|android|crios|fxios).)*safari/i.test(ua);
    if (isIOS && isSafari) {
        let hinted = false;
        try { hinted = localStorage.getItem('tur.try.ios-a2hs-hint.v1') === '1'; } catch {}
        if (!hinted) {
            setTimeout(showIosA2hsHint, 3000);
        }
    }
}

function showInstallButton() {
    if (document.getElementById('install-btn')) return;
    const host = document.querySelector('.editor-actions');
    if (!host) return;
    const btn = document.createElement('button');
    btn.id = 'install-btn';
    btn.className = 'btn btn-icon';
    btn.title = 'Install Turmeric as an app';
    btn.innerHTML = `<svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
        <path d="M5 20h14v-2H5v2zM19 9h-4V3H9v6H5l7 7 7-7z"/>
    </svg>Install`;
    btn.addEventListener('click', async () => {
        if (!deferredInstallPrompt) return;
        deferredInstallPrompt.prompt();
        try { await deferredInstallPrompt.userChoice; } catch {}
        deferredInstallPrompt = null;
        hideInstallButton();
    });
    host.appendChild(btn);
}

function hideInstallButton() {
    const btn = document.getElementById('install-btn');
    if (btn) btn.remove();
}

function showIosA2hsHint() {
    if (document.getElementById('ios-a2hs-hint')) return;
    const hint = document.createElement('div');
    hint.id = 'ios-a2hs-hint';
    hint.setAttribute('role', 'status');
    hint.innerHTML = `
        <span>Install Turmeric: tap <strong>Share</strong> then <strong>Add to Home Screen</strong>.</span>
        <button class="btn btn-icon btn-sm" id="ios-a2hs-close" aria-label="Dismiss">&times;</button>
    `;
    document.body.appendChild(hint);
    const dismiss = () => {
        hint.remove();
        try { localStorage.setItem('tur.try.ios-a2hs-hint.v1', '1'); } catch {}
    };
    hint.querySelector('#ios-a2hs-close').addEventListener('click', dismiss);
    setTimeout(dismiss, 12000);
}

// ============================================================================
// Tutorial System
// ============================================================================

// Tutorial state
let currentTutorial = null;
let currentStep = 0;


/**
 * Load the given step of a tutorial into the editor and update the tutorial bar.
 */
function loadStep(tutorialId, stepIndex) {
    const steps = TUTORIAL_STEPS[tutorialId];
    if (!steps || stepIndex >= steps.length) return;

    const step = steps[stepIndex];
    if (editor) {
        editor.setValue(step.starter);
    }

    // Update tutorial info bar
    const bar = document.getElementById('tutorial-bar');
    const stepEl = document.getElementById('tutorial-bar-step');
    const descEl = document.getElementById('tutorial-bar-desc');
    const solveBtn = document.getElementById('solve-btn');

    if (bar) bar.style.display = 'flex';
    if (stepEl) stepEl.textContent = `${step.title} (${stepIndex + 1}/${steps.length})`;
    if (descEl) descEl.textContent = step.description;
    if (solveBtn) solveBtn.style.display = '';
}

/**
 * Fill in the answer for the current tutorial step without running it.
 */
function solveStep() {
    if (!currentTutorial) return;

    const steps = TUTORIAL_STEPS[currentTutorial];
    if (!steps) return;

    const step = steps[currentStep];
    if (!step) return;

    if (editor) {
        editor.setValue(step.answer);
        showStatus('Answer filled in -- click Run to execute', 'success');
    }
}

/**
 * Start a tutorial
 */
async function startTutorial(tutorialId) {
    // Hide tutorial overlay
    document.getElementById('tutorial-overlay')?.style.setProperty('display', 'none');

    currentTutorial = tutorialId;
    currentStep = 0;

    const steps = TUTORIAL_STEPS[tutorialId];
    if (steps && steps.length > 0) {
        showStatus(`Starting tutorial: ${tutorialId}`, 'success');
        loadStep(tutorialId, 0);
    }
}

/**
 * Show tutorial overlay
 */
function showTutorialOverlay() {
    const overlay = document.getElementById('tutorial-overlay');
    if (overlay) {
        overlay.style.display = 'flex';
    }
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
                    return new Worker(new URL('monaco-editor/esm/vs/language/json/json.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'css' || label === 'scss' || label === 'less') {
                    return new Worker(new URL('monaco-editor/esm/vs/language/css/css.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'html' || label === 'handlebars' || label === 'razor') {
                    return new Worker(new URL('monaco-editor/esm/vs/language/html/html.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'typescript' || label === 'javascript') {
                    return new Worker(new URL('monaco-editor/esm/vs/language/typescript/ts.worker.js', import.meta.url), { type: 'module' });
                }
                return new Worker(new URL('monaco-editor/esm/vs/editor/editor.worker.js', import.meta.url), { type: 'module' });
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

/* The service worker is a production feature, and on a dev server it is
 * actively harmful.
 *
 * sw.js falls through to cache-first for same-origin static assets, and its
 * precache names `/main.js` and `/styles.css` -- which on a dev server are the
 * files you are editing. So a worker installed by one `npm run dev` session
 * keeps serving that session's JS and CSS to every later one: the page comes up
 * unstyled, or running code you changed an hour ago, and the only way out is a
 * hard reload every single time.
 *
 * On a loopback host we therefore tear the worker down instead of registering
 * it. `?sw=1` opts back in, which is how the PWA and offline-docs specs still
 * exercise the real thing. */
const SW_LOOPBACK_HOSTS = new Set(['localhost', '127.0.0.1', '::1', '[::1]', '']);
const swDevDisabled = SW_LOOPBACK_HOSTS.has(location.hostname) &&
                      !new URLSearchParams(location.search).has('sw');

/* Was this page load already under a worker's control?
 *
 * The first visit is not: `activate` calls clients.claim(), which makes the
 * page controlled and fires `controllerchange` for a worker whose assets the
 * page is ALREADY running. Reloading on that is a pointless reload of a
 * perfectly current page. A later `controllerchange` on a load that started
 * controlled is the real signal -- a NEW worker took over, so what is on
 * screen came from the one it replaced. */
const swHadControllerAtLoad = 'serviceWorker' in navigator &&
                              !!navigator.serviceWorker.controller;
let swReloadArmed = false;

/**
 * Apply an activated update, once, at a moment that cannot lose work.
 *
 * Not immediately: an edit reaches localStorage through two chained 250ms
 * debounces, so re-navigating on the keystroke that happened to coincide with
 * an activation drops up to half a second of typing. Doing it when the app is
 * next backgrounded-then-foregrounded costs the reader nothing -- a resumed PWA
 * is exactly where they expect to find the new version anyway.
 */
function applySwUpdate() {
    if (swReloadArmed) return;
    swReloadArmed = true;
    const go = () => {
        document.removeEventListener('visibilitychange', onVisible);
        /* A navigation, not a reload -- the same reason forceUpdatePWA needs
         * one. The whole defect this exists to fix is that an iOS standalone
         * web app can answer location.reload() out of WebKit's own HTTP/page
         * cache; a routine update applied with reload() would land in exactly
         * that hole and the reader would again see nothing change. A URL with
         * no cache entry of its own cannot be answered that way.
         *
         * No loop: the load this starts is controlled by the worker that just
         * activated, so no further controllerchange fires until a genuinely
         * newer worker takes over. */
        hardNavigate(`${window.location.pathname}?u=${Date.now()}`);
    };
    const onVisible = () => { if (document.visibilityState === 'visible') go(); };
    if (document.visibilityState === 'hidden') { go(); return; }
    document.addEventListener('visibilitychange', onVisible);
}

if ('serviceWorker' in navigator && !swDevDisabled) {
    // Register after the page settles. Scope `/` so the origin-wide
    // kill-switch and runtime caching can target docs paths too.
    window.addEventListener('load', () => {
        navigator.serviceWorker.register('/sw.js', { scope: '/' })
            .then((reg) => {
                /* An installed PWA on iOS is the case this exists for. The
                 * browser checks for a new sw.js on a NAVIGATION in scope, and
                 * `register()` above is one -- but it only runs on `load`, and
                 * a standalone web app relaunched from the app switcher is
                 * RESUMED, not navigated. So the update check never ran, and
                 * closing and reopening the app -- the thing everyone tries
                 * first -- could not possibly help. Ask on every foreground. */
                const check = () => { reg.update().catch(() => {}); };
                document.addEventListener('visibilitychange', () => {
                    if (document.visibilityState === 'visible') check();
                });
                window.addEventListener('pageshow', (e) => { if (e.persisted) check(); });
            })
            .catch((err) => console.warn('SW registration failed:', err));
    });

    /* ... and once a new worker HAS taken over, apply it. sw.js ends `install`
     * with skipWaiting() and `activate` with clients.claim(), so a new worker
     * becomes this page's controller on its own -- but nothing re-rendered the
     * page, so the reader kept looking at the assets the OLD worker served and
     * the update was invisible until some later cold launch. On iOS that launch
     * may never come. */
    navigator.serviceWorker.addEventListener('controllerchange', () => {
        if (swHadControllerAtLoad) applySwUpdate();
    });
} else if ('serviceWorker' in navigator) {
    window.addEventListener('load', async () => {
        try {
            const regs = await navigator.serviceWorker.getRegistrations();
            // Nothing installed is the steady state, and returning here is what
            // makes the reload below safe: after one teardown there are no
            // registrations left, so the next load cannot reload again.
            if (!regs.length) return;

            const wasControlled = !!navigator.serviceWorker.controller;
            await Promise.all(regs.map((r) => r.unregister()));
            if ('caches' in window) {
                const keys = await caches.keys();
                await Promise.all(keys.map((k) => caches.delete(k)));
            }
            console.info('Service worker disabled on localhost (append ?sw=1 to keep it).');

            // This page load was served BY the worker just removed, so what is
            // on screen is the stale copy. Reload once to get the real files --
            // otherwise the fix appears not to have worked until a manual
            // reload, which is the thing being fixed.
            if (wasControlled) window.location.reload();
        } catch (err) {
            console.warn('SW teardown failed:', err);
        }
    });
}

/**
 * Name the build that is actually running, in the overflow menu.
 *
 * Read from the service worker's own cache key, which the build stamps as
 * `tur-try-v1-<version>-<short sha>`: that is the token the precache is keyed
 * on, so it names the build whose assets are on screen -- not the build the
 * page WANTS to be, which is what a compiled-in constant would report even
 * while a stale worker served everything around it.
 *
 * Silent when there is no worker (a plain browser tab with SW disabled, the dev
 * server): "no cached build" is the normal state there and not worth a row.
 */
async function refreshBuildStamp() {
    const el = document.getElementById('build-stamp');
    if (!el) return;
    el.textContent = '';
    try {
        if (!('caches' in window) || !navigator.serviceWorker?.controller) return;
        const key = (await caches.keys()).find((k) => k.startsWith('tur-try-'));
        if (!key) return;
        // tur-try-v1-0.49.2-d32ec4ff7-precache -> 0.49.2-d32ec4ff7
        const build = key.replace(/^tur-try-v\d+-/, '').replace(/-(precache|runtime)$/, '');
        el.textContent = `Build ${build}`;
    } catch (_) {
        /* Cache Storage can throw outright in a private window or with site
         * data blocked. A missing stamp is not worth an error. */
    }
}

/**
 * Force-update the installed PWA: unregister every service worker, drop all
 * Cache Storage entries, then NAVIGATE (not reload) so the page and its assets
 * are fetched fresh from the network. This is the user-facing escape hatch for
 * a stuck cache -- it works even when the shipped sw.js forgot to bump
 * CACHE_VERSION, because it nukes the SW entirely so the following navigation
 * is uncontrolled and hits the network directly. localStorage (the editor tabs)
 * is intentionally left alone, so no code is lost.
 *
 * It used to end in location.reload(), which made it a placebo in an installed
 * iOS PWA for months: WebKit can answer a reload out of its own HTTP/page
 * cache, which neither the unregister nor the cache wipe touches, so the same
 * HTML came back naming the same hashed assets. It showed "Updating..." and
 * reloaded, so it looked like it had worked. See the navigation at the end.
 */
async function forceUpdatePWA() {
    if (typeof showStatus === 'function') showStatus('Updating...', 'info');
    try {
        if ('serviceWorker' in navigator) {
            const regs = await navigator.serviceWorker.getRegistrations();
            await Promise.all(regs.map((r) => r.unregister()));
        }
        if ('caches' in window) {
            const keys = await caches.keys();
            await Promise.all(keys.map((k) => caches.delete(k)));
        }
    } catch (err) {
        console.warn('Force update failed:', err);
    }
    /* Navigate to a URL nothing has ever cached, rather than reload().
     *
     * The unregister and the cache wipe above are the easy half. The hard half
     * is getting a fetch to actually happen afterwards: in an iOS standalone
     * web app `location.reload()` can be answered from WebKit's own HTTP/page
     * cache, which the two lines above do not touch. The old HTML comes back,
     * it names the same hashed assets, and Force update appears to do nothing
     * -- which is exactly what it appeared to do.
     *
     * A navigation to a URL with no cache entry of its own cannot be answered
     * that way, and the service worker treats HTML navigations as network-first
     * anyway, so the query parameter costs nothing when a worker is present.
     * `replace`, not `assign`, so the cache-busted URL does not become a
     * history entry the reader can go back to. */
    hardNavigate(`${window.location.pathname}?u=${Date.now()}`);
}

/**
 * Reload the page.  The one indirection exists for the smoke test: current
 * Chromium makes `location.reload` non-configurable, so a test cannot stub
 * it -- it installs `window.__turiReload` instead and observes the intent
 * without navigating away.
 */
function hardReload() {
    if (typeof window.__turiReload === 'function') { window.__turiReload(); return; }
    window.location.reload();
}

/**
 * Navigate to `url`, replacing the current history entry.
 *
 * Same test indirection as hardReload, and for the same reason: a spec cannot
 * stub `location.replace` on current Chromium, so it installs
 * `window.__turiNavigate` and observes the intent instead of navigating away.
 */
function hardNavigate(url) {
    if (typeof window.__turiNavigate === 'function') { window.__turiNavigate(url); return; }
    window.location.replace(url);
}

// ============================================================================
// Doc Panel (D5: autodoc integration)
// ============================================================================

// All documented names loaded from /doc-names.json on startup.
let docNames = [];

/**
 * Fetch the doc name list and set up the search bar.
 */
let docNamesPromise = null;

async function fetchDocNames() {
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
function wasmDocLookup(name) {
    if (!evalWorker || wasmState !== WASM_STATE.READY) return Promise.resolve(null);
    return new Promise((resolve) => {
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve,
            reject: () => resolve(null),
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ type: 'doc', id, name });
    });
}

/**
 * Look up type for `expr` via the eval Worker.
 */
function wasmTypeOf(expr) {
    if (!evalWorker || wasmState !== WASM_STATE.READY) return Promise.resolve('');
    return new Promise((resolve) => {
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve,
            reject: (err) => resolve(`error: ${err.message}`),
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ type: 'type-of', id, expr });
    });
}

/**
 * Get detailed explanation for `code` via the eval Worker.
 */
function wasmExplain(code) {
    if (!evalWorker || wasmState !== WASM_STATE.READY) return Promise.resolve('');
    return new Promise((resolve) => {
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve,
            reject: (err) => resolve(`Error: ${err.message}`),
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ type: 'explain', id, code });
    });
}

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

/**
 * Show the doc panel with content for `name`.
 */
function showDocPanel(name, docText, entry) {
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
function hideDocPanel() {
    const pane = document.getElementById('doc-pane');
    const container = document.querySelector('.repl-container');
    if (pane) pane.style.display = 'none';
    container?.classList.remove('doc-open');
}

/**
 * If the evaluated code looks like `(doc name)`, trigger the doc panel.
 * Called after each REPL eval / Run.
 */
function maybeShowDoc(code) {
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

// ============================================================================
// In-app docs browser (OD2)
//
// Reads the docs pack -- chrome-free fragments plus index.json, emitted by the
// same generators that build turmeric-lang.com/docs/html/ -- and renders it
// over the REPL. Nothing here navigates: the editor buffer, console history,
// and WASM session all survive a docs session untouched, which is the whole
// point of embedding rather than linking out.
//
// The pack is precached by the service worker (OD3), so every fetch below is
// expected to hit the cache when offline.
// ============================================================================

const DOCS_PACK_BASE = '/docs-pack';

/**
 * How many pages each of the nav's dated sections lists.
 *
 * Ten apiece, matching the Recently Added and Recently Updated cards at the top
 * of the website's guides index. Those cards are guides only; these also carry
 * API modules, since the pane is a browser over the whole pack. The dates behind
 * both are the same numbers -- the generator stamps each page with the day its
 * source was first committed and the day it was last committed to, and passes
 * the guides' dates straight through -- so the two cannot disagree about when a
 * guide arrived or when it last changed.
 */
const DOCS_RECENT_COUNT = 10;

/**
 * The page the pane opens on when nothing else says where to go.
 *
 * The quickstart, not whatever guide happens to sort first in the pack. A
 * reader who opened the docs without asking for a page has not told us what
 * they want, and "the beginning" is the only answer that is right for a first
 * visit and harmless for every other -- an alphabetical accident is neither.
 * A previously visited page still wins; this is the floor, not a redirect.
 */
const DOCS_DEFAULT_REF = 'guides/quickstart';

let docsIndex = null;          // parsed index.json, once
let docsIndexPromise = null;   // in-flight load, so concurrent opens share one
let docsCurrentRef = null;     // e.g. 'guides/hkt-guide'
let docsNavCollapsed = false;
/**
 * Where you were in each page you have read this session, keyed by ref.
 *
 * Per-ref rather than one global offset on purpose: the pane is a browser over
 * many pages, so coming back to guide A must not restore guide B's offset.
 *
 * The map itself is session-scoped: every page you read this session is worth
 * an entry in memory, and none of them is worth a localStorage write. The one
 * exception is the page the pane is actually showing when the app goes away,
 * which persistDocsState() banks alongside the ref -- restoring the open guide
 * but not the place in it just moves the complaint one step along.
 */
const docsScrollByRef = new Map();

/**
 * Assign scrollTop without the smooth animation `.docs-article` sets in CSS.
 *
 * Restoring a remembered offset under `scroll-behavior: smooth` glides down
 * the page on every reopen, which reads as the pane scrolling by itself rather
 * than as returning you to your place.
 */
function docsSetScroll(article, top) {
    try {
        article.scrollTo({ top, behavior: 'instant' });
    } catch {
        // 'instant' is not universally accepted by scrollTo's options form;
        // the plain assignment still lands, it just animates.
        article.scrollTop = top;
    }
}

/**
 * Record where the article column is scrolled to for the page it is showing.
 *
 * Refuses to record while the pane is closed. A hidden element reports
 * scrollTop 0, so a `scroll` callback still queued when the overlay went
 * display:none would overwrite the offset closeDocsPane had just banked --
 * with exactly the value the restore is supposed to avoid.
 */
function rememberDocsScroll() {
    const article = document.getElementById('docs-article');
    if (!article || !docsCurrentRef || !docsPaneIsOpen()) return;
    docsScrollByRef.set(docsCurrentRef, article.scrollTop);
}

/**
 * Fetch and cache the pack manifest. Resolves to null when the pack is absent
 * (a dev server that never ran `just docs`), which the pane reports rather
 * than failing silently.
 */
function loadDocsIndex() {
    if (docsIndex) return Promise.resolve(docsIndex);
    if (docsIndexPromise) return docsIndexPromise;
    docsIndexPromise = (async () => {
        try {
            const res = await fetch(`${DOCS_PACK_BASE}/index.json`);
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            docsIndex = await res.json();
            return docsIndex;
        } catch (err) {
            console.warn('docs pack unavailable:', err);
            docsIndexPromise = null;
            return null;
        }
    })();
    return docsIndexPromise;
}

/** All pack pages as one flat list of {ref, title, description, words, kind}. */
function docsAllPages() {
    if (!docsIndex) return [];
    const out = [];
    for (const g of docsIndex.guides || []) {
        out.push({ ref: `guides/${g.slug}`, title: g.title, kind: 'guide',
                   category: g.category, description: g.description, words: g.words,
                   added: g.added, updated: g.updated });
    }
    for (const m of docsIndex.api || []) {
        out.push({ ref: `api/${m.slug}`, title: m.title, kind: 'module',
                   category: m.category, description: m.description, words: m.words,
                   added: m.added, updated: m.updated });
    }
    for (const s of docsIndex.spices || []) {
        out.push({ ref: `spices/${s.slug}`, title: s.title, kind: 'spice',
                   category: s.category, description: s.description, words: s.words,
                   added: s.added, updated: s.updated });
    }
    return out;
}

/** Look up the manifest entry for a pack ref (no #fragment). */
function docsEntryFor(ref) {
    return docsAllPages().find(p => p.ref === ref) || null;
}

/**
 * Split a `#doc=` value into its page ref and in-page anchor.
 * 'guides/hkt-guide#kinds' -> { ref: 'guides/hkt-guide', anchor: 'kinds' }
 */
function parseDocsRef(value) {
    if (!value) return { ref: null, anchor: null };
    const hashAt = value.indexOf('#');
    if (hashAt === -1) return { ref: value, anchor: null };
    return { ref: value.slice(0, hashAt), anchor: value.slice(hashAt + 1) };
}

/**
 * The URL hash as an ordered list of [key, value] pairs.
 *
 * `key=value&key=value`, the shape the share feature already used for
 * `#code=<compressed>` -- but parsed by hand rather than with URLSearchParams,
 * because a docs deep link should read `#doc=guides/hkt-guide`, not
 * `#doc=guides%2Fhkt-guide`. Values are written literally and only `&` and `%`
 * are escaped; neither of our two values (url-safe base64 for `code`, a slug
 * path for `doc`) contains either, so in practice the hash stays plain text.
 */
function parseHashPairs() {
    const raw = window.location.hash.slice(1);
    if (!raw) return [];
    return raw.split('&').filter(Boolean).map(part => {
        const eq = part.indexOf('=');
        const key = eq === -1 ? part : part.slice(0, eq);
        const value = eq === -1 ? '' : part.slice(eq + 1);
        return [key, value.replace(/%26/gi, '&').replace(/%25/g, '%')];
    });
}

/**
 * Merge a key into the URL hash without disturbing the others.
 *
 * The share feature writes `code`; the docs pane writes `doc`. A shared link
 * that also points at a guide round-trips, and neither key clobbers the other.
 * Passing null removes the key.
 */
function setHashParam(key, value) {
    const pairs = parseHashPairs().filter(([k]) => k !== key);
    if (value !== null && value !== undefined) {
        pairs.push([key, String(value).replace(/%/g, '%25').replace(/&/g, '%26')]);
    }
    const next = pairs.map(([k, v]) => (v === '' ? k : `${k}=${v}`)).join('&');
    // replaceState, not `location.hash =`: a docs navigation should not push a
    // history entry per page, and it must not fire our own hashchange handler.
    const url = `${window.location.pathname}${window.location.search}${next ? '#' + next : ''}`;
    window.history.replaceState(null, '', url);
}

function getHashParam(key) {
    const hit = parseHashPairs().find(([k]) => k === key);
    return hit ? hit[1] : null;
}

/** Group pack pages by category, preserving 'Other'/'core' last. */
function docsGroupByCategory(pages) {
    const groups = new Map();
    for (const p of pages) {
        const key = p.category || 'Other';
        if (!groups.has(key)) groups.set(key, []);
        groups.get(key).push(p);
    }
    const names = [...groups.keys()].sort((a, b) => {
        const rank = (n) => (n === 'core' ? -1 : n === 'Other' ? 1 : 0);
        return rank(a) - rank(b) || a.localeCompare(b);
    });
    return names.map(name => ({ name, pages: groups.get(name) }));
}

/**
 * The newest pages in the pack, newest first.
 *
 * Reads the `added` date the pack stamps on each page from git. A page git had
 * no add commit for carries no date and is skipped rather than sorted to one
 * end: "we do not know when this arrived" is not the same claim as "it is old",
 * and only one of the two belongs in a list of what is new.
 *
 * Deliberately not windowed to the last N days. A quiet month would empty the
 * section, and a nav that comes and goes is harder to learn than one whose top
 * entry is simply sometimes older -- the dates are on screen, so a reader can
 * see for themselves how recent "recent" currently is.
 */
function docsRecentPages(pages) {
    return pages
        .filter(p => p.added)
        .sort((a, b) => b.added.localeCompare(a.added)
                     || a.title.localeCompare(b.title))
        .slice(0, DOCS_RECENT_COUNT);
}

/**
 * The most recently edited pages in the pack, newest edit first.
 *
 * Reads `updated`, the day the page's source was last committed to -- the other
 * end of the same history `added` reads the near end of.
 *
 * A page whose last commit is the one that added it is left out: `added ===
 * updated` means nothing has happened to it since it arrived, which the
 * Recently Added section above already says. Without that filter every page
 * written this month would fill both lists, and the second one would stop
 * answering the question it is for -- "what changed under me?" is only
 * interesting about pages I might already have read.
 */
function docsUpdatedPages(pages) {
    return pages
        .filter(p => p.updated && p.updated !== p.added)
        .sort((a, b) => b.updated.localeCompare(a.updated)
                     || a.title.localeCompare(b.title))
        .slice(0, DOCS_RECENT_COUNT);
}

const DOCS_MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
                     'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/**
 * '2026-09-14' -> 'Sep 14'.
 *
 * Split rather than parsed through Date: `new Date('2026-09-14')` is UTC
 * midnight, which renders as the 13th for every reader west of Greenwich. The
 * date is a calendar day from a commit, not an instant, so it has no business
 * being shifted into anyone's timezone.
 */
function docsFormatDay(day) {
    const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(day || '');
    if (!m) return '';
    return `${DOCS_MONTHS[Number(m[2]) - 1] || ''} ${Number(m[3])}`.trim();
}

/**
 * A pinned link to the quickstart, above everything else in the nav.
 *
 * The one page a first-time reader wants, and the hardest to find by browsing:
 * it is filed under "Getting Started" like a dozen siblings, several rows down
 * a collapsed tree. Pinning it costs one line and spares that search. Rendered
 * only when the pack actually carries the page, so the nav never offers a link
 * it cannot open.
 */
function docsQuickstartSection() {
    const entry = docsEntryFor(DOCS_DEFAULT_REF);
    if (!entry) return '';
    return '<div class="docs-nav-section docs-nav-pinned"><ul>'
         + `<li><a href="#doc=${escapeAttr(DOCS_DEFAULT_REF)}"`
         + ` data-doc-ref="${escapeAttr(DOCS_DEFAULT_REF)}"`
         + ` title="${escapeAttr(entry.description || '')}">`
         + `${escapeHtml(entry.title)}</a></li>`
         + '</ul></div>';
}

/**
 * One dated, flat section -- "Recently Added" or "Recently Updated" -- across
 * all three kinds of page.
 *
 * A `<details>`, closed on load. Both sections answer a "what changed since I
 * last looked?" question, which you ask occasionally and never on the way to a
 * page you already know the name of -- so ten rows of it sitting permanently
 * between the nav's top and the tree taxes every other visit to pay for that
 * one. Closed each is a single line, and markDocsNavActive() still springs the
 * right one open when the page you are reading is one of its entries.
 *
 * `field` names the date the section is about ('added' / 'updated'), and is
 * also the `data-` attribute each row carries, so a row says which claim it is
 * making rather than leaving the reader to infer it from its section.
 */
function docsDatedSection(cls, heading, field, pages) {
    let html = `<details class="docs-nav-section docs-nav-datelist ${cls}">`
             + `<summary><h4>${escapeHtml(heading)}</h4></summary><ul>`;
    for (const p of pages) {
        const day = p[field];
        const title = `${p.kind} -- ${field} ${day}`
                    + (p.description ? `\n${p.description}` : '');
        html += `<li><a href="#doc=${escapeAttr(p.ref)}" data-doc-ref="${escapeAttr(p.ref)}"`
             +  ` data-${field}="${escapeAttr(day)}" title="${escapeAttr(title)}">`
             +  `<span class="docs-recent-title">${escapeHtml(p.title)}</span>`
             +  `<span class="docs-recent-date">${escapeHtml(docsFormatDay(day))}</span>`
             +  '</a></li>';
    }
    return html + '</ul></details>';
}

function docsNavSection(label, groups) {
    let html = `<div class="docs-nav-section"><h4>${escapeHtml(label)}</h4>`;
    for (const g of groups) {
        html += `<details class="docs-nav-group"><summary>${escapeHtml(g.name)}</summary><ul>`;
        for (const p of g.pages) {
            html += `<li><a href="#doc=${escapeAttr(p.ref)}" data-doc-ref="${escapeAttr(p.ref)}"`
                 +  ` title="${escapeAttr(p.description || '')}">${escapeHtml(p.title)}</a></li>`;
        }
        html += '</ul></details>';
    }
    return html + '</div>';
}

/** Render the full nav tree (guides by category, API by group, spices). */
function renderDocsNav() {
    const nav = document.getElementById('docs-nav');
    if (!nav) return;
    if (!docsIndex) {
        nav.innerHTML = '<p class="docs-empty">Documentation pack not found. '
                      + 'Run <code>just docs</code> to build it.</p>';
        return;
    }
    const pages = docsAllPages();
    // The quickstart first, then what is new, then the tree.
    let html = docsQuickstartSection();
    // Above the tree, and flat: what changed cuts across guides, API modules and
    // spices, so grouping it by category would bury the one thing it is for.
    // Added first, then updated -- arrival is the bigger news, and a page that
    // has only just landed is filtered out of the second list rather than
    // repeated in it. Either section is absent entirely when the pack carries no
    // dates for it, rather than shown empty.
    const recent = docsRecentPages(pages);
    if (recent.length) {
        html += docsDatedSection('docs-nav-recent', 'Recently Added', 'added', recent);
    }
    const updated = docsUpdatedPages(pages);
    if (updated.length) {
        html += docsDatedSection('docs-nav-updated', 'Recently Updated', 'updated', updated);
    }
    const guides = pages.filter(p => p.kind === 'guide');
    const modules = pages.filter(p => p.kind === 'module');
    const spices = pages.filter(p => p.kind === 'spice');
    if (guides.length) html += docsNavSection('Guides', docsGroupByCategory(guides));
    if (modules.length) html += docsNavSection('API', docsGroupByCategory(modules));
    // Spices only appear when this build actually carries their pages, so the
    // nav never offers a link the pack cannot serve offline.
    if (spices.length) html += docsNavSection('Spices', docsGroupByCategory(spices));
    nav.innerHTML = html;
    markDocsNavActive();
}

function markDocsNavActive() {
    const nav = document.getElementById('docs-nav');
    if (!nav) return;
    nav.querySelectorAll('a[data-doc-ref]').forEach(a => {
        const active = a.dataset.docRef === docsCurrentRef;
        a.classList.toggle('active', active);
        if (active) {
            a.closest('details')?.setAttribute('open', '');
            a.scrollIntoView({ block: 'nearest' });
        }
    });
}

/**
 * Render search results into the nav column.
 *
 * One box, two result kinds. Pages come from index.json's `words` blob; symbols
 * come from the doc-names.json the panel already uses, and selecting one routes
 * to the existing docstring panel rather than duplicating it here.
 */
function renderDocsSearch(query) {
    const nav = document.getElementById('docs-nav');
    if (!nav) return;
    const q = query.trim().toLowerCase();
    if (!q) { renderDocsNav(); return; }

    const pages = docsAllPages()
        .map(p => ({ p, score: docsScore(p, q) }))
        .filter(x => x.score > 0)
        .sort((a, b) => b.score - a.score)
        .slice(0, 40);

    const symbols = (docNames || [])
        .filter(d => d.name.toLowerCase().includes(q))
        .slice(0, 25);

    let html = '';
    if (pages.length) {
        html += '<div class="docs-nav-section"><h4>Pages</h4><ul class="docs-results">';
        for (const { p } of pages) {
            html += `<li><a href="#doc=${escapeAttr(p.ref)}" data-doc-ref="${escapeAttr(p.ref)}">`
                 +  `<span class="docs-result-title">${escapeHtml(p.title)}</span>`
                 +  `<span class="docs-result-kind">${escapeHtml(p.kind)}</span>`
                 +  `<span class="docs-result-desc">${escapeHtml(p.description || '')}</span>`
                 +  '</a></li>';
        }
        html += '</ul></div>';
    }
    if (symbols.length) {
        html += '<div class="docs-nav-section"><h4>Symbols</h4><ul class="docs-results">';
        for (const s of symbols) {
            const short = s.summary.replace(/^[\w/\-!?*+<>]+\s+--\s+/, '');
            html += `<li><a href="#" data-doc-symbol="${escapeAttr(s.name)}">`
                 +  `<span class="docs-result-title">${escapeHtml(s.name)}</span>`
                 +  `<span class="docs-result-kind">${escapeHtml(s.kind)}</span>`
                 +  `<span class="docs-result-desc">${escapeHtml(short)}</span>`
                 +  '</a></li>';
        }
        html += '</ul></div>';
    }
    nav.innerHTML = html || '<p class="docs-empty">No matches.</p>';
}

/** Rank a page against a query: title hits beat description hits beat body. */
function docsScore(page, q) {
    const title = (page.title || '').toLowerCase();
    if (title === q) return 100;
    if (title.includes(q)) return 50;
    if ((page.description || '').toLowerCase().includes(q)) return 20;
    if ((page.words || '').includes(q)) return 5;
    return 0;
}

/**
 * Fetch a pack fragment and render it into the article column.
 *
 * `ref` is a pack-relative page id, optionally with an in-page anchor:
 * 'guides/hkt-guide', 'api/tur-list#map', 'spices/json'.
 */
async function showDocsPage(refWithAnchor, { updateHash = true } = {}) {
    const article = document.getElementById('docs-article');
    if (!article) return;
    const { ref, anchor } = parseDocsRef(refWithAnchor);
    if (!ref) return;

    // Leaving the page you are on: bank its offset before the fragment is
    // replaced, so navigating away and back returns you to your place.
    if (docsCurrentRef && docsCurrentRef !== ref) rememberDocsScroll();

    const entry = docsEntryFor(ref);
    if (!entry) {
        article.innerHTML = `<p class="docs-empty">No page <code>${escapeHtml(ref)}</code> `
                          + 'in this documentation pack.</p>';
        return;
    }

    article.setAttribute('aria-busy', 'true');
    try {
        const res = await fetch(`${DOCS_PACK_BASE}/${ref}.html`);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        article.innerHTML = await res.text();
    } catch (err) {
        article.innerHTML = '<p class="docs-empty">This page is not available offline yet. '
                          + 'Reconnect once and it will be cached for good.</p>';
        console.warn('docs fragment fetch failed:', ref, err);
        article.removeAttribute('aria-busy');
        return;
    }
    article.removeAttribute('aria-busy');

    docsCurrentRef = ref;
    decorateDocsArticle(article);
    markDocsNavActive();
    if (updateHash) setHashParam('doc', refWithAnchor);
    persistDocsState();

    const siteLink = document.getElementById('docs-site-link');
    if (siteLink) siteLink.href = docsSiteUrl(ref);

    // Anchor scrolling has to wait for the fragment to be in the document --
    // and so does restoring an offset, since the element is not tall enough to
    // accept one until its content is there.
    //
    // Precedence: an explicit anchor always wins (you asked for that heading),
    // then a remembered offset for this page, then the top for a page opened
    // for the first time. A remembered offset can outlive the content it
    // pointed into if the pack updated between visits; the browser clamps it
    // to the scrollable range, which is the right behaviour, so it just does
    // not round-trip exactly.
    if (anchor) {
        const target = article.querySelector(`#${CSS.escape(anchor)}`);
        if (target) { target.scrollIntoView({ block: 'start' }); return; }
    }
    docsSetScroll(article, docsScrollByRef.get(ref) || 0);
}

/** The turmeric-lang.com URL for a pack ref, for the "open on the site" link. */
function docsSiteUrl(ref) {
    const [section, ...rest] = ref.split('/');
    const slug = rest.join('/');
    if (section === 'guides') return `/docs/html/guides/${slug}.html`;
    if (section === 'api') return `/docs/html/api/${slug}.html`;
    if (section === 'spices') return `/docs/html/spices/${slug}/`;
    return '/docs/html/guides/';
}

/**
 * Post-process a freshly rendered fragment: syntax highlighting, the
 * turmeric/sweet-exp toggles, and a "load into editor" affordance on every
 * runnable code block.
 *
 * The first three come from window.turmericGuide, which /docs-pack/guide.js
 * defines -- the same code the site's guide pages run, so highlighting,
 * toggles and diagrams behave identically in both places.
 */
function decorateDocsArticle(article) {
    if (window.turmericGuide) {
        window.turmericGuide.highlightGuideCode(article);
        window.turmericGuide.initSyntaxToggles(article);
        // Diagrams are lazy and network-backed; with the pane offline they
        // stay as their own source text, which renderMermaid handles itself.
        if (window.turmericGuide.renderMermaid) {
            window.turmericGuide.renderMermaid(article);
        }
    }
    article.querySelectorAll('pre').forEach(pre => {
        const code = pre.querySelector('code.language-turmeric, code.language-sweet-exp');
        if (!code || pre.querySelector('.docs-load-btn')) return;
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'docs-load-btn';
        btn.textContent = 'Load into editor';
        btn.title = 'Replace the editor buffer with this snippet';
        btn.addEventListener('click', () => {
            loadSnippetIntoEditor(code.textContent);
            btn.textContent = 'Loaded';
            setTimeout(() => { btn.textContent = 'Load into editor'; }, 1400);
        });
        pre.appendChild(btn);
    });
}

/** Put a guide snippet in the editor and close the docs pane so it is visible. */
function loadSnippetIntoEditor(source) {
    if (!editor) return;
    editor.setValue(source.replace(/\s+$/, '') + '\n');
    closeDocsPane();
    editor.focus();
    if (typeof showStatus === 'function') showStatus('Snippet loaded into the editor', 'info');
}

/**
 * Report offline readiness from the pack-status the service worker writes
 * during install (OD3). Says nothing when the pack is complete -- a working
 * guarantee does not need announcing; a partial one does.
 */
async function refreshDocsStatus() {
    const el = document.getElementById('docs-status');
    if (!el) return;
    el.textContent = '';
    el.className = 'docs-status';
    try {
        const res = await fetch('/docs-pack/pack-status');
        if (!res.ok) return;
        const status = await res.json();
        if (!status || typeof status.cached !== 'number') return;
        if (status.cached < status.expected) {
            el.textContent = `${status.cached} of ${status.expected} pages cached -- reconnect to finish`;
            el.classList.add('partial');
            // Ask the worker to top up now rather than waiting for a release.
            // An install is per-URL failure-tolerant, so a flaky first load can
            // leave the pack short; a guarantee that silently waits a version
            // bump to come true is not a guarantee.
            if (navigator.onLine) {
                navigator.serviceWorker?.controller?.postMessage('REPAIR_DOCS_PACK');
            }
        }
    } catch {
        // No service worker (dev server, or a browser with SW disabled). The
        // pane still works online; there is nothing useful to report.
    }
}

function openDocsPane(refWithAnchor) {
    const overlay = document.getElementById('docs-overlay');
    if (!overlay) return;
    overlay.style.display = 'flex';
    document.body.classList.add('docs-open');

    // On a phone the nav column would cover the page it navigates to, so open
    // onto the article and leave the tree behind the toolbar toggle.
    if (window.matchMedia('(max-width: 768px)').matches && !docsCurrentRef) {
        docsNavCollapsed = true;
    }
    overlay.classList.toggle('nav-collapsed', docsNavCollapsed);

    // The symbol half of the pane's search comes from doc-names.json; make
    // sure it is on its way even if the WASM boot has not got there yet.
    fetchDocNames();

    loadDocsIndex().then(index => {
        const version = document.getElementById('docs-version');
        if (version) {
            version.textContent = index ? `Docs v${index.version}` : '';
        }
        renderDocsNav();
        // An explicit request wins, then wherever you were last, then the
        // quickstart. The pack's first guide is only a last resort now: it is
        // whatever sorted first, which is a fine fallback and a poor greeting.
        const target = refWithAnchor
            || docsCurrentRef
            || (docsEntryFor(DOCS_DEFAULT_REF) ? DOCS_DEFAULT_REF : null)
            || (index && index.guides && index.guides.length
                ? `guides/${index.guides[0].slug}`
                : null);
        if (target) showDocsPage(target);
        refreshDocsStatus();
        document.getElementById('docs-search')?.focus();
    });
}

function closeDocsPane() {
    // Bank the offset while the overlay is still laid out. Reading scrollTop
    // after display:none would give 0, which is exactly the position the pane
    // is not supposed to come back to.
    rememberDocsScroll();
    const overlay = document.getElementById('docs-overlay');
    if (overlay) overlay.style.display = 'none';
    document.body.classList.remove('docs-open');
    setHashParam('doc', null);
    persistDocsState();
}

function docsPaneIsOpen() {
    const overlay = document.getElementById('docs-overlay');
    return !!overlay && overlay.style.display !== 'none';
}

/**
 * Bank the pane's location so a later LAUNCH can come back to it.
 *
 * `#doc=` already carries this across a reload and drives back/forward, but a
 * hash does not survive a relaunch: an installed app cold-starts at the
 * manifest's start_url, which has no fragment. So a reader who closed the app
 * inside a guide reopened it on the editor, with nothing on screen saying
 * where they had been -- the pane remembers within a session and forgot across
 * the only boundary an installed app actually has.
 *
 * Null when the pane is closed, which is what makes this a memory of state
 * rather than a bookmark: closing the docs is how you say you are done with
 * them, and the next launch has to honour that.
 */
function persistDocsState() {
    if (!docsPaneIsOpen() || !docsCurrentRef) {
        safeWrite(STORAGE_KEYS.docs, null);
        return;
    }
    safeWrite(STORAGE_KEYS.docs, {
        ref: docsCurrentRef,
        scrollTop: docsScrollByRef.get(docsCurrentRef) || 0,
    });
}

/**
 * Reopen the pane where it was left, on a launch that arrived without a
 * `#doc=` of its own.
 *
 * The ref is checked against the pack before the pane opens. A guide that was
 * renamed or dropped between releases would otherwise restore as
 * "No page ... in this documentation pack", which is a worse greeting than the
 * editor; a stale entry clears itself instead. Nothing is restored when the
 * pack is missing entirely (a dev server that never ran `just docs`), since
 * there is no page to restore into.
 */
function restoreDocsPane() {
    const saved = safeRead(STORAGE_KEYS.docs);
    if (!saved || typeof saved.ref !== 'string') return;
    loadDocsIndex().then((index) => {
        if (!index) return;
        if (!docsEntryFor(saved.ref)) {
            safeWrite(STORAGE_KEYS.docs, null);
            return;
        }
        if (saved.scrollTop) docsScrollByRef.set(saved.ref, saved.scrollTop);
        openDocsPane(saved.ref);
    });
}

/**
 * Intercept clicks inside the pane.
 *
 * Three cases: a `#doc=` link is a pack navigation, a bare `#anchor` scrolls
 * within the current page, and anything else is an outbound link that opens in
 * a new tab so the REPL session is never replaced.
 */
function initDocsLinkInterception() {
    const overlay = document.getElementById('docs-overlay');
    if (!overlay) return;
    overlay.addEventListener('click', (e) => {
        const link = e.target.closest('a');
        if (!link) return;

        if (link.dataset.docSymbol) {
            e.preventDefault();
            const name = link.dataset.docSymbol;
            const entry = (docNames || []).find(d => d.name === name) || null;
            closeDocsPane();
            wasmDocLookup(name).then(text => showDocPanel(name, text, entry));
            return;
        }

        const href = link.getAttribute('href') || '';
        if (href.startsWith('#doc=')) {
            e.preventDefault();
            const raw = href.slice('#doc='.length);
            let ref = raw;
            try { ref = decodeURIComponent(raw); } catch { /* literal already */ }
            // On mobile the nav floats over the article, so picking a page
            // should get out of the way and show it.
            if (link.dataset.docRef
                && window.matchMedia('(max-width: 768px)').matches) {
                docsNavCollapsed = true;
                overlay.classList.add('nav-collapsed');
            }
            showDocsPage(ref);
            return;
        }
        if (href.startsWith('#')) {
            e.preventDefault();
            const id = href.slice(1);
            const article = document.getElementById('docs-article');
            const target = id && article ? article.querySelector(`#${CSS.escape(id)}`) : null;
            if (target) target.scrollIntoView({ block: 'start' });
            return;
        }
        if (href && !href.startsWith('javascript:')) {
            // Absolute or site-relative: leave /try/ intact.
            link.target = '_blank';
            link.rel = 'noopener';
        }
    });
}

function initDocsPane() {
    document.getElementById('docs-btn')?.addEventListener('click', () => openDocsPane());
    document.getElementById('docs-close')?.addEventListener('click', closeDocsPane);

    // showDocsPage and closeDocsPane already bank the offset on the two ways of
    // leaving a page, so this listener is a backstop rather than the mechanism:
    // it keeps the map current for any exit path that forgets to, at the cost
    // of one rAF-coalesced store per scroll burst.
    const article = document.getElementById('docs-article');
    if (article) {
        let pending = false;
        article.addEventListener('scroll', () => {
            if (pending) return;
            pending = true;
            requestAnimationFrame(() => { pending = false; rememberDocsScroll(); });
        }, { passive: true });
    }

    // The doc panel's "Open full docs" used to navigate to /docs/html/api/,
    // dropping the REPL session. It opens the pane now.
    document.getElementById('doc-full-link')?.addEventListener('click', (e) => {
        e.preventDefault();
        const href = e.currentTarget.getAttribute('href') || '';
        const ref = href.startsWith('#doc=') ? href.slice('#doc='.length) : null;
        openDocsPane(ref && docsEntryFor(ref) ? ref : undefined);
    });

    document.getElementById('docs-nav-toggle')?.addEventListener('click', () => {
        docsNavCollapsed = !docsNavCollapsed;
        document.getElementById('docs-overlay')
            ?.classList.toggle('nav-collapsed', docsNavCollapsed);
    });

    const search = document.getElementById('docs-search');
    if (search) {
        search.addEventListener('input', debounce(() => {
            renderDocsSearch(search.value);
        }, 120));
        search.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                if (search.value) { search.value = ''; renderDocsNav(); }
                else closeDocsPane();
            }
        });
    }

    // Click the backdrop (not the shell) to dismiss.
    document.getElementById('docs-overlay')?.addEventListener('mousedown', (e) => {
        if (e.target.id === 'docs-overlay') closeDocsPane();
    });

    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && docsPaneIsOpen()) closeDocsPane();
    });

    initDocsLinkInterception();

    // Going away is the last moment the offset is readable, and for an
    // installed app it is the ONLY one: it is backgrounded rather than
    // unloaded, and iOS may kill it later without running anything else.
    // `pagehide` covers the ordinary tab being closed or navigated away.
    const bankDocsState = () => { rememberDocsScroll(); persistDocsState(); };
    window.addEventListener('pagehide', bankDocsState);
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') bankDocsState();
    });

    // Deep link: /try/#doc=guides/hkt-guide restores a docs location, and
    // composes with the existing #code= share hash rather than replacing it.
    //
    // The hash wins outright where it has a `doc` of its own -- following a
    // link is a request for that page, not for wherever you were last.
    //
    // The other two launches are suppressed for one reason between them: they
    // arrive pointed at something, and the pane would land on top of it. A
    // `#code=` share link is a request to look at a program; tutorial mode is
    // a lesson that owns the screen (and already opts out of persistence
    // wholesale, for the same reason). Neither clears the memory -- the next
    // ordinary launch still restores.
    const initial = getHashParam('doc');
    if (initial) openDocsPane(initial);
    else if (!hasUrlHashCode() && !isTutorialMode()) restoreDocsPane();
}

/** hashchange hook: a #doc= change opens or moves the pane. */
function syncDocsPaneFromHash() {
    const ref = getHashParam('doc');
    if (!ref) {
        if (docsPaneIsOpen()) closeDocsPane();
        return;
    }
    if (docsPaneIsOpen()) showDocsPage(ref, { updateHash: false });
    else openDocsPane(ref);
}

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
        wasmState,
        hasEditor: !!editor,
        hasWasm: !!evalWorker && wasmState === WASM_STATE.READY,
        docsOpen: docsPaneIsOpen(),
        docsRef: docsCurrentRef,
    })
};

/* ---------------------------------------------------------------------------
 * T3: the time-travel timeline
 *
 * Run records; then you scrub the recording, forwards and backwards, watching
 * the gutter follow the cursor and each live frame's bindings change under it.
 *
 * The page does not decode the .turtrace format.  Every question here --
 * where am I, what are the frames, what had been printed by now -- is answered
 * by turi_trace_replay_* through the WASM bridge, which is the same replay
 * `tur dap` answers stepBack with.  One decoder for one format.
 * ------------------------------------------------------------------------- */

/* The browser's cap is deliberately far below the recorder's own 1,000,000
 * default.  That number was chosen for a native process; here the interpreter
 * retains roughly 4 KiB per step of a trampolined loop on top of the ~13 bytes
 * a step costs the recording itself, and the tab is what pays.
 *
 * Raised from 50,000 alongside the recorder's move from line to expression
 * granularity.  A cap is a bound on the recording, but what it MEANS is how
 * much of a program fits under it, and a step is now about a third to a fifth
 * of what it used to be -- so holding 50,000 would have quietly cut the reach
 * of every recording by that factor.  The interpreter-side retention at the
 * cap is unchanged by construction: this is the same amount of program.
 * 250,000 steps is about 3 MB of trace and a session that stays responsive. */
const TRACE_MAX_STEPS = 250000;

const traceState = {
    active: false,
    steps: 0,
    index: 0,
    baseLine: 1,
    frames: [],
    selectedFrame: 0,
    stats: null,
    savedConsoleHTML: null,
    savedConsoleLog: null,
    decorations: null,
    seekPending: false,
    seekQueued: null,
};

function traceWorkerCall(message) {
    if (!evalWorker || wasmState !== WASM_STATE.READY) return Promise.resolve(null);
    return new Promise((resolve, reject) => {
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve,
            reject,
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ ...message, id });
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
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve: () => {
                currentLangMode = 'turmeric';  // turi_env_new() starts in default mode
                // The session forgot what it had accepted, so the prompt's
                // document has to forget it too.
                replSessionReset();
                lastRunByTab.clear();
                resolve(true);
            },
            reject: () => resolve(false),
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ type: 'reset', id });
    });
}

/**
 * Record the editor's program and open the timeline.
 */
async function traceCode() {
    if (wasmState !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = editor.getValue();
    if (!code.trim()) {
        appendToConsole('<span class="console-error">Error: No code to trace</span>');
        return;
    }

    showStatus('Recording...', 'info');
    if (!await traceResetEnv()) {
        appendToConsole('<span class="console-error">Trace failed: could not reset the session</span>');
        showStatus('Trace failed', 'error');
        return;
    }

    const started = performance.now();
    // Forwarded for the same reason Run forwards it: turi_eval_typed strips an
    // inline #lang itself, but set_lang is what puts the session in the
    // program's base dialect.
    const { lang } = parseLangDirective(code);
    if (lang !== null) currentLangMode = lang;

    // hasMain is the page's existing Run rule, not a second one: a program with
    // a top-level `main` loads its forms and then runs `(main)`, and the
    // recording covers the run rather than the definitions.
    // A stopped recording (the watchdog, or Stop) rejects; report it the way
    // any other failed recording is reported.
    const res = await traceWorkerCall({
        type: 'trace-run',
        input: code,
        maxSteps: TRACE_MAX_STEPS,
        hasMain: definesMainEntry(code),
        lang,
    }).catch((err) => ({ steps: -1, error: err.message }));

    if (!res || res.steps < 0) {
        const why = res && res.steps === -2
            ? 'this program declares a main entry point but did not define one'
            : (res && res.error) || 'the program did not load';
        appendToConsole(`<span class="console-error">Trace failed: ${escapeHtml(why)}</span>`);
        showStatus('Trace failed', 'error');
        return;
    }
    if (res.steps === 0) {
        appendToConsole('<span class="console-error">Trace recorded nothing -- the program ran no interpreted steps.</span>');
        showStatus('Ready', 'success');
        return;
    }

    updateExecTime(performance.now() - started);
    traceState.stats = res.stats || null;
    traceState.steps = res.steps;
    traceState.baseLine = (res.stats && res.stats.baseLine) || 1;
    traceOpen();
    await traceSeek(0);
}

function traceOpen() {
    const panel = document.getElementById('trace-panel');
    if (!panel) return;
    if (!traceState.active) {
        // The transcript belongs to the user, so it is put back on close
        // rather than overwritten: while the timeline is open the console
        // shows what the program had printed by the cursor's step, which is
        // what the OUTPUT records exist for.
        const consoleEl = document.getElementById('console');
        traceState.savedConsoleHTML = consoleEl ? consoleEl.innerHTML : '';
        traceState.savedConsoleLog = consoleLog.slice();
    }
    traceState.active = true;
    panel.hidden = false;

    const slider = document.getElementById('trace-slider');
    if (slider) {
        slider.min = '0';
        slider.max = String(Math.max(0, traceState.steps - 1));
        slider.value = '0';
    }

    const banner = document.getElementById('trace-banner');
    if (banner) {
        const st = traceState.stats;
        if (st && st.truncated) {
            banner.hidden = false;
            banner.textContent =
                `Recording stopped at the ${TRACE_MAX_STEPS.toLocaleString()}-step cap -- ` +
                `this is the beginning of the run, not all of it.`;
        } else if (st) {
            banner.hidden = false;
            banner.textContent =
                `${st.steps.toLocaleString()} steps, peak depth ${st.peakDepth}, ` +
                `${(st.bytes / 1024).toFixed(1)} KB recorded from a fresh session.`;
        } else {
            banner.hidden = true;
        }
    }
}

function traceClose() {
    const panel = document.getElementById('trace-panel');
    if (panel) panel.hidden = true;
    if (traceState.decorations) {
        traceState.decorations.clear();
        traceState.decorations = null;
    }
    if (traceState.active) {
        const consoleEl = document.getElementById('console');
        if (consoleEl && traceState.savedConsoleHTML !== null) {
            consoleEl.innerHTML = traceState.savedConsoleHTML;
            consoleEl.scrollTop = consoleEl.scrollHeight;
        }
        if (traceState.savedConsoleLog) {
            consoleLog = traceState.savedConsoleLog;
            safeWrite(STORAGE_KEYS.consol, consoleLog);
        }
    }
    traceState.active = false;
    traceState.savedConsoleHTML = null;
    traceState.savedConsoleLog = null;
    traceState.frames = [];
    // Hand the megabyte back rather than letting it sit in WASM memory for the
    // rest of the session.
    traceWorkerCall({ type: 'trace-release' });
}

/**
 * Move the cursor.  Seeks coalesce: a slider drag issues one seek at a time
 * and remembers only the most recent target, so dragging across a 50,000-step
 * recording costs one rebuild per settled position rather than one per pixel.
 */
async function traceSeek(index) {
    if (!traceState.active && index !== 0) return;
    const target = Math.max(0, Math.min(index, traceState.steps - 1));
    if (traceState.seekPending) {
        traceState.seekQueued = target;
        return;
    }
    traceState.seekPending = true;
    const state = await traceWorkerCall({
        type: 'trace-seek',
        index: target,
        // At the last step, ask for every OUTPUT record rather than the ones
        // before the cursor: a program whose final act is a println drains it
        // after the final STEP, and an empty console at the end of a run that
        // printed reads as a broken timeline rather than a precise one.
        wantFullOutput: target === traceState.steps - 1,
    });
    traceState.seekPending = false;

    if (state) traceRender(state);

    if (traceState.seekQueued !== null) {
        const next = traceState.seekQueued;
        traceState.seekQueued = null;
        await traceSeek(next);
    }
}

function traceRender(state) {
    traceState.index = state.index;
    traceState.frames = state.frames || [];
    if (traceState.selectedFrame >= traceState.frames.length) {
        traceState.selectedFrame = 0;
    }

    const slider = document.getElementById('trace-slider');
    if (slider && String(state.index) !== slider.value) slider.value = String(state.index);

    const pos = document.getElementById('trace-pos');
    if (pos) pos.textContent = `${state.index + 1} / ${state.steps}`;

    const top = traceState.frames[0];
    const site = document.getElementById('trace-site');
    if (site) {
        site.textContent = top
            ? `${top.fn || '(top level)'}  line ${traceEditorLine(top.line)}`
            : '';
    }

    traceRenderFrames();
    traceRenderOutput(state.fullOutput !== undefined ? state.fullOutput : (state.output || ''));
    traceHighlight(top ? traceEditorLine(top.line) : 0, top);
}

/* Interpreter line -> editor line.  The env accumulates every eval into one
 * blob, so an interpreter line is absolute in that blob; baseLine is where
 * this run's source started in it. */
function traceEditorLine(line) {
    const mapped = (line | 0) - traceState.baseLine + 1;
    return mapped > 0 ? mapped : 0;
}

function traceRenderFrames() {
    const framesEl = document.getElementById('trace-frames');
    const localsEl = document.getElementById('trace-locals');
    if (!framesEl || !localsEl) return;

    if (!traceState.frames.length) {
        framesEl.innerHTML = '<div class="trace-empty">no frames</div>';
        localsEl.innerHTML = '';
        return;
    }

    framesEl.innerHTML = traceState.frames.map((f, i) => {
        const cls = i === traceState.selectedFrame ? ' class="trace-frame active"' : ' class="trace-frame"';
        const line = traceEditorLine(f.line);
        return `<button${cls} data-frame="${i}">` +
               `<span class="trace-frame-fn">${escapeHtml(f.fn || '(top level)')}</span>` +
               `<span class="trace-frame-line">${line ? 'line ' + line : ''}</span>` +
               `</button>`;
    }).join('');

    const frame = traceState.frames[traceState.selectedFrame];
    const locals = (frame && frame.locals) || [];
    localsEl.innerHTML = locals.length
        ? locals.map(l =>
            `<div class="trace-local">` +
            `<span class="trace-local-name">${escapeHtml(l.name)}</span>` +
            `<span class="trace-local-val">${escapeHtml(l.repr)}</span>` +
            `</div>`).join('')
        : '<div class="trace-empty">no bindings in this frame yet</div>';
}

function traceRenderOutput(output) {
    const consoleEl = document.getElementById('console');
    if (!consoleEl) return;
    consoleEl.innerHTML = output
        ? `<span class="console-output">${escapeHtml(output)}</span>`
        : '<div class="console-welcome"><p>Nothing printed yet at this step.</p></div>';
    consoleEl.scrollTop = consoleEl.scrollHeight;
}

/**
 * Mark where the cursor is: the line always, and the expression within it when
 * the recording says which one.
 *
 * The second decoration is the visible half of recording per expression rather
 * than per line.  A step is one evaluation, so several consecutive steps share
 * a line and the line marker alone would sit still through all of them --
 * looking stuck rather than looking like progress.  `endCol` is 0 on a v1
 * recording (and absent on a wasm build older than the field), in which case
 * there is no range to draw and the line marker is the whole story.
 */
function traceHighlight(line, frame) {
    if (!editor) return;
    if (!traceState.decorations) {
        traceState.decorations = editor.createDecorationsCollection([]);
    }
    if (!line) {
        traceState.decorations.set([]);
        return;
    }
    const decorations = [{
        range: new monaco.Range(line, 1, line, 1),
        options: {
            isWholeLine: true,
            className: 'trace-current-line',
            /* No glyphMarginClassName: the editor is created without a glyph
             * margin, and turning one on would shift the whole gutter. */
            linesDecorationsClassName: 'trace-current-marker',
        },
    }];
    const col = frame ? (frame.col | 0) : 0;
    const endCol = frame ? (frame.endCol | 0) : 0;
    if (col > 0 && endCol > col) {
        decorations.push({
            range: new monaco.Range(line, col, line, endCol),
            options: { className: 'trace-current-expr' },
        });
    }
    traceState.decorations.set(decorations);
    editor.revealLineInCenterIfOutsideViewport(line);
}

/**
 * Jump to the next (dir > 0) or previous execution of an editor line.
 *
 * This is what a breakpoint is in a recording: the run already happened, so
 * "next hit on line 12" is a scan rather than a resume.  A miss lands on the
 * boundary and says so, which is the difference between "ran to the end" and
 * "found it".
 */
async function traceJumpToLine(editorLine, dir) {
    if (!traceState.active) return;
    const absolute = editorLine + traceState.baseLine - 1;
    const found = await traceWorkerCall({
        type: 'trace-find-line',
        dir,
        file: '',          // "" matches any file; the tab is the only source here
        line: absolute,
    });
    if (!found) return;
    if (!found.hit) {
        showStatus(`No ${dir > 0 ? 'later' : 'earlier'} step on line ${editorLine}`, 'info');
        return;
    }
    await traceSeek(found.index);
}

async function traceDownload() {
    const buf = await traceWorkerCall({ type: 'trace-download' });
    if (!buf) {
        showStatus('Nothing to download', 'error');
        return;
    }
    const blob = new Blob([buf], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'run.turtrace';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
}

function traceInstallHandlers() {
    document.getElementById('trace-btn')?.addEventListener('click', traceCode);
    document.getElementById('trace-close')?.addEventListener('click', traceClose);
    document.getElementById('trace-download')?.addEventListener('click', traceDownload);
    document.getElementById('trace-first')?.addEventListener('click', () => traceSeek(0));
    document.getElementById('trace-last')?.addEventListener('click', () => traceSeek(traceState.steps - 1));
    document.getElementById('trace-back')?.addEventListener('click', () => traceSeek(traceState.index - 1));
    document.getElementById('trace-fwd')?.addEventListener('click', () => traceSeek(traceState.index + 1));

    document.getElementById('trace-slider')?.addEventListener('input', (e) => {
        traceSeek(parseInt(e.target.value, 10) || 0);
    });

    document.getElementById('trace-frames')?.addEventListener('click', (e) => {
        const btn = e.target.closest('[data-frame]');
        if (!btn) return;
        traceState.selectedFrame = parseInt(btn.dataset.frame, 10) || 0;
        traceRenderFrames();
    });

    document.addEventListener('keydown', (e) => {
        if (!traceState.active || !e.altKey) return;
        if (e.key === 'ArrowLeft')  { e.preventDefault(); traceSeek(traceState.index - 1); }
        if (e.key === 'ArrowRight') { e.preventDefault(); traceSeek(traceState.index + 1); }
    });
}

/* Click a line number while the timeline is open -> jump to that line's next
 * execution; Alt+click for the previous one. Registered from the editor setup
 * path, once Monaco exists. */
function traceInstallGutterHandler(ed) {
    ed.onMouseDown((e) => {
        if (!traceState.active) return;
        if (e.target.type !== monaco.editor.MouseTargetType.GUTTER_LINE_NUMBERS) return;
        const line = e.target.position?.lineNumber;
        if (line) traceJumpToLine(line, e.event.altKey ? -1 : 1);
    });
}

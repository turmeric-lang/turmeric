/**
 * Try Turmeric - Web-based REPL
 * Main JavaScript file for the Turmeric WASM REPL
 */

import { isTutorialMode, debounce, resetWorkspace } from './storage.js';
import { EXAMPLES } from './examples.js';
import { escapeHtml } from './html.js';
import { provideEditor } from './editor-ref.js';
import { syncMinimapMenuItem, toggleMinimap, initMinimap } from './minimap.js';
import { initSplitHandle } from './split-handle.js';
import { initInstallAffordances } from './pwa-install.js';
import { initSymbolsMenu } from './outline.js';
import { docsPaneState, loadDocsIndex, showDocsPage, openDocsPane, closeDocsPane, docsPaneIsOpen, initDocsPane, syncDocsPaneFromHash } from './docs-pane.js';
import { configureTrace, traceState, traceCode, traceClose, traceSeek, traceInstallHandlers, traceInstallGutterHandler } from './trace.js';
import { monacoThemeName } from './editor-theme.js';
import { configureMonaco } from './editor-language.js';
import { buildZip } from './zip.js';
import { initHScrollDrag } from './hscroll-drag.js';
import { initConsoleHover } from './console-hover.js';
import { configureEvalClient, WASM_STATE, wasmState, setWasmState, hasWorker, startEvalWorker, send, terminateWorker, pendingStats } from './eval-client.js';
import { parseLangDirective, fetchLangRegistry, reconcileLangPicker, initLangPicker } from './lang-picker.js';
import { refreshBuildStamp, forceUpdatePWA } from './pwa-update.js';
import { fetchDocNames, wasmDocLookup, showDocPanel, hideDocPanel, maybeShowDoc } from './doc-panel.js';
import { tabs, activeId, lastRunByTab, tabsSnapshot, persistTabs, currentTab, ensureModel, switchTab, createTab, closeTab, renameTab, jumpStack, jumpBack, renderJumpBack, renderTabs, hydrateTabs, useTutorialTab } from './tabs.js';
import { showStatus, updateCursorPosition, updateExecTime } from './status.js';
import { hydrateConsole, appendToConsole, clearConsole, copyConsole } from './console.js';
import { buildProjectEntries, downloadProject, parseProjectZip, loadProjectFromBytes, loadProjectFromFile, openProjectFile, initProjectDrop } from './project.js';
import { replSessionAccept, replSessionReset, promptSetEnabled, initReplInput, configureRepl } from './repl.js';
import { startLspClient } from './lsp.js';
import { CONFIG } from './config.js';
import { updateUrlHash, loadFromUrlHash, shareCode } from './share.js';

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

// Modules split out of this file read the editor through these getters.
provideEditor(() => editor, () => monaco);
configureRepl({ executeCode, resetWasm });

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
        useTutorialTab(editor.getValue());
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

// The trace timeline (trace.js) resets the session for a recording through this.
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
    resetSession: traceResetEnv,
    definesMainEntry,
});


// session.js -- the REPL session: the queue of evaluations, Run and Format,
// what each tab's Run put in the session (PS5), resetting and rewinding it,
// and the watchdog and Stop (security-audit-plan W-3).
//
// W-3:
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

import { CONFIG } from './config.js';
import { WASM_STATE, wasmState, setWasmState, configureEvalClient, hasWorker, startEvalWorker,
         send, terminateWorker, pendingStats } from './eval-client.js';
import { escapeHtml } from './html.js';
import { getEditor } from './editor-ref.js';
import { appendToConsole, clearConsole } from './console.js';
import { showStatus, updateExecTime } from './status.js';
import { parseLangDirective } from './lang-picker.js';
import { maybeShowDoc } from './doc-panel.js';
import { tabs, activeId, lastRunByTab } from './tabs.js';
import { promptSetEnabled, replSessionReset, replSessionAccept } from './repl.js';
import { getEngine } from './engine.js';
import { configureJitClient, runCompiled, evalCompiled, jitPendingStats, stopCompiled } from './jit-client.js';

// The eval Worker, its state and its outstanding requests live in
// eval-client.js; these are its hooks back into the console and the Run queue.
// At import, so they are in place before main.js boots the Worker.
configureEvalClient({
    onPrint: (text) => appendToConsole(`<span class="console-output">${escapeHtml(text)}</span>`),
    onPrintErr: (text) => appendToConsole(`<span class="console-error">${escapeHtml(text)}</span>`),
    onEvalTime: (ms) => updateExecTime(ms),
    nextEval: () => processQueue(),
    onPending: () => armEvalWatchdog(),
});

// The Compile engine's worker (wasm-jit-plan P4): its output goes to the same
// console, and its runs count toward the same watchdog and Stop.
configureJitClient({
    onPrint: (text) => appendToConsole(`<span class="console-output">${escapeHtml(text)}</span>`),
    onPrintErr: (text) => appendToConsole(`<span class="console-error">${escapeHtml(text)}</span>`),
    onLoading: (on) => on ? showStatus('Loading the compiler...', 'info') : showStatus('Ready', 'success'),
    onPending: () => armEvalWatchdog(),
});

/** Outstanding requests across both engines' workers, for the watchdog. */
function allPendingStats() {
    const a = pendingStats();
    const b = jitPendingStats();
    return { count: a.count + b.count, oldestStart: Math.min(a.oldestStart, b.oldestStart) };
}

let executionQueue = [];
let isExecuting = false;

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

    // wasm-jit-plan P6: with the Compile engine picked, a prompt turn goes to
    // that engine's own session, where an inline-C defn compiles on its first
    // call.  (Only the prompt reaches here then: a Compile Run is
    // runCompiledProgram.)
    if (getEngine() === 'compile') {
        const next = () => setTimeout(processQueue, 0);
        evalCompiled(code).then((r) => {
            if (r.trapped) {
                r.result += '\nthe Compile engine\'s session is gone; the next turn starts a fresh one';
            }
            resolve(r);
            next();
        }, (err) => { reject(err); next(); });
        return;
    }

    send({ type: 'eval', input: code, lang, quiet }, { resolve, reject, isEval: true });
}

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
    const { count, oldestStart } = allPendingStats();
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
        stopRun(`Stopped: still running after ${CONFIG.EXECUTION_TIMEOUT / 1000} s`);
    } else if (age >= EVAL_STOP_OFFER_MS) {
        if (stopBtn) stopBtn.hidden = false;
        if (panel && panel.style.display === 'none') {
            panel.style.display = 'flex';
            evalWatchdogShowedPanel = true;
        }
    }
}

/**
 * Stop whatever is running: a Compile run stops its own worker, which holds no
 * session, so the interpreter's session survives it; anything else stops the
 * interpreter's worker (stopEvalWorker).
 */
export function stopRun(reason) {
    if (jitPendingStats().count > 0) {
        stopCompiled(reason);
        evalWatchdogTick();   // nothing is pending now: hides Stop and its panel
        return;
    }
    stopEvalWorker(reason);
}

/**
 * Terminate the eval Worker and start a fresh one. Every request it owed an
 * answer -- and every Run still queued behind them -- is rejected with
 * `reason`, which is what the console shows for the Run that was stopped.
 */
export function stopEvalWorker(reason) {
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
export function resetWasm() {
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
export async function executeCode(source, promptHtml, showTiming = false, echoSource = true, suppressResult = false) {
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
            // The interpreter's own advice names `tur run`, which the page does
            // not have; the page's way to run inline C is the Compile engine.
            if (/inline-C not supported in interpreter mode/.test(result)) {
                appendToConsole('<span class="console-engine">To run inline C here, pick Compile '
                                + 'in the language menu\'s Engine section.</span>');
            }
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
export function definesMainEntry(code) {
    return /(^|\n)[ \t]*\(?defn\s+main\b/.test(code);
}

/**
 * Run the current editor contents
 */
export async function runCode() {
    if (getEngine() === 'compile') {
        const code = getEditor().getValue();
        if (!code.trim()) {
            appendToConsole('<span class="console-error">Error: No code to evaluate</span>');
            return;
        }
        await runCompiledProgram(code);
        return;
    }
    if (wasmState() !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = getEditor().getValue();
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

/** One-line summary of how a Compile run went: which engine ran it, and when
 * it compiled, what each stage cost. */
function engineLine(a) {
    const ms = (x) => `${Math.round(x)} ms`;
    const exit = a.exit ? `, exit ${a.exit}` : '';
    if (a.engine === 'compiled') {
        return `compiled${exit} -- front ${ms(a.front_ms || 0)}, c2mir ${ms(a.compile_ms || 0)}, `
             + `run ${ms(a.run_ms || 0)}`;
    }
    if (a.engine === 'interpreted') return `interpreted (the compiler fell back)${exit}`;
    return `not run${a.reason ? `: ${a.reason}` : ''}`;
}

/**
 * Run the buffer on the Compile engine (wasm-jit-plan P4): compiled to C and
 * run on MIR's interpreter, as a whole program -- `main` if it defines one --
 * the way `tur run` would.  It does not touch the interpreter's session: what
 * the program defines is not callable at the prompt afterwards, and the other
 * tabs' runs are not replayed first.
 */
async function runCompiledProgram(code) {
    const consoleLoading = document.getElementById('console-loading');
    if (consoleLoading) consoleLoading.style.display = 'flex';
    try {
        const a = await runCompiled(code);
        if (a.trapped) {
            // The module instance is gone (wasm-jit-plan 5.8); jit-client.js
            // already dropped it, so the next Run boots a fresh one.
            appendToConsole(`<span class="console-error">Error: ${escapeHtml(a.reason)}</span>`);
            appendToConsole('<span class="console-engine">the Compile engine restarts on the next Run</span>');
        } else {
            appendToConsole(`<span class="console-engine">${escapeHtml(engineLine(a))}</span>`);
        }
        updateExecTime(a.wallMs || 0);
    } catch (err) {
        appendToConsole(`<span class="console-error">Error: ${escapeHtml(err.message)}</span>`);
        updateExecTime(0);
    } finally {
        if (consoleLoading) consoleLoading.style.display = 'none';
    }
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
export async function formatCode() {
    if (wasmState() !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const editor = getEditor();
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
export function traceResetEnv() {
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

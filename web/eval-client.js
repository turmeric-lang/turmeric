// eval-client.js -- the eval Worker that hosts the interpreter, and the one
// way to ask it anything.
//
// All WASM calls (eval, format, doc lookup, reset, trace) run inside the
// Worker so that Atomics.wait is permitted and blocking select cannot freeze
// the tab. The Worker answers one request at a time; every request carries an
// id, waits in pendingCalls until its answer comes back, and is settled by the
// router in startEvalWorker().
//
// This module owns the Worker, its state and the outstanding requests. What a
// request's answer means -- the console, the queue of Runs, the session --
// stays in main.js, which hands the router its hooks via configureEvalClient().

export const WASM_STATE = {
    INITIALIZING: 'initializing',
    LOADING: 'loading',
    READY: 'ready',
    ERROR: 'error'
};

let state = WASM_STATE.INITIALIZING;
let evalWorker = null;
let evalCallId = 0;

let hooks = {
    onPrint: () => {},        // a line the program printed
    onPrintErr: () => {},     // a line it printed to stderr
    onEvalTime: () => {},     // how long an eval took, before its result lands
    nextEval: () => {},       // dispatch the next queued Run
    onPending: () => {},      // a request was added (arms the watchdog)
};

// Every request awaiting the eval Worker, by id. Adding one calls onPending,
// so no call site has to remember to arm the watchdog.
const pendingCalls = new (class extends Map {
    set(id, call) {
        super.set(id, call);
        hooks.onPending();
        return this;
    }
})();

/** Hand the router what it calls back into main.js. Called once, at load. */
export function configureEvalClient(h) {
    hooks = h;
}

/** Where the WASM session is: one of WASM_STATE. */
export function wasmState() {
    return state;
}

export function setWasmState(next) {
    state = next;
}

/** True while a Worker exists, whatever state it is in. */
export function hasWorker() {
    return evalWorker !== null;
}

/**
 * Send one request. `resolve` / `reject` settle it when the Worker answers
 * (see the router below for what each answer resolves with); `isEval` marks a
 * Run, whose settling also dispatches the next queued Run.
 */
export function send(message, { resolve, reject, isEval = false }) {
    const id = ++evalCallId;
    pendingCalls.set(id, { resolve, reject, startTime: performance.now(), isEval });
    evalWorker.postMessage({ ...message, id });
}

/** How many requests are outstanding, and when the oldest was sent. */
export function pendingStats() {
    let oldest = Infinity;
    for (const call of pendingCalls.values()) oldest = Math.min(oldest, call.startTime);
    return { count: pendingCalls.size, oldestStart: oldest };
}

/**
 * Terminate the Worker. Returns every request it still owed an answer, now
 * forgotten here; the caller rejects them once it has cleaned up the session.
 */
export function terminateWorker() {
    evalWorker.terminate();
    evalWorker = null;
    const owed = [...pendingCalls.values()];
    pendingCalls.clear();
    return owed;
}

/**
 * Start a fresh eval Worker and resolve once its wasm module is ready.
 *
 * Used at boot and again by stopEvalWorker, which is why it does nothing but
 * the worker: the one-time steps (doc names, the #lang registry, a share
 * link's code) stay in initWasm.
 */
export function startEvalWorker() {
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
                if (msg.boot) {
                    window._turiBoot = Object.assign(window._turiBoot || {}, msg.boot,
                                                     { workerReadyAt: performance.now() });
                }
                resolve();
                return;
            }
            if (msg.type === 'init-error') {
                reject(new Error(msg.error));
                return;
            }

            // Console output forwarded from WASM print/printErr callbacks.
            if (msg.type === 'print') {
                hooks.onPrint(msg.text);
                return;
            }
            if (msg.type === 'printErr') {
                hooks.onPrintErr(msg.text);
                return;
            }

            // Resolve pending call promises (eval, format, doc, reset).
            const pending = pendingCalls.get(msg.id);
            if (!pending) return;
            pendingCalls.delete(msg.id);

            if (msg.type === 'eval-result') {
                const execTime = performance.now() - pending.startTime;
                hooks.onEvalTime(execTime);
                const isError = msg.result.startsWith('Error:') || msg.result.includes('error');
                pending.resolve({ result: msg.result, isError, execTime });
                setTimeout(hooks.nextEval, 0);
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
                if (pending.isEval) setTimeout(hooks.nextEval, 0);
            }
        });

        worker.addEventListener('error', (e) => {
            reject(new Error(String(e.message || e)));
        });

        worker.postMessage({ type: 'init' });
    });
}

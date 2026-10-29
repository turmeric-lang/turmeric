// jit-client.js -- the Compile engine's worker (public/jit-worker.js), and the
// one way to ask it to run a program (wasm-jit-plan P4).
//
// The worker is started on first use, not at boot: its module is 5 MB the
// page needs only once someone picks Compile.  It answers one request at a
// time.  A Run compiles and runs the whole buffer, as `tur run` does; the
// prompt's turns (evalCompiled, wasm-jit-plan P6) go to a session of the
// module's own, where an inline-C defn compiles on its first call.  Stopping
// the worker, or an instance that trapped (the answer carries `trapped`),
// loses that session: the worker is terminated and the next request boots a
// fresh one (wasm-jit-plan 5.8).

let worker = null;
let readyPromise = null;
let callId = 0;
const pending = new Map();   // id -> { resolve, reject, startTime }

let hooks = {
    onPrint: () => {},
    onPrintErr: () => {},
    onLoading: () => {},     // the module is being fetched (first Compile)
    onPending: () => {},     // a run was sent (arms the watchdog)
};

/** Hand the client its callbacks into the page. Called once, at load. */
export function configureJitClient(h) {
    hooks = Object.assign(hooks, h);
}

function start() {
    const w = new Worker('/jit-worker.js');
    worker = w;
    readyPromise = new Promise((resolve, reject) => {
        w.addEventListener('message', (e) => {
            // A stopped worker's last words belong to nobody.
            if (w !== worker) return;
            const msg = e.data;
            if (msg.type === 'ready') {
                if (msg.boot) window._turiJitBoot = msg.boot;
                resolve();
            } else if (msg.type === 'init-error') {
                reject(new Error(msg.error));
            } else if (msg.type === 'print') {
                hooks.onPrint(msg.text);
            } else if (msg.type === 'printErr') {
                hooks.onPrintErr(msg.text);
            } else if (msg.type === 'eval-result') {
                const call = pending.get(msg.id);
                if (!call) return;
                pending.delete(msg.id);
                if (msg.trapped) retire();
                const result = msg.result || '';
                call.resolve({
                    result,
                    isError: result.startsWith('Error:') || result.includes('error'),
                    trapped: !!msg.trapped,
                    execTime: performance.now() - call.startTime,
                });
            } else if (msg.type === 'jit-result') {
                const call = pending.get(msg.id);
                if (!call) return;
                pending.delete(msg.id);
                const answer = msg.answer || {};
                answer.wallMs = performance.now() - call.startTime;
                if (answer.trapped) retire();
                call.resolve(answer);
            }
        });
        w.addEventListener('error', (e) => reject(new Error(String(e.message || e))));
    });
    // A boot that fails leaves no worker behind: the next Run tries again.
    readyPromise.catch(() => { if (worker === w) retire(); });
    w.postMessage({ type: 'init' });
    return readyPromise;
}

/** Terminate the worker without failing anything; the next Run starts one. */
function retire() {
    if (worker) worker.terminate();
    worker = null;
    readyPromise = null;
}

async function request(type, source, extra = {}) {
    if (!worker) {
        hooks.onLoading(true);
        try { await start(); } finally { hooks.onLoading(false); }
    } else {
        await readyPromise;
    }
    return new Promise((resolve, reject) => {
        const id = ++callId;
        pending.set(id, { resolve, reject, startTime: performance.now() });
        hooks.onPending();
        worker.postMessage(Object.assign({ type, id, input: source }, extra));
    });
}

/**
 * Compile and run `source` as a whole program.  Resolves with the engine
 * line (see jit-worker.js), plus `wallMs`, the time the page waited.
 */
export function runCompiled(source) {
    return request('jit-run', source);
}

/**
 * Evaluate one prompt turn in the Compile engine's session.  Resolves with
 * {result, isError, trapped, execTime}, the shape eval-client.js gives an
 * interpreter turn.
 */
export function evalCompiled(source, { compiledTurns = false } = {}) {
    return request('eval', source, { compiled: compiledTurns });
}

/**
 * Start the Compile engine's prompt session afresh, failing nothing: the next
 * request boots a new worker.  Used when compiled turns are switched, since a
 * compiled session's definitions live in its own table.  Returns whether
 * there was a session to drop.
 */
export function resetCompiledSession() {
    if (!worker || pending.size) return false;
    retire();
    return true;
}

/** How many runs are outstanding, and when the oldest was sent. */
export function jitPendingStats() {
    let oldest = Infinity;
    for (const call of pending.values()) oldest = Math.min(oldest, call.startTime);
    return { count: pending.size, oldestStart: oldest };
}

/**
 * Stop: terminate the worker and fail the request it owed with `reason`.  The
 * interpreter's session is untouched; the Compile engine's prompt session is
 * gone, and the next request starts a fresh one.
 */
export function stopCompiled(reason) {
    const owed = [...pending.values()];
    pending.clear();
    retire();
    const err = new Error(reason);
    for (const call of owed) call.reject(err);
}

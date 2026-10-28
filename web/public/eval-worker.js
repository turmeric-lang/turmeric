// eval-worker.js — Classic Worker
// Loads the Emscripten WASM module and handles eval/format/doc/reset requests
// from the main thread via postMessage.  Running eval inside a Worker means
// Atomics.wait (used by Emscripten pthreads) is allowed and blocking select
// cannot freeze the browser tab.

let turiModule = null;

// Forward a #lang directive to the WASM runtime. Silently no-ops on older
// WASM builds that don't export turi_wasm_set_lang.
function wasmSetLang(lang) {
    try {
        if (typeof turiModule._turi_wasm_set_lang !== 'function') return;
        const len = turiModule.lengthBytesUTF8(lang) + 1;
        const ptr = turiModule._malloc(len);
        turiModule.stringToUTF8(lang, ptr, len);
        turiModule._turi_wasm_set_lang(ptr);
        turiModule._free(ptr);
    } catch (_) {}
}

// PS5: a replayed Run (an eval sent with `quiet`) is bookkeeping -- its output
// was shown when it first ran -- so nothing it prints reaches the page.
let muted = false;

function postPrint(text) {
    if (!muted) self.postMessage({ type: 'print', text });
}

function postPrintErr(text) {
    if (!muted) self.postMessage({ type: 'printErr', text });
}

// ---------------------------------------------------------------------------
// stdout/stderr: assemble the lines here, so a partial last line still shows
// ---------------------------------------------------------------------------
// Emscripten's default stdout is a TTY device that hands `print` one COMPLETE
// line at a time -- a trailing partial line sits in its buffer until some
// later write happens to emit a newline.  Scheme's `display` writes no
// newline, so `(display "Hello, world!")` printed NOTHING at all, and then
// surfaced glued to the front of the next Run that did print one.
//
// `Module.stdout`/`Module.stderr` are per-byte device callbacks (FS.init ->
// FS.createDevice), so taking the bytes ourselves lets every request end with
// an explicit flush of whatever has no newline behind it yet.
const streamDecoder = new TextDecoder('utf-8');

const stdoutStream = { bytes: [], post: postPrint };
const stderrStream = { bytes: [], post: postPrintErr };

// A line is complete: deliver it WITHOUT the newline, which is what `print`
// used to hand the page (a bare `(newline)` is therefore an empty line, not a
// dropped one).
function emitStreamLine(s) {
    const text = s.bytes.length
        ? streamDecoder.decode(new Uint8Array(s.bytes))
        : '';
    s.bytes.length = 0;
    s.post(text);
}

function streamByte(s, byte) {
    if (byte === null) { flushStreams(); return; }   // device closed
    if (byte === 0) return;                          // ignored, as the TTY does
    if (byte === 10) { emitStreamLine(s); return; }
    s.bytes.push(byte);
}

// Called at the end of every request that can print, so output that stops
// mid-line reaches the page with the run that produced it rather than the
// next one.  A no-op when both streams are empty.
function flushStreams() {
    if (stdoutStream.bytes.length) emitStreamLine(stdoutStream);
    if (stderrStream.bytes.length) emitStreamLine(stderrStream);
}

// Queue messages that arrive before the module is ready.
const queue = [];
let ready = false;

function drainQueue() {
    const drained = queue.splice(0);
    drained.forEach(msg => handleMessage(msg));
}

function handleMessage(msg) {
    const id = msg.id;

    if (msg.type === 'eval') {
        muted = !!msg.quiet;
        try {
            if (msg.lang) wasmSetLang(msg.lang);
            const inputLen = turiModule.lengthBytesUTF8(msg.input) + 1;
            const inputPtr = turiModule._malloc(inputLen);
            turiModule.stringToUTF8(msg.input, inputPtr, inputLen);
            const resultPtr = turiModule._turi_wasm_eval(inputPtr);
            turiModule._free(inputPtr);
            // Before the result, so the transcript keeps program-output-then-
            // value order.
            flushStreams();
            const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : '';
            if (resultPtr) turiModule._free(resultPtr);
            self.postMessage({ type: 'eval-result', id, result });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        } finally {
            // Still inside the mute window: a quiet replay's leftovers are
            // dropped here rather than surfacing on the next run.
            flushStreams();
            muted = false;
        }

    } else if (msg.type === 'format') {
        try {
            const inputLen = turiModule.lengthBytesUTF8(msg.input) + 1;
            const inputPtr = turiModule._malloc(inputLen);
            turiModule.stringToUTF8(msg.input, inputPtr, inputLen);
            const resultPtr = turiModule._turi_wasm_format(inputPtr);
            turiModule._free(inputPtr);
            if (!resultPtr) {
                self.postMessage({ type: 'error', id, error: 'format failed' });
                return;
            }
            const result = turiModule.UTF8ToString(resultPtr);
            turiModule._free(resultPtr);
            self.postMessage({ type: 'format-result', id, result });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'doc') {
        try {
            const fn = turiModule._turi_doc_lookup;
            if (!fn) { self.postMessage({ type: 'doc-result', id, result: null }); return; }
            const nameLen = turiModule.lengthBytesUTF8(msg.name) + 1;
            const inputPtr = turiModule._malloc(nameLen);
            turiModule.stringToUTF8(msg.name, inputPtr, nameLen);
            const resultPtr = fn(inputPtr);
            turiModule._free(inputPtr);
            const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : null;
            self.postMessage({ type: 'doc-result', id, result });
        } catch (_) {
            self.postMessage({ type: 'doc-result', id, result: null });
        }

    } else if (msg.type === 'type-of') {
        try {
            const fn = turiModule._turi_type_of;
            if (!fn) { self.postMessage({ type: 'type-of-result', id, result: 'type-of not exported' }); return; }
            const exprLen = turiModule.lengthBytesUTF8(msg.expr) + 1;
            const inputPtr = turiModule._malloc(exprLen);
            turiModule.stringToUTF8(msg.expr, inputPtr, exprLen);
            const resultPtr = fn(inputPtr);
            turiModule._free(inputPtr);
            const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : '';
            self.postMessage({ type: 'type-of-result', id, result });
        } catch (err) {
            self.postMessage({ type: 'type-of-result', id, result: 'Error: ' + String(err) });
        }

    } else if (msg.type === 'explain') {
        try {
            const fn = turiModule._turi_explain;
            if (!fn) { self.postMessage({ type: 'explain-result', id, result: 'explain not exported' }); return; }
            const codeVal = msg.code || '';
            const codeLen = turiModule.lengthBytesUTF8(codeVal) + 1;
            const inputPtr = turiModule._malloc(codeLen);
            turiModule.stringToUTF8(codeVal, inputPtr, codeLen);
            const resultPtr = fn(inputPtr);
            turiModule._free(inputPtr);
            const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : '';
            self.postMessage({ type: 'explain-result', id, result });
        } catch (err) {
            self.postMessage({ type: 'explain-result', id, result: 'Error: ' + String(err) });
        }

    } else if (msg.type === 'lang-registry') {
        // #lang dialect/layer registry (try-turmeric-lang-toggle-plan T1).
        // Returns the raw JSON string exported by the C tables, or null on an
        // older WASM build without the export -- the main thread then falls
        // back to a minimal bases-only list.
        try {
            const fn = turiModule._turi_wasm_lang_registry;
            if (!fn) { self.postMessage({ type: 'lang-registry-result', id, result: null }); return; }
            const resultPtr = fn();
            const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : null;
            self.postMessage({ type: 'lang-registry-result', id, result });
        } catch (_) {
            self.postMessage({ type: 'lang-registry-result', id, result: null });
        }

    } else if (msg.type === 'trace-run') {
        // Record a run (try-turmeric-tracer-plan T3.0).  Returns the step count
        // and the stats blob; the recording itself stays in WASM memory and is
        // scrubbed through 'trace-seek' below, so the format has exactly one
        // decoder and it is the C one.
        try {
            const fn = turiModule._turi_wasm_trace_run;
            if (!fn) {
                self.postMessage({ type: 'trace-run-result', id, steps: -1,
                                   error: 'this WASM build has no tracer' });
                return;
            }
            if (msg.lang) wasmSetLang(msg.lang);
            const inputLen = turiModule.lengthBytesUTF8(msg.input) + 1;
            const inputPtr = turiModule._malloc(inputLen);
            turiModule.stringToUTF8(msg.input, inputPtr, inputLen);
            const steps = fn(inputPtr, msg.maxSteps >>> 0, msg.hasMain ? 1 : 0);
            turiModule._free(inputPtr);
            flushStreams();

            let stats = null;
            if (steps >= 0) {
                const p = turiModule._turi_wasm_trace_stats();
                if (p) { try { stats = JSON.parse(turiModule.UTF8ToString(p)); } catch (_) {} }
            }
            self.postMessage({ type: 'trace-run-result', id, steps, stats });
        } catch (err) {
            self.postMessage({ type: 'trace-run-result', id, steps: -1, error: String(err) });
        }

    } else if (msg.type === 'trace-seek') {
        try {
            turiModule._turi_wasm_trace_seek(msg.index >>> 0);
            const p = turiModule._turi_wasm_trace_state();
            const state = p ? JSON.parse(turiModule.UTF8ToString(p)) : null;
            // The transcript at the LAST step needs every OUTPUT record, not
            // just the ones before it: a program whose final act is a println
            // drains it after the final STEP.
            if (state && msg.wantFullOutput) {
                const q = turiModule._turi_wasm_trace_output_full();
                state.fullOutput = q ? turiModule.UTF8ToString(q) : '';
            }
            self.postMessage({ type: 'trace-state', id, state });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'trace-site-at') {
        // Deliberately NOT a seek: a seek rebuilds state from the start of the
        // stream, so asking it per index (the depth ribbon does, thousands of
        // times) is a hang rather than a slowdown.  See turi/trace.h.
        try {
            const out = [];
            for (let i = 0; i < msg.indices.length; i++) {
                const p = turiModule._turi_wasm_trace_site_at(msg.indices[i] >>> 0);
                out.push(p ? JSON.parse(turiModule.UTF8ToString(p)) : null);
            }
            self.postMessage({ type: 'trace-sites', id, sites: out });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'trace-find-line') {
        try {
            const fileVal = msg.file || '';
            const fileLen = turiModule.lengthBytesUTF8(fileVal) + 1;
            const filePtr = turiModule._malloc(fileLen);
            turiModule.stringToUTF8(fileVal, filePtr, fileLen);
            const p = turiModule._turi_wasm_trace_find_line(msg.dir | 0, filePtr,
                                                            msg.line >>> 0);
            turiModule._free(filePtr);
            const found = p ? JSON.parse(turiModule.UTF8ToString(p)) : null;
            self.postMessage({ type: 'trace-found', id, found });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'trace-download') {
        // The one thing that crosses as bytes.  Copied out of HEAPU8 rather
        // than viewed: the heap can be detached by a later growth, and
        // UTF8ToString would stop at the first NUL inside a rendered value.
        try {
            const ptr = turiModule._turi_wasm_trace_buffer();
            const len = turiModule._turi_wasm_trace_buffer_len() >>> 0;
            if (!ptr || !len) {
                self.postMessage({ type: 'trace-bytes', id, bytes: null });
                return;
            }
            const copy = new Uint8Array(len);
            copy.set(turiModule.HEAPU8.subarray(ptr, ptr + len));
            self.postMessage({ type: 'trace-bytes', id, bytes: copy.buffer },
                             [copy.buffer]);
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'trace-release') {
        try {
            turiModule._turi_wasm_trace_release();
            self.postMessage({ type: 'trace-released', id });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'rewind') {
        // PS5: rewind to the preloaded stdlib.  A cached older wasm without the
        // export keeps its old behavior (the Run appends to the session).
        try {
            if (typeof turiModule._turi_wasm_rewind_to_prelude === 'function')
                turiModule._turi_wasm_rewind_to_prelude();
            self.postMessage({ type: 'reset-done', id });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'reset') {
        try {
            turiModule._turi_wasm_reset();
            self.postMessage({ type: 'reset-done', id });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }
    }
}

self.addEventListener('message', function (e) {
    const msg = e.data;

    if (msg.type === 'init') {
        // The pthreads-capable WASM binary requires SharedArrayBuffer, which is
        // only available in cross-origin isolated contexts.  Safari < 15.4 has a
        // bug where dedicated Workers do not inherit cross-origin isolation from
        // the page, so SharedArrayBuffer is undefined even when the page is
        // isolated.  Detect this early and report a clear error.
        if (typeof SharedArrayBuffer === 'undefined') {
            self.postMessage({
                type: 'init-error',
                error: 'shared-array-buffer-unavailable',
            });
            return;
        }

        // performance-audit-plan WP5: what a cold start costs, stage by stage.
        // Durations in ms, posted with 'ready' and published by eval-client.js as
        // window._turiBoot (tests/web-perf.spec.js reads it).
        const bootT0 = performance.now();
        importScripts('/turmeric.js');
        const bootScript = performance.now();

        const factory = self.TurmericModule;
        if (typeof factory !== 'function') {
            self.postMessage({ type: 'init-error', error: 'TurmericModule not found after importScripts' });
            return;
        }

        // `print`/`printErr` still catch Emscripten's own `out()`/`err()`
        // messages; `stdout`/`stderr` are the program's bytes, which we line
        // up ourselves (see flushStreams above).
        factory({
            print:    postPrint,
            printErr: postPrintErr,
            stdout:   (byte) => streamByte(stdoutStream, byte),
            stderr:   (byte) => streamByte(stderrStream, byte),
        }).then(function (mod) {
            turiModule = mod;
            const bootModule = performance.now();
            const initResult = turiModule._turi_wasm_init();
            if (initResult !== 0) {
                self.postMessage({ type: 'init-error', error: 'turi_wasm_init failed: ' + initResult });
                return;
            }
            const bootInit = performance.now();
            // An older turmeric.wasm has no phase table; the stage times still hold.
            let phases = {};
            if (typeof turiModule._turi_wasm_boot_timings === 'function') {
                try { phases = JSON.parse(turiModule.UTF8ToString(turiModule._turi_wasm_boot_timings())); }
                catch (_) { phases = {}; }
            }
            ready = true;
            self.postMessage({
                type: 'ready',
                boot: {
                    script_ms: bootScript - bootT0,          // fetch + parse turmeric.js
                    module_ms: bootModule - bootScript,      // fetch + compile + instantiate the wasm, embedded files
                    init_ms:   bootInit - bootModule,        // turi_wasm_init: the stdlib preload
                    phases:    phases,                       // turi_wasm_init's own split
                },
            });
            drainQueue();
        }).catch(function (err) {
            self.postMessage({ type: 'init-error', error: String(err) });
        });
        return;
    }

    if (!ready) {
        queue.push(msg);
        return;
    }

    handleMessage(msg);
});

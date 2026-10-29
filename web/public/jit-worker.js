// jit-worker.js -- Classic Worker for the Compile engine (wasm-jit-plan P4).
//
// Loads the browser JIT module, turmeric-jit.{js,wasm}: the driver's front
// half emits C, c2mir compiles it, and MIR's interpreter runs it -- the way
// `tur jit` runs a program, so inline C works on the page.  A second module
// beside the interpreter's (eval-worker.js), loaded only when someone picks
// Compile.  Both fetch the same stdlib.data (wasm-jit-plan 5.11).
//
// Two requests.  'jit-run' {input} runs a whole program and answers
// 'jit-result' with the module's engine line -- {engine: 'compiled'|
// 'interpreted'|'error', exit, reason, front_ms, compile_ms, run_ms}.  'eval'
// {input} is a prompt turn (wasm-jit-plan P6): it goes to this module's own
// interpreter session, turi_wasm_eval, where an inline-C defn the interpreter
// cannot run is compiled on its first call (aot-compiled-repl-plan C1), and
// answers 'eval-result' {result} as eval-worker.js does; with {compiled: true}
// the turn runs as a compiled program instead (aot-compiled-repl-plan C2,
// turi_wasm_set_compiled_repl).  Either posts the
// program's output as 'print'/'printErr' lines first.
//
// A JavaScript exception out of the module means the instance is done for
// (wasm-jit-plan 5.8): a wasm trap leaves its state wherever the trap
// happened, and the JS stack limit (a RangeError, which deep recursion under
// MIR's interpreter reaches at ~1,700 frames) leaves the linear stack pointer
// and the interpreter's frames mid-call.  The answer then carries `trapped`,
// and jit-client.js terminates this worker; the next Compile boots a fresh one.

let jitModule = null;
let ready = false;
const queue = [];

// stdout/stderr, assembled into lines here so a partial last line still
// reaches the page with the run that printed it (see eval-worker.js, which
// this mirrors).
const streamDecoder = new TextDecoder('utf-8');
const stdoutStream = { bytes: [], type: 'print' };
const stderrStream = { bytes: [], type: 'printErr' };

function emitStreamLine(s) {
    const text = s.bytes.length ? streamDecoder.decode(new Uint8Array(s.bytes)) : '';
    s.bytes.length = 0;
    self.postMessage({ type: s.type, text });
}

function streamByte(s, byte) {
    if (byte === null) { flushStreams(); return; }
    if (byte === 0) return;
    if (byte === 10) { emitStreamLine(s); return; }
    s.bytes.push(byte);
}

function flushStreams() {
    if (stdoutStream.bytes.length) emitStreamLine(stdoutStream);
    if (stderrStream.bytes.length) emitStreamLine(stderrStream);
}

function withString(str, fn) {
    const len = jitModule.lengthBytesUTF8(str) + 1;
    const ptr = jitModule._malloc(len);
    jitModule.stringToUTF8(str, ptr, len);
    try { return fn(ptr); } finally { jitModule._free(ptr); }
}

function runProgram(msg) {
    const id = msg.id;
    try {
        const resultPtr = withString(msg.input, (srcPtr) =>
            withString(msg.args || '', (argsPtr) => jitModule._turi_wasm_jit_run(srcPtr, argsPtr)));
        flushStreams();
        const text = resultPtr ? jitModule.UTF8ToString(resultPtr) : '';
        if (resultPtr) jitModule._turi_wasm_free_string(resultPtr);
        let answer;
        try { answer = JSON.parse(text); }
        catch (_) { answer = { engine: 'error', exit: 1, reason: 'the module gave no answer' }; }
        self.postMessage({ type: 'jit-result', id, answer });
    } catch (err) {
        flushStreams();
        const stackOverflow = err instanceof RangeError;
        const first = String(err && err.message ? err.message : err).split('\n')[0];
        self.postMessage({
            type: 'jit-result', id,
            answer: {
                engine: 'error', exit: 1, trapped: true,
                reason: stackOverflow ? 'stack overflow: recursion too deep'
                                      : `the compiled program trapped: ${first}`,
            },
        });
    }
}

function evalTurn(msg) {
    const id = msg.id;
    try {
        // aot-compiled-repl-plan C2: compiled turns for this session, when
        // the page asks (an older module has no switch, and stays off).
        if (typeof jitModule._turi_wasm_set_compiled_repl === 'function')
            jitModule._turi_wasm_set_compiled_repl(msg.compiled ? 1 : 0);
        const resultPtr = withString(msg.input, (srcPtr) => jitModule._turi_wasm_eval(srcPtr));
        flushStreams();
        const result = resultPtr ? jitModule.UTF8ToString(resultPtr) : '';
        if (resultPtr) jitModule._turi_wasm_free_string(resultPtr);
        self.postMessage({ type: 'eval-result', id, result });
    } catch (err) {
        flushStreams();
        const reason = err instanceof RangeError
            ? 'stack overflow: recursion too deep'
            : `the compiled code trapped: ${String(err && err.message ? err.message : err).split('\n')[0]}`;
        self.postMessage({ type: 'eval-result', id, result: `Error: ${reason}`, trapped: true });
    }
}

function handleMessage(msg) {
    if (msg.type === 'jit-run') runProgram(msg);
    else if (msg.type === 'eval') evalTurn(msg);
}

self.addEventListener('message', function (e) {
    const msg = e.data;

    if (msg.type === 'init') {
        // The module is -pthread, like turmeric.wasm: it needs
        // SharedArrayBuffer, which only a cross-origin isolated page has.
        if (typeof SharedArrayBuffer === 'undefined') {
            self.postMessage({ type: 'init-error', error: 'shared-array-buffer-unavailable' });
            return;
        }
        const t0 = performance.now();
        importScripts('/turmeric-jit.js');
        const tScript = performance.now();
        const factory = self.TurmericJitModule;
        if (typeof factory !== 'function') {
            self.postMessage({ type: 'init-error', error: 'TurmericJitModule not found after importScripts' });
            return;
        }
        factory({
            print:    (text) => self.postMessage({ type: 'print', text }),
            printErr: (text) => self.postMessage({ type: 'printErr', text }),
            stdout:   (byte) => streamByte(stdoutStream, byte),
            stderr:   (byte) => streamByte(stderrStream, byte),
        }).then(function (mod) {
            jitModule = mod;
            const tModule = performance.now();
            if (mod._turi_wasm_init() !== 0) {
                self.postMessage({ type: 'init-error', error: 'turi_wasm_init failed' });
                return;
            }
            ready = true;
            self.postMessage({
                type: 'ready',
                boot: {
                    script_ms: tScript - t0,
                    module_ms: tModule - tScript,
                    init_ms: performance.now() - tModule,
                },
            });
            queue.splice(0).forEach(handleMessage);
        }).catch(function (err) {
            self.postMessage({ type: 'init-error', error: String(err) });
        });
        return;
    }

    if (!ready) { queue.push(msg); return; }
    handleMessage(msg);
});

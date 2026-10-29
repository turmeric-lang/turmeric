// tools/wasm-jit/run.js -- run a Turmeric program through the browser JIT
// module (turmeric-jit.{js,wasm}) under node, the way the page will
// (wasm-jit-plan P3).
//
//   node tools/wasm-jit/run.js [--module DIR] [--result FILE] [--repeat N]
//                              [--dump-c FILE] [--turns [--compiled-turns] [--time]]
//                              program.tur [arg ...]
//
// The program's stdout and stderr bytes go to this process's stdout and
// stderr unchanged; its stdin is this process's stdin.  The module's answer
// (the engine line: {"engine":"compiled"|"interpreted"|"error", "exit":N,
// "reason":...}) goes to --result when given, else to stderr as one
// "tur-wasm-jit: {...}" line.  The exit status is the program's.
//
// --module defaults to build-wasm/wasm-jit (the tur_wasm_jit target's output).
// --repeat runs the same program N times in one module instance, which is how
// the page reuses it; every run's answer is reported, the last one decides
// the exit status.  --dump-c writes the text c2mir was handed (prelude and
// all), where a <tur-jit>:LINE diagnostic points.
//
// --turns runs the file as prompt turns instead (wasm-jit-plan P6): each chunk
// between lines of `---` goes to turi_wasm_eval, the call the Compile
// engine's worker makes for a prompt submission, and its result is printed
// as `=> RESULT`.  One session, as at the page's prompt, so a defn in one
// turn is there in the next; C1 compiles an inline-C defn on its first call.
// --time prints `run.js: turn N: T ms` on stderr after each turn: the wall
// time of its turi_wasm_eval call, which is what the page waits for
// (benchmarks/repl-turn/README.md).
// --compiled-turns runs each turn as a compiled program instead
// (aot-compiled-repl-plan C2, turi_wasm_set_compiled_repl), as the page does
// with "Compile prompt turns too" ticked.
'use strict';
const fs = require('fs');
const path = require('path');

const argv = process.argv.slice(2);
let moduleDir = path.resolve(__dirname, '../../build-wasm/wasm-jit');
let resultFile = null;
let repeat = 1;
let dumpC = null;
let turns = false;
let compiledTurns = false;
let timeTurns = false;
while (argv.length && argv[0].startsWith('--')) {
  const flag = argv.shift();
  if (flag === '--module') moduleDir = path.resolve(argv.shift());
  else if (flag === '--result') resultFile = argv.shift();
  else if (flag === '--repeat') repeat = parseInt(argv.shift(), 10);
  else if (flag === '--dump-c') dumpC = argv.shift();
  else if (flag === '--turns') turns = true;
  else if (flag === '--compiled-turns') { turns = true; compiledTurns = true; }
  else if (flag === '--time') timeTurns = true;
  else { console.error(`run.js: unknown flag ${flag}`); process.exit(2); }
}
if (!argv.length) {
  console.error('usage: node run.js [--module DIR] [--result FILE] [--repeat N] [--dump-c FILE] [--turns [--compiled-turns] [--time]] program.tur [arg ...]');
  process.exit(2);
}
const programPath = argv.shift();
const programArgs = argv;
const source = fs.readFileSync(programPath, 'utf8');

// stdin: whatever was piped in, byte by byte; EOF after that.
let stdinBytes = Buffer.alloc(0);
if (!process.stdin.isTTY) {
  try { stdinBytes = fs.readFileSync(0); } catch (e) { /* no stdin */ }
}
let stdinPos = 0;

// stdout/stderr: per byte, so a line without a newline is not held back.
function byteSink(fd) {
  const chunk = [];
  return {
    put(c) {
      if (c === null || c === undefined) return;
      chunk.push(c & 0xff);
      if (chunk.length >= 4096) this.flush();
    },
    flush() {
      if (!chunk.length) return;
      fs.writeSync(fd, Buffer.from(chunk));
      chunk.length = 0;
    },
  };
}
const out = byteSink(1);
const err = byteSink(2);

const factory = require(path.join(moduleDir, 'turmeric-jit.js'));
factory({
  locateFile: (p) => path.join(moduleDir, p),
  preRun: [(mod) => {
    mod.ENV.TUR_STDLIB_DIR = '/stdlib';
    if (dumpC) mod.ENV.TUR_JIT_DUMP_FULL = '/tmp/tur-jit-full.c';
    // TUR_JIT_* knobs (TUR_JIT_TIMING, ...) reach the module as they reach `tur jit`.
    for (const [k, v] of Object.entries(process.env))
      if (k.startsWith('TUR_JIT_')) mod.ENV[k] = v;
    mod.FS.init(
      () => (stdinPos < stdinBytes.length ? stdinBytes[stdinPos++] : null),
      (c) => out.put(c),
      (c) => err.put(c));
  }],
}).then((mod) => {
  if (turns) {
    if (mod._turi_wasm_init() !== 0) { console.error('run.js: turi_wasm_init failed'); process.exit(1); }
    const evalTurn = mod.cwrap('turi_wasm_eval', 'number', ['string']);
    if (compiledTurns && !mod._turi_wasm_set_compiled_repl(1)) {
      console.error('run.js: this module cannot compile prompt turns'); process.exit(1);
    }
    let turnNo = 0;
    for (const turn of source.split(/^---$/m)) {
      if (!turn.trim()) continue;
      let ptr = 0, text;
      const t0 = performance.now();
      try {
        ptr = evalTurn(turn);
        text = ptr ? mod.UTF8ToString(ptr) : '';
      } catch (e) {
        text = `the module threw: ${String(e && e.message ? e.message : e).split('\n')[0]}`;
      } finally {
        if (ptr) mod._free(ptr);
      }
      const ms = performance.now() - t0;
      out.flush();
      err.flush();
      fs.writeSync(1, `=> ${text}\n`);
      turnNo++;
      if (timeTurns) fs.writeSync(2, `run.js: turn ${turnNo}: ${ms.toFixed(1)} ms\n`);
    }
    process.exit(0);
  }
  const run = mod.cwrap('turi_wasm_jit_run', 'number', ['string', 'string']);
  const free = mod.cwrap('turi_wasm_free_string', null, ['number']);
  let answer = { engine: 'error', exit: 1, reason: 'not run' };
  for (let i = 0; i < repeat; i++) {
    let ptr = 0;
    try {
      ptr = run(source, programArgs.join('\n'));
      answer = JSON.parse(mod.UTF8ToString(ptr));
    } catch (e) {
      // A JS exception out of the module: the stack limit (RangeError) or a
      // wasm trap.  The page reports these itself (P4).
      const msg = String(e && e.message ? e.message : e).split('\n')[0];
      answer = { engine: 'error', exit: 1, reason: `the module threw: ${msg}` };
    } finally {
      if (ptr) free(ptr);
    }
    out.flush();
    err.flush();
    if (dumpC) {
      try { fs.writeFileSync(dumpC, mod.FS.readFile('/tmp/tur-jit-full.c')); } catch (e) { /* never reached c2mir */ }
    }
    const line = JSON.stringify(answer);
    if (resultFile) fs.appendFileSync(resultFile, line + '\n');
    else fs.writeSync(2, `tur-wasm-jit: ${line}\n`);
  }
  process.exit(answer.exit);
});

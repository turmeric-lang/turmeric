// tools/wasm-jit/run.js -- run a Turmeric program through the browser JIT
// module (turmeric-jit.{js,wasm}) under node, the way the page will
// (wasm-jit-plan P3).
//
//   node tools/wasm-jit/run.js [--module DIR] [--result FILE] [--repeat N]
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
// the exit status.
'use strict';
const fs = require('fs');
const path = require('path');

const argv = process.argv.slice(2);
let moduleDir = path.resolve(__dirname, '../../build-wasm/wasm-jit');
let resultFile = null;
let repeat = 1;
while (argv.length && argv[0].startsWith('--')) {
  const flag = argv.shift();
  if (flag === '--module') moduleDir = path.resolve(argv.shift());
  else if (flag === '--result') resultFile = argv.shift();
  else if (flag === '--repeat') repeat = parseInt(argv.shift(), 10);
  else { console.error(`run.js: unknown flag ${flag}`); process.exit(2); }
}
if (!argv.length) {
  console.error('usage: node run.js [--module DIR] [--result FILE] [--repeat N] program.tur [arg ...]');
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
    mod.FS.init(
      () => (stdinPos < stdinBytes.length ? stdinBytes[stdinPos++] : null),
      (c) => out.put(c),
      (c) => err.put(c));
  }],
}).then((mod) => {
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
    const line = JSON.stringify(answer);
    if (resultFile) fs.appendFileSync(resultFile, line + '\n');
    else fs.writeSync(2, `tur-wasm-jit: ${line}\n`);
  }
  process.exit(answer.exit);
});

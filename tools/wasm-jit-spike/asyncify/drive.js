// drive.js -- run one program through turi_wasm_jit_run with ccall's async
// mode, so a program that sleeps under Asyncify returns a promise, and count
// the event-loop ticks that ran meanwhile (raylib-web-playground-plan
// prerequisite 3).
//   node tools/wasm-jit-spike/asyncify/drive.js MODULE_DIR FILE.tur [sync]
const fs = require('fs');
const path = require('path');
const [dir, file, mode] = process.argv.slice(2);
const factory = require(path.resolve(dir, 'turmeric-jit.js'));
const src = fs.readFileSync(file, 'utf8');
let ticks = 0;
const timer = setInterval(() => { ticks++; }, 5);   // does the event loop run while the program does?
factory({
  locateFile: (p) => path.resolve(dir, p),
  print: (t) => process.stdout.write(t + '\n'),
  printErr: (t) => process.stderr.write(t + '\n'),
}).then(async (mod) => {
  const t0 = performance.now();
  let text;
  try {
    const r = mod.ccall('turi_wasm_jit_run', 'number', ['string', 'string'], [src, ''],
                        mode === 'sync' ? {} : { async: true });
    const ptr = await r;
    text = mod.UTF8ToString(ptr);
  } catch (e) {
    text = 'threw: ' + String(e && e.message ? e.message : e).split('\n')[0];
    if (process.env.STACK) console.error(e.stack);
  }
  clearInterval(timer);
  console.error(`drive: ${text}  wall ${(performance.now() - t0).toFixed(0)} ms, event-loop ticks ${ticks}`);
  process.exit(0);
});

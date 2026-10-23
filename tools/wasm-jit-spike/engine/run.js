// node run.js <file.tur> [flags]  -- run a program through the wasm JIT engine.
// flags: 1 emit as the wasm AOT target does, 2 no split, 4 no prune, 8 dump TU.
const fs = require('fs');
const path = require('path');
const M = require(path.resolve(__dirname, 'build/turmeric-jit.js'));
const file = process.argv[2];
const flags = parseInt(process.argv[3] || '0', 10);
M({
  print: s => console.log('[out] ' + s),
  printErr: s => console.log('[err] ' + s),
  preRun: [mod => { mod.ENV.TUR_STDLIB_DIR = '/stdlib'; mod.ENV.TUR_JIT_TIMING = '1'; mod.ENV.TUR_JIT_DUMP_FULL = '/tmp/jit-full.c'; }],
}).then(mod => {
  const t0 = performance.now();
  const ir = mod._turi_wasm_init();
  const t1 = performance.now();
  console.log(`init rc=${ir} ${(t1 - t0).toFixed(0)} ms`);
  const run = mod.cwrap('turi_wasm_jit_run', 'number', ['string', 'string', 'number']);
  const src = fs.readFileSync(file, 'utf8');
  const t2 = performance.now();
  let rc;
  try { rc = run(src, path.basename(file), flags); }
  catch (e) { console.log('THREW ' + String(e).split('\n')[0]); rc = -1; }
  console.log(`jit rc=${rc} ${(performance.now() - t2).toFixed(0)} ms`);
  if (flags & 8) { try { fs.writeFileSync(path.join(__dirname, 'build/jit-full.c'), mod.FS.readFile('/tmp/jit-full.c')); console.log('full TU dumped to build/jit-full.c'); } catch (e) { console.log('no full dump: ' + e); } }
  if (flags & 8) { try { fs.writeFileSync(path.join(__dirname, 'build/jit-tu.c'), mod.FS.readFile('/tmp/jit-tu.c')); console.log('TU dumped to build/jit-tu.c'); } catch (e) { console.log('no tu dump: ' + e); } }
  process.exit(rc === 0 ? 0 : 1);
});

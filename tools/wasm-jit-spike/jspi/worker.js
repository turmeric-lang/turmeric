// The same harness inside a Web Worker, as Try's jit-worker.js runs
// the module, so the worker's stack limit is what is measured.
let mod = null;
const out = [], err = [];
self.onmessage = async (e) => {
  const m = e.data;
  if (m.type === 'boot') {
    importScripts(m.dir + '/turmeric-jit.js');
    mod = await self.TurmericJitModule({
      locateFile: (p) => m.dir + '/' + p,
      print: (t) => out.push(t),
      printErr: (t) => err.push(t),
    });
    if (mod._turi_wasm_init() !== 0) { self.postMessage({ type: 'boot', error: 'init failed' }); return; }
    self.postMessage({ type: 'boot', ok: true, jspi: 'Suspending' in WebAssembly });
    return;
  }
  if (m.type === 'run') {
    out.length = 0; err.length = 0;
    const alloc = (s) => { const n = mod.lengthBytesUTF8(s) + 1; const p = mod._malloc(n); mod.stringToUTF8(s, p, n); return p; };
    const a = alloc(m.src), b = alloc('');
    let answer;
    const t0 = performance.now();
    try {
      const r = await mod._turi_wasm_jit_run(a, b);
      answer = mod.UTF8ToString(r);
      mod._turi_wasm_free_string(r);
    } catch (ex) {
      answer = 'threw: ' + String(ex && ex.message ? ex.message : ex).split('\n')[0];
    }
    const wall = performance.now() - t0;
    mod._free(a); mod._free(b);
    self.postMessage({ type: 'run', result: { out: out.join('\n'), err: err.join('\n'), answer, wall } });
  }
};

// The JSPI spike's page (see README): boots a JIT module
// from DIR on the main thread and runs one program through turi_wasm_jit_run,
// awaiting the result so a JSPI (promising) export and a plain one both work.
window.boot = async (dir) => {
  await new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = dir + '/turmeric-jit.js';
    s.onload = resolve;
    s.onerror = () => reject(new Error('script load failed: ' + s.src));
    document.head.appendChild(s);
  });
  const out = [], err = [];
  const mod = await TurmericJitModule({
    locateFile: (p) => dir + '/' + p,
    print: (t) => out.push(t),
    printErr: (t) => err.push(t),
  });
  if (mod._turi_wasm_init() !== 0) throw new Error('turi_wasm_init failed');
  window.mod = mod; window.out = out; window.err = err;
  return { sab: typeof SharedArrayBuffer !== 'undefined', jspi: 'Suspending' in WebAssembly };
};
window.runProgram = async (src) => {
  const mod = window.mod, out = window.out, err = window.err;
  out.length = 0; err.length = 0;
  const alloc = (s) => { const n = mod.lengthBytesUTF8(s) + 1; const p = mod._malloc(n); mod.stringToUTF8(s, p, n); return p; };
  const a = alloc(src), b = alloc('');
  let answer;
  const t0 = performance.now();
  try {
    const r = await mod._turi_wasm_jit_run(a, b);
    answer = mod.UTF8ToString(r);
    mod._turi_wasm_free_string(r);
  } catch (e) {
    answer = 'threw: ' + String(e && e.message ? e.message : e).split('\n')[0];
  }
  const wall = performance.now() - t0;
  mod._free(a); mod._free(b);
  return { out: out.join('\n'), err: err.join('\n'), answer, wall };
};
// Worker variant: boot the module inside a Worker (worker.js) and run there.
window.bootWorker = (dir) => new Promise((resolve, reject) => {
  const w = new Worker('/jspi-harness/worker.js');
  window.worker = w;
  w.onmessage = (e) => { if (e.data.type === 'boot') (e.data.ok ? resolve(e.data) : reject(new Error(e.data.error))); };
  w.onerror = (e) => reject(new Error('worker error: ' + e.message));
  w.postMessage({ type: 'boot', dir });
});
window.runInWorker = (src) => new Promise((resolve) => {
  const w = window.worker;
  w.onmessage = (e) => { if (e.data.type === 'run') resolve(e.data.result); };
  w.postMessage({ type: 'run', src });
});

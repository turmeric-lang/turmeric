const M = require('./build/spike-opt.js');
M().then(mod => {
  const compile = mod.cwrap('spike_compile', 'number', ['string']);
  const rawcall = mod.cwrap('spike_call', 'number', ['string', 'number']);
  const src = `
#include <stdio.h>
#include <spike.h>
int sum (int n) { return n == 0 ? 0 : n + sum (n - 1); }
int loop_pure (int n) { long long s = 0; for (int i = 0; i < n; i++) s += i; return (int)(s % 1000003); }
int loop_native (int n) { long long s = 0; for (int i = 0; i < n; i++) s = native_add (s, i); return (int)(s % 1000003); }
int loop_tail (int n, long long acc) { return n == 0 ? (int)(acc % 1000003) : loop_tail (n - 1, acc + n); }
`;
  if (compile(src) !== 0) { console.log('compile failed'); return; }
  // A JS exception thrown through wasm frames does not unwind the wasm linear
  // stack pointer, and leaves the interpreter mid-frame: restore the stack and
  // rebuild the context after every failure, as c2mp rebuilds its worker.
  const call = (fn, n) => { const sp = mod.stackSave(); try { return rawcall(fn, n); } catch (e) { mod.stackRestore(sp); compile(src); throw e; } };
  let lo = 10, hi = 4000000, ok = 0, lastErr = '';
  while (lo <= hi) { const mid = (lo + hi) >> 1; let good = true;
    try { if (call('sum', mid) !== ((mid * (mid + 1) / 2) | 0)) good = false; } catch (e) { good = false; lastErr = String(e).split('\n')[0]; }
    if (good) { ok = mid; lo = mid + 1; } else hi = mid - 1; }
  console.log('max recursion depth (sum), -O2 interpreter, node default stack:', ok, ' (failure:', lastErr + ')');
  const time = (fn, n) => { const t0 = performance.now(); const r = call(fn, n); return [performance.now() - t0, r]; };
  for (const [fn, n] of [['loop_pure', 1000000], ['loop_pure', 10000000], ['loop_pure', 10000000], ['loop_native', 1000000], ['loop_native', 1000000], ['loop_tail', 1000]]) {
    time(fn, 1000); const [ms, r] = time(fn, n);
    console.log(`${fn} n=${n}: ${ms.toFixed(1)} ms  (${(ms * 1e6 / n).toFixed(0)} ns/iter)  result=${r}`);
  }
});

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
/* A mutual tail-call chain, without and with a local aggregate: what it
   leaves behind, as the distance the aggregate (or the linear stack pointer)
   moved between the first call and the last.  Both are 0 since the
   interpreter keeps its frames on a stack of its own
   (wasm-mir-interp-tail-call-keeps-allocas). */
static long long sp0; static char *first;
struct S { long long x[4]; };
static void use (struct S *p) { p->x[1] = p->x[0]; }
int na (int n); int nb (int n);
int na (int n) { if (sp0 == 0) sp0 = sp_now (); if (n == 0) { int d = (int) (sp0 - sp_now ()); sp0 = 0; return d; } __attribute__((musttail)) return nb (n - 1); }
int nb (int n) { __attribute__((musttail)) return na (n); }
int wa (int n); int wb (int n);
int wa (int n) { struct S s; s.x[0] = n; use (&s); if (sp0 == 0) { sp0 = sp_now (); first = (char *) &s; }
  if (n == 0) { int d = (int) (sp0 - sp_now ()) + (int) (first > (char *) &s ? first - (char *) &s : (char *) &s - first); sp0 = 0; return d; }
  __attribute__((musttail)) return wb (n - 1); }
int wb (int n) { __attribute__((musttail)) return wa (n); }
/* A variadic call in a loop: c2mir's wasm32 va buffer is one per function,
   not an alloca per call (c2mir-wasm-vararg-buffer-allocas-per-call). */
int pf (int n) { long long b = sp_now (); for (int i = 0; i < n; i++) printf ("", i); return (int) (b - sp_now ()); }
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
  for (const [fn, n] of [['loop_pure', 1000000], ['loop_pure', 10000000], ['loop_pure', 10000000], ['loop_native', 1000000], ['loop_native', 1000000], ['loop_tail', 500]]) {
    time(fn, 500); const [ms, r] = time(fn, n);
    console.log(`${fn} n=${n}: ${ms.toFixed(1)} ms  (${(ms * 1e6 / n).toFixed(0)} ns/iter)  result=${r}`);
  }
  let bad = 0;
  for (const fn of ['na', 'wa']) for (const n of [10, 1000, 100000]) {
    let r; try { r = call(fn, n); } catch (e) { r = String(e).split('\n')[0]; }
    if (r !== 0) bad++;
    console.log(`tail-call chain ${fn} n=${n}: bytes left behind = ${r}`);
  }
  for (const n of [10, 2000000]) {
    let r; try { r = call('pf', n); } catch (e) { r = String(e).split('\n')[0]; }
    if (r !== 0) bad++;
    console.log(`variadic call in a loop pf n=${n}: stack grown = ${r}`);
  }
  if (bad) { console.log('FAIL: a tail-call chain or a variadic loop grew the stack'); process.exitCode = 1; }
});

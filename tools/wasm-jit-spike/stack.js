// Linear-stack growth of interpreted code that should run in constant stack.
// Pins two fixed reports: a tail-call chain through a function that made an
// alloca (docs/archive/wasm-mir-interp-tail-call-keeps-allocas.md, mir#22) and
// a variadic call in a loop (docs/archive/c2mir-wasm-vararg-buffer-allocas-per-call.md,
// mir#21).  Each row prints the bytes the linear stack grew; withalloc must
// match noalloc, and printf_loop must be 0, for every n.  Exits 1 otherwise.
const M = require('./build/spike-opt.js');
M().then(mod => {
  const compile = mod.cwrap('spike_compile', 'number', ['string']);
  const call = mod.cwrap('spike_call', 'number', ['string', 'number']);
  const src = `
#include <spike.h>
int printf (const char *, ...);
static long long base;
struct S { long long x[4]; };
static void use (struct S *p) { p->x[1] = p->x[0]; }
int na (int n); int nb (int n);
int na (int n) { if (n == 0) return (int) (base - sp_now ()); __attribute__((musttail)) return nb (n - 1); }
int nb (int n) { __attribute__((musttail)) return na (n); }
int noalloc (int n) { base = sp_now (); return na (n); }
int wa (int n); int wb (int n);
int wa (int n) { struct S s; s.x[0] = n; use (&s); if (n == 0) return (int) (base - sp_now ()); __attribute__((musttail)) return wb (n - 1); }
int wb (int n) { __attribute__((musttail)) return wa (n); }
int withalloc (int n) { base = sp_now (); return wa (n); }
int printf_loop (int n) { long long b = sp_now (); for (int i = 0; i < n; i++) printf ("", i); return (int) (b - sp_now ()); }
`;
  if (compile(src) !== 0) { console.log('compile failed'); process.exit(1); }
  let bad = 0;
  for (const n of [10, 1000, 100000, 2000000]) {
    const row = {};
    for (const fn of ['noalloc', 'withalloc', 'printf_loop']) {
      try { row[fn] = call(fn, n); } catch (e) { row[fn] = String(e).split('\n')[0]; }
    }
    const ok = row.withalloc === row.noalloc && row.printf_loop === 0;
    if (!ok) bad++;
    console.log(`n=${n}: noalloc ${row.noalloc}  withalloc ${row.withalloc}  printf_loop ${row.printf_loop}  ${ok ? 'ok' : 'GROWS'}`);
  }
  process.exit(bad ? 1 : 0);
});

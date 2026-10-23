const M = require(require('path').resolve(process.argv[2] || 'build/spike-dbg.js'));
M().then(mod => {
  const compile = mod.cwrap('spike_compile', 'number', ['string']);
  const call = mod.cwrap('spike_call', 'number', ['string', 'number']);
  console.log('compile:', compile('#include <spike.h>\nint sum (int n) { return n == 0 ? 0 : n + sum (n - 1); }\nint lp (int n) { long long s = 0; for (int i = 0; i < n; i++) s += i; return (int) s; }\nint ln (int n) { long long s = 0; for (int i = 0; i < n; i++) s = native_add (s, i); return (int) s; }\nint id (int n) { return n; }\n'));
  for (const [f, n] of [['id', 5], ['lp', 10], ['sum', 5], ['sum', 50], ['ln', 10]]) {
    try { console.log(f, n, '->', call(f, n)); } catch (e) { console.log(f, n, 'THREW', String(e).split('\n')[0]); }
  }
});

# More than 256 threadable callbacks: the rest abort with "unhandled effect"

**Severity:** medium-low. A correct program aborts at run time once it has
more than 256 effectful callbacks that the CPS backend threads, and nothing
diagnoses the cause.

## Repro

`n` functions, each passing its own effectful lambda to one effectful
callback slot:

```python
# gen.py N -- writes regN.tur
import sys
n = int(sys.argv[1])
print("(defeffect Ask [] : int)")
print("(defn apply-eff [f : (fn [] #fx{Ask} int)] : int (f))")
for i in range(n):
    nxt = f"(c{i+1})" if i + 1 < n else "0"
    print(f"(defn c{i} [] : int (+ (apply-eff (fn [] : int (+ {i} (perform (Ask))))) {nxt}))")
print("(defn main [] : int\n  (println (handle (c0) (Ask [] k) (resume k 0)))\n  0)")
```

- `n = 250`: prints `31125` (correct).
- `n = 260` or `n = 270`: `tur: unhandled effect (tag 2)`, then `Aborted`.

## Root cause

Each threadable lambda registers its `__cps` twin at static-init time
(`__tur_e2reg_<fn>` calls `__tur_cps_register`). The registry is a fixed
array in the emitted runtime (`src/compiler/emit_dk_runtime.c`):

```c
static struct { intptr_t direct; __tur_cps_fn cps; } __tur_cps_reg[256];
...
if (__tur_cps_reg_n < 256) { ... __tur_cps_reg_n++; }
```

Registrations past 256 are silently dropped. A call through the fat-closure
dispatch (`emit_cps_ir.c:emit_e2a_fat_dispatch`) then misses the lookup and
falls back to the plain direct entry. That entry seeds a fresh DK root, so the
callback's `perform` sees no handler. (`__tur_cps_lookup_checked`, which would
name the function and call it an internal error, is not the path these calls
take.)

## Fix directions

- Grow the registry (`realloc` in `__tur_cps_register`). The number of
  registrations is known at compile time, so the array could also be sized
  exactly.
- Make the fat dispatch's lookup miss as loud as `__tur_cps_lookup_checked`.
  Then any future miss names the function instead of reporting an unhandled
  effect.

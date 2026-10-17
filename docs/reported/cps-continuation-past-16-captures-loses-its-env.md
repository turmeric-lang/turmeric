# A CPS continuation that captures more than 16 values is emitted with no env

**Severity:** low-medium (compile failure). cc rejects the program with
undeclared temporaries; nothing at the Turmeric level says why. It needs one
function whose lifted continuation reads more than 16 enclosing values, which
a long chain of sequential `perform`s reaches quickly.

## Repro

```python
# gen.py N -- writes mN.tur
import sys
n = int(sys.argv[1])
for i in range(n): print(f"(defeffect E{i} [] : int)")
body = " ".join(f"(perform (E{i}))" for i in range(n))
print(f"(defn many [] : int (+ {body}))")
hs = " ".join(f"(E{i} [] k) (resume k {i})" for i in range(n))
print(f"(defn main [] : int (println (handle (many) {hs})) 0)")
```

- `n = 17`: prints `136`.
- `n = 18`: cc fails:

```
error: '__t0' undeclared (first use in this function); did you mean '__t18'?
    __t18 = (((...((__t0) + (__t1)) + ... + (__t17);
```

in `many_pf17`, whose env is `(void)env` -- the zero-capture shortcut --
while its body reads `__t0` .. `__t16`, the results of the 17 earlier
performs.

## Root cause

`src/compiler/emit_cps_ir.c`: `CapSet` holds at most `CC_MAX_CAPS` (16)
captures. `cap_add` / `cap_add_cvar` set `cs->ok = false` past it, and every
lifting site (`caps = (ok && cs.n > 0) ? &cs : NULL`) then emits the helper
with **no** env, which is only right when there is nothing to capture.

The admission gate does not catch it either. `CT_PERFORM` in the subset check
calls `collect_caps` on the OUTERMOST perform's continuation only;
`collect_caps_rec` walks a nested perform as a binder (its `x` is bound for the
rest) and never checks that the nested perform's OWN continuation -- a
separately lifted helper with its own env -- fits in 16. So the function is
admitted, and the 18th perform's helper overflows at emission.

The comment over `CapSet` says "a false `ok` means the caller keeps the
zero-capture fallback" -- correct for a body that captures nothing, wrong for
one that captured too much.

## Fix directions

- Make `CapSet` growable (it is a stack value at ~27 sites, so an arena-backed
  array with the inline 16 as the first block), or
- check each nested perform / await / reset continuation against the cap in
  the admission gate, so an overflowing function is evicted with a reason
  instead of emitted, and
- at the lifting sites, treat `!ok` with a non-empty capture walk as an
  internal error rather than the zero-capture fallback.

Found fixing
[effect-row-merge-overflows-past-32-effects](../archive/effect-row-merge-overflows-past-32-effects.md),
whose repro sums 40 performs; that report's fixture discards the results so
nothing is captured.

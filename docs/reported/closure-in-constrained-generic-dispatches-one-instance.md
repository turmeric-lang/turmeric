# A closure inside a constrained generic calls one instance for every instantiation

**Severity: high (silent wrong behaviour on the compiled engines).**

Found 2026-10-29 while checking whether a generic numeric-array API is
sensible (simd-and-vectorization-plan, open question 1). Pre-existing: the
same output before that day's changes.

A lambda written inside a constrained generic, whose body calls a class method
on the constraint's type variable, is lifted ONCE, and its call is resolved to
a single instance. Every spec of the generic shares that closure, so all but
one instantiation run the wrong method. `tur --interpret` is right.

## Repro

```turmeric
(defclass Sh [v] (sh [a : v] : nil))
(defopaque W1 :int)
(defopaque W2 :int)
(definstance Sh [W1] (sh [a] (println "W1")))
(definstance Sh [W2] (sh [a] (println "W2")))
(defn direct [V] [(Sh V)] [a : V] : nil (sh a))
(defn clo [V] [(Sh V)] [a : V] : nil
  (let [f (fn [] (sh a))] (f)))
(defn main [] : int
  (direct (:: 0 W1)) (direct (:: 0 W2))
  (clo (:: 0 W1)) (clo (:: 0 W2))
  0)
```

```
compiled (tur run, tur jit):  W1 W2 W2 W2
tur --interpret:              W1 W2 W1 W2
```

With an associated-type value captured (`(let [t (c-get a) f (fn [] (c-put a
t))] (f))`, `E = float` / `float32`), the shared closure also passes the
float's carrier word to the `float` instance by value conversion:
`tests/check-emitted-float-conversions.py` reports an implicit I2F at the call
in the lifted `__fn_N`.

## Root cause (likely)

The lifted closure (`__fn_N`, one `struct __env_N`) is emitted once, outside
every spec, so the per-spec re-resolution (`emit_reresolve_method_call`,
`src/compiler/emit_core.c`) never sees a spec binding for `V` inside it and
keeps whichever instance it last resolved. The spec clones the closure only
for a generic that RETURNS a closure (`inner_closure_spec_idx`,
`emit_inner_closure_needs_float_spec`, `src/compiler/emit_module.c`).

## Fix directions

- Clone a lambda lifted from a spec's body per spec when its body dispatches
  on (or holds a value of) a spec type variable, as the closure-returning case
  already does.
- Until then, refusing the shape at compile time would be better than the
  silent misdispatch.
- Pin with a fixture beside `constrained-instance-element-dispatch`, two
  instances, both a dispatch and a captured float associated-type value.

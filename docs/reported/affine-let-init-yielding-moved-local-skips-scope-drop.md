# A `let` bound to a `let` that yields a moved affine local is never dropped

**Severity: medium-low (a leak, one allocation per evaluation; a workaround exists).**

Found 2026-10-28 while landing phase N0 of
[simd-and-vectorization-plan](../upcoming/simd-and-vectorization-plan.md)
(`stdlib/numvec.tur`).

A local of a move-only `Drop` opaque (`defopaque ^affine`) is freed at scope
exit by its `Drop` instance -- unless its initialiser is itself a `let` whose
value is a local moved out of that inner `let`. Then the outer binding is
treated as "moved during init" (or its type is not seen), no scope-exit drop is
injected, and the value leaks. The same value bound through a *call* is
dropped, so the leak is invisible to a fixture that only checks output.

It bit a macro: `(f64vec-of 1.5 2.5)` first expanded to
`(let [__nv (f64vec-new 2 0.0)] (do (set ...) __nv))`, and every
`(let [a (f64vec-of ...)] ...)` leaked its array. `tests/run-leak-check.sh`
caught it (`numvec-drop-no-leak`); output was correct throughout.

## Repro

```turmeric
(defn ctr [op : int] : int
  ```c
  static int64_t a = 0, f = 0;
  if (op == 0) return ++a;
  if (op == 1) return ++f;
  if (op == 2) return a;
  return f;
  ```)
(defopaque ^affine Widget :int)
(defn mk [] : Widget (ctr 0) (:: 1 Widget))
(defn rel [w : Widget] : void (ctr 1) (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn id [w : Widget] : Widget w)

(defn direct [] : int (let [a (mk)] 0))
(defn via-call [] : int (let [a (id (let [t (mk)] t))] 0))
(defn via-let [] : int (let [a (let [t (mk)] t)] 0))

(defn main [] : int
  (direct)   (println (- (ctr 2) (ctr 3)))   ; => 0
  (via-call) (println (- (ctr 2) (ctr 3)))   ; => 0
  (via-let)  (println (- (ctr 2) (ctr 3)))   ; => 1  (expected 0)
  0)
```

`direct` and `via-call` balance; `via-let` leaves one Widget live.

## Where to look

`elab_let`'s auto-drop pass (`src/compiler/elab_forms.c`, the "skipped when the
local is moved, moved during init, or consumed" rule in
[emitted-code-ownership-guide](../guides/emitted-code-ownership-guide.md)). An
initialiser that is a `let`/`do` block ending in a bare local of the inner
scope should count as a fresh owned value, as a call result does.

## Workaround in tree

`stdlib/numvec.tur` wraps the macro's `let` in an identity call
(`f64vec-id__`), which makes the initialiser a call.

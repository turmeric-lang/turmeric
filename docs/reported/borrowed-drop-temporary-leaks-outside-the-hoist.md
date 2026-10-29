# A fresh Drop temporary passed to a `^borrow` parameter still leaks outside the hoist's two conditions

**Severity: low (a leak, one value per evaluation; binding it with `let` avoids it).**

Found 2026-10-28, landing the partial fix
(`hoist_borrowed_closure_args`, `src/compiler/elab_call.c`; fixture
`tests/fixtures/borrowed-drop-temporary-released`) during
[simd-and-vectorization-plan](../upcoming/simd-and-vectorization-plan.md) N0.

A fresh value of a move-only `Drop` opaque (a `defopaque ^affine` with a
`Drop` instance and no `Clone`: the `stdlib/numvec.tur` arrays, httpd's
`Handler`) passed straight to a `^borrow` parameter has no owner: the callee
borrows it and no binding holds it. The hoist binds it to a `__borrowd_N` temp
and drops it after the call, but only when both:

- the call returns a scalar, so the result cannot point into the temp; and
- every argument before it is a variable or literal, so hoisting it reorders
  nothing.

Otherwise the temporary is left unowned and leaks, as every such temporary did
before. That is deliberate (a leak rather than a dangling borrow), but the two
shapes are common enough to want fixing.

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
(defn peek [^borrow w : Widget] : int 1)
(defn peek2 [k : int ^borrow w : Widget] : int k)
(defn label [^borrow w : Widget] : cstr "w")
(defn live [] : nil (println (- (ctr 2) (ctr 3))))

(defn main [] : int
  (println (peek (mk)))           (live)   ; 1 0 -- hoisted, freed
  (println (label (mk)))          (live)   ; w 1 -- non-scalar result: leaks
  (println (peek2 (ctr 2) (mk)))  (live)   ; 2 2 -- effectful earlier arg: leaks
  0)
```

In numvec terms: `(f64vec-show (f64vec-new 4 7.1))`, `(f64vec-copy ...)` and
`(f64vec->vec ...)` of a temporary leak it; `(f64vec-len (f64vec-new 4 7.1))`
does not.

## Fix directions

- **Effectful earlier argument** (the easy half): hoist every argument up to and
  including the temporary into the `let`, in order, instead of only the
  temporary. The call then reads all of them from temps and evaluation order is
  kept. Arguments after it need not move.
- **Non-scalar result**: the hoist needs to know the result does not alias the
  borrowed argument. Options: a callee-side fact the elaborator already has for
  some functions (a freshly allocated result, like `returns_fresh_closure` for
  closures -- `P-copy`, `P-show` and `P->vec` all build a new value); or a
  lifetime check that the result type cannot carry a borrow of the argument
  (no `cstr`, `ptr<T>` or borrowed reference in it). Until then the module
  header of `stdlib/numvec.tur` tells users to bind first.

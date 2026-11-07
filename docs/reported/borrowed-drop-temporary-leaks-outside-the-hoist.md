# A fresh Drop temporary passed to a `^borrow` parameter still leaks outside the hoist's two conditions

**Severity: low (a leak, one value per evaluation; binding it with `let` avoids it).**

**Narrowed 2026-11-07: a fn-typed earlier argument with no effect beside it
is hoisted.** Re-measured on `main`, the shape was wider than this report
said: `(peekf f 7 (mk))` -- `f` a let-bound closure, a parameter, a named
defn or a lambda -- leaked its temporary with NO effectful argument at all, as
did `(peekf1 f (mk))`. A fn-typed argument arrives wrapped in its fn-value
conversion (`EX_POLY_WRAP`, `EX_FN_TO_FAT`, `EX_POLY_TO_FAT`), and
`arg_is_effect_free` (`src/compiler/elab_call.c`) peeled only ascriptions, so
the call's prefix read as effectful and the order check then refused the
owning value. It peels the wrappers `HOIST_PEEL_SLOT` already peels now, and
takes a captureless `EX_FN` as effect-free. Pinned by
`tests/fixtures/borrowed-drop-temporary-fn-earlier-arg` (leak-checked: a
let-bound and a capturing lambda, a named defn, a captureless lambda, with and
without a middle argument, 50 turns; the live count was 306 after the loop, it
is 0). Still open, unchanged: an OWNING earlier argument -- a closure, a `Vec`,
a Drop value -- beside an EFFECTFUL one.

**Narrowed again 2026-11-06: a plain-data struct before the effect is
hoisted too.** `(peekp q (do (set! q ...) (ctr 2)) (mk))` releases its
temporary and reads `q` before the effect, as the interpreter does (compiled
code read it after: C leaves argument order unspecified). A by-value product
whose every field is a plain scalar, with no Drop instance and nothing affine,
owns nothing a copy could double-own (`hoist_arg_is_plain_copy`,
`src/compiler/elab_call.c`). Left: an OWNING non-scalar earlier argument (a
closure, a `Vec`, a Drop value) beside an effect.

**Narrowed 2026-11-06: a `cstr`, `Vec` or other non-scalar result is
released too, when the callee's body cannot hand back a view of the
argument.** `(f64vec-show (f64vec-new 4 7.1))` and `(f64vec->vec ...)` free
their temporary. What is left is one shape: an effectful argument before the
temporary while a NON-scalar argument (a struct, a closure) also precedes it.
See "Fixed 2026-11-06" at the end.

**Narrowed 2026-11-05: the effectful-earlier-argument half is fixed, and a
move-only Drop result is admitted.** What is left is a call whose result is
neither a scalar nor a move-only Drop value -- a `cstr`, a `Vec`, a struct --
and an earlier argument that is not a plain scalar when one before the
temporary has an effect. See "Fixed 2026-11-05" at the end.

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

## Fixed 2026-11-05

- **Effectful earlier argument.** When an argument before the temporary has an
  effect and every non-literal argument before it is a plain scalar, those are
  hoisted into `__borrowa_N` temps ahead of it, in order
  (`hoist_ordered_arg_temp`, `src/compiler/elab_call.c`). Variables are hoisted
  too, because an effectful neighbour may assign one: `(peek3 x (do (set! x 7)
  (ctr 2)) (mk))` still reads `x` as 1. `(peek2 (ctr 2) (mk))` releases its
  temporary. A non-scalar earlier argument (a struct, a closure) keeps the old
  behaviour, a leak.
- **Move-only Drop result.** A result whose type is a move-only Drop opaque
  owns its own resource -- its Drop releases it -- so it is not a view of the
  argument (`F64View` is neither affine nor Drop). `(f64vec-copy (f64vec-new 4
  7.1))` releases the temporary; the copy goes to the `let` that binds it, or
  to a further hoist, since `init_yields_fresh_owner` sees through the let the
  hoist builds. `(peek (dup (dup (mk))))` frees all three.

Pinned by `tests/fixtures/borrowed-drop-temporary-released` (both engines
agree; the fixture's inline C keeps it out of `run-turi.sh`).

Still open: `(f64vec-show (f64vec-new 4 7.1))` (a `cstr` result) and
`(f64vec->vec ...)` (a `Vec`). Either could point into the argument as far as
the elaborator knows, so they need a callee-side "fresh result" fact or a
lifetime check, as the fix directions above say.

## Fixed 2026-11-06

- **Non-scalar result.** The callee-side fact the fix directions asked for,
  read off the callee's body (`borrow_param_result_may_view`,
  `src/compiler/elab_call.c`). The result cannot hold anything of the
  `^borrow` parameter when every use of the parameter in the body is an
  argument of a call that returns a plain scalar (`(f64vec-ref v i)`,
  `(f64vec-len v)`), or one handed to a `^borrow` parameter of a callee of
  which the same holds. Recursion is read as "holds" (the greatest fixpoint:
  an alias needs some finite use the walk refuses), and an answer that leaned
  on a question still in progress is withdrawn when that question answers
  "may". A bare use, a capture, an ascription to another type, a
  dictionary-dispatched callee, and inline C anywhere in the body answer
  "may", so those keep the old leak. `f64vec-show` (a `String` built from the
  floats it reads) and `f64vec->vec` (a `Vec` likewise) qualify; 152 B in 4
  allocations leaked by the three-line repro before, 0 now.
- `stdlib/numvec.tur`'s module header no longer tells users to bind before
  `P-copy`, `P->vec` or `P-show`.

Pinned by `tests/fixtures/borrowed-drop-temporary-released`: `label` (a
`cstr` result) and `tagv` (a `Vec` built by a recursive helper) release the
temporary; `viewc` (inline C with a `cstr` result) and `viewt` (a body that
hands the parameter to `viewc`) keep it.

Still open: an effectful earlier argument next to a non-scalar earlier
argument. Hoisting the non-scalar one into a temp would make the `let`
an owner of a value the call consumes, which needs the same ownership care as
any other hoisted owner; it is left as a leak.

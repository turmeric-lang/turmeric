---
title: sf-compose-typed fixture prints garbage floats (`2.1e-314`) instead of `8` / `10`
severity: silent miscompile -- compiles cleanly, runs to completion, prints uninitialised bytes
status: FIXED -- bare `>>>` over float-carrier functions now routes to the
  register-class-correct free defn instead of the int64-carrier (->) instance
  method; inner thunk is `double` end-to-end (see Resolution below)
discovered: 2026-06-09
fixed: 2026-06-10
discovered-in: tests/fixtures/sf-compose-typed -- "stdout mismatch" failure in run.sh
---

# `>>>` arrow combinator over typed float SFs prints uninitialised floats

## Summary

`tests/fixtures/sf-compose-typed` composes `add-1` and `scale-2` via
`stdlib/arrow.tur`'s generic typed `>>>` combinator and samples the result
twice.  The expected outputs are `8` and `10`; the actual outputs are
`2.12355e-314` (or similar subnormal garbage) on both samples.

This fixture is in `tests/run.sh`'s sweep -- it shows up as a `stdout
mismatch` FAIL.  The failure predates the session's Defect-A
(`param_poly_types` for `^fat` TY_FN) work; reverting that fix produces the
same garbage output, so this is not a regression of any current change.

## Repro

```sh
./build/tur run tests/fixtures/sf-compose-typed/input.tur
```

Observed:

```
2.12355e-314
2.12355e-314
```

Expected:

```
8
10
```

## Repro source (verbatim from the fixture)

```turmeric
(load "stdlib/arrow.tur")

(defn scale-2 [x : float] : float (* x 2.0))
(defn add-1   [x : float] : float (+ x 1.0))
(defn call-f [^fat f : (fn [:float] #{} : float) x : float] : float (f x))

(defn main [] : int
  (let [h1 (>>> add-1 scale-2)]
    (println (call-f h1 3.0))    ;; (3.0 + 1.0) * 2.0 = 8
    (println (call-f h1 4.0)))   ;; (4.0 + 1.0) * 2.0 = 10
  0)
```

## Suspected cause

`stdlib/arrow.tur`'s `>>>` is declared with a type-variable signature:

```turmeric
(defn >>> [A B C]
  [^fat f : (fn [A] #{} B) ^fat g : (fn [B] #{} C)] : ptr<void>
  (let [fv f gv g] (fn [x : A] : C (gv (fv x)))))
```

When monomorphised to `A=B=C=float`, the inner `(fn [x : A] : C ...)` must
preserve the float carrier through the captured `fv` / `gv` invocations.  The
garbage-float symptom is exactly the signature of an XMM/integer register
mismatch -- the closure returns from `fv` via the int64 slot but `gv` reads
its float argument from XMM0, picking up whatever the caller left there.

Both the Phase-CCL/CCL3 fn-first-class plumbing and the `^fat` poly→fat
shim selection have history of exactly this class of bug; the typed
`>>>` path probably routes through a typed-shim that picks the int64 slot
because the type-variable monomorphisation does not propagate the float
result type all the way down.

This pattern -- single-file `(load ...)` of `stdlib/arrow.tur` plus a typed
generic combinator -- is the canonical "use the high-level arrow library"
shape, so this matters for the SF / signal ecosystem even though the
turmeric-spices signal spice itself sidesteps `>>>` and uses hand-rolled
composition.

## Validation

- Output of the fixture must be `8\n10\n`.
- A diagnostic dump of the lifted thunk's C return type for
  `>>>`-monomorphised-at-float should show `double` end-to-end (not
  `int64_t`).
- `bash tests/run.sh` no longer lists `sf-compose-typed` as a FAIL.

## Resolution (2026-06-10)

The original "garbage float" symptom had already been masked by later
session work: by the time this was revisited, the fixture *printed* `8` /
`10` -- but only **by luck**. The actual code path is not the typed free
`>>>` the fixture comment expects; the bare call `(>>> add-1 scale-2)`
dispatches to the **`Arrow [(->)]` typeclass instance method**
`(>>> [f g] (fn [x] (g (f x))))`, whose element types are erased at the
instance boundary. That instance body is emitted once with an int64
carrier thunk (`tur_thunk_int64_t_int64_t_t`), so it calls the float
fat-shims (`__tur_fatshim_double_double`) through the wrong register class.
It produced the right answer only because the int64-typed thunk emits no
XMM instructions at all, so the `double` in XMM0 threaded through
untouched between the two mismatched calls -- exactly the
"works by luck because the register classes happen to match" miscompile
CLAUDE.md flags as a real bug. The moment a combinator body did float
arithmetic on the carrier, the luck would run out.

### Root cause

The type-erased `(->)` Arrow instance method *cannot* be made
register-class-correct for floats: it is monomorphic over the element
type. The register-class-correct implementation is the typed free defn
`(defn >>> [A B C] ...)`, which specializes per carrier (verified: it
emits `__env_..__spec__double` with `tur_thunk_double_double_t` end to
end). But dispatch never reached it: `elab_user_method_instance_matches`
(src/compiler/elab_call.c) reported the `(->)` instance as a match for any
concrete function receiver (the `itk == TY_FN && rk == TY_FN` arm), so
`prefer_method_dispatch` always picked the int64-carrier instance over the
free defn.

### Fix

In `src/compiler/elab_call.c`, when the receiver is a concrete function
type whose carrier (any argument or the result) is float-class, the
function-arrow `(->)` instance no longer shadows a same-named free defn --
the call stays on the register-class-correct free combinator. Int-carrier
function composition is unchanged and still dispatches through the
instance dictionary (so `tests/fixtures/arrow-instance-stdlib-basic`'s
intent and snapshot are preserved). `<<<` and `first`/`second`, which have
no free defn, are unaffected.

Added helper `fn_type_has_float_carrier`; gated the `TY_FN`/`TY_FN`
match arm on it. After the fix the lifted thunk for the float pipeline is
`double __fn_..__spec__double_void___double(void *, double)` dispatching
`fv`/`gv` via `tur_thunk_double_double_t` -- `double` end to end, as the
validation requires.

## Cross-references

- `[[defmodule-export-scoping-track]]` -- adjacent in spirit (typed `^fat`
  carrier preservation across boundaries) but a distinct root cause.
- `tests/fixtures/poly-to-fat-float-named-fn/`,
  `tests/fixtures/poly-to-fat-float-roundtrip/` -- nearby passing fixtures
  that probably miss whatever monomorphisation path `>>>` hits.
- `docs/reported/fn-first-class-float-carrier-gap.md` -- prior known shape
  of this register-class mismatch class of bug.

# `make_poly_wrapper` forces int64 arg params -- non-int64 named-fn poly arg miscompiles into a `^fat` sink

> **Status:** Fixed (2026-06-04) -- fix direction 1, see Resolution below.

**One-line summary:** A typeclass-method closure supplied as a *named
function* or *non-capturing lambda* (lowered through `make_poly_wrapper`)
and handed to a non-int64 `^fat` sink is silently miscompiled: the
wrapper's argument is forced to `int64_t`, so a `:float` argument is read
from the integer register file instead of `xmm0`.

**Severity:** Silent miscompile (wrong-number bug, no crash). Narrow
trigger today (a concrete non-int64 `^fat` sink reachable from a generic
typeclass method is only expressible with a purpose-built class/instance),
but it is a register-class ABI mismatch with no cover -- exactly the
"works by luck because the register classes happen to match" class of
defect.

## Context

Found while implementing
[docs/upcoming/poly-to-fat-typed-shim-plan.md](../upcoming/poly-to-fat-typed-shim-plan.md).
That plan generalises the `EX_POLY_TO_FAT` slot-0 shim
(`__tur_poly_to_fat1`) to a per-signature typed shim
(`__tur_poly_to_fat1_<R>_<A0>`) so a non-int64 typeclass method handed to
a `^fat` consumer round-trips. The plan's Non-goals state the carrier
`tur_poly_fn_t.fn` "already stores the method's real (typed) function
pointer there via a pointer cast that preserves the address", and Risk #3
flags exactly the failure below: "Phase 1 must verify the producer does
not insert an int64 ABI thunk between the method and the carrier; if it
does, that thunk is the erasure point and must be typed instead."

The typed slot-0 shim is correct **when the producer stores the real
typed thunk**. That holds for the *capturing-closure* pass-through
(`EX_POLY_WRAP` with `is_closure`, `emit_expr.c:3379-3388`), which casts
the closure's real `double (*)(void *, double)` thunk to the int64 carrier
field address-preservingly. It does **not** hold for the *named-function*
(or non-capturing lambda) path, which routes through `make_poly_wrapper`.

## Minimal repro

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")

(defn call-ff [^fat f : (fn [:float] #{} : float) x : float] : float
  (f x))

(defstruct BoxW [A] (raw :int))

(definstance Functor [BoxW]
  (fmap [container fn]
    (call-ff fn 3.5)))

(defn scale2 [x : float] : float (* x 2.0))   ; named fn (or any non-capturing lambda)

(defn main [] : int
  (let [b (:: (make-struct BoxW (schema/int)) (BoxW int))]
    (println (.fmap b scale2)))               ; expect 7  (3.5 * 2)
  0)
```

- **Observed:** prints `48` (garbage; `3.5` never reaches `scale2`).
- **Expected:** prints `7`.

Swapping `scale2` for a *capturing* closure
(`(let [k 0.0] (fn [x : float] : float (+ (* x 2.0) k)))`) prints `7`
correctly -- that path stores the real typed thunk, and the typed slot-0
shim recovers the ABI. This is the path exercised by the passing fixture
`tests/fixtures/poly-to-fat-float-roundtrip/`.

## Root cause

`make_poly_wrapper` (`src/compiler/elab_call.c:2648`) builds the bridging
thunk with **all argument params hard-typed to `int64_t`**, while
preserving only the result kind:

- `src/compiler/elab_call.c:2670` -- `binding_new(e, apsym, TYPE_INT, ...)`
- `src/compiler/elab_call.c:2672` -- `wparam_types[i + 1] = TYPE_INT;`
- `src/compiler/elab_call.c:2706` -- `warg_kinds[i + 1] = TY_INT;`
- `src/compiler/elab_call.c:2707` -- `type_fn(warg_kinds, w_arity, inner_result_kind)`

So for `scale2 : (:float) -> :float` the emitted wrapper has C signature
`double __poly_N(void *, int64_t)` (verify with `tur emit-c` on the repro:
`static double __poly_1062(void * , int64_t );`). `EX_POLY_WRAP` then casts
this wrapper to the int64 carrier `int64_t (*)(void *, int64_t)`
(`emit_expr.c:3403-3405`) and stores it in `tur_poly_fn_t.fn`. The typed
slot-0 shim added by the plan re-casts slot 1 to the sink's declared
`double (*)(void *, double)` and calls it with the `:float` argument in
`xmm0` -- but the underlying wrapper reads its argument as an `int64_t`
from the integer register. Register-class mismatch -> the argument is
garbage. (The result kind is preserved, so the *return* register is
correct; only arguments are corrupted.)

This is a second erasure point, independent of slot 0: the carrier holds
the wrapper, not the real method. The capturing-closure path has no such
intermediary, which is why it round-trips.

## Why it has not fired before

`make_poly_wrapper`'s int64 args are correct for every int64/pointer
signature (the only poly-fn arg types reachable through current stdlib
typeclasses -- Functor/Applicative/Monad/schema combinators). A concrete
non-int64 `^fat` sink reachable from a generic typeclass method has to be
hand-built (as in the repro), so the int64-arg assumption has never been
violated in the suite.

## Proposed fix directions

1. **Type the wrapper's arg params by the inner fn's real arg kinds.**
   Replace the `TYPE_INT` forcing at `elab_call.c:2670/2672/2706` with the
   inner binding's `arg_kinds[i]` (mirroring how `inner_result_kind`
   already threads the real result). This makes the carrier hold a thunk
   whose ABI matches the sink's typed-thunk cast for *both* the named-fn
   and capturing-closure paths.

   - **Risk:** the same `tur_poly_fn_t` carrier is *also* invoked through
     the int64 carrier ABI (`TUR_APPLY1` / `int64_t (*)(void *, int64_t)`)
     in generic contexts. A wrapper with non-int64 args would mismatch
     *those* call sites. A single wrapper cannot satisfy both the int64
     carrier invoke and a typed-thunk invoke; this is the same
     dual-ABI tension that the capturing-closure path sidesteps by storing
     the real thunk and relying on each call site to re-cast. Any fix here
     must confirm every invoke of the wrapped carrier agrees on the ABI
     (likely: emit the wrapper with the real types *and* ensure the int64
     carrier callers re-cast, symmetric to the slot-0 shim).

2. **Defuse with a hard diagnostic (cheap, immediately safe).** Where
   `make_poly_wrapper` is asked to wrap an `inner_b` with any non-int64
   register-class argument kind, emit a `TUR-E` error
   ("rank-N/typeclass poly wrapping of a function with a non-int64
   argument is not yet supported; pass a capturing closure instead")
   rather than silently emitting the int64-arg wrapper. This converts the
   miscompile into a compile error and points at the working alternative
   (the capturing-closure path). Pairs naturally with direction 1 as the
   eventual real fix.

## How to validate a fix

- The repro above prints `7` (not `48`) for the *named-fn* form.
- `tests/fixtures/poly-to-fat-float-roundtrip/` (capturing-closure form)
  still prints `7` and still selects `__tur_poly_to_fat1_double_double`
  at slot 0.
- `bash tests/run.sh` clean; int64/pointer poly boxes churn-free (still
  emit `__tur_poly_to_fat1` and int64-arg `__poly_N` wrappers).
- For direction 2: a negative fixture under `tests/fixtures/errors/`
  asserting the diagnostic fires on the named-fn non-int64 form.

## Resolution (fix direction 1, scoped to the float register class)

Implemented in `make_poly_wrapper` (`src/compiler/elab_call.c`). The
wrapper's argument params are retyped to the inner function's real kind
**only for float-class kinds** (`TY_FLOAT`/`TY_FLOAT32`/`TY_FLOAT64`) of a
*plain* named inner fn; every int64-register-class kind (int/ptr/cstr/bool)
keeps the int64 carrier, so existing fixtures stay churn-free.

This scoping sidesteps the dual-ABI risk noted under direction 1: the int64
carrier invoke (`TUR_APPLY1`) only ever round-trips int64-register-class
payloads, and a float payload *already* could not survive that path, so
retyping float args cannot regress a previously-correct int64 invoke -- it
only makes the carrier's stored thunk agree with the typed slot-0 shim that
a `:float` `^fat` sink applies. A closure inner fn never reaches this
wrapper (it lowers through the `is_closure` tur_poly_fn_t pass-through,
which already stores its real typed thunk), so the change is limited to the
named-fn / non-capturing-lambda path that was broken.

Result: for `scale2 : (:float) -> :float` the wrapper is now
`double __poly_N(void *, double)`; the repro prints the correct value.

**Regression coverage:** `tests/fixtures/poly-to-fat-float-named-fn/`
exercises both a named top-level fn and a non-capturing lambda handed to a
`:float` `^fat` sink through a typeclass method (prints `7` and `7`).
`tests/fixtures/poly-to-fat-float-roundtrip/` (capturing-closure form) is
unchanged. `bash tests/run.sh` is clean (1350 passed, 0 failed);
int64/pointer poly boxes remain churn-free (int64-arg `__poly_N` wrappers,
`__tur_poly_to_fat1`).

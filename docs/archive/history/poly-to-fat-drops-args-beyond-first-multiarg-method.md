# Multi-arg typeclass-method closure into a `^fat` sink silently drops every argument after the first

> **Status:** Fixed (Fix direction 2 landed) -- the poly-to-fat carrier and
> shim family are now N-ary. A binary (or higher-arity) typeclass-method
> closure boxed into a matching `^fat` sink forwards every argument: the box's
> slot-0 shim is `__tur_poly_to_fat<N>` (int64 carrier) or
> `__tur_poly_to_fat<N>_<R>_<Ai...>` (typed), and slot 1 holds the method's
> real N-ary thunk from `make_poly_wrapper`. The earlier Fix-direction-1 hard
> diagnostic at the box site has been removed -- the case now compiles and
> runs correctly instead of erroring. Validated by
> `tests/fixtures/poly-to-fat-multiarg-roundtrip` (prints `7`).

**One-line summary:** When a typeclass method whose closure parameter is
**binary or higher arity** is handed to a `^fat` sink, `EX_POLY_TO_FAT`
boxes it through the unary `tur_poly_fn_t` protocol regardless of arity. The
sink's N-ary fat-call then invokes the unary slot-0 shim
(`__tur_poly_to_fat1` / `__tur_poly_to_fat1_<R>_<A0>`), which forwards only
`a0`; every argument after the first is silently dropped.

**Severity:** Silent miscompile (wrong result, no crash, no diagnostic). A
binary method call computes against a dropped/garbage second argument.

## Context

Surfaced while auditing the type-passing gaps after the
[poly-to-fat typed-shim work](../upcoming/poly-to-fat-typed-shim-plan.md)
and its follow-ups (`make_poly_wrapper` float args; the bare-`^fat` slot-0
mismatch). Those all addressed the *register class* of a **unary** poly
closure. This is the orthogonal *arity* axis: the carrier itself is unary,
so nothing above unary round-trips.

The plan's Non-goals already call this out:

> Arities beyond unary. `tur_poly_fn_t` is inherently unary
> (`(env, arg) -> result`); a single `(R, A0)` shim family suffices.

That non-goal is sound for what the plan delivered, but the boxing path does
not *enforce* it -- a higher-arity method silently takes the unary path
instead of being rejected, so the non-goal is a latent miscompile rather
than a clean limitation.

## Minimal repro

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")

;; binary ^fat sink
(defn call2 [^fat f : (fn [:int :int] #{} : int) a : int b : int] : int
  (f a b))

(defstruct BoxW [A] (raw :int))

;; instance method whose closure argument is binary
(definstance Functor [BoxW]
  (fmap [container fn]
    (call2 fn 3 4)))

(defn add2 [x : int y : int] : int (+ x y))

(defn main [] : int
  (let [b (:: (make-struct BoxW (schema/int)) (BoxW int))]
    (println (.fmap b add2)))    ; add2(3, 4) -> expected 7
  0)
```

- **Observed:** prints `3` (= `add2(3, 0)` -- the `4` is dropped).
- **Expected:** prints `7`, or a hard compile error stating that a
  higher-arity poly-method closure cannot be boxed into a `^fat` sink.

## Codegen evidence

The wrapper for the named binary fn is correctly **binary**:

```c
static int64_t __poly_1064(void *__poly_env, int64_t __poly_x0, int64_t __poly_x1) { ... }
```

but the fat box's slot 0 is the **unary** carrier shim:

```c
static int64_t __tur_poly_to_fat1(void *__e, int64_t a0) {
    int64_t *__b = (int64_t *)__e;
    return ((int64_t (*)(void *, int64_t))(intptr_t)__b[1])((void *)(intptr_t)__b[2], a0);
}
...
__t24[0] = (int64_t)(intptr_t)__tur_poly_to_fat1;     /* slot 0: unary */
```

The sink fat-calls slot 0 as a binary thunk (`int64_t (*)(void *, int64_t,
int64_t)`), but the actual function there reads only `a0` and forwards a
single argument to slot 1's binary `__poly_1064` -- so `__poly_x1` is never
written. (For a `:float` second argument the failure is identical;
generalising the slot-0 shim to `(R, A0)` did not add arity.)

## Root cause

`tur_poly_fn_t` is unary by definition (`src/compiler/emit_module.c`):

```c
typedef struct { void *env; int64_t (*fn)(void *, int64_t); } tur_poly_fn_t;
```

and both the preamble shim and the typed shim family are unary:

- `__tur_poly_to_fat1(void *__e, int64_t a0)` -- `emit_module.c`
- `__tur_poly_to_fat1_<R>_<A0>(void *__e, A0 a0)` -- `ensure_typed_poly_to_fat`,
  keyed on a single `(R, A0)` pair.

`EX_POLY_TO_FAT` (created in `src/compiler/elab_call.c`, the
`arg_is_poly_fn` branch of the `^fat` argument handling) boxes the
`tur_poly_fn_t` into `{ shim, fn, env }` and always selects a *unary* slot-0
shim -- there is no arity check against the poly method (or against the
sink's declared `^fat` fn signature, which already records the real arity in
`fn_type.arg_full_types[idx]`). The producer-side wrapper
(`make_poly_wrapper`) does build a correctly N-ary thunk, so the arity is
known on the producer side; it is only the unary carrier/shim that erases
it.

## Why it has not fired before

Every typeclass-method closure that reaches this path in practice is unary:
`Functor.fmap`, `Applicative`/`Monad` map/bind, schema combinators -- all
take a single-argument callable. A binary poly method handed to a `^fat`
sink has to be hand-built (as in the repro), so the unary assumption has
never been violated in the suite.

## Proposed fix directions

1. **Defuse with a hard diagnostic (cheap, immediately safe).** At the
   `EX_POLY_TO_FAT` box site (`elab_call.c`), check the boxed closure's
   arity -- available from the sink's `^fat` fn signature
   (`fn_type.arg_full_types[idx]`, a `TY_FN` whose `arity` is known for both
   the annotated and the synthesized-bare forms). When it is `> 1`, emit a
   `TUR-E` ("a typeclass-method closure with arity N > 1 cannot be boxed
   into a `^fat` consumer; tur_poly_fn_t is unary") instead of silently
   emitting the unary box. Converts the silent wrong-answer into a clear
   compile error.

2. **Real fix: N-ary poly carrier + shim family.** Generalise
   `tur_poly_fn_t` and the poly-to-fat shim to arity N:
   - a per-arity carrier (or a single carrier whose `.fn` is cast per call
     site to the real `R (*)(void *, A0..An)`), and
   - an `ensure_typed_poly_to_fat` keyed on `(R, A0..An)` that emits
     `__tur_poly_to_fat<N>_<R>_<Ai...>` forwarding all N arguments to
     slot 1.

   The producer (`make_poly_wrapper`) already emits an N-ary thunk, so the
   main work is the carrier/shim and the box-site selection. This mirrors
   the existing `__tur_fatshim<arity>` family (which is already N-ary for
   `EX_FN_TO_FAT`), so the shape is known. Larger than the unary plan, and
   explicitly its non-goal -- track separately.

## How to validate a fix

- The repro prints `7` (real fix) **or** fails to compile with a clear
  arity diagnostic (defuse).
- For the defuse: a negative fixture under `tests/fixtures/errors/` whose
  `expected.diag` matches the arity error.
- For the real fix: a binary round-trip fixture (e.g. the repro) prints `7`,
  and `grep` confirms slot 0 holds `__tur_poly_to_fat2_*`; the unary
  fixtures (`poly-to-fat-float-roundtrip`, `-named-fn`, `-bare-fat-sink`)
  stay green and churn-free.
- `bash tests/run.sh` clean either way.

---
title: let-bound SF (signal-function) loses outer-arg type when inner closure captures and calls it
category: Reported
severity: medium
description: When a `defn` returns a two-level closure `(fn [^fat sig : (fn [float] float)] (fn [t :float] :float ... (sig t) ...))` and the *inner* closure references the outer's captured `^fat` parameter, a let-binding of the result is elaborated with the wrong argument type. Specifically, the elaborator types the outer closure's first parameter as `:float` (the inner `t`'s type) instead of `(fn [float] float)` (the actual declared type). The same source compiles correctly if the inner closure does not reference the captured parameter.
---

# Let-bound SF loses outer-arg type when inner closure captures it

> **Resolution (type-check half):** Fixed in `src/compiler/elab_forms.c`.
> A `let` binding whose init is a *call* to a function that returns a thin
> (non-boxed) closure-returning fn now routes through
> `returns_closure_fn_binding` instead of `closure_fn_binding`, so the binding
> keeps its declared outer signature while a chained call still sees its result
> as a closure.  The discriminator is the new `Binding.returns_boxed_closure`
> flag (set at defn/lambda elaboration from `body->type.as.fn.boxed`), which
> tells a genuine fat-closure-returning call like `(adder 10)` apart from a
> thin closure-returning fn like `(make-sf)`.  Guarded by
> `tests/check-sf-let-bind-inner-call.sh` (ctest `tur_sf_let_bind_inner_call`).
> The native-codegen half of this shape is still broken and is tracked
> separately in
> [sf-two-level-closure-return-miscompiles-out-binding](sf-two-level-closure-return-miscompiles-out-binding.md);
> that is why the guard stops at `tur check` rather than build+run.

## Summary

A `defn` whose return value is a Signal-Function (a two-level closure
`(fn [sig] (fn [t] ...))`) elaborates correctly when called directly,
but loses its outer-argument type when the result is bound by a `let`
**and** the inner closure body references the outer `^fat`-typed
parameter.

Distilled while migrating the `tur-signal` spice through Phase 0c (the
`:float` sample ABI in
`docs/upcoming/still-in-flight-plan.md` § "tur-signal spice broken
build"). The composable SF style used by
`voice`/`voice-sf`/`poly-synth`/`ks-voice`/`effects-chain`/
`step-sequencer` -- "build an SF, let-bind it, pipe a signal into it,
then sample the result" -- trips this consistently. Those defns now
sit behind placeholder stubs in
`../turmeric-spices/spices/signal/src/signal/synth.tur` waiting on this
fix.

Severity: **medium.** Compile-time hard error -- no miscompile risk --
but it blocks the idiomatic SF-pipeline style. Workarounds exist
(inline the call sites, pass SFs as `defn` params rather than `let`
bindings) but produce significantly uglier code.

## Observed vs. expected

### Expected

`make-sf-with-call`'s return type is `(fn [(fn [float] float)] (fn [float] float))`. After `(let [sf (make-sf-with-call)] ...)`, the symbol
`sf` should have that type, and `(sf input)` where `input` is `^fat
input :(fn [float] float)` should typecheck.

### Observed

```
$ ./build/tur check /tmp/sf_min_b.tur
/tmp/sf_min_b.tur:9:17: error [TUR-E0001]: function 'sf' arg 1: expected float, got (fn [float] : float)
```

`sf` is elaborated as `(fn [float] ...)` -- the outer arg type has been
replaced by the *inner* closure's argument type (`t :float`).

## Minimal reproducer

Two files, identical except for whether the inner closure calls the
captured outer parameter.

**A -- inner closure does NOT call `sig`. Passes.**

```turmeric
(defn make-sf-no-call []
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float 0.0)))

(defn drive-a [^fat input : (fn [float] float)] : float
  (let [sf  (make-sf-no-call)
        out (sf input)]
    (out 0.0)))
```

**B -- inner closure calls `(sig t)`. Fails.**

```turmeric
(defn make-sf-with-call []
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float (sig t))))

(defn drive-b [^fat input : (fn [float] float)] : float
  (let [sf  (make-sf-with-call)
        out (sf input)]
    (out 0.0)))
```

```
$ ./build/tur check /tmp/sf_min_a.tur   # clean
$ ./build/tur check /tmp/sf_min_b.tur
/tmp/sf_min_b.tur:9:17: error [TUR-E0001]: function 'sf' arg 1: expected float, got (fn [float] : float)
```

Calling `make-sf-with-call` *directly without a let* also works -- the
mis-inference only fires when the result is captured in a binding.

## Root cause analysis (best guess)

The previous fix in this area is documented in
`docs/archive/signal-spice-blocking-issues-plan.md` § "Issue 1b: type
info loss." That patch added `result_full_type` propagation in
`src/compiler/elab_fns.c` and `src/compiler/elab_call.c`, mirroring
the `TY_SESSION` path -- when a defn returns a `TY_FN`, a heap-allocated
copy of the inner `Type` is stored in `fn_type.as.fn.result_full_type`
and re-read at the call site.

Issue 1b's fix handles **one** level of `TY_FN` nesting -- the defn's
return type. A SF is **two** levels: the *outer* closure returned by
the defn has its own `arg_types`/`result_kind`, and when the inner
closure captures and calls the outer's `^fat` parameter, something in
the captured-environment typing apparently substitutes the *inner*'s
argument-type signature into the *outer*'s arg-type slot.

Hypothesis: the elaborator inspects the inner closure's body to figure
out the captured fn's signature (so it can build the right thunk), and
then writes that inferred signature back into the outer closure's
parameter type rather than keeping the declared `:(fn [float] float)`
annotation. The substitution is harmless when the inner body doesn't
actually call the captured fn (A passes), but the moment `(sig t)`
appears, the inferred-from-`t` type overrides the declared annotation.

Files to inspect:

- `src/compiler/elab_fns.c` -- where `result_full_type` is materialised
  for the outer fn.
- `src/compiler/elab_call.c` near the SS3a-analogue path -- whether the
  `result_full_type` it re-reads is the *as-declared* type or the
  *post-capture* inferred one.
- The captured-environment / `^fat` lowering for nested closures.

## Why the language-readiness matrix didn't catch this

`docs/archive/history/language-readiness-for-typed-signal-plan.md`
(verdict 2026-06-05) marks all of G1-G8 green for the typed-signal
rebuild, but its spikes call SFs through *monomorphic per-result-type
combinators* (`compose-float`, `constant-float`) rather than the
idiomatic let-bind-and-pipe pattern. The blocker only surfaces when
you let-bind an intermediate SF and call it with another typed `^fat`
argument -- which is exactly the style of the `voice`/`voice-sf`/
`poly-synth` family.

## Proposed fix direction

1. Reproduce locally:
   ```sh
   ./build/tur check /tmp/sf_min_a.tur   # should pass
   ./build/tur check /tmp/sf_min_b.tur   # should fail today
   ```
2. Trace `make-sf-with-call`'s elaboration. After the inner `(fn [t :float] :float (sig t))` is elaborated, inspect what the *outer* closure's
   `arg_types[0]` is recorded as: I expect to find `TY_FLOAT` where
   the declared `(fn [float] float)` should be.
3. The fix likely sits in the same neighborhood as Issue 1b's patch:
   keep the declared parameter type intact when materialising the
   outer closure's signature, even if the inner body has refined it.

## Validation of a fix

- `./build/tur check /tmp/sf_min_b.tur` passes.
- A new fixture under
  `tests/fixtures/sf-let-bind-with-inner-call/` covers exactly this
  shape and is added to `tests/run.sh`.
- The synth-side stubs in
  `../turmeric-spices/spices/signal/src/signal/synth.tur` (search for
  "Phase 0c note") can be restored to their full SF-composition bodies
  and the spice's runtime tests (once the codegen bug in
  [stdlib-poly-codegen-undeclared-identifier](stdlib-poly-codegen-undeclared-identifier.md)
  is unblocked) cover `voice` / `voice-sf` / `poly-synth`.

## Related

- `docs/archive/signal-spice-blocking-issues-plan.md` -- the closed
  plan that introduced the partial fix this report extends.
- `docs/archive/history/language-readiness-for-typed-signal-plan.md`
  -- the readiness matrix that doesn't track this specific shape.
- `docs/reported/stdlib-poly-codegen-undeclared-identifier.md` --
  blocks the runtime validation that would otherwise exercise the
  restored SF-composition path.

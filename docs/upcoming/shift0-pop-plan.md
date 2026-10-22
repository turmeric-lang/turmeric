# Plan: `shift0` removes the reset it captured to

> **Status:** PROPOSED 2026-10-21 (from continuation-forms-gaps item 1; the
> request was to implement it unless it is a large amount of work). Feasibility
> investigated the same day: **small** on its own -- two local changes, below --
> but the observable cases need
> [compiled-shift-delimited-by-root](../reported/compiled-shift-delimited-by-root.md)
> fixed first. Not started.

## The semantics

Turmeric's `shift` and `shift0` are abortive and take a value receiver: `(shift0
f x)` discards the context up to the nearest `reset`, and that `reset` yields `(f
x)`. The difference the guides describe (delimited-control-operators-guide,
effects-system-guide) is in *where `f` runs*: `shift` runs it inside the reset
(re-installed), `shift0` runs it with that reset removed, so a `shift` inside `f`
is delimited by the next reset out.

```turmeric
(defn main [] : int
  (println (reset (+ 100 (reset (+ 10 (shift0 (fn [v : int] (shift (fn [w : int] (* 1000 w)) v)) 5))))))
  0)
```

Wanted 5000: `f` runs with the inner reset gone, its `shift` captures `(+ 100 _)`
up to the outer reset and replaces it with `(* 1000 5)`. Both engines print 5100
today, as `shift` does. A `shift0` whose receiver does no control of its own is
the same as `shift`, which is every fixture using it.

## Where the receiver runs today

- **Interpreter** (`eval_abortive_shift`, `src/turi/eval.c`): evaluates `f(x)`
  with every reset boundary still on `g_reset_stack`, then aborts to
  `reset_find(PROMPT_PLAIN)`. `shift0` and `shift` share the function.
- **Direct emitter** (a function evicted from the CPS backend, where a `reset` is
  a setjmp landing on `tur_cur_shift_reset`): the receiver is called while the
  inner landing is current, then `emit_shift_escape_abort` longjmps to it with the
  result (`emit_effects_shift0`, `emit_effects.c`).
- **DK machine** (`dk_run_impl`, `case DKK_SHIFT0`): runs the receiver body
  nested with the captured slice ending in `done` instead of a re-installed
  prompt. But a shift body is admitted only when it is straight-line
  (`shift_body_ok`, `emit_cps_ir.c`), so a receiver that does its own control
  evicts the function to the direct emitter. In the DK machine the two are
  observably the same, and nothing there needs to change for this plan.

## Changes

1. **Interpreter.** In `eval_abortive_shift` for `shift0`: find the target
   boundary first (`b = reset_find(PROMPT_PLAIN)`, error if none, as today), mark
   it hidden (a `popped` flag that `reset_find` skips) while `f(x)` runs, clear the
   flag, then abort to `b` specifically (`env->abort_target = b` rather than the
   nearest of its kind -- `f` may have installed or aborted through others). Undo
   the flag on every exit from `f` (error, panic, abort past `b`).
2. **Direct emitter.** In `emit_effects_shift0`, around the receiver call:
   `__saved = tur_cur_shift_reset; if (__saved) tur_cur_shift_reset = __saved->prev;`
   ... call ... `tur_cur_shift_reset = __saved;` then the existing abort to
   `__saved`. A `shift` inside `f` that is also direct-emitted then longjmps to the
   outer landing.
3. **Mixed case.** In the example above the receiver lambda is CPS-colored, so
   its `shift` runs on the DK machine and is delimited at the lambda's entry, not
   at either reset -- the root-delimiting defect. With that report's fix (a
   reset-tag DK shift that reaches the root lands on the live direct landing),
   change 2 makes the example right compiled. Do this plan after it.
4. **Resumable `shift0`.** Out of scope: `shift0` takes only a value receiver
   (TUR-E0001 for a `cont` receiver), and the language's base `shift` is abortive.

## Fixtures

The example above (5000) and its `shift` twin (5100) on all three engines; a
`shift0` whose receiver does nothing (unchanged); a `shift0` with no outer reset
whose receiver shifts (the interpreter's `shift used outside of any reset
boundary`, and the compiled error once the root-delimiting report is fixed).
Update the "Status" note in delimited-control-operators-guide and the `shift0`
entry in `stdlib/forms/continuations.tur`.

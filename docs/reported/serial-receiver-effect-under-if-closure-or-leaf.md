# An escaping effect in a serial-shift context is refused under an `if`, in a capturing receiver, or in a leaf

**Narrowed 2026-10-03: shapes 1 and 2 are fixed; shape 3 (a leaf) stays
open, and is a semantics question rather than a lowering gap.**  See "Fixed
2026-10-03" at the end.

**Severity: low.** A compile-time refusal (`TUR-E0706`), not a wrong answer;
`tur --interpret` runs all three.  The residue of
[serial-receiver-effect-cannot-reach-enclosing-handler](../archive/serial-receiver-effect-cannot-reach-enclosing-handler.md),
which made a NAMED receiver's escaping effect reach the handlers around the
reset by calling the receiver as a colored callee on the reset's own
continuation (`recv_outward`).

## The three shapes

```turmeric
(load "stdlib/serial.tur")
(defeffect Ask [] :int)
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn asks [] : int (+ (perform (Ask)) 1))
(defn recv-e [k : serial-cont] : int (k (asks)))

;; 1. an `if` branch point in the context
(serial-reset (if c (page "" (serial-shift recv-e 0)) 5))

;; 2. a CAPTURING closure receiver
(serial-reset (page "" (serial-shift (fn [k : serial-cont] : int
                                        (k (+ m (asks)))) 0)))

;; 3. a context callee (leaf) the effect escapes
(defn page2 [env : cstr hole : int] : int (+ hole (perform (Ask))))
(serial-reset (page2 "" (serial-shift recv 0)))
```

Shapes 1 and 2 compile now (`serial-shift-receiver-effect-under-if`,
`serial-shift-closure-receiver-effect`); shape 3 is pinned by
`tests/fixtures/errors/serial-shift-context-callee-effect`.

## Why each was refused

1. **`if`.** The outward lowering replaces the shift with a tail call whose
   continuation is the rest of the function.  With an `if` in the context the
   pure arm yields its value straight into the same rest, so the rest would
   have to be lifted once and reached from both arms -- the native serial
   lowering emits the pure arm inline instead.  `build_cloneable` refuses
   `recv_outward && saw_if` (cps_ir.c).
2. **Capturing closure.** The outward call names the receiver's `__cps` twin;
   a capturing lambda's twin takes its env first and is registered only for
   lambdas in the threadable set (`fn_sig_ok`).  Threading the closure value
   to that entry is the E2a fat-dispatch machinery, not wired here.  A
   non-capturing `fn` literal is lifted to a named global and works.
3. **Leaf.** A context callee runs when the continuation is RESUMED, not when
   it is captured -- possibly in another process, from bytes -- so its effect
   would need the resumer's handlers.  That is a question about what a
   marshalled continuation's effects mean, not a lowering gap.

## Fix directions

- (1): lift the rest as a resume frame before the branch point and deliver the
  pure arm into it (`dk_run(rest, pure)`), so both arms share one
  continuation.
- (2): call the closure through its env-taking `__cps` entry, the way the E2a
  heap join threads a fat fn value.
- (3): decide the semantics first -- probably "the resumer's handlers", which
  means `resume-cont!` would need to run the chain on the caller's `__kont`.

## Fixed 2026-10-03: shapes 1 and 2

- **(1) `if`.**  The emitter, not the IR, was the obstacle:
  `emit_serial_outward_call` already lifts the rest as a resume frame for the
  receiver's call, so with an `if` in the context the PURE arm now delivers
  its value -- the outer frames re-applied by `emit_cloneable_pure_arm`, as
  the native lowering computes it -- into that same frame
  (`dk_run(dk_frame_resume(rest, env, cur_k), pure)`).  Both arms share one
  continuation and the rest is emitted once; `build_cloneable` no longer
  refuses `recv_outward && saw_if`.  An outer frame, a shift in the `else`
  arm, a prelude `let` the condition reads, and an effect performed later in
  the rest all agree with `tur --interpret`
  (`tests/fixtures/serial-shift-receiver-effect-under-if`).
- **(2) Capturing closure.**  A closure literal used as a serial-shift
  receiver an effect escapes (`serial_closure_recv_escapes`, the same test as
  `fn_effect_may_escape`) is a threading use of its lifted lambda -- tier
  `now`, since the outward call is a tail call on the reset's continuation --
  so the lambda joins the threadable set and gets its env-taking `__cps`
  twin.  The builder marks the receiver outward; the emitter evaluates the
  closure at the reset site and calls `<lambda>__cps(<closure>, k, <rest>)`,
  the same first argument the direct thunk gets; and Rule D
  (`outward_receivers_in_s`) evicts the function if that twin is not
  emitted after all -- a capture `fn_sig_ok` does not admit -- so the
  fallback's TUR-E0706 still names it then.  Int and cstr captures, a
  receiver that never resumes, and the `if` shape together all agree with
  `--interpret` (`tests/fixtures/serial-shift-closure-receiver-effect`).

Both new fixtures run through `tur jit` with no `TUR-W0070` fallback.  Not
changed and not this report's: every serial continuation's DK chain is
leaked when the receiver resumes it, outward or not (`run-leak-check`-style
builds of `serial-shift-colored-receiver` and
`cps-oracle-serial-closure-recv` leak 52 and 25 allocations on `main`) --
filed as [serial-cont-chain-never-freed](../archive/serial-cont-chain-never-freed.md) (since resolved).

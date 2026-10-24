# The effect forms: `discontinue` runs on neither engine

**ARCHIVED 2026-10-21** -- every open item has a plan; see the disposition below.

## Disposition (2026-10-21)

- Item 1 (`discontinue` runs on neither engine): decision -- keep it and
  implement it on both engines. Plan: [discontinue-plan](../upcoming/discontinue-plan.md).
- Items 2-3 and the composed-handler note: decided and planned in
  [effect-continuation-followups-plan](effect-continuation-followups-plan.md).
- Everything else: resolved, see
  [effect-handler-forms-top-level-resume-compose](effect-handler-forms-top-level-resume-compose.md).

**Severity: medium.** Found 2026-10-19 checking the examples for the effect forms'
API reference (`stdlib/forms/effects.tur`).

**Narrowed 2026-10-21.** Top-level `try-with` / `handle-shallow` / `with-handler`
statements, `resume` value checking, `compose-handlers` over handler literals,
`cont?` after a one-shot resume, the interpreter's `discontinue` stack smash and
`(handle 5)` are resolved: see
[effect-handler-forms-top-level-resume-compose](effect-handler-forms-top-level-resume-compose.md).

## 1. `discontinue` cannot be used (decided 2026-10-21: keep and implement)

```turmeric
(defeffect Ask [] : int)
(defn main [] : int
  (handle (+ 1 (perform (Ask))) (Ask [] k) (do (discontinue k "boom") 0))
  0)
```

Compiled: a located error naming `discontinue` (plus the generic `no lowering
here` on the `perform`); `TUR_TRACE_EVICT=1` shows `BODY-UNSUPPORTED
EX_DISCONTINUE` (`src/passes/cps_ir.c`). Interpreted: `discontinue is not
supported`, exit status 1 (it used to be `stack smashing detected`). Neither
engine can raise at the `perform` and run the body's `defer`s.

`discontinue` is typed `nil`, so `(handle (f) (Ask [] k) (discontinue k "boom"))`
in an `int` position is TUR-E0252: there is no way to write a use that
type-checks and also means "raise at the perform". The ownership rules
(`^linear` "resume or discontinue", TUR-E0101/E0201) are still checked, which is
all `errors/effect-cont-linear-drop` and its siblings use it for.

Fix: lower `discontinue` in the CPS backend (raise at the `perform` after the
body's defers) and in the interpreter's work-stack driver, give it a result type
that fits a clause, or retire the form and its mention in the `^linear` rule.

### What `discontinue` would add over not resuming (2026-10-21)

The usual reason for `discontinue` is cleanup: OCaml's `discontinue k exn` raises
`exn` at the `perform`, so the body's `finally`/cleanup runs. Turmeric already
runs the body's cleanup when a clause simply does not resume:

```turmeric
(defeffect Ask [] : int)
(defn body [] : int
  (defer (println "body cleanup"))
  (+ 1 (perform (Ask))))
(defn main [] : int
  (println (handle (body) (Ask [] k) (do (println "clause gives up") -1)))
  0)
```

Compiled and `tur jit`: `clause gives up`, `body cleanup`, `-1`. Interpreted:
`clause gives up`, `-1`, `body cleanup` -- the same cleanup, run late
([turi-fiber-handle-defers-fire-at-the-wrong-time](turi-fiber-handle-defers-fire-at-the-wrong-time.md),
which the defer-semantics plan should also close). So what `discontinue` adds is
narrower:

1. **The body can catch the failure and carry on.** With `(catch-unwind ...)` or
   `catch-panic-of` around the `perform`, the body recovers and its value becomes
   the handle's. Abandoning gives the body no say.
2. **The clause runs after the body has unwound,** and gets control back with the
   body's result (or the failure re-raised at the `discontinue`), the way `resume`
   returns what the rest of the body produced.

What implementing it takes:

- **Type.** `discontinue` returns what `resume` returns (the handle's answer type),
  not `nil`, so a clause can end in it.
- **Compiled.** Resume the continuation with a "raise" instead of a value: set a
  panic (`tur_panic_with`, the message as payload) and `dk_invoke` the chain, with a
  check at the continuation's entry that turns the pending raise into a panic at
  the `perform` site. The CPS frames already propagate `tur_panicking`
  (`cps_panic_check`), run pending defers on the way (`__dk_panic_abandon`), and a
  `catch-unwind` in the body catches it. The new part is the entry check on the
  resumed frame and the multishot/copy paths. Medium.
- **Interpreted.** In the work-stack driver, a resume that pushes the captured slice
  back with a pending panic, so the panic is raised in the body's frames
  (`turi_runtime_panic`) and unwinds through its defers and boundaries. Medium.
- **`^linear`.** Keeps "resume or discontinue exactly once".

Retiring it instead: remove the form, say "resume exactly once" for `^linear`,
and reword the ownership diagnostics that mention it ("has already been resumed or
discontinued", "was not resumed or discontinued"), with the `expected.diag` of the
fixtures that match on that text. The programs that call `discontinue` are the
two added 2026-10-21 for its failure (`errors/discontinue-no-lowering`,
`discontinue-interp-unsupported`), which would go with it.

Recommendation: retire it unless (1) -- a body that recovers from a handler's
refusal -- is a pattern you want; returning a `Result` from the clause, or not
resuming, covers the rest.

## Items 2-3 and the composed-handler note: moved to a plan

Decided 2026-10-21 and planned in
[effect-continuation-followups-plan](effect-continuation-followups-plan.md):
`cont?` of a `^multishot` k is true after a resume; the compiled unhandled-effect
error names the effect (the perform site passes its own name, since a separately
compiled build never lets a perform reach another unit's handler); a composed
handler held in a variable stays a flat table and is documented.

## Also open

- The interpreter's `handle` runs the body's pending `defer`s at the wrong time
  when a clause does not resume:
  [turi-fiber-handle-defers-fire-at-the-wrong-time](turi-fiber-handle-defers-fire-at-the-wrong-time.md)
  (already filed; the `handle` page links it).
- A composed handler held in a variable is still one flat table: a clause that
  performs an effect of its sibling aborts as unhandled (nest `with-handler`s, or
  write the `handler` literals in place).

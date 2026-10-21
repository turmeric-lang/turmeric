# The effect forms: `discontinue` runs on neither engine, `cont?` of a `^multishot` k, and a compiled unhandled effect has no name

**Severity: medium.** Found 2026-10-19 checking the examples for the effect forms'
API reference (`stdlib/forms/effects.tur`).

**Narrowed 2026-10-21.** Top-level `try-with` / `handle-shallow` / `with-handler`
statements, `resume` value checking, `compose-handlers` over handler literals,
`cont?` after a one-shot resume, the interpreter's `discontinue` stack smash and
`(handle 5)` are resolved: see
[effect-handler-forms-top-level-resume-compose](../archive/effect-handler-forms-top-level-resume-compose.md).

## 1. `discontinue` cannot be used (decision: lower it or retire it)

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

## 2. `cont?` of a `^multishot` continuation

```turmeric
(defeffect Ask [] : int)
(println (handle (+ 1 (perform (Ask)))
           (Ask [] ^multishot k)
           (let [r (resume k 41)] (if (cont? k) (+ r (resume k 1)) -1))))
```

Compiled: `-1`, since the resume marks the chain consumed at the first resume
even for a `^multishot` k -- `emit_resume` writes `((DK *)(k))->consumed = 1` at
each site (`src/compiler/emit_cps_ir.c`), and `__dk_resume_word` does the same for a
resume through a helper's `effect-cont` parameter. Interpreted:
`this continuation has already been resumed and its body has finished` -- a
clause that calls `cont?` is one the work-stack driver cannot descend
(`ws_capturable` does not know `EX_CONT_PRED`), so it takes the single-shot fiber
path. The same clause without the `cont?` call returns 44 on both. The one-shot
meaning (false after the resume) is now the same on both engines.

Decision: `cont?` of a `^multishot` k should be true after a resume (it can still
be invoked). Compiled that is a per-site change -- `emit_resume` skips the store
when the k's binding is `CK_MULTISHOT` -- not a preamble one (an earlier version of
this note said otherwise). The interpreter needs `EX_CONT_PRED` in its work-stack
driver, without which the clause falls off the multi-shot path; changing only the
compiled side would trade one divergence for another.

## 3. A compiled unhandled effect does not name the effect

`tur: unhandled effect (tag 2)`, exit status 134, where the interpreter says
`unhandled effect: Ask`. `dk_perform` has the tag only. Tags are numbered per
elaborated program (`effect_tag` in `emit_cps_ir.c`, reset with the CPS
classification), so a name table the program publishes for the runtime (the way
`__tur_any_register` publishes `any` type names) works for one translation unit,
but in a multi-TU build the same tag can name different effects in different
units, and a last-writer-wins table would print the wrong name. Either number
effects program-wide (which a cross-unit handle/perform already needs to agree on)
or key the published rows by unit.

## Also open

- The interpreter's `handle` runs the body's pending `defer`s at the wrong time
  when a clause does not resume:
  [turi-fiber-handle-defers-fire-at-the-wrong-time](turi-fiber-handle-defers-fire-at-the-wrong-time.md)
  (already filed; the `handle` page links it).
- A composed handler held in a variable is still one flat table: a clause that
  performs an effect of its sibling aborts as unhandled (nest `with-handler`s, or
  write the `handler` literals in place).

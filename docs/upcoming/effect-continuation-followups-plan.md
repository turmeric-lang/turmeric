# Plan: `cont?` of a `^multishot` continuation, naming an unhandled effect, composed handler values

> **Status:** PROPOSED 2026-10-21. Decisions taken (effect-forms-gaps items 2-4):
> `cont?` of a `^multishot` k is true after a resume; name the effect in the
> compiled unhandled-effect error; document (not change) a composed handler held
> in a variable. Part C is done (docs). Not started otherwise.

## Part A -- `cont?` of a `^multishot` continuation stays true after a resume

```turmeric
(defeffect Ask [] : int)
(println (handle (+ 1 (perform (Ask)))
           (Ask [] ^multishot k)
           (let [r (resume k 41)] (if (cont? k) (+ r (resume k 1)) -1))))
```

Wanted: 44 (a multishot continuation can still be invoked, so `cont?` is true and
the second resume runs). Now: `-1` compiled; interpreted, the second resume fails
with `this continuation has already been resumed and its body has finished`.

Compiled: `emit_resume` (`src/compiler/emit_cps_ir.c`) writes
`((DK *)(k))->consumed = 1` at every resume site so a later `cont?` reads false.
Skip that store when the resumed binding is `CK_MULTISHOT`
(`t->as.resume.k.var->type.copy_kind`). A per-site change, no preamble. The
wrapper path (`cont_is_wrapper`, through `__dk_resume_word`) already answers live.

Interpreted: a clause that calls `cont?` is not one the work-stack driver can
descend (`ws_capturable` does not admit `EX_CONT_PRED`), so the handle falls back
to the single-shot fiber path and the second resume finds the body finished.
Admit `EX_CONT_PRED` in `ws_capturable` / `ws_has_perform` (it performs nothing)
and evaluate it in the driver like any pure node; `EX_CONT_PRED` already reads
`k.as_cont->resumed` and the static `CK_MULTISHOT`.

Fixtures: the program above on all three engines (44); the one-shot shape stays
`effect-cont-pred-after-resume`.

## Part B -- name the effect in the compiled unhandled-effect error

Now: `tur: unhandled effect (tag 2)` and exit 134; the interpreter says
`unhandled effect: Ask`. `dk_perform` (`emit_dk_runtime.c`) has only the tag.

The check this plan was waiting on (2026-10-21): tags are numbered per elaborated
program (`effect_tag`, `emit_cps_ir.c`), and a separately compiled build refuses
at compile time to let a `perform` in one module reach a handler in another
(`'f' (module m) can perform the effect 'E', and this call crosses into it from
another module of a separately compiled build`). A module cannot export an effect
either. So a tag is only ever matched, and only ever needs naming, inside the unit
that performs it. A program-wide name table is therefore the wrong shape (two
units can both use tag 2 for different effects), and the right one is local: the
perform site knows its own effect.

Design: `dk_perform` takes the effect's name as a `const char *` (a literal at
each perform site; with the string-literal plan it is one object per unit) and
uses it only on the unhandled path: `tur: unhandled effect: Ask`. Keep the tag in
the message for debugging: `tur: unhandled effect: Ask (tag 2)`. The other
`dk_perform`-shaped entry points (the handler-value install path,
`src/runtime/cps_prompt.c`) take it too. A runtime-preamble change: regenerate
the snapshots and the split runtime.

Exit status stays 134 (abort) compiled vs 1 interpreted; not part of this plan.

Fixture: an `errors/`-style runtime fixture with `expected.stderr` `unhandled
effect: Ask`, compiled (the interpreter already prints it).

## Part C -- a composed handler held in a variable (documentation)

`(with-handler (compose-handlers L1 L2) body)` over handler literals nests the
handles (L1 outermost) since 2026-10-21. A composed handler built earlier and held
in a variable is still one flat table, so a clause that performs its sibling's
effect aborts as unhandled. Decision: document it. Done in the `compose-handlers`
entry of `stdlib/forms/effects.tur`; first-class-handlers-semantics FH0.1 states
the nested meaning, so a later change that makes handler values carry their
layers would close it.

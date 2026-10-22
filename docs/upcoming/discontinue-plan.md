# Plan: implement `discontinue` -- resume a continuation by raising at the `perform`

> **Status:** PROPOSED 2026-10-21. Decision taken: keep `discontinue` and
> implement it on both engines (from effect-forms-gaps item 1, archived). Not
> started. **2026-10-22:** open reports block its fixtures, see
> [Blockers](#blockers-checked-2026-10-22).

## Blockers (checked 2026-10-22)

The plan's own work (the IR term, the raise-on-resume runtime, typing, the
experiment row) is unblocked. Three open reports stand in the way of its
fixtures, so of its five fixtures only 4 (`^linear`) can pass on all three
engines today. The probes behind this table substitute `resume` plus a `panic`
in the continuation for `discontinue`, since that is the panic `discontinue`
will raise.

| Report | Engine | Blocks | Why |
| --- | --- | --- | --- |
| [cps-catch-unwind-cannot-cross-an-effect-capture](../reported/cps-catch-unwind-cannot-cross-an-effect-capture.md) | compiled | fixture 3 only (narrowed 2026-10-22) | Fixtures 2 and 5 are unblocked: a `catch-unwind` written in the body around the `perform` lowers as a catch prompt, and a panic raised in the resumed continuation -- where `discontinue`'s raise will start -- is caught there, per `^multishot` copy too. Still refused: a `catch-unwind` inside the CLAUSE around the `discontinue` (fixture 3). Until that lands, fixture 3 can catch around the `handle` instead, which works in the same function. The raise itself should enter the existing catch path: set `tur_panicking` with the payload staged, and the frame check / body hook / `dk_perform` find the prompt (`__dk_panic_catch`) |
| ~~[turi-fiber-resume-from-clause-scope-crashes-on-body-defer](../archive/turi-fiber-resume-from-clause-scope-crashes-on-body-defer.md)~~ | interpreted | -- | **Resolved 2026-10-22.** The fiber body now has its own defer chain, so resuming from a scope in the clause works |
| ~~[compiled-uncaught-panic-skips-caller-defers](../archive/compiled-uncaught-panic-skips-caller-defers.md)~~ | compiled | -- | **Resolved 2026-10-22.** An uncaught panic unwinds to `main` firing every defer. The raise `discontinue` makes must enter through the root-unwind path (`tur_panic_with_site`, or set `tur_root_unwind` to 2 around its raise), not a runtime helper's plain `tur_panic_with` |
| [turi-fiber-handle-defers-fire-at-the-wrong-time](../reported/turi-fiber-handle-defers-fire-at-the-wrong-time.md) | interpreted | fixture 5 with a `defer` or `catch-unwind` in the body (narrowed 2026-10-22: only the single-shot limit is left) | The fiber runtime is single-shot. `(+ (resume k 1) (resume k 2))` over a defer-bearing body is refused with "already been resumed", and so is one whose `perform` sits under a `catch-unwind` (`ws_capturable` refuses both). Its fix direction 1 (work-stack-capturable `defer`, and `catch-unwind` too) is the real unblock for the interpreter, and would also retire the crash above |

| [turi-catch-panic-of-mismatch-in-fiber-body-overflows](../reported/turi-catch-panic-of-mismatch-in-fiber-body-overflows.md) | interpreted | a `catch-panic-of` of the wrong type in a body with a `defer` | Found 2026-10-22. The plan types the payload with `catch-panic-of`, so a fixture that re-raises past one crashes the interpreter |

Related but not blocking:

- [compiled-shift-delimited-by-root](../reported/compiled-shift-delimited-by-root.md)
  has the same structure as the first row (a direct-emitted delimiter around a
  CPS callee). A design for one should fit the other.
- [effect-nontail-resume-copies-held-until-outer-entry](../reported/effect-nontail-resume-copies-held-until-outer-entry.md):
  not a blocker, but the in-place resume paths it added (`dk_case_resumes_once`,
  `resume_inplace`, `dk_resume_into`, the `k_last` free) are paths the raise
  flag in "Compiled" step 2 must survive. `term_resumes_k_once` /
  `term_k_only_resumed` should count a `discontinue` as the case's one resume,
  or a discontinue case falls back to the copy path.
- [missing-guides-for-language-primitives](../reported/missing-guides-for-language-primitives.md)
  item 5 and [stale-comments-emitter](../reported/stale-comments-emitter.md)
  item 6 cover the guide section and the `emit_effects.c` header. This plan's
  last step already does both.

Suggested order (updated 2026-10-22, after the first three rows were worked):
both halves of this plan can start, behind `--enable=discontinue`. Compiled,
fixtures 1, 2, 4 and 5 are reachable now, and fixture 3 with its catch around
the `handle`. Interpreted, fixtures 1 to 4 are reachable, and fixture 5 waits
on the single-shot limit. The remaining blockers are a catch in a handler
clause (compiled) and the two interpreter fiber reports.

## Today

```turmeric
(defeffect Ask [] : int)
(defn main [] : int
  (handle (+ 1 (perform (Ask))) (Ask [] k) (do (discontinue k "boom") 0))
  0)
```

Neither engine runs it. Compiled: the CPS IR has no rule for `EX_DISCONTINUE`
(`src/passes/cps_ir.c` marks it unsupported), so the function is evicted and the
build fails with a located error naming `discontinue` (`emit_effects_discontinue`)
beside the generic `no lowering here` on the `perform`. Interpreted:
`discontinue is not supported`, exit 1. It is also typed `nil`, so a clause that
ends in it is TUR-E0252 in any non-`nil` handle. The ownership rules
(`^linear` -- "resume or discontinue exactly once", TUR-E0101 / E0201) already
treat it as consuming `k`.

## Semantics

`(discontinue k payload)` resumes `k` the way `(resume k v)` does, except that the
`perform` that captured `k` does not return a value: it raises a panic carrying
`payload` at the `perform` site, inside the body. From there it is an ordinary
panic:

- the body's `defer`s run as it unwinds;
- a `catch-unwind` / `catch-panic-of` in the body that encloses the `perform`
  catches it, and the body carries on from there -- this is what not resuming
  cannot do, and the reason to keep the form;
- if nothing in the body catches it, the panic leaves the resumed continuation
  and is re-raised at the `discontinue` call in the clause, as the result of a
  `resume` would be returned there. The clause can catch it in turn.

`discontinue` returns what `resume` returns: the handle's answer type (the value
the rest of the body produced, when the body recovered). So a clause can end in
it. The payload follows `panic-with`: any value, typed by `catch-panic-of`; a
`cstr` payload is the common case and reads as the panic message.

For comparison, a clause that does not resume abandons the body; the body's
cleanup still runs (compiled at once, interpreted late:
[turi-fiber-handle-defers-fire-at-the-wrong-time](../reported/turi-fiber-handle-defers-fire-at-the-wrong-time.md)),
but the body cannot intervene.

## Compiled (CPS / DK backend)

1. **IR.** Admit `EX_DISCONTINUE` in `cps_ir.c` as a resume whose value slot is
   replaced by a raise: a `CT_RESUME` with a `raise` flag and the payload atom,
   or its own term kind next to `CT_RESUME`. `emit_resume` (`emit_cps_ir.c`) is
   the model.
2. **Runtime.** Before invoking the chain, store the payload as the pending panic
   (the `tur_panic_with` machinery: type tag, value, site) and mark the chain
   "resume by raising". The first frame the chain runs is the `perform`'s
   continuation frame; give that frame an entry check -- if the resume is a raise,
   set `tur_panicking` (or call `tur_panic_with` there, so the reported site is the
   `perform`) and return without using the value. From there the existing panic
   path does the rest: CPS frames return on `tur_panicking` (`cps_panic_check`),
   pending defers of the abandoned frames fire (`__dk_panic_abandon`), and a
   `catch-unwind` boundary in the body consumes it.
3. **Back in the clause.** If the chain comes back with `tur_panicking` still set,
   the clause sees a panic at the `discontinue` -- the normal propagation after a
   call. If the body caught it, the chain returns a value and `discontinue`
   evaluates to it.
4. **Shapes.** Tail and non-tail discontinue, `^multishot` k (each copy raises
   independently), the wrapper path (`cont_is_wrapper`, through
   `__dk_resume_word`), and a `perform` reached through a helper function. A
   runtime-preamble change if the entry check lives in the DK machine
   (regenerate snapshots and the split runtime); per-frame if it is emitted with
   each `perform`'s continuation.
5. Remove the "no lowering" error from `emit_effects_discontinue`, or keep it only
   for the direct emitter, which cannot lower effects at all.

## Interpreted

1. **Work-stack driver** (`eval_drive_ex`, `DK_RESUME`). A resume that pushes the
   captured slice back can carry a pending raise: instead of delivering the value
   to the hole, raise the panic there (`turi_runtime_panic` / the `panic-with`
   path), so it unwinds through the slice's frames, fires their defers and stops at
   a `DK_CATCH_UNWIND` in the body if there is one.
2. **Fiber fallback** (`eval_resume_cont`). Set a "raise" flag on the
   `TuriEffectCont` and swap into the body; `eval_perform_fiber`, on waking,
   checks the flag and raises in the body's context. The removed 2026-10-21 code
   did the swap from the wrong side (handler context into a body that was not
   suspended at a perform); the wake-up check is what makes it a resume.
3. A panic that leaves the body comes back to the clause as an error value from
   the resume, and is re-raised there.

## Typing

`elab_discontinue` (`elab_effects.c`) types the node like `elab_make_resume`: the
continuation's `cont_result_type`, else the resume-value fallback. The payload is
any type (like `panic-with`). Keep `cont_mark_consumed` and the double-use check.

## Fixtures (all three engines)

1. Uncaught: the program above -- the body's `defer` runs, the panic reaches the
   clause and then the top level; `expected.stderr` names the payload.
2. Caught in the body: a `catch-unwind` around the `perform` turns the raise into
   an `Err` and the body returns a value, which is the handle's result.
3. Caught in the clause: `(catch-unwind (fn [] (discontinue k "x")))` in the
   clause.
4. `^linear` k discontinued once (accepted) and twice (TUR-E0101).
5. A `^multishot` k: one resume and one discontinue of copies.

Replace `errors/discontinue-no-lowering` and `discontinue-interp-unsupported`,
which pin today's refusal.

## Staging

Each engine can land on its own; until both run it, gate the new behaviour behind
`--enable=discontinue` with an `EXPERIMENTS[]` row (CLAUDE.md's
experimental-features rule) so the two engines never disagree on a program that
builds. Graduate when both pass the fixtures. Then update the `discontinue` entry
in `stdlib/forms/effects.tur` and effects-system-guide.

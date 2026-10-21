# Effect forms: top-level handler statements, `resume` values, `compose-handlers`, `cont?`

**RESOLVED 2026-10-21** (items 2, 3, 4, the one-shot half of 5, and two of the
smaller items of effect-forms-gaps). The rest stays open in
[effect-forms-gaps](../reported/effect-forms-gaps.md). Pinned by
`effect-toplevel-handler-forms`, `effect-compose-clause-performs`,
`effect-cont-pred-after-resume` (all three engines), `errors/resume-value-type-mismatch`,
`errors/handle-no-clauses`, `errors/discontinue-no-lowering` and
`discontinue-interp-unsupported`.

## What changed

- **Top-level `try-with` / `handle-shallow` / `with-handler` statements** were
  abandoned by the fold into a synthesized `main` because
  `form_has_toplevel_unhandled_perform` (`elab_toplevel.c`) named only `handle`
  and `reset` as guards, so the build said `no lowering here`. The three forms
  join the list.
- **`resume` was unchecked.** A handler clause's `k` now records the kind its
  effect operation declares (`Binding.cont_op_result_kind`), and
  `elab_make_resume` compares the value with it (TUR-E0001). Scalar families
  only (integers, floats, bool, cstr); a `nil`-result effect takes any filler,
  which the corpus relies on (`(resume k 0)`).
- **`compose-handlers` over handler literals** lowers to nested `handle`s,
  h1 outermost (`elab_with_handler`), the form FH0.1 says it is equal to. The
  flat table made a clause that performed its sibling's effect abort as
  unhandled. A composed value held in a variable keeps the flat table. The
  handled row of a composed handler is also pushed while the body elaborates, so
  `(with-handler (compose-handlers ...) (perform ...))` at top level is no longer
  TUR-E0008.
- **`cont?`** is false after a resume on the interpreter as well (it answered
  true; the compiled program said false), via a `resumed` flag on the
  continuation because a one-shot work-stack continuation is freed at its resume.
  A `^multishot` `k` stays true when interpreted.
- **`discontinue`** no longer smashes the interpreter's stack (it swapped from
  the handler's stack into the body's saved context): it reports `discontinue is
  not supported`. The compiled build adds a located error naming the form beside
  the generic `no lowering here` on the `perform`.
- **`(handle 5)`**, a `handle` with no clauses, is a diagnostic instead of a
  missing-lowering message when compiled and a silent `5` interpreted.

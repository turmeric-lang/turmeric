# Compiled effect-handler multishot resumes return the FIRST resume's result

> **RESOLVED 2026-06-14.** Both paths now implement true multishot.
> - **Compiled:** the multishot continuation env captures a *snapshot* of the
>   suspended fiber's stack at the perform point and restores it before each
>   resume, so every `(resume k v)` re-runs an independent copy
>   (`src/compiler/emit_effects.c` `__ms_env`/`__ms_cont_fn`; the dynamic
>   dispatcher mirror in `src/compiler/emit_module.c` `__tur_msdyn_env`). The
>   stack-copy lives in the generated binary, which is built by plain `cc`
>   (no ASan), so the copy is safe there.
> - **Link failure** (non-trivial continuation -> `undefined reference to
>   tur_cloneable_cont_alloc`) was a second bug: `expr_has_multishot_handler`
>   (`src/compiler/emit_core.c`) did not descend into `EX_BUILTIN`/`EX_CALL`/
>   `EX_MATCH`/... so a handler nested in e.g. `(println (handle ...))` failed to
>   emit the cloneable runtime. The traversal is now comprehensive.
> - **Interpreter:** the work-stack continuation clones its captured slice per
>   resume (`src/turi/eval.c` `clone_ws_slice`) instead of caching the first
>   result.
> - **Fixtures:** `multishot-handler` / `fh-multishot-value` now expect `30`
>   (was `20`); `multishot-copy-capture` stays `200`. Verified compiled vs
>   `--interpret` agree on trivial and non-trivial continuations.
>
> Original report (degenerate behaviour) preserved below.

**One-line summary:** For an algebraic-effect handler with `^multishot k`, the
**compiled** path made every `(resume k v)` after the first return the *first*
resume's result, ignoring the new `v` (and re-running nothing). True multishot
(the documented design) runs the captured continuation afresh per resume.

**Severity:** Medium -- **silent miscompile** of `^multishot` effect handlers
(rc=0, wrong value). Single-resume handlers are correct; only the 2nd+ resume of
the same `k` is wrong. This is *not* an interpreter bug -- it reproduces in the
compiled binary, and the `--interpret` path was deliberately made to match it
(see "Interaction with the interpreter" below).

## Minimal repro

`tests/fixtures/multishot-handler/input.tur`:

```turmeric
(defeffect Ask [] : int)
(defn main [] : int
  (let [r (handle (perform (Ask))
            (Ask [] ^multishot k)
              (+ (resume k 10) (resume k 20)))]   ; resumes k twice
    (println r))
  0)
```

Observed (compiled and `--interpret`): `20`. Expected under **true** multishot:
`30` (`k(10)=10`, `k(20)=20`, sum `30`).

Tracing what each resume returns (compiled):

```turmeric
(let [a (resume k 10)]      ; a = 10
  (let [b (resume k 20)]    ; b = 10  (!!  not 20)
    (+ (* a 1000) b)))      ; => 10010, i.e. b == 10
```

and with three resumes `1`/`2`/`3` over a body `(+ (perform (Ask)) 100)`, the
result is `11211` = `101*100 + 101*10 + 101`, i.e. **all three** resumes return
`101` (the first resume's result `1+100`). So the rule is concretely: *the first
`(resume k _)` computes a result `R`; every later `(resume k _)` returns `R`.*

`multishot-copy-capture` (`(+ (resume k offset) (resume k offset))`, both `100`)
yields `200` under both the buggy and the correct semantics, so it does not
expose the divergence; only handlers that resume with **different** values do.

A related, separate facet: when the captured continuation is **non-trivial**
(the perform is not the whole handled body, e.g. `(handle (+ (perform (Ask)) 1)
(Ask [] ^multishot k) (+ (resume k 10) (resume k 20)))`), the compiled path
fails to **link** (`undefined reference to tur_cloneable_cont_resume`) rather
than producing a value at all. The interpreter's work-stack continuation handles
this case (cached-replay -> `22`); no fixture pins the compiled link failure, so
there is nothing to match there.

## Why this is a bug

`docs/archive/multishot-continuations-plan.md` ("Proposed Design") intends
`CK_MULTISHOT` to be a genuinely many-shot continuation -- its motivating
example is `(list/flat-map options (fn [opt] (resume k opt)))`, which only makes
sense if each `(resume k opt)` runs the continuation with a *distinct* `opt`.
Returning the first result for every resume silently collapses that to a single
shot whose answer is broadcast.

## Root cause (hypothesis, not yet pinpointed)

The compiled multishot path snapshots the continuation but appears to memoise or
share the *first* resumption's result rather than re-running the reified
continuation with the new resume value. This is the effect-handler analog of the
`tur_continuation_snapshot` independence bug fixed for `cloneable-shift` in
`docs/archive/turi-multishot-continuation-snapshot-miscompile.md` (R5) -- but
that fix was for `cloneable-reset`/`cloneable-shift`, not for `^multishot`
effect handlers, which still exhibit the collapse. The relevant compiled
machinery is the CPS multi-prompt runtime (`src/runtime/cps_prompt.c`,
`src/compiler/emit_cps.c`); the snapshot/clone primitive there needs to produce
an independently-resumable copy whose hole is refilled from the *new* resume
value on each resume.

## Interaction with the interpreter (delimited-control plan)

`docs/upcoming/v1/turi-interpreter-delimited-control-plan.md` carried five
`requires.tur-only` fixtures for the interpreter's missing multishot / escaping /
nested-resume support. While implementing work-stack delimited control for that
plan, the interpreter was made to **match the compiled output** (so the fixtures
stay green on both paths and the carve-outs lift): the work-stack continuation
runs the captured slice on the first resume, caches the result, and returns the
cache on subsequent resumes (`src/turi/eval.c`, `TuriWsCont` / the `EX_RESUME`
arm of `eval_drive_ex`'s descending switch). This is a deliberate bug-for-bug
match, marked in the code with a pointer to this report.

If/when the compiled multishot is fixed to be genuinely many-shot, both the
interpreter's cached-replay and the affected fixtures' `expected.stdout`
(`multishot-handler` -> `30`, `fh-multishot-value` -> `30`) must be updated in
the same change.

## Proposed fix directions

1. Fix the compiled CPS snapshot so a resumed continuation refills its hole from
   the *current* resume value, giving each resume an independent run.
2. Update `multishot-handler` / `fh-multishot-value` `expected.stdout` to `30`,
   and switch the interpreter's `TuriWsCont` resume from cached-replay to a
   per-resume clone (re-push a fresh copy of the slice; the work-stack
   representation already supports this -- only the deliberate cache short-circuit
   needs removing).
3. Re-run `tests/run.sh` + `tests/run-turi.sh`; both paths should now print `30`.

## Validation of the current (matched) behaviour

- `tests/run.sh` and `tests/run-turi.sh` both print `20` for `multishot-handler`
  / `fh-multishot-value`, `200` for `multishot-copy-capture`.
- Single-resume handlers (`effect-capture-k`, `effect-handler-capture-nested`)
  are unaffected by the bug and are correct under both paths.

# An outer effect performed under an inner non-tail `resume` captures only up to the resume

**RESOLVED 2026-10-09 (filed the same day).** Fix direction 1: a case that
resumes its own `k` non-tail resumes it INTO the chain. See "Resolution" at
the end.

**Severity: medium-high (silent wrong answer, compiled path only).** Found
2026-10-09 while resolving
[effect-copy-path-tail-resume-delivers-out-of-order](../archive/effect-copy-path-tail-resume-delivers-out-of-order.md),
which is the same symptom from a different mechanism. Pre-existing: the
answers below are the same before and after that fix.

```turmeric
(defeffect Ask [] int)
(defeffect Out [] int)
(defn inner-nt [] : int
  (handle (+ (perform (Ask)) (perform (Out)))
    (Ask [] k) (+ 100 (resume k 1))))            ; non-tail
(defn inner-ms [] : int
  (handle (+ (perform (Ask)) (perform (Out)))
    (Ask [] ^multishot k) (+ (resume k 1) (resume k 2))))
(defn main [] : nil
  (println (handle (* 10 (inner-nt)) (Out [] k) (+ 1000 (resume k 5))))
  (println (handle (* 10 (inner-nt)) (Out [] k) 42))
  (println (handle (* 10 (inner-ms)) (Out [] k) 42)))
```

| | compiled | `tur --interpret` (right) |
| --- | --- | --- |
| non-tail outer | 11060 | 2060 |
| abortive outer | 1420 | 42 |
| abortive outer, multi-shot inner | 840 | 42 |

An outer case that tail-resumes gets the right answer (1060), because the
order of the work cannot show.

The abortive shape is the one to worry about: an outer handler used as an
exception (`(Fail [] k) default`) around code whose own handler does work
after it resumes. The "aborted" computation's inner handle and everything
after it still run on the outer case's value.

## Root cause

A non-tail `resume` inside a handler case is `dk_invoke` (`emit_resume`,
`src/compiler/emit_cps_ir.c`): it runs a copy of the continuation -- the
frames up to the handler, its group re-installed, then marker copies of the
enclosing handlers and `done` (`dk_perform`, `src/compiler/emit_dk_runtime.c`)
-- and returns its value to the case on the C stack. The rest of the case
(`(+ 100 _)`) and the handle's continuation after it (`dk_perform`'s
`dk_run_impl(H_next, r)`, then `(* 10 _)`) are not on the chain while the
copy runs. `Out`, performed inside the copy, finds its handler's marker copy
and captures up to it: `(+ 1 _)` and nothing more. Its case's resume returns
6 at once; the case adds 1000 (or returns 42), and that value comes back out
of `dk_invoke` as the inner resume's result, to run through `(+ 100 _)` and
`(* 10 _)` after the fact.

[await-parks-only-to-the-nearest-c-frame](../reported/await-parks-only-to-the-nearest-c-frame.md)
shape 2 is the same mechanism with `await` in place of the outer effect.

## Fix directions

1. **CPS the case past a non-tail resume.** Lift what follows the resume in
   the case (`CT_RESUME`'s body) as a resume-frame, as `emit_perform` lifts a
   perform's continuation, and resume by running the copy with that frame
   and the handle's real continuation appended where the marker copies and
   `done` are now. The case then delivers its own value (as a re-opening case
   already does: `dk_case_delivers`, `dk_case_enclosing_real`), so
   `dk_perform` returns it verbatim. Every capture taken in the resumed part
   then includes the rest of the case and the rest of the program, and a
   multi-shot case's second resume runs from the first one's frame.
2. **Refuse rather than miscompile** an effect, performed under a non-tail
   resume, whose handler lies outside the resuming handle -- the CPS backend
   already refuses a perform it cannot lower. This needs the effect sets of
   the resumed body, which the colorer has.

## Resolution (2026-10-09)

A handler case that resumes its own `k` non-tail on its own straight-line or
branch structure (`case_resumes_into`, `src/compiler/emit_cps_ir.c`) now runs
like a re-opening case: its `__kont` is the handle's real continuation
(`dk_case_enclosing_real`), it delivers its own value through it, and its
handler node is marked so `dk_perform` returns that value as is. Each resume
of `k` in the case body, or in the rest of a resume, is `dk_resume_into`
(`src/compiler/emit_dk_runtime.c`): the copy of `sub` up to the point where
the resumed computation ends (`resume_cut`, which `dk_perform` sets on the
first of the enclosing-handler markers), then a resume-frame holding the rest
of the case -- lifted from `CT_RESUME`'s body, as `emit_perform` lifts a
perform's continuation -- whose next is `__kont`. A tail resume continues into
`__kont` itself. The scan crosses a `perform` in the case (a re-opening case,
`(+ (perform (Log)) (+ 100 (resume k 1)))`): that perform's continuation is a
resume-frame whose `__kont` is the case's, and resumes into it the same way. Every capture taken in the resumed part now holds the rest of
the case and of the program; the repro prints 2060, 42 and 42.

When every use of `k` is such a resume (`term_k_only_resumed`), nothing will
resume `sub` with `dk_invoke`, so the handler is installed with
`dk_case_resumes_into` and `dk_perform` ends `sub` at `done` instead of
copying the enclosing handlers. That copy walks the rest of the chain, which
under these cases grows a frame a turn (the case's pending rest), so without
it a loop of 64,000 non-tail resumes under one handle took 61 s; with it,
0.09 s. A loop of 100,000 used to die of C-stack overflow (`dk_invoke` nested
a few C frames a turn); at -O2 the resume is a tail call now and it runs, in
112 MB -- what the turn keeps is
[effect-nontail-resume-copies-held-until-outer-entry](../reported/effect-nontail-resume-copies-held-until-outer-entry.md).

Also fixed by it: shape 2 of
[await-parks-only-to-the-nearest-c-frame](../reported/await-parks-only-to-the-nearest-c-frame.md),
an `await` inside a multi-shot case's non-tail resume (102 for 105).

Still on `dk_invoke`, and so still able to split a capture: a `k` that
escapes the case -- passed to a function, captured by a lambda, stored -- and
is resumed there.

Pinned by `tests/fixtures/effect-nontail-resume-under-outer` (non-tail,
multi-shot, branching, re-performing, re-opening, shallow and float cases, and
a 2,000-turn loop; leak-checked) and `await-below-evicted-caller-or-nontail-resume`.

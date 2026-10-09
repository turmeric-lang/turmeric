# An outer effect performed under an inner non-tail `resume` captures only up to the resume

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

[await-parks-only-to-the-nearest-c-frame](await-parks-only-to-the-nearest-c-frame.md)
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

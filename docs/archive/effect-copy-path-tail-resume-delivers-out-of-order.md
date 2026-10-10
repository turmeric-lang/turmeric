# A tail-resumed handler that is not resumed in place delivers its handle's continuation out of order under an outer non-tail handler

**RESOLVED 2026-10-09 (filed the same day).** Fix direction 2 below, taken
whole: a deep tail-resume case that is not resumed in place now gets a copy
of the whole chain from the perform -- the frames up to the handler, its
group, and then the handle's continuation and the rest of the program -- so
nothing is queued, and the meta-stack that held the queued deliveries is gone
(it had no other producer). See "Resolution" at the end.

**Severity: medium (silent wrong answer, compiled path only), narrow.**

```turmeric
(defeffect Ask [] int)
(defeffect Out [] int)
(defn inner [] : int
  (handle (+ (perform (Ask)) (perform (Out)))
    (Ask [] k) (let [k2 k] (resume k2 1))))
(defn main [] : nil
  (println (handle (* 10 (inner)) (Out [] k) (+ 1000 (resume k 5)))))
```

Compiled, this prints **10060**; `tur --interpret` prints **1060**, the
right answer: `Out`'s continuation is `(+ 1 _)`, the inner handle's exit and
`(* 10 _)`, so its resume gives 60 and the case adds 1000.

## Root cause

The inner case reduces to a tail resume, so `dk_perform` takes the E7
trampoline path: it resumes a copy of the chain from the perform up to the
handler, ending in `done`, and queues the rest -- the inner handle's
continuation, `(* 10 _)`, and everything after it -- on the meta stack as a
separate delivery, to run once the copy settles. `Out`, performed inside the
copy, captures only up to that `done`, so its non-tail `resume` returns
`1 + 5` before `(* 10 _)` has run; the case adds 1000, and the queued
delivery then multiplies 1006 by 10.

Since 2026-10-09 a case that resumes `k` once, in tail position, and uses it
for nothing else is resumed in place, on the original chain, with no split --
so `(Ask [] k) (resume k 1)` gives 1060
(`tests/fixtures/effect-inner-tail-resume-under-outer`). A tail-resume case
that touches `k` first -- here an alias, `(let [k2 k] ...)` -- still takes the
copy path (`case_resumes_k_only_in_tail`, `src/compiler/emit_cps_ir.c`).

## Fix directions

1. Widen in-place resumption to cases that alias `k` but still resume it once
   in tail position (follow the alias through `CT_LETVAL`).
2. For the copy path itself, append the delivery to the resumed copy instead of
   queuing it, so an outer capture taken inside the copy includes the handle's
   continuation. The delivery is already a full copy of the rest of the chain
   (`dk_copy_range(H->next, NULL)`), so this costs no more; what it changes is
   the meta stack's job, which the trampoline's flatness argument relies on.

## Resolution (2026-10-09)

`dk_perform` (`src/compiler/emit_dk_runtime.c`) has two branches for a deep
tail-resume case under the trampoline: the in-place one (unchanged), and a
copy branch that hands the case `dk_copy_range(k, NULL)` -- the in-place
branch's continuation, copied. An outer capture taken inside it now reaches
the handle's continuation: the repro prints 1060 compiled, as `--interpret`
does.

The bug was wider than the report's row. An ABORTIVE outer case under the
same inner shape also went wrong -- `(handle (* 10 (inner)) (Out [] k) 42)`
printed **420**: the case's value went back to the driver, which then ran the
queued `(* 10 _)` on it. With two aliasing inner handles nested, the
non-tail outer case printed 3078 for 1078 and an abortive one 21 for 7.
All of them are in `tests/fixtures/effect-tail-resume-copy-under-outer`
(leak-checked).

The copy costs what the queued delivery did, since the delivery was a copy of
the same rest of the chain (`dk_copy_range(H->next, NULL)`). With nothing
pushing onto it, the meta-stack went too: `__dk_meta_push`,
`__dk_delivery_noop`, the three `g_dk_meta*` thread-locals (and their host
accessors in `src/runtime/tur_tls.c` and rows in
`tools/gen-runtime-split.py`), the fiber resume's save of its depth, and the
drivers' delivery loops. `__dk_drive_bounded` lost its `floor` argument.

Not fixed here, and not this mechanism: an inner case that resumes NON-tail
(`(+ 100 (resume k 1))`) under an outer handler whose effect is performed in
the resumed part. Its continuation after the resume lives on the C stack
(`dk_invoke`), so an outer capture cannot include it -- see
[effect-nontail-resume-under-outer-handler-splits-the-capture](../reported/effect-nontail-resume-under-outer-handler-splits-the-capture.md).

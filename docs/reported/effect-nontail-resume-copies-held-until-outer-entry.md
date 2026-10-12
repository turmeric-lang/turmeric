# An effect resumed non-tail, or more than once, in a loop keeps its continuation copies until the outermost entry returns

**Narrowed 2026-10-12: a case that resumes `k` once, non-tail, on
straight-line code resumes the ORIGINAL chain in place.** The repro below
runs in 1.4 MB at 200,000 turns (339 MB before; the Debug `tur`'s own
program, no sanitizer), and the reap list of a loop that opens such a handle
a turn, or performs a turn under one, stays flat.  What is left is the shape
the title still names: a case that resumes `k` more than once, or reaches its
one resume through a frame of its own on the chain -- a `perform`, a colored
call, a join -- still resumes a copy, and the copies are kept.  See "Fixed
2026-10-12" at the end.

**Severity: low-medium (memory grows for the life of the loop's outermost
call).** What is left of
[fn-value-call-cps-frames-held-until-outer-entry](../archive/fn-value-call-cps-frames-held-until-outer-entry.md):
a handler case that resumes its continuation once, in tail position, now
resumes the original chain in place and allocates nothing that outlives the
turn.  Since 2026-10-12 so does one that resumes it once NON-tail on code that
can put no frame of its own on the chain first.  Any other case still resumes
a COPY, and the copies are kept.

## Repro

```turmeric
(defeffect Ask [] int)
(defeffect Inner [] int)
(defn inner-step [x : int] : int
  (handle (+ x (perform (Inner)) (perform (Ask)))
    (Inner [] k) (+ 1 (resume k 10))))          ; non-tail
(defn loop [i : int n : int acc : int] : int
  (if (= i n) acc (loop (+ i 1) n (+ acc (inner-step i)))))
(defn main [] : nil
  (println (handle (loop 0 200000 0) (Ask [] k) (resume k 3))))
```

| turns | peak RSS (2026-10-12, in place) | 2026-10-09, after resume-into | before |
| --- | --- | --- | --- |
| 100,000 | 1.4 MB | 151 MB | 177 MB |
| 200,000 | 1.4 MB | 300 MB | 352 MB |
| 1,000,000 | 1.4 MB | -- | -- |

**This repro is fixed** (see "Fixed 2026-10-12").  It cost about 1,500 B a
turn (1,750 before resume-into, 2,250 before 2026-10-09).  A loop of performs
under one handle whose case is `(+ 0 (resume k 1))` cost ~1.1 KB a turn; now
one 144 B node a turn, which is inherent there -- each turn's `(+ 0 _)` waits
for the whole loop -- and nothing on the reap list.  That loop no longer
nests C frames a turn: the resume yields its chain to the trampoline driver
(100,000 turns used to die of stack overflow).

## What is left

A case that resumes `k` more than once, or reaches its one resume through a
frame of its own on the chain, still takes the copy path:

```turmeric
(Inner [] k) (+ (resume k 1) (resume k 2))              ; twice
(Inner [] k) (let [y (perform (Outer))] (+ y (resume k 1)))  ; after a perform
(Inner [] k) (let [y (effectful-helper)] (+ y (resume k 1)))  ; after a colored call
```

The second and third are refused for a reason, not for want of a predicate:
the perform's (or call's) continuation is a frame of the case on the chain,
and an outer multi-shot resume can run it twice -- and with it the resume of
`k` -- so the original cannot be handed over.  A per-resume runtime count
cannot see that second run coming either; what it would take is a proof that
no enclosing handler is multi-shot, or a copy taken lazily at the second
resume (the first runs the original, a second finds it consumed and copies
what it needs -- which is the whole chain, already run).

## Where it comes from

`dk_perform` copies the chain from the perform up to the handler (`sub`) and
registers it (`__dk_reap_keep(sub)`). Since 2026-10-09
([effect-nontail-resume-under-outer-handler-splits-the-capture](../archive/effect-nontail-resume-under-outer-handler-splits-the-capture.md))
a non-tail `resume` in the case's own body is `dk_resume_into`, which copies
`sub` up to its `resume_cut`, appends the rest of the case as a resume-frame,
and registers that copy too (`__dk_reap_keep(c)`), since a tail resume inside
may yield past it. A `k` resumed anywhere else is still `dk_invoke`, which
copies `sub` again and keeps that copy (`__dk_drive_bounded`'s
`__dk_reap_keep(ch)`). The originals the first copy crossed are marked
copied, so the join release never takes them. All of it waits for the
outermost entry.

## Fix directions (as filed; 1 is done for the straight-line case)

1. **One-shot non-tail resume in place.** Under `dk_resume_into` the resumed
   part no longer returns to the case -- it runs on into the case's
   resume-frame -- so a case that resumes `k` once could run the ORIGINAL
   chain, as a tail resume does, if the frame were spliced in after the
   handler's group (before the handle's continuation frame) for the duration
   of the run. The splice mutates a live chain, so it needs the same "nothing
   else holds `k`" proof `dk_handler_tail_inplace` has
   (`term_k_only_resumed` is most of it).
2. **Free a delivered copy.** Once a one-shot resume has run and no capture
   was taken of the copy it ran (the per-node copy count, `DK.ncopy`, says
   so), the copy is dead: free it then instead of keeping it. Under
   `dk_resume_into` "run" means the copy's own nodes, up to the case's
   resume-frame -- the frame and what follows it are the case's continuation
   and must stay. The originals still need (1) or a similar proof.

## Fixed 2026-10-12: one non-tail resume, straight-line, in place

Fix direction 1, with the proof it asked for.  `emit_handle` marks a case
with `dk_case_resumes_once` (`src/compiler/emit_cps_ir.c`,
`case_resumes_once`) when every path through its body reaches at most one
`resume` of `k` through rebinds, primitives, DIRECT calls and branches only
(`term_resumes_k_once`) -- no `perform`, colored call, join point, `await`,
loop or raw step, which are the shapes that put a frame of the case on the
chain, where an outer multi-shot resume could run it, and the resume, twice
-- and `k` is used for nothing else (`term_k_only_resumed`) and not mentioned
after the resume.  The case function is then called once per perform and
resumes `k` at most once, so nothing can need the chain again.

`dk_perform` (`src/compiler/emit_dk_runtime.c`) hands such a case the
original chain, as `dk_handler_tail_inplace` does a tail case, marking its
head `resume_inplace`; the case's one `dk_resume_into` finds the flag, splices
the rest-of-case frame in after the handler group (the node whose `next` is
the handle's real continuation) and yields the chain to the driver, so the
perform's frames and the joins under them run uncopied and are handed back as
they run.  The spliced frame (`splice_into`, with the node it follows in
`body_env`) is taken back out and freed by `dk_run_impl` when it runs; until
then the group's own free walks through it, so a chain abandoned first frees
it with the group.  The frame OWNS its env (`dk_frame_resume_borrow_owned`;
`emit_cont_env_owned`): registered at the resume, as a perform continuation's
is, it sat on the reap list above the perform's frame and kept it from being
handed back.  `dk_run_impl` registers it just before the frame function reads
it, so that function's `__dk_join_release_env` finds it last; under the copy
path it is never registered and the frame's chain frees it, and a copy dups
it.  A case that answers without resuming returns to `dk_perform`, whose
pend-perform record for it carries no `sub` (`__dk_pp_kept`), and its k..H
are abandoned as on the copy path.  Not taken without a driver (the resume
must leave the perform's C frame), for a shallow handler, or once r7rs
`call/cc` has pinned DK memory.

Pinned by `tests/fixtures/effect-nontail-resume-once-in-place`
(leak-checked; the reap list's length at turns 10 and 900 of a handle opened
a turn, of performs under one handle, and of a case whose rest captures a
value: 1 / 1 each, against 71 / 6,301, 7,062 / 12,402 and 13,093 / 21,103
before) and `effect-nontail-resume-once-shapes` (an abortive branch, two
performs in one body, an outer multi-shot capture across the splice, a
three-case handle, a direct call before the resume, the handle opened inside
a multi-shot copy, the value used twice after, a loop body: the copying
path's answers on both back ends).

Suite: `run.sh` 3757/0, `run-leak-check.sh` 177 / 0 / 3 known-open, the
JIT over the effect, CPS, multishot, handle, async, resume and defer
fixtures 474/0.  The repro at 1,000,000 turns: 1.4 MB.

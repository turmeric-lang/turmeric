# An effect resumed non-tail, or more than once, in a loop keeps its continuation copies until the outermost entry returns

**Narrowed twice 2026-10-12: a case that resumes `k` once resumes the
ORIGINAL chain in place -- on straight-line code, and, when nothing outward
of the handle is multi-shot, after a `perform` of an outer effect too.** The
repro below runs in 1.4 MB at 200,000 turns (339 MB before; the Debug
`tur`'s own program, no sanitizer), and so do the log-then-continue idiom
`(do (perform (Write y)) (resume k 10))` (401 MB before) and its non-tail
twin (345 MB before); the reap list of a loop that opens any of these
handles a turn stays flat.  What is left is a `k` resumed more than once
(`^multishot`), a case under a `^multishot` handler, a prompt or a shift,
and a `k` resumed elsewhere than the case's own body (`dk_invoke`) -- and
for those, since the third pass, every delivered copy nothing captured is
freed once it has run (fix direction 2), so a `^multishot` case resuming
twice a turn costs a third of what it did.  See the three "Fixed
2026-10-12" sections at the end.

**Severity: low-medium (memory grows for the life of the loop's outermost
call).** What is left of
[fn-value-call-cps-frames-held-until-outer-entry](../archive/fn-value-call-cps-frames-held-until-outer-entry.md):
a handler case that resumes its continuation once, in tail position, now
resumes the original chain in place and allocates nothing that outlives the
turn.  Since 2026-10-12 so does one that resumes it once NON-tail, on code that
can put no frame of its own on the chain first, or through a `perform` when
nothing outward of the handle can run that frame twice.  Any other case
still resumes a COPY, and the copies are kept.

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

```turmeric
(Inner [] ^multishot k) (+ (resume k 1) (resume k 2))   ; twice: a copy each, freed after each run
(Inner [] k) (let [y (perform (Outer))] (+ y (resume k 1)))
  ;; ...under a ^multishot Outer, a reset, or a with-handler table: the
  ;; perform's continuation can run twice, so the copy path (see `ss`)
(Inner [] k) (helper k)                                  ; k resumed elsewhere: dk_invoke
```

What the first still keeps, per perform, is not a copy: the perform's
ORIGINAL frame and its env, which only copies of ever run (so no hand-back
fires for them), and behind them on the reap list the handler group and the
loop's join.  The env is shared by every copy of the frame, so freeing it
needs a count of those copies, which the nodes do not keep.  A stored `k`
(the scheduler idiom) keeps its stored copy for good, since a stored
continuation may be resumed any number of times (a second `(resume saved
v)` compiles and runs).  The second is a runtime fact, not a predicate gap: `^multishot` is the one
way a non-tail resume runs a continuation twice (a plain `k` is `^unique`,
so a second `resume` is TUR-E0201), and a shift to an enclosing prompt the
other; `ss` reads both off the chain in O(1).  A `with-handler` table's
cases are marked multi-shot because their shape is not known at the
install.  The third resumes a copy, freed after its run since the third pass.  A colored call before the resume is outside the CPS subset
today (the call's `perform` is refused with "no lowering here"), so it is
not a shape this report can measure.

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

## Fix directions (as filed; 1 is done, for the straight-line case and the re-opening one)

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

## Fixed 2026-10-12, second pass: one resume after a `perform`, in place when nothing outward is multi-shot

The same, with the proof made at run time.  `case_resumes_once_dyn`
(`src/compiler/emit_cps_ir.c`) admits the straight-line predicate's walk
across a `perform` of an outer effect, and `emit_handle` marks the case
`dk_case_resumes_once_dyn`; `case_resumes_into` now also admits a TAIL resume
anywhere but on a dk_handler_tail case's own spine (the continuation of a
perform, a branch), so the log-then-continue idiom resumes into the chain
(`dk_resume_into`, borrowing the handle's continuation) instead of running a
copy nested on the C stack (`dk_invoke`) and delivering after it.  What
could run the perform's continuation -- and the resume -- twice is on the
chain outward of the handle: a `^multishot` case (`dk_case_multishot`:
`CK_MULTISHOT`, a resumable-payload or shift effect's cloneable `k`, or a
`with-handler` table's cases, whose shape the install does not know), a
prompt other than the root's (a shift's body may invoke its continuation
any number of times), or a pending shift.  Every node carries `ss`, "the
chain past this node holds none of those", computed at `dk_new` from its
`next` (O(1); a copy takes its original's, whose chain outward has every
blocker the copy's has; a node linked later keeps the value it was made
with -- conservative, since linking only appends copies), and `dk_perform`
asks `__dk_ss_after` of the node after the handler group before taking the
in-place branch.  `dk_perform` leaves `H` on the chain's head (`body_env`),
so when the resume is made from a COPY of the case's continuation after all
-- an enclosing non-tail case captured across it, which `ss` allows, since
such a case runs its copy once -- `dk_resume_into` finds the handle's
continuation absent from the chain and copies the perform's frames and the
group once itself (one copy where there were two), continuing into the
copy's rest.

| program | before | after |
| --- | --- | --- |
| `(do (perform (Write y)) (resume k 10))`, a handle a turn, 200,000 turns | 401 MB | 1.4 MB |
| `(+ (perform (Write y)) (resume k 10))`, the same | 345 MB | 1.4 MB |

Pinned by `tests/fixtures/effect-reopen-resume-once-in-place`
(leak-checked; the reap list's length at turns 10 and 900 for the tail
idiom, the non-tail twin, and a rest that captures the perform's answer: 1 /
1 each, against 71 / 6,301, 7,072 / 13,302 and 14,073 / 20,303 before) and
`effect-reopen-resume-once-shapes` (a `^multishot` outer resuming twice, a
non-tail outer -- the fallback copy -- an abortive branch after the perform,
two performs before the resume, two performs of the inner effect in one
body, a loop body: the copying path's answers on both back ends).

Suite, second pass: `run.sh` 3759/0, `run-leak-check.sh` 178 / 0 / 3
known-open, the JIT over the effect, CPS, multishot, handle, async, resume,
defer, shift and reset fixtures 499/0.

## Fixed 2026-10-12, third pass: a delivered copy nothing captured is freed after its run (fix direction 2)

Three copies, each released where its run provably ends, when no node of it
was ever copied (`__dk_chain_copied`: a copy shares its original's envs and
may still run) and nothing pinned DK memory, by taking its entry back off
the reap list (`__dk_reap_take`, a bounded search down from the top) and
freeing the chain:

- **`dk_invoke`'s copy** at the end of its bounded run
  (`__dk_drive_bounded`): registered before the run, as before, so an
  escape that longjmps over the driver still leaves it owned; every chain
  yielded from inside it ran in that loop, so once the loop is over nothing
  runs through it again.
- **`dk_resume_into`'s copy** when its rest-of-case frame runs
  (`copy_tail`: the frame carries the copy's head), or, for a tail resume
  into the handle's own frame, when its last node is walked past
  (`copy_last`).
- **`dk_perform`'s `sub`** at the case's last use of `k` (`k_last`,
  stamped by `emit_resume` on a resume after which nothing names `k`), when
  every use the case made of `k` was a copy -- a `resume`, a copy-on-store
  into a `^mut` (`case_k_copied_only`, `dk_case_k_copied_only`) -- and, for
  a resume that yields to the driver, when nothing outward of the handle is
  multi-shot (`ss`): a resume in a lifted frame of the case is its last use
  on paper, but an enclosing `^multishot` case runs that frame again
  (measured as a use-after-free in `effect-reopen-resume-once-shapes` before
  the `ss` gate).  A case that returns to `dk_perform` frees it there.

| program, 200,000 turns of a handle a turn | before | after |
| --- | --- | --- |
| `(Ask [] ^multishot k) (+ (resume k 1) (resume k 2))` | 447 MB | 148 MB |
| `(Ask [] k) (do (set! saved k) 0)`, then `(resume saved 10)` | 458 MB | 267 MB |

Pinned by `tests/fixtures/effect-copies-freed-after-run` (leak-checked: the
two loops, a once case under a `^multishot` outer, a `^multishot` case
resuming a chain that holds a once-handle, a stored `k` resumed after its
handle exited; the kept-copies compiler's answers, which the interpreter
agrees with).

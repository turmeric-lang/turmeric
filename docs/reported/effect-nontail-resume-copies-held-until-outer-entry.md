# An effect resumed non-tail, or more than once, in a loop keeps its continuation copies until the outermost entry returns

**Severity: low-medium (memory grows for the life of the loop's outermost
call).** What is left of
[fn-value-call-cps-frames-held-until-outer-entry](../archive/fn-value-call-cps-frames-held-until-outer-entry.md):
a handler case that resumes its continuation once, in tail position, now
resumes the original chain in place and allocates nothing that outlives the
turn. Any other case still resumes a COPY, and the copies are kept.

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

| turns | peak RSS (2026-10-09, after resume-into) | before |
| --- | --- | --- |
| 100,000 | 151 MB | 177 MB |
| 200,000 | 300 MB | 352 MB |

About 1,500 B a turn (1,750 before resume-into, 2,250 before 2026-10-09). A
loop of performs under one handle whose case is `(+ 0 (resume k 1))` costs
~1.1 KB a turn. Part of that is inherent there -- each turn's `(+ 0 _)` waits
for the whole loop, a resume-frame and its env -- and the rest is the copies.
That loop no longer nests C frames a turn at -O2 (100,000 turns used to die of
stack overflow); under ASan's -O0-ish frames it still does.

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

## Fix directions

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

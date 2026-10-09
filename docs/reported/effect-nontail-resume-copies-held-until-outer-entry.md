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

| turns | peak RSS |
| --- | --- |
| 100,000 | 177 MB |
| 200,000 | 352 MB |

About 1,750 B a turn (2,250 before 2026-10-09). A loop of performs under one
handle whose case is `(+ 0 (resume k 1))` costs ~1.25 KB a turn, and nests a C
frame per turn as well, since a non-tail resume is not trampolined.

## Where it comes from

`dk_perform` copies the chain from the perform up to the handler (`sub`) and
registers it (`__dk_reap_keep(sub)`); a non-tail `resume` is `dk_invoke`,
which copies `sub` again and runs that copy under a bounded trampoline, which
keeps it too (`__dk_drive_bounded`'s `__dk_reap_keep(ch)`). The originals the
first copy crossed are marked copied, so the join release never takes them.
All of it waits for the outermost entry.

## Fix directions

1. **One-shot non-tail resume in place.** A case that resumes `k` once,
   non-tail, still needs the continuation to stop at the handle and return
   to the case. The copy ends there by construction (`done` after the
   re-installed handler); running the original would need the handler node
   to act as a stop for the duration of the resume -- counted, since the
   resumed part can re-enter the same handler.
2. **Free a delivered copy.** Once a one-shot `dk_invoke` has returned and
   no capture was taken of the copy it ran (the per-node copy count,
   `DK.ncopy`, says so), the copy is dead: free it then instead of keeping it.
   The originals still need (1) or a similar proof.

# TUR-E0500 checks a `^multishot` clause's captures, not the continuation that actually runs twice

**Severity:** medium (soundness hole in the ownership checks). A `^linear` or
move-only value that the handled body consumes after a `perform` is consumed
once per resume of a `^multishot` `k`, with no diagnostic. Meanwhile the clause,
which runs once per `perform`, is the only code TUR-E0500 inspects.

Filed 2026-10-25, found while documenting `^multishot` in the effects guide.

## Repro

```turmeric
(defeffect Ask [] : int)

(defn close! [^linear h : int] : int
  (do (println "close") h))

(defn body [] : int
  (let [^linear h 7]
    (let [v (perform (Ask))]
      (+ v (close! h)))))

(defn main [] : int
  (println (handle (body) (Ask [] ^multishot k) (+ (resume k 1) (resume k 2))))
  0)
```

It compiles cleanly and prints `close` twice, then `17`, on every engine. The
linear `h` was consumed twice. With a plain `(defstruct D [x : int])` value
moved into a function after the `perform`, the same happens: no error, and the
value is used once per run.

For comparison, the clause-side shape is refused:

```turmeric
(defstruct D [x : int])
(let [d (D 42)]
  (handle (perform (Ask)) (Ask [] ^multishot k) (+ (resume k (.x d)) (resume k 1))))
;; error [TUR-E0500]: ^multishot handler captures 'd' which is unique (move-only)
```

There the clause reads `d` once.

## Root cause

`src/compiler/elab_effects.c:2593` (MS2) runs `collect_free_vars` over
`cases[i].body`, the **clause**, and rejects any `CK_UNIQUE` / `CK_LINEAR`
capture. Its comment says "the handler body may run multiple times -- each
resume takes a snapshot of k, so the body executes N times". But what a resume
re-runs is the continuation: the rest of the handled body from the `perform`
outward to the `handle`. The clause runs once per `perform`, as it does for a
one-shot `k`. A deep handler's clause can run several times for several
`perform`s, but that holds whether or not `k` is `^multishot`, and fresh
parameters are bound each time.

So the rule both refuses safe programs (a clause that reads a struct once) and
accepts unsafe ones (a continuation that consumes a linear value once per run).

## Fix directions

1. Check the continuation. Any `^unique`/`^linear` binding still live at a
   `perform` that a `^multishot` clause can answer is duplicated by a second
   resume, so it should be refused (or required to be dead at the `perform`).
   The effect row says which `perform`s a handle answers, but they can be in
   callees. A conservative first step: refuse when the handled body, inlined
   or not, holds a live linear binding across a `perform` of the effect.
   Across function boundaries this needs a summary, for example "this
   function holds a linear value across a `perform` of E".
2. Then decide whether the clause-side rule should stay. It is not needed for
   `^multishot` soundness, and it refuses ordinary struct reads. Relax it to
   "the clause consumes a move-only capture", which ordinary linearity
   already reports as TUR-E0101 on a second use.
3. Fix the `tur explain TUR-E0500` rationale to match.

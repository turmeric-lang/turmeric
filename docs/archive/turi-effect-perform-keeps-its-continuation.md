# `tur --interpret`: every `perform` keeps its continuation, handler frame and the frames it captured (~2.8 KB a perform)

**RESOLVED 2026-10-07** by fix direction 1, the one-shot fast path, for the
shape it names. `ws_case_is_oneshot_resume` (`src/turi/eval.c`) recognizes a
clause whose body is exactly `(resume k v)` with an inert `v` -- literals,
variables other than `k`, and builtin operators over them, so nothing in it
can perform (no outer multishot handler can re-run the clause) or capture
`k` -- and a `k` that is not `^multishot`. For such a clause the work-stack
perform:

- copies the slice into a malloc'd array and leaves it as it was: its frames
  are not marked escaped and its argument accumulators stay the driver's own;
- allocates the `TuriWsCont` and the `k` value with malloc, and the case frame
  as a reclaimable call frame;
- on the resume, pushes the slice back IN PLACE (no `clone_ws_slice`),
  re-arms the clause's own disabled prompt when the resume sits directly on
  it (so the work stack stays flat) instead of stacking a new one, releases
  the case frame and frees the copy, the continuation and `k`.

It is off while a re-entrant `call/cc` continuation exists, under a debugger,
and with `TUR_TURI_FRAME_RECLAIM=0`; every other clause shape keeps the
cloning path. A second resume of a one-shot `k` (which the static shape rules
out) is an error rather than a re-run of a consumed slice.

Measured on the repro (Release): **200,000 performs peak at 31 MB, from 556
MB**, in 0.16 s. Compiled and `--interpret` agree on nested handlers, two
performs in one expression, a parameterized clause, a clause that calls a
function (slow path) and a `defer` inside the handled body. Pinned by the
fourth program in `tests/check-turi-frame-reclaim.py` (100,000 performs,
growth bound 16 MB; 0 MB on Debug and Release). `tests/run-turi.sh` (2652),
the session/REPL/embed ctests and `tests/run-r7rs-import.sh` are green.

**Not done:** directions 2 and 3 for every other clause shape -- a clause that
does more than resume, a `^multishot` `k`, or a `k` that escapes still keeps
its continuation for the life of the process.

**Severity:** medium (interpreter memory). Under `tur --interpret`, an
effect that is performed and resumed in a loop grows memory by ~2.8 KB per
perform for the whole run. The call-frame reclamation of 2026-10-05
([turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md))
does not reach it: a captured slice marks every frame in it as escaped, which
is correct, because nothing knows when the continuation is dead. Found
2026-10-05.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn ask-loop [n : int acc : int] : int
  (if (= n 0) acc (ask-loop (- n 1) (+ acc (perform (Ask))))))
(defn run [n : int] : int
  (handle (ask-loop n 0)
    (Ask [] k) (resume k 1)))
(defn main [] : int
  (println (run 200000))
  0)
```

```sh
tur --interpret ask.tur                              # 200000, 556 MB (Release)
TUR_TURI_FRAME_RECLAIM=0 tur --interpret ask.tur     # 200000, 556 MB: no difference
```

With 50,000 performs the peak is 144 MB (massif, Release). The arena slabs
were last grown by three sites in roughly equal parts:

- `turi_ws_cont_val` (eval.c:7607), the `k` value;
- the slice copy in the work-stack capture (eval.c:~9281);
- `frame_bind` of the handler case frame's parameters and `k`
  (eval.c:~9332).

## Root cause

The work-stack perform path in `eval_drive_ex` (the `TuriWsCont` capture,
src/turi/eval.c around 9270-9340) does five things per perform:

- allocates a `TuriWsCont` and a copy of the slice above the prompt in the
  value pool;
- re-homes the slice's argument accumulators into the pool;
- marks every frame in the slice, and the handler frame, as escaped (the
  2026-10-05 change);
- makes a fresh handler case frame `hf` with `eval_frame_new`, which is
  never reclaimable;
- deep-copies the slice again on resume (`clone_ws_slice`), so a multishot
  `k` gets independent runs.

The interpreter has no collector, so it never learns that `k` was resumed
once and dropped. Everything above lives until `turi_env_free`.

## Fix directions

1. **A one-shot fast path.** When the clause's body resumes `k` exactly once,
   in tail position, and `k` does not otherwise escape the clause, run the
   captured slice in place without cloning it. The elaborator can tell this
   statically, and it is by far the common shape (`(resume k v)` as the whole
   body). Then free the `TuriWsCont`, the slice copy and the handler frame
   when the resume completes, and leave the slice's frames unmarked so their
   activations release them normally.
2. **Count references to `k`.** A `TuriWsCont` is reachable only through the
   `k` binding and the values it is copied into. Count those copies. When the
   handler frame is released and the count is zero, reclaim the continuation
   and un-escape its frames (or release them directly).
3. **At least reclaim the handler case frame.** Link `hf` to the prompt's
   activation, like `eval_frame_new_owned`, when `k` provably does not leave
   the clause.

## Pinned by

Nothing yet. A per-perform RSS bound on the repro above, against
`TUR_TURI_FRAME_RECLAIM=0`, would pin direction 1.

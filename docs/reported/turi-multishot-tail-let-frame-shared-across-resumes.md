# turi: a `^multishot` resume shares a function's tail-`let` locals with the other copies

**Severity:** medium-low (engine disagreement, silent wrong value under
`tur --interpret` only). Each resume of a `^multishot` `k` should run its own
copy of the rest of the handled body, so a local bound inside the body starts
from its value at the `perform` in every run. Compiled and `tur jit` do that.
The interpreter shares the local when its `let` is the tail form of a
function, so a second run sees the first run's writes.

Filed 2026-10-25, found while checking how `^multishot` resumes see state (the
effects guide's "What each run sees").

## Repro

```turmeric
(defeffect Choose [] : int)

(defn tail-let [] : int
  (let [^mut s 1]
    (let [v (perform (Choose))]
      (set! s (+ s v))
      s)))

(defn nontail-let [] : int
  (+ 0 (let [^mut s 1]
         (let [v (perform (Choose))]
           (set! s (+ s v))
           s))))

(defn main [] : int
  (println (handle (tail-let)
             (Choose [] ^multishot k) (+ (* 1000 (resume k 10)) (resume k 20))))
  (println (handle (nontail-let)
             (Choose [] ^multishot k) (+ (* 1000 (resume k 10)) (resume k 20))))
  0)
```

| Engine | `tail-let` | `nontail-let` |
| --- | --- | --- |
| compiled / `tur jit` | `11021` | `11021` |
| `tur --interpret` | `11031` | `11021` |

`11021` is `11 * 1000 + 21`: each run adds its value to `s = 1`. `11031` means
the second run started from the first run's `s = 11`. Two nested tail `let`s
share both of their locals (`23063` against `23043`). A `let` written directly
in the handle body, or one whose binding performs, is copied correctly.

## Root cause

`clone_ws_slice` (`src/turi/eval.c:8912`) copies only the frames that a
captured `DriveCont` names (`DK_LET_BIND`, `DK_LET_BODY`, `DK_MATCH_BODY`,
`DK_CALL_RET`). `clone_frame_bindings` (`src/turi/eval.c:8883`) keeps each
clone's `parent` pointing at the original unless that parent was itself
cloned.

A `let` in tail position takes the F1 path (`src/turi/eval.c:10718`, and the
`DK_LET_BIND` pop at `src/turi/eval.c:12091`). It descends into the body with
no `DriveCont` owning its frame, so the frame is reachable only as the
`parent` of the inner `let`'s frame. The inner frame is cloned per resume, but
its parent, which holds `s`, is the same frame in every copy. A `set!` there
writes the shared binding.

The fixed-size map (`keys[64]`) also stops cloning past 64 owned frames
without saying so.

## Fix directions

1. When cloning, walk each cloned frame's `parent` chain up to the frame the
   handle body started in (the prompt's frame), cloning every frame on the
   way that the slice does not already own. That frame is the boundary:
   frames outside the handle must stay shared.
2. Or give the F1 tail-`let` a frame owner on the work-stack that the capture
   can see, at the cost of the tail-position saving.
3. Replace the 64-entry map with a growable one, or abort loudly when it
   overflows.
4. Add the repro to `tests/fixtures/` so `run-turi.sh` checks it on both
   engines.

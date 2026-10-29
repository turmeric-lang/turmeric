# The interpreter REPL resumes a stored continuation from a later turn through freed frames

**Severity:** medium (use-after-poison at the `tur repl` prompt; the Release
build happens to print the right answer). Found 2026-10-29 while deciding what
the compiled REPL does with a continuation that crosses turns
(aot-compiled-repl-plan C2).

## Repro

```turmeric
(defeffect Ask [] : int)
(defn w4 [] : int (+ 1 (perform (Ask))))
(def ^mut saved 0)
(handle (w4) (Ask [] k) (do (set! saved k) 0))
(resume saved 5)
```

Typed one form per turn into `tur repl` on the Debug (ASan) build:

```
=> 0
==ERROR: AddressSanitizer: use-after-poison ...
    #0 frame_release src/turi/eval.c:8561
    #1 eval_drive_ex src/turi/eval.c:12560
    ...
allocated by thread T1 here:
    #1 slab_new src/runtime/arena.c:121
```

The Release build prints `=> 6`. The same program in one file, run with
`tur --interpret`, is right on both builds: the resume happens before the
handle's turn ends (tests/fixtures/effect-stored-k-with-defer-resumed-after-handle).

## Root cause (partly confirmed)

The resumed continuation's activation frames were allocated in an arena slab
that belongs to the turn that captured `k`, and that slab is poisoned (reset)
when the turn ends. Resuming from a later turn walks those frames, and
`frame_release` reads one (`f->reclaimable` / `f->escaped`) after the poison.
Which arena is reset at a turn boundary, and why the capture's frames are not
marked escaped (`frame_escape`) when `k` is stored into a global, is not yet
pinned down.

## Fix directions

- Mark the frames a continuation holds as escaped when the continuation is
  stored anywhere that outlives the turn (a global's `set!`), so they move to
  storage that lives for the session.
- Or keep the turn arena of any turn whose continuations are still reachable.

## What the compiled REPL does instead

`--enable=compiled-repl` refuses the store at compile time (`a continuation
stored in the top-level saved would outlive this REPL turn`), since the
compiled backend's reaper frees a stored chain when the turn's entry returns.
Resuming within the same turn works on both engines.

## Resolution (2026-10-29)

The partly-confirmed cause was close. The continuation in both repros is a
fiber continuation (`ws == NULL`: `w4`'s `perform` has no work-stack prompt
to capture). Its `TuriEffectCont` is `calloc`'d off the value pool, so
`promo_check` (`src/turi/eval.c`) let it through the turn boundary's
promotion ("already off scratch"). But a suspended fiber's body is C frames
on its own stack, holding the `EvalFrame`s of the activation it suspended,
and those were scratch. The rewind poisoned them (the `f7` ASan bytes were
the arena's, not the frame free list's), and the resume's `frame_release`
read one. `TUR_TURI_FRAME_RECLAIM=0` did not help, which ruled the free
list out.

A fiber continuation that is not `done` now refuses the rewind, as a started
generator does: the turn keeps its scratch while such a continuation lives,
the second fix direction above. A finished one passes as before.

Pinned by `tests/turi/repl-smoke.sh` (ctest `tur_repl_smoke`): the report's
`set!` form, the same continuation `def`'d directly, and a later turn after
the resume. On the Debug build the old code aborted before the `=> 6`.

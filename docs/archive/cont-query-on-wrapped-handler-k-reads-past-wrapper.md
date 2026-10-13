# `(cont? k)` on a fn-payload case's `k` reads past the end of its wrapper

**Fixed 2026-10-13.** The repro prints `101` and is clean under ASan.
`tests/fixtures/cont-query-wrapped-handler-k` pins it and is opted into the
leak gate (`requires.leak-check`), where the old read was a
heap-buffer-overflow.

- **The fix (fix direction 1, the runtime half).** Beside `__dk_resume_word`,
  `emit_dk_resume_word_prelude` (`src/compiler/emit_dk_runtime.c`) now emits
  `__dk_cont_live_word`. It checks a word for `__dk_cont_fn` first, the way
  `__dk_resume_word` does, and reads `consumed` only off a bare chain. Both
  `cont?` lowerings call it: the CPS `TUR_DK_CONT_PRED` arm
  (`src/compiler/emit_cps_ir.c`) and the direct emitter's
  `emit_effects_cont_pred` (`src/compiler/emit_effects.c`). A runtime check
  rather than `Binding.cont_is_wrapper` covers a `k` that reached a helper,
  where the binding no longer says which shape it is.
- **Fix direction 2: a wrapper is live.** `__dk_cont_fn` `dk_invoke`s a copy
  of the wrapped chain, so that chain is never consumed and the wrapper stays
  resumable. `(cont? k)` on a wrapper is `true`, before and after a resume.
  `__dk_resume_word` never marked a wrapper either.
- **A second bug, found on the way.** `cont?` on an `effect-cont` parameter
  (`(defn pick [k : effect-cont] : int (if (cont? k) (resume k 5) 0))`) did
  not link. The direct emitter called `tur_cont_consumed` for every `TY_CONT`
  and `tur_effect_cont_valid` for an `int` k. The second was deleted with the
  fiber effect runtime on 2026-07-19, and the first is never in an emitted
  program. Both now go to `__dk_cont_live_word`. Only a non-effect cont
  flavor still reaches `tur_cont_consumed`.
- **Left open.** `(cont? 5)` used to fail to link and now segfaults, as
  `(resume 5 v)` does. That is
  [resume-of-non-continuation-int-crashes](../reported/resume-of-non-continuation-int-crashes.md).

The original report follows.

**Severity: medium.** This is an out-of-bounds heap read, and the answer is
whatever byte is there. A fn-payload case's `k` is not a DK chain but the
32-byte `tur_cloneable_cont` around one, and `cont?` reads it as a `DK`.
Found 2026-10-13 probing a gap left by
[lexical-shift-receiver-cont-never-freed](../archive/lexical-shift-receiver-cont-never-freed.md).
Neither half changed on that branch: the wrapping (`case_is_multishot`'s
fn-payload arm) and the `cont?` lowering both predate it. It was not re-run
on `main`.

## Repro

```turmeric
(defeffect ShiftE [f : (fn [effect-cont] int)] : int)
(defn producer [] : int (+ 100 (perform (ShiftE (fn [k : effect-cont] : int (k 1))))))
(defn main [] : nil
  (println (handle (producer) (ShiftE [f] k) (if (cont? k) (f k) 0))))
```

It prints `101`, the same as `tur --interpret`, but only because the stray
byte happened to be 0. Built the way `tests/run-leak-check.sh` builds:

```
ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 1
    #1 ... in dk_perform
```

An ordinary case's `k` is a bare chain, and the same check is clean:
`(handle (+ 1 (perform (Ask))) (Ask [] k) (if (cont? k) (resume k 5) 0))`.

## Root cause

The CPS backend lowers `(cont? k)` to `!((DK *)(intptr_t)(k))->consumed`
(`src/compiler/emit_cps_ir.c`, the `cont?` arm near the `consumed` read).
The `k` of a fn-payload or `__Shift` case is bound as the cloneable wrapper.
That is the `tur_cloneable_cont_alloc(__dk_cont_fn, <kenv>, ...)` in the
handler-case prologue, and `Binding.cont_is_wrapper` marks such a `k`. The
wrapper is 32 bytes, and `consumed` sits further into a `DK`.

`emit_resume` already tells the two apart for a resume. That is how this
branch fixed a fn-payload case resuming its own `k`. `cont?` was never given
the same treatment.

## Fix directions

1. For a `k` whose binding is `cont_is_wrapper`, read the wrapped chain:
   `!((DK *)((tur_cloneable_cont *)(intptr_t)k)->env)->consumed`. Or, for a
   `k` that reached a helper where the binding is unknown, add a
   `__dk_cont_live_word` beside `__dk_resume_word` that checks for
   `__dk_cont_fn` first.
2. Decide what `consumed` means for a multi-shot `k`. A wrapper is resumed
   through `__dk_cont_fn`, which `dk_invoke`s a copy, so the wrapped chain is
   never marked consumed. A payload `k` may be resumed any number of times,
   so "live" may simply be the answer.
3. Add the repro as a fixture under `requires.leak-check`, where ASan sees
   the read.

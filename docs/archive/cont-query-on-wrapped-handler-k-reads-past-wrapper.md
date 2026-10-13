# `(cont? k)` on a fn-payload case's `k` reads past the end of its wrapper

**Fixed 2026-10-13** along fix directions 1-3 below.

- **`cont?` asks the runtime which shape it has.** `__dk_cont_live_word`
  sits beside `__dk_resume_word` (`emit_dk_resume_word_prelude`,
  `src/compiler/emit_dk_runtime.c`) and tells the two apart the same way:
  a wrapper's first word is `__dk_cont_fn`. Both back ends now lower
  `(cont? k)` to it: the CPS backend's `TUR_DK_CONT_PRED` arm
  (`src/compiler/emit_cps_ir.c`) and the direct emitter's
  `emit_effects_cont_pred` (`src/compiler/emit_effects.c`). So a `k` that
  reached a helper is covered too, where the binding cannot say which
  shape arrived.
- **A wrapper is live (direction 2).** It is resumed through a copy of its
  chain any number of times, and the interpreter answers true for any
  continuation. A chain is live until a resume marks it consumed, as
  before. A 0 word answers false.
- **Fixed on the way:** `cont?` in a helper that the CPS backend does not
  lower went to the direct emitter. That emitter called
  `tur_effect_cont_valid` for an `int` `k`, which was deleted with the fiber
  effect runtime. For an `effect-cont` it called `tur_cont_consumed`, which
  belongs to a Phase 18 `tur_cont` and is declared by no emitted program.
  Either way the program failed to build.

`tests/fixtures/cont-query-on-wrapped-handler-k` (`requires.leak-check`)
covers the repro, a wrapper asked after its payload resumed it, a bare
chain, and both shapes asked in a helper. It prints the same on the cc path,
the JIT and `--interpret`, and is clean under the ASan leak gate.

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

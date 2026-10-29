# On the browser JIT, a list comparison's answer varies between runs in one module

**Severity:** medium (browser JIT only). Two corpus fixtures print `false`
where they expect `true`, on some runs and not others, with no diagnostic.
Native `tur jit` (generator and `TUR_JIT_GEN=interp`) is right every time.

Filed 2026-10-28, found by wasm-jit-plan P5's corpus run: `typed/list-concat`
and `eq-carrier-capturing-comparator` were the two fixtures whose answer went
wrong on the P5 module. Neither is a P5 regression: the module built from
`main` before P5 gets them wrong too, on alternate runs.

## Repro

```sh
cmake -S . -B build-wasm -DTUR_WASM=ON && cmake --build build-wasm --target tur_wasm_jit
node tools/wasm-jit/run.js --repeat 4 tests/fixtures/typed/list-concat/input.tur
# true true true 5  false true true 5  true true true 5  false true true 5
```

The first of the four tests (`list-eq?` of `(list-concat (list) (list 1 2))`
against `(list 1 2)` through a `(fn [a b] (= a b))` comparator) flips between
runs in one module instance. Which runs flip, and which of the four tests,
moves with the module's layout: on the P5 module the first run is already
wrong, and an earlier build printed `false false ... 6`.

## What is known

- The comparator is a captureless lambda boxed as a fat value
  (`TUR_FATBOX_DEF(..., __tur_fatshim_bool_int64_t_int64_t, __fn_9)`), and
  `list-eq?` is CPS-emitted: the comparator's `bool` comes back through
  `dk_run` into a resume frame that reads it as `(bool)(__t3__slot)`.
- Not the causes (each ruled out by building without it): the DK word
  (`tur_dkw`), the exact-signature call stubs, `setjmp` in `eval`, the
  whole-register address fix, the inliner's no-return guard.
- Varies with run count and layout, so something reads state a previous run
  or a previous frame left behind: a register's upper half, a buffer, or a
  reused thunk.

## Fix directions

- Narrow which value goes wrong: `--dump-c` the unit, then add `fprintf`s to a
  copy of the program's inline-C (the comparator's result, the slot the resume
  frame receives, `list-eq?`'s arguments) and diff a good run against a bad one
  under `--repeat`.
- Suspects, in order: an interpreted `bool` result widened without clearing
  the upper bits somewhere other than `call()` (the thunk path through
  `MIR_interp_arr`, the JS trampoline's `i` marshal); a thunk table entry
  reused across runs while a box still holds the old address.

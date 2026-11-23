# On wasm32, three CPS runtime reads assumed a pointer is 8 bytes

**Status 2026-11-23: RESOLVED.** Three reads in the emitted CPS code and its
runtime treated a pointer-sized slot as a 64-bit word. On LP64 the two are the
same bits; on wasm32 (the browser JIT, ILP32) each read the wrong bytes. Nine
fixtures that failed through `tests/run-jit-wasm.sh` with no baseline row now
come out `compiled`, and so do ten whose row said `fail`.

**Severity:** high for the browser JIT, none natively. An effectful capturing
callback aborted (`unhandled effect`, `no CPS entry registered`, or `await: a
pending future would park below a direct-style call`), and a `^multishot`
handler over a `^mut` local printed wrong numbers with no diagnostic.

Found 2026-11-23 while triaging
[jit-wasm-baseline-stale-and-not-in-ci](../reported/jit-wasm-baseline-stale-and-not-in-ci.md).
Same family as
[ilp32-word-slots-truncate-64-bit-carriers](../reported/ilp32-word-slots-truncate-64-bit-carriers.md).

## Repro (before the fix; emcc 6.0.10, node 22)

```sh
node tools/wasm-jit/run.js tests/fixtures/cps-set-value-performs/input.tur
```

Line 10 printed `5050`; `tur run`, `tur jit` and `tur --interpret` print
`50100`.

## Root causes and fixes

1. **The fat dispatcher read a closure env's slot 0 as an `int64_t`**
   (`emit_e2a_fat_dispatch`, `src/compiler/emit_cps_ir.c`). A capturing
   closure's env starts with its lifted entry, a function pointer. On wasm32
   that is 4 bytes followed by 4 bytes of padding that `malloc` left
   uninitialised, so `__tur_cps_lookup(__e2ab[0])` missed the registry and the
   dispatch fell through to slot 1, a capture. Slot 0 is now read as a pointer:
   `(tur_dkw)(intptr_t)*(void **)__e2ab`.
2. **The env-taking CPS twin made the same read**
   (`((int64_t *)__pwe)[0]`, `src/compiler/emit_module.c`). Same fix.
3. **A multishot resume did not give its copy its own cells.** Two defects in
   one path:
   - the emitted `<name>_env_CELLS` mask numbered a cell field by
     `offsetof / 8`, while `__dk_cells_fresh` walks the env in `void *` words,
     so on wasm32 the bit named the wrong word and the cell stayed shared. The
     mask now divides by `sizeof(void *)`, and a word past bit 31 stays shared
     instead of shifting out of range;
   - `__dk_cells_fresh` read a cell's size as `((const size_t *)old)[-2]`.
     `__dk_cell_new` writes it 16 **bytes** before the value, which is two
     `size_t` only on LP64. On wasm32 the read landed on uninitialised header
     bytes, which is where `memory access out of bounds` came from. It now
     reads `*(const size_t *)((const char *)old - 16)`.

The second half of 3 is in the runtime preamble, so
`src/runtime/generated/tur_rt_split*` and 162 `expected.c` snapshots were
regenerated in the same change.

## Fixtures that now compile through the browser JIT

| Fixture | Before | Cause |
| --- | --- | --- |
| `cps-effectful-callback-wide-aggregate` | `no CPS entry registered for effectful fn-value 'h'` | 1 |
| `generic-hof-unannotated-fat-param-mixed` | `unhandled effect: E` | 1 |
| `cps-effectful-capturing-callback-narrow-result` | `unhandled effect: Ask` | 2 |
| `cps-effectful-capturing-callback-ptr-void-arg` | `unhandled effect: Ask` | 2 |
| `await-through-fn-value` | `await: a pending future would park below a direct-style call` | 2 |
| `await-in-generic-capturing-lambda-struct` | same | 2 |
| `cps-set-value-performs` | `5050` for `50100` | 3 |
| `cps-multishot-cell-per-run-narrow-and-struct` | wrong output | 3 |
| `cps-multishot-cell-per-run` | wrong output, then `memory access out of bounds` | 3 |

## Baseline rows that could go

A full run after the fix (3309 fixtures: 2621 compiled, 191 interpreted, 208
failed, 249 skipped, exit 0) also reported ten fixtures whose baseline row
said `fail` and which now come out `compiled`. Their rows are removed from
`tests/jit-wasm-baseline.txt`:

`await-below-evicted-caller-or-nontail-resume`,
`await-below-struct-capturing-lambda`, `await-in-generic-capturing-lambda`,
`cps-effectful-callback-aggregate-args`,
`cps-effectful-capturing-callback-scalar-kinds`,
`cps-effectful-capturing-closure-callback`,
`cps-effectful-closure-through-unannotated-param`,
`cps-local-closure-value-and-call`, `effect-capturing-closure-thin-param`,
`generic-hof-effectful-callback`.

## Verification

- The nine fixtures: `compiled` through `tests/run-jit-wasm.sh`.
- `bash tests/run.sh`: 4328 passed, 0 failed.
- `bash tests/run-jit.sh`: 4180 passed, 0 failed, 71 skipped.

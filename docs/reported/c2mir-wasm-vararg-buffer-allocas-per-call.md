# On wasm32, every variadic call c2mir compiles allocates stack that lasts until the function returns

**Severity:** high for the planned wasm JIT (wasm-jit-plan), none today --
nothing ships it yet. `(println i)` on an int is `printf("%lld\n", ...)`
(`src/compiler/builtins.c:410`), and the emitter turns self tail calls into
loops. So a Turmeric loop that prints once per iteration grows the linear stack
16 bytes per iteration, until the function returns. At `tur_wasm`'s 16 MiB
stack, that is about one million prints in one function before the stack
overflows. The overflow failed as
`RuntimeError: null function or function signature mismatch`, not as a clean
stack-overflow trap, which suggests memory was corrupted.

Filed 2026-10-24, found while checking the wasm residue of
[mir-interp-tail-calls-grow-the-stack](../archive/mir-interp-tail-calls-grow-the-stack.md)
(see [wasm-mir-interp-tail-call-keeps-allocas](wasm-mir-interp-tail-call-keeps-allocas.md)).
Native targets are not affected: there c2mir passes the variadic tail as call
arguments.

## Repro (measured, emcc 6.0.13, node 22)

Use the same spike build as the sibling report, which adds `sp_now`. Then
interpret:

```c
int printf (const char *, ...);
int pf (int n) { long long b = sp_now (); for (int i = 0; i < n; i++) printf ("", i); return (int) (b - sp_now ()); }
```

| n | stack grown (bytes) |
| --- | --- |
| 10 | 160 |
| 1000 | 16000 |
| 100000 | 1600000 |
| 2000000 | `RuntimeError: null function or function signature mismatch` |

## Root cause

On `VA_BUF_TARGET_P` targets (`c2mir/wasm32/cwasm32.h:26`, wasm32 only), c2mir
builds a variadic call's tail in a buffer and passes its address. It emits that
buffer as a `MIR_ALLOCA` at the call site (`external/mir/c2mir/c2mir.c:14700`),
so the alloca runs on every execution of the call. Nothing frees it: c2mir
emits no `MIR_BSTART`/`MIR_BEND` around it, and on wasm those are no-ops anyway
(sibling report). The interpreter's `MIR_ALLOCA` is a C `alloca` in `eval`'s
frame (`mir-interp.c:1794`), so every buffer stays until the interpreted
function returns.

## Fix directions

- In c2mir, size one va buffer per function to the largest variadic tail in
  that function, and put it in the function's frame (the prologue `MIR_ALLOCA`
  at `c2mir.c:14882`, `ns->size`) or in a single prologue alloca. Each call
  site then fills that buffer at its own offset 0. The buffer is dead once the
  call returns, so one shared buffer is safe. This needs no working `bend` on
  wasm.
- Pin it in the spike with the loop above (growth 0 for any n), and in the
  wasm JIT's corpus with a print loop of at least 2e6 iterations once P1 runs
  fixtures.
- Batch it into the fork's one open PR (CLAUDE.md "MIR fork" rule).

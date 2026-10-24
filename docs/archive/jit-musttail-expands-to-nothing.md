# `TUR_MUSTTAIL` is empty under `tur jit`, so mutual tail calls grow the stack there

**Status 2026-10-24: RESOLVED for the generator.** The MIR fork learned a
tail call (turmeric-lang/mir#13): `MIR_TCALL` takes `MIR_CALL`'s operands and
returns the call's results, and on x86-64 and aarch64 the generator makes one
whose args and results are all in registers the epilogue plus a jump. c2mir
generates it for `__attribute__ ((musttail)) return f (...);` and defines
`__MIRC_MUSTTAIL__`, which `TUR_MUSTTAIL` now keys on
(`ensure_musttail_macro`, `src/compiler/emit_module.c`). `tailcall-musttail-deep`
prints `5` under `tur jit` on an 8 MiB entry stack, and `tests/run-jit.sh`'s
`jit-musttail-8mib` (the flipped `jit-musttail-gap-pinned`) checks that. The
MIR interpreter -- `TUR_JIT_GEN=interp`, and the planned browser engine --
still runs a tail call as a call and a return; that half is
[mir-interp-tail-calls-grow-the-stack](../reported/mir-interp-tail-calls-grow-the-stack.md).

**Earlier status 2026-10-23: documented and pinned; the fix itself is fork work.**
The macro's emitted comment and the JIT guide's `TUR_MAIN_STACK_MB` row say
that `tur jit` makes no guaranteed tail calls and that its entry stack is what
hides it, and `tailcall-musttail-deep`'s own comment no longer claims MIR-gen's
code does not overflow (it does, on 8 MiB). `tests/run-jit.sh` now runs that
fixture on an 8 MiB entry stack after the corpus and expects the overflow
(`jit-musttail-gap-pinned`); it fails, asking for this report to be closed,
the day the cycle runs there. What is left is direction 1 below: a tail-call
form in MIR/c2mir, batched into the fork's one open PR.

**Severity:** low natively, high for the planned wasm JIT. Found 2026-10-23
running the fixture corpus under `TUR_JIT_GEN=interp` for
[wasm-jit-plan](../upcoming/wasm-jit-plan.md) P0.

## What happens

The emitter spells a guaranteed tail call as `TUR_MUSTTAIL return f(...);`
(proper-tail-calls T2b). The macro is defined once, in
`emit_musttail_macro` (`src/compiler/emit_module.c:10988-11016`), as
`__attribute__((musttail))` only when `__clang__` is defined, `__wasm__` is
not, and the target is x86-64 or aarch64 -- and as **nothing** otherwise.

c2mir defines `__MIRC__`, not `__clang__`, so under `tur jit` every
`TUR_MUSTTAIL` call is an ordinary call. A mutual tail-call cycle the T5
fusion does not collapse therefore runs in O(n) stack under the JIT. Nothing
notices, because the engine runs `main` on a 1 GiB thread
(`TUR_MAIN_STACK_MB`, `src/jit_engine.c:1216`): `tailcall-musttail-deep`
passes under `tur jit` by having room, not by making tail calls.

The same macro arm also excludes `__wasm__`, so `tur build --target wasm`
gets no tail calls either; that half is in
[wasm-target-stack-is-emcc-default](../archive/wasm-target-stack-is-emcc-default.md).

## Repro

```sh
TUR=./build-rel/tur TUR_TEST_FILTER='^tailcall-musttail-deep$' bash tests/run-jit.sh                        # PASS
TUR=./build-rel/tur TUR_JIT_GEN=interp TUR_TEST_FILTER='^tailcall-musttail-deep$' bash tests/run-jit.sh     # FAIL
```

The second prints `stack overflow: recursion too deep` and exits 139: the
interpreter's frames are larger than the generator's (a 1-million-deep
non-tail recursion that the generator survives on the 1 GiB thread also
overflows under `TUR_JIT_GEN=interp`), so the same O(n) stack runs out.
Under the generator the fixture only passes because 1 GiB is enough.

## Why it matters for wasm

The planned browser engine is the MIR interpreter with no 1 GiB thread: the
measured ceiling is **1,696** interpreted frames (`tools/wasm-jit-spike/`).
Every mutual tail-call cycle the emitter did not fuse becomes a recursion
bounded by that, with no warning at compile time.

## Fix directions

- **c2mir has no general tail call to spell the macro with.** Its
  `__builtin_jcall (func, ...)` is a direct jump whose callee must return
  through `__builtin_jret (addr)` (fork `c2mir/README.md`, `c-tests/new/
  jcall.c`): a threaded-code primitive, not a `return f(args);` the caller's
  caller receives. A real `musttail` would be a MIR/c2mir feature in the
  fork (a tail-call form of `MIR_CALL` the interpreter and the generators
  honour), batched into the fork's one open PR.
- **Say so in the JIT guide and in the macro's comment**: today the comment
  reads "where the C compiler can promise one, and an ordinary `return
  f(args);` elsewhere", which is true, but nothing records that "elsewhere"
  includes the JIT and that the entry thread is what hides it.
- **`tests/run-jit.sh` could pin the gap**: run `tailcall-musttail-deep`
  with `TUR_MAIN_STACK_MB=8` under the JIT and expect the overflow, so the
  day the fork learns tail calls the test flips and says so.

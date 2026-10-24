# The MIR interpreter runs a tail call as a call, so `musttail` cycles grow its stack

**Severity:** low natively (`TUR_JIT_GEN=interp` is a debugging mode), high
for the planned wasm JIT, whose engine is this interpreter. Split off
2026-10-24 from
[jit-musttail-expands-to-nothing](../archive/jit-musttail-expands-to-nothing.md),
whose generator half the MIR fork fixed (turmeric-lang/mir#13).

## What happens

Under `tur jit`, `TUR_MUSTTAIL return f(...);` is
`__attribute__((musttail))`, and c2mir makes the call a `MIR_TCALL`. The
generator turns that into the epilogue and a jump. The interpreter does not:
`generate_icode` (fork `mir-interp.c`) lowers a `MIR_TCALL` to its `CALL`
icode followed by a `RET` of the results, and an interpreted call recurses on
the C stack through `call_insn_execute`. A mutual tail-call cycle the T5
fusion did not collapse is therefore O(n) stack under the interpreter, as it
was everywhere under the JIT before mir#13 -- and an interpreted frame is over
1 KiB.

## Repro

```sh
TUR_JIT_GEN=interp TUR_MAIN_STACK_MB=64 ./build/tur jit tests/fixtures/tailcall-musttail-deep/input.tur
# stack overflow: recursion too deep
TUR_MAIN_STACK_MB=8 ./build/tur jit --debug tests/fixtures/tailcall-musttail-deep/input.tur
# 5  (the generator)
```

`tests/run-jit.sh`'s `jit-musttail-8mib` SKIPs under `TUR_JIT_GEN=interp`
for this reason.

## Fix directions

- In the fork's interpreter, execute an `IC_IMM_CALL` that came from a
  `MIR_TCALL` to an interpreted MIR function by replacing the current
  activation rather than nesting one: copy the args into the callee's arg
  regs, switch `code` / `bp` to the callee's (growing the frame when the
  callee needs more regs), and jump to its first insn. Calls to native
  functions (FFI) stay calls. That needs a marker on the call icode -- an
  `IC_IMM_TCALL` beside `IC_IMM_JCALL` -- in place of today's CALL + RET.
- Batch it into the fork's one open PR when the wasm JIT work reaches the
  interpreter (wasm-jit-plan P1).

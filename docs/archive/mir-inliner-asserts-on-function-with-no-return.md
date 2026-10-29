# MIR's inliner asserts on a Turmeric function that loops forever with no exit

**Resolved 2026-10-28** by turmeric-lang/mir#17 (vendored as `9859f851`):
the inliner no longer inlines a function with no `[J]RET` (`func_ret_p`).
The standalone repro is `c-tests/new/inline-no-return.c` in the fork, and
`spin` below now loops, natively and in the browser module.

**Severity:** low-medium. `tur jit` aborts the whole process (and the
browser JIT module traps) instead of running the program, on a function
whose only path is an endless self tail call. A program that means to loop
forever is rare outside tests and demos, but the failure is an assertion in
MIR, not a diagnostic.

Filed 2026-10-28, found writing the Try Turmeric Compile engine's Stop test
(wasm-jit-plan P4), which wanted a runaway program to stop.

## Repro

```sh
printf '(defn spin [n : int] : int (spin (+ n 1)))\n(println (spin 0))\n' > spin.tur
tur jit spin.tur
# tur: external/mir/mir.c:4210: process_inlines: Assertion `insn != ((void *)0)' failed.
TUR_JIT_GEN=interp tur jit spin.tur      # the same
node tools/wasm-jit/run.js spin.tur      # the browser module: Aborted(Assertion failed ...)
```

`tur run spin.tur` (cc) loops forever, as it should. Giving the function an
exit it never takes -- `(if (< n 0) n (spin (+ n 1)))` -- avoids it.

The emitter turns the self tail call into a loop, so the function MIR sees
has no return at all:

```c
static int64_t spin(int64_t n) {
        __tur_tailcall:;
        int64_t __t202 = (n) + (INT64_C(1));
        n = __t202;
        goto __tur_tailcall;
}
```

That function alone, in a small C file run with `c2m -ei`, does not
reproduce it, so the trigger involves the rest of the emitted unit (the call
site `spin(INT64_C(0))` inside `main`'s frame, presumably inlined). Not
narrowed further.

## Root cause (unconfirmed)

`process_inlines` (`external/mir/mir.c:4210`) asserts on a NULL instruction
while inlining a callee into its caller. A callee with no `ret` instruction
leaves something the inliner expects to find -- a return to splice the
result through -- missing.

## Fix directions

- Narrow the repro to a standalone C file (`TUR_JIT_DUMP_FULL` or
  `run.js --dump-c` writes the unit c2mir saw) and fix the inliner in the
  fork, with a c-test. It rides the next turmeric-lang/mir PR.
- Or, on turmeric's side, not mark such functions inlinable. Inlining a
  function that never returns buys nothing.

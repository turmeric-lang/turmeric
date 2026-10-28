# On wasm, an interpreted function's address is not callable once it is stored

**Severity:** high for the wasm JIT (wasm-jit-plan P3). It is the largest
failure family in the first corpus run through the browser JIT module: 201
fixtures trap with `table index is out of bounds`, and the trap leaves the
module unusable for the rest of the page's session (wasm-jit-plan 5.8).
Native targets are unaffected.

Filed 2026-10-28, found by `bash tests/run-jit-wasm.sh`. These are the rows of
`tests/jit-wasm-baseline.txt` whose reason names `table index is out of
bounds`.

## The defect

On wasm, MIR cannot generate code, so `_MIR_get_thunk` hands each function
an address inside a memory array, `wasm_thunks[]`
(`external/mir/mir-wasm.c:168`), not a wasm table index. Interpreted code
calls through `wasm_ff_dispatch`, which recognizes a thunk address and
re-enters the interpreter, so interpreted-to-interpreted calls work.

Host code (the resident runtime, libc) is compiled wasm. It calls a function
pointer with `call_indirect`, which treats the pointer as a table index. A
thunk address is a heap address far past the table's end, so the call traps.
`wasm_ff_dispatch` papers over the commonest case: when a thunk address is
passed as an **i32 argument** of a call into native code, it substitutes a
real table entry built on demand (`wasm_thunk_fn_ptr`, mir-wasm.c:288). That
is what makes `qsort` with an interpreted comparator work.

It misses every other way an address reaches host code:

- **Through memory.** A closure record, a `DK` frame, a reactor callback slot
  or a HAMT key-ops struct is filled by interpreted code and read later by
  the runtime. No call boundary is crossed when the address is stored, so no
  substitution happens.
- **As a carrier.** Turmeric passes a function value as an `int64_t`. An i64
  argument is not substituted (only the `WASM_TC_I32` case is), so a
  `(fn ...)` handed to a runtime function as a carrier arrives as the raw
  thunk address.

The CPS, effect-handler, reactor and closure-heavy fixtures make up most of
the 201.

## Repro

```sh
cmake -S . -B build-wasm -DTUR_WASM=ON && cmake --build build-wasm --target tur_wasm_jit
grep 'table index is out of bounds' tests/jit-wasm-baseline.txt | head
node tools/wasm-jit/run.js tests/fixtures/<one of them>/input.tur
```

## Fix direction (a turmeric-lang/mir change)

Make every thunk a real table entry from the start, so a thunk's address
*is* a callable function pointer and there is nothing to substitute:

- `_MIR_get_thunk` reserves a table slot (`wasmTable.grow`) and returns its
  index. `wasm_thunk_of` maps the index back to the thunk record.
- `_MIR_redirect_thunk` sets the slot either to the interpreter trampoline
  (`mir_wasm_add_interp_entry`'s closure, built at redirect time from the
  function's signature) or, for a native target, to the target's own table
  entry.
- Drop the argument substitution in `wasm_ff_dispatch`. `wasm_ff_dispatch`
  keeps its fast path: a thunk index still re-enters the interpreter directly.

The cost is one table slot and one JS closure per function, paid at link
time rather than on demand. A program has hundreds of functions, not tens of
thousands, so that is affordable. This rides the next fork PR together with
the `MIR_gen` stubs (`src/web/wasm_jit_stubs.c`) and the `_Generic` fix
([c2mir-generic-promotes-controlling-expression](c2mir-generic-promotes-controlling-expression.md)),
per CLAUDE.md's one-PR-at-a-time rule for the fork.

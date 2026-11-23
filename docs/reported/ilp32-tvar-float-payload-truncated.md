# On wasm32, a float kept in a TVar reads back as 0

**Severity:** low. Only a 4-byte-pointer target (the browser JIT module,
`tur build --target wasm`), only a float (or an int above 2^31) kept in a
TVar, and STM on the page is single-threaded anyway. Wrong answer, no
diagnostic. Natively nothing is wrong.

Filed 2026-11-23, split out of
[ilp32-word-slots-truncate-64-bit-carriers](../archive/ilp32-word-slots-truncate-64-bit-carriers.md)
when that report was closed: this is the one row of its family the slot
fixes there do not reach.

## Repro

```sh
cmake -S . -B build-wasm -DTUR_WASM=ON && cmake --build build-wasm --target tur_wasm_jit
node tools/wasm-jit/run.js tests/fixtures/tvar-float-payload-value-converted/input.tur
```

```turmeric
(defn main [] : int
  (let [t (tvar/new 7.1)]
    (atomically (stm (tvar/write t 3.25)))
    (println (:: (atomically (stm (tvar/read t))) float)))   ; 3.25, prints 0 on wasm32
  0)
```

The CAS lines of the fixture are right by accident (both sides truncate the
same way), which is the trap the fixture's own header describes for the
native value-conversion bug.

## Root cause

Two layers, and the second is why this is not a one-line slot fix:

1. The emitted STM runtime keeps a TVar's value in a `void *`
   (`typedef struct TVar { void *value; ... }`, `new_values[128]`,
   `tur_tvar_read`/`write`/`swap`/`cas` and `tur_atomically`, all in the
   preamble `src/compiler/emit_module.c` writes; the host twin is
   `src/runtime/stm.h`). The emitter crosses a payload in as
   `(void*)(intptr_t)<bits>` (`EX_TVAR_NEW`/`WRITE`/`SWAP`/`CAS` in
   `src/compiler/emit_expr.c`).
2. `tvar/read` and `tvar/swap` are *typed* `ptr<void>`
   (`TYPE_PTR_VOID` in `src/compiler/elab_concurrent.c`), and so are the
   `stm` / `atomically` blocks around them. `ptr<void>` lowers to C
   `void *`, so even with a 64-bit slot the value passes through 32-bit
   temporaries (`void *__t211 = tur_tvar_read(...)`) before
   `(:: ... float)` reinterprets it.

## Fix directions

- Give a TVar an element type (`(TVar float)`), so a read is typed at the
  payload and the emitter converts at the slot, as the typed HAMT and vector
  wrappers do. The slot itself becomes an `int64_t` word in both runtimes.
- Or type an untyped TVar read as a 64-bit word that still ascribes by
  *bits* (`(:: w float)` on an `int` is a value conversion, so `int` will not
  do as it stands).
- The measurement is `tests/run-jit-wasm.sh`: the fixture's row leaves
  `tests/jit-wasm-baseline.txt` when this is fixed.

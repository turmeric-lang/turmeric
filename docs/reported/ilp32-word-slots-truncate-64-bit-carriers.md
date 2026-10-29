# On wasm32, word slots typed `intptr_t` or `void *` truncate 64-bit carriers

**Severity:** high for the wasm JIT (wasm-jit-plan P3) and for
`tur build --target wasm`: 44 corpus fixtures run to completion on the
browser JIT module and print a **wrong answer**, with no diagnostic. Natively
(LP64) nothing is wrong, because there `intptr_t`, `void *` and `int64_t` are
all 64 bits.

Filed 2026-10-28, found by the first full corpus run through the browser JIT
module (`bash tests/run-jit-wasm.sh`; the 44 are the rows of
`tests/jit-wasm-baseline.txt` whose reason reads `stdout mismatch on the
compiled path`).

## The defect

A Turmeric carrier is an `int64_t`: an int, a double's bit pattern, or a
pointer widened to 64 bits. Several places in the emitter, the runtime and the
stdlib's inline C keep a carrier in a slot typed at pointer width -- `intptr_t`
or `void *` -- or declare a runtime function at a different width than it
has. On wasm32, `intptr_t` and `void *` are 32 bits and `long` is 32 bits, so
the top half of the carrier is dropped on the way through. A pointer survives
(its top half is zero), an int below 2^31 survives, and **every double and
every large int does not**.

Four shapes, all measured on the module:

1. **The CPS backend's slots are `intptr_t`.** *Fixed 2026-10-28
   (wasm-jit-plan P5):* the DK machine's word is `tur_dkw`, `intptr_t` where
   that is 64 bits and `int64_t` where it is narrower, across the DK runtime,
   the CPS and effects emitters and the resume value's thread-local. What
   follows is the defect as found.
   `tests/fixtures/cps-backend-float` prints `0` instead of `7.75`. The emitted
   continuation is
   `static intptr_t compute_k0(intptr_t __lh_env, intptr_t __t0__slot, DK *__kont)`,
   and a double crosses it as
   `(intptr_t)((union { double d; int64_t i; }){ .d = x }).i`. On wasm32 that
   keeps the low 32 bits of 7.75's pattern, which are zero. Every
   `cps-backend-*float*` and `direct-fiber-effect-float-*` row is this.
2. **HAMT and vector values are `void *`.** `tur_hamt_get` returns `void *`
   (`src/runtime/hamt.h:156`), so a double stored as a map value comes back as
   a denormal (7.1 reads as 8.48798e-315). `tce3-map-cstr-val`,
   `mut-map-reassign`, `generic-map-assoc-sig-tyvar-value` and the `typed/vec-*`
   rows are this.
3. **Declarations at the wrong width.** `stdlib/hamt.tur:16` declares
   `(extern-c tur_hamt_count [m :ptr] :int)`, which emits
   `extern int64_t tur_hamt_count(void *)` against the runtime's
   `uint32_t tur_hamt_count(Hamt *)`. Natively the 32-bit return is
   zero-extended in a 64-bit register and happens to work. On wasm the caller
   and callee disagree on the return type (i64 against i32). It is listed from
   reading the code; which fixture rows it accounts for was not separated from
   shape 2.

4. **A struct's pointer field read through a 64-bit cell view.** *Found
   2026-10-29* (raylib-web-playground-plan prerequisite 2), by Emscripten's
   null-write canary. `stdlib/list.tur` declares
   `(defstruct ^heap Cons [A] (head A) (tail (Cons A)))`, which emits
   `struct { int64_t head; tur_adt_Cons__... *tail; }`, but `list-tail`,
   `list-head` and `list-length` read a cell as
   `struct __tur_cell_t { int64_t head; int64_t tail; }`
   (`stdlib/list.tur:188`, `:383`, `:390`). On wasm32 the real `tail` is 4
   bytes followed by 4 bytes of padding, and the view reads both. When the
   padding is not zero (a reused region or malloc block), a nil tail reads as a
   non-zero carrier, `null?` calls it a cell, and the next step's
   `(intptr_t)` cast truncates it back to 0 and reads **address 0**. Address 0
   holds 0 in a module linked without the canary, so the walk saw a zero
   cell and, by luck, usually the right answer; with
   `-sSTACK_OVERFLOW_CHECK` (which turns `CHECK_NULL_WRITES` on, writing
   `0x63736d65`, "emsc", at address 0) the same program printed `1668509029`.
   Repro, on a module linked with `-sSTACK_OVERFLOW_CHECK=2` and the canary on:

   ```turmeric
   (load "stdlib/list-typed.tur")
   (defn main [] : int
     (let [fs (list-of 7.1 2.25)]
       (println (list-first (list-map fs (fn [f : float] : int 7))))   ; 7, printed 1668509029
       0))
   ```

   The fix is the view, not the struct: read the tail as `void *` and widen it
   (`(int64_t)(intptr_t)cell->tail`). That matches the `defstruct` layout on
   both targets, and still reads the carrier-built cells (`__tur_cons_of`, the
   `*args*` list) right: they store the pointer widened to 64 bits, and on
   little-endian wasm32 its low half, the part a `void *` read takes, is the
   pointer. `tests/fixtures/generic-forward-nullary-arg-tyvar-name` is
   the corpus row; the module linked today keeps the canary off
   (`-sCHECK_NULL_WRITES=0`, `tools/wasm-jit/build-module.sh`), so the
   baseline does not show it.

The Option/Result boxes are probably the same family:
`inline-c-result-builder` returns `47685760` (a heap address) where it expects
`2005` from `tur_some_ptr`, while the `tur_ok_ptr` path beside it is right.
This one was not traced.

## Repro

```sh
cmake -S . -B build-wasm -DTUR_WASM=ON && cmake --build build-wasm --target tur_wasm_jit
node tools/wasm-jit/run.js tests/fixtures/cps-backend-float/input.tur
# 0            (native: 7.75)
node tools/wasm-jit/run.js --dump-c full.c tests/fixtures/cps-backend-float/input.tur
grep -n 'intptr_t compute_k0' full.c
```

`tur build --target wasm` (the AOT path) carries the same emitted C and gives
the same answers. It was checked by hand on the HAMT shape, not across the
corpus.

## Fix directions

- A carrier slot is `int64_t`, never `intptr_t` or `void *`. The CPS backend's
  `DK` slots and `dk_run`'s result are the first case. Converting a pointer to
  and from a carrier goes through `(int64_t)(intptr_t)p` and
  `(void *)(intptr_t)c`, which is already the emitter's spelling elsewhere.
- Runtime containers that hold carriers (`Hamt`, the vec helpers) store
  `int64_t` values. Their pointer-typed API can stay as a wrapper for the C
  callers that really store pointers.
- `extern-c` declarations name the runtime's real types. A check could compare
  `stdlib/*.tur` `extern-c` rows with the runtime headers. The import table's
  generator (`tools/wasm-jit/gen-symtab.py`) already reads both, so it could
  report mismatches.
- `tests/run-jit-wasm.sh` is the measurement: each fixed shape turns its rows
  from `fail` to `compiled`, and the baseline ratchets.

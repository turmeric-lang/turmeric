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
2. **HAMT and vector values are `void *`.** *Fixed 2026-10-30.* `tur_hamt_get`
   returned `void *` (`src/runtime/hamt.h:156`), so a double stored as a map
   value came back as a denormal (7.1 read as 8.48798e-315):
   `tce3-map-cstr-val`, `mut-map-reassign`,
   `generic-map-assoc-sig-tyvar-value`, `map-typed-consumer`,
   `generic-spec-carrier-crossings-4`; all five left the baseline. The HAMT's
   stored key and value are `int64_t` words now, through the whole
   `tur_hamt_*` API (`set`/`get`/`has`/`del`, the `_eq`, `_eq_o`, `_eq_vo`,
   `_owned`, `_ctx`, `_cstr` and `_dynamic` families, the iterators, and the
   `map`/`filter`/`reduce`/`merge_with` callbacks); only the boxed-key
   helpers and the retain/release ops keep pointers, since those are
   pointers. The stdlib's `extern-c` rows and `hamt/*` wrappers say `:int`
   for keys and values (so `hamt/get` returns `int`, not `ptr<void>`), and
   the interpreter's natives pass the word through, which fixes the same
   defect on the Interpret engine. Natively the layouts and values are
   unchanged (LP64). The `typed/vec-*` rows were shape 3 (below), not this.
3. **Declarations at the wrong width.** *Fixed 2026-10-30.*
   `stdlib/hamt.tur:16` declares `(extern-c tur_hamt_count [m :ptr] :int)`,
   which emits `extern int64_t tur_hamt_count(void *)` against the runtime's
   `uint32_t tur_hamt_count(Hamt *)`. Natively the 32-bit return is
   zero-extended in a 64-bit register and happens to work. On wasm the caller
   and callee disagree on the return type (i64 against i32). The runtime
   returns `int64_t` now, and `tests/check-extern-c-widths.py` (in Docs lint)
   compares every stdlib `extern-c` return against the runtime prototypes:
   this was the only such mismatch among 300 prototypes.
   The same shape inside the stdlib's own inline C: `vec.tur`'s `vec-eq?`,
   and every vector `csv.tur` and `digest.tur` build, viewed the vector
   header as `{ int64_t *data; size_t len; size_t cap; }` where the rest of
   `vec.tur` has `int64_t len; int64_t cap`. On wasm32 `size_t` is 4 bytes,
   so `vec-eq?` compared the wrong lengths (false for equal vectors) and a
   CSV-built vector had a header the vector functions misread. All eleven
   views say `int64_t` now; `typed/vec-basic`, `vec-of-tvec-eq`,
   `vec-of-tvec-eq-manual`, `eq-carrier-capturing-comparator`,
   `comparator-float-elements-word-adapter` and `option-niche-vec-closure-cmp`
   left the baseline.

4. **A struct's pointer field read through a 64-bit cell view.** *Found
   2026-10-29* (raylib-web-playground-plan prerequisite 2), by Emscripten's
   null-write canary. *The stdlib half is fixed the same day; the emitter half
   (below) is open.* `stdlib/list.tur` declares
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

   The padding is whatever the allocator left there, so the answer also varied
   from run to run in one module: this was
   [wasm-jit-comparator-result-varies-between-runs](../archive/wasm-jit-comparator-result-varies-between-runs.md),
   and, against the module built from `main` on 2026-10-29, seven corpus rows
   (`generic-forward-nullary-arg-tyvar-name`, `list-basic`, `typed/list-basic`,
   `list-byvalue-adt-elements`, `list-count-phantom-opaque-aggregate-element`,
   `parsec-guide-step3-do-m`, and `typed/list-concat` alternating) printed a
   wrong answer that the baseline recorded as `compiled`.

   **Fixed in the stdlib:** `list-tail`, `list-head` and `list-length` read
   the tail as `void *` and widen it (`(int64_t)(intptr_t)cell->tail`). That matches the `defstruct` layout on
   both targets, and still reads the carrier-built cells (`__tur_cons_of`, the
   `*args*` list) right: they store the pointer widened to 64 bits, and on
   little-endian wasm32 its low half, the part a `void *` read takes, is the
   pointer. All of the rows above but one now print the right answer on every
   run, and `typed/list-concat`, `typed/list-macro` and
   `generic-hof-unannotated-fat-param-mixed` left the baseline.

   **The emitter half, fixed 2026-10-30 (on the 32-bit target only):** the erased instance
   of a parameterized struct types a recursive link as a carrier, `struct
   tur_adt_Cons { int64_t head; int64_t tail; }`, while each element instance
   types it as a pointer, `struct tur_adt_Cons__int { int64_t head;
   tur_adt_Cons__int *tail; }` -- and a `(List cstr)` cell is `{ const char
   *head; tur_adt_Cons__cstr *tail; }`, eight bytes with the tail at offset 4.
   A phantom-typed helper (`list-count` -> `tlength`, over `(List A)`) walked
   the erased layout for every "carrier-compatible" element, which is every
   scalar and pointer element: on LP64 the layouts agree byte for byte, on
   wasm32 the walk read the 4-byte pointer plus its padding, and
   `list-count-phantom-opaque-aggregate-element` counted 6 for 5 and 4 for 3.

   The first attempt, minting the per-element clone for every element on a
   32-bit target so the walk used the instance layout, broke the other
   direction: `list-of` builds its cells through the variadic rest list,
   which is the carrier layout (`__tur_cons_of`, two 8-byte words), and the
   clone for `(List Box)` then read a carrier cell through `Cons__Box`'s
   8-byte layout (`list-of-heap-adt-elements` counted 1 for 2). The two
   layouts have to agree, as they do on LP64.

   So on a 4-byte-pointer target a parametric ADT's layouts keep **8-byte
   slots**: every monomorph member narrower than the word (a pointer, a
   `bool`, an `int32_t`, a `float`) is followed by padding up to it, a
   tagged layout gets the `int64_t __tur_align` union member that keeps the
   payload at offset 8, the erased base gets the same where it has a concrete
   narrow member, and every constructor of such a cell zeroes it first so an
   erased whole-word compare (`tail == 0`) sees the pointer and not stale
   bytes. This is the rule the emitter already applied to one member -- a
   type parameter instantiated at `float32` is padded on every target so
   "the erased twin reads this slot as int64" -- made general for the target
   where every pointer is that narrow (`adt_member_word_pad` and
   `emit_target_ptr_bytes` in `types.c`; the typedef sites in `types.c` and
   `emit_module.c`; the constructors in `types.c`, `emit_core.c`,
   `emit_module.c`). `tur build --target wasm` says the target is 4 bytes
   (`g_emit_target_ptr_bytes`, `cmd_build.c`); the browser JIT module
   compiles on the target, so its own `sizeof(void *)` says so. A 64-bit
   target emits exactly what it did (the fixture snapshots are unchanged),
   and the compiler's size model (`int tag` padded to the union's 8 bytes,
   8 bytes a slot) now describes wasm32 too. Both fixtures answer right on
   the module.

   Still untraced: the Option/Result box line below.

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

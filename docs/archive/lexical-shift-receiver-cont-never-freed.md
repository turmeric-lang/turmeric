# A lexical `shift` leaks its receiver's continuation (488 B per evaluation)

**Fixed 2026-10-12** along the fix direction below, by the same confinement
analysis the serial fix uses.

- **When the receiver keeps nothing of `k`, the shift body drops it after
  the receiver returns.** "Keeps nothing" means every use is
  `(k v)` / `tur_cloneable_cont_resume`, `tur_cloneable_cont_clone`,
  `tur_continuation_snapshot`, a `resume`, or a pass to a callee that
  confines its parameter in turn. The confinement walk
  (`serial_k_use_ok` / `serial_recv_confines_k`, `src/compiler/emit_cps_ir.c`)
  gained a `K_CLONE` role whose readers are those builtins.
  `tur_cloneable_cont_drop(__k)` frees the wrapper and, through the bridge's
  `__dk_env_drop`, the chain copy. Shape 1 (no context frame) drops its
  identity wrapper the same way. It also releases its capturing receiver's
  closure box (`emit_cl_shift_env_drop`), which Shape 2 already did.
- **Fixed on the way: an explicit `(tur_cloneable_cont_drop k)` was never
  emitted.** In statement position the direct emitter returned the call as
  an expression, and `emit_stmt` assumed a `nil` builtin had already written
  itself (`emit_builtin`, `src/compiler/emit_core.c`). The CPS backend
  assigned the `void` call to a temporary and did not compile
  (`prim_expr`'s `BS_FUNC_CALL`). It was the only `nil`-result
  `BS_FUNC_CALL` builtin.
- **Found and fixed on the way:**
  [cps-join-after-cloneable-reset-misses-kont](cps-join-after-cloneable-reset-misses-kont.md).
- **The leak gate.**
  `tests/fixtures/cloneable-shift-receiver-cont-freed` runs a 500-turn loop
  over both shapes and named and closure receivers, plus multi-shot, a
  helper, a receiver that drops `k` itself, and an escaped `k` dropped by
  the program. It is leak-clean. Fourteen existing fixtures became
  leak-clean and joined the gate with `requires.leak-check`:
  `callcc-kv-sugar`, `cloneable-basic`, `cloneable-defer-suspend`,
  `cont-flavors`, `cps-oracle-cloneable-basic`,
  `cps-oracle-cloneable-closure-recv`, `cps-oracle-cloneable-nested-op`,
  `shift-crossfn-resume-named-fn`, `shift-crossfn-resume-nested-reset`,
  `shift-receiver-multishot-effect-cont`, `shift-reset-capability-folding`,
  `shift-single-shot`, `shift-typed-cont` and `shift-unified-keyword`.
- **Measured over the 99 fixtures that use `shift`, `cloneable-shift` or
  `call/cc*`:** 67,488 B leaked before and 40,208 B after. 14 became clean,
  11 leak less, none leak more, and none shows an ASan error.
- **What is left is filed as
  [cloneable-cont-escaped-chain-leaks](../reported/cloneable-cont-escaped-chain-leaks.md):**
  a `k` the receiver returns or stores, and a clone the program never drops.

---

**Severity: low-medium.** Leaks only, and every evaluation leaks, so a loop
grows linearly. A `shift` run 1,000 times leaks 488,000 B in 4,000
allocations. Found 2026-10-12 while leak-checking
`tests/fixtures/shift-receiver-multishot-effect-cont`. The leak is the same
with a plain `cont` receiver and on the branch's base, so it predates that
fixture's change.

## Repro

```turmeric
(defn once [i : int] : int (reset (+ i (shift (fn [k : cont] : int (k 1)) 0))))
(defn run [n : int acc : int] : int (if (= n 0) acc (run (- n 1) (+ acc (once n)))))
(defn main [] : nil (println (run 1000 0)))
```

```sh
TUR_RUNTIME=source \
TUR_CC_FLAGS="-O1 -g -std=c99 -Wall -fno-strict-aliasing -fsanitize=address -L./build/src" \
  ./build/tur build loop.tur -o loop
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=use_stacks=0:use_registers=0 ./loop
# SUMMARY: AddressSanitizer: 488000 byte(s) leaked in 4000 allocation(s).
```

## Root cause

Each evaluation leaks the same set of allocations:

- The reified shift's body (`main_ccbody<N>`, the cloneable `__k` template
  in `src/compiler/emit_cps_ir.c`) allocates a `tur_cloneable_cont` around a
  `dk_copy_range` copy of the captured chain.
- It then hands that wrapper to the receiver.
- Nothing frees the 32-byte struct, the chain copy (the indirect `dk_new`
  nodes), or both.

The handler-case bridge, the fn-payload and `__Shift` case in the same file,
reaps both halves at the outermost entry boundary (`__dk_reap_keep` for the
chain, `__dk_reap_ptr` for the struct). The lexical shift body does not.

## Fix directions

Register the wrapper and its chain with the boundary reap list, as the
handler-case bridge does. The receiver only ever clones (snapshots) or
resumes it, and dk_invoke copies the chain, so nothing outlives the entry
that made it. Then add `requires.leak-check` to
`tests/fixtures/shift-receiver-multishot-effect-cont`.

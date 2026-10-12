# A lexical `shift` leaks its receiver's continuation (488 B per evaluation)

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

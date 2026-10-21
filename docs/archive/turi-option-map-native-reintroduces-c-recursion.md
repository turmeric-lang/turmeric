# The interpreter's native `option-map` re-nests the C stack

**Severity:** low. Deep recursion through an `option-map` callback overflows
the C stack under `tur --interpret`; the compiled binary is fine.

## Repro

```turmeric
(defn down [n : int] : int
  (if (= n 0) 0
    (match (option-map (some n) (fn [x : int] : int (+ 1 (down (- x 1)))))
      (Some v) v
      (None) 0)))

(defn main [] : int
  (println (down 100000))
  0)
```

- `tur run`: `100000`.
- `tur --interpret`: `AddressSanitizer: stack-overflow ... in eval_expr_impl`.

## Root cause (from the investigation; not bisected)

`stdlib/option.tur:option-map` is plain Turmeric now, which the stackless
driver could run without nesting. But `src/main.c:cmd_eval_h` (and the REPL
and macro env) call `turi_env_register_interpreter_natives` again **after** the
stdlib preload. That re-installs the old `native_option_map`
(`src/turi/interpreter_natives.c`), which calls the callback through
`turi_call` on the C stack, one native frame per level. The `result-*`
combinators may have the same problem; unverified.

## Fix directions

- Delete the natives whose stdlib bodies are now pure Turmeric (start with
  `option-map`; audit the rest with `tests/check_turi_native_parity.py`), or
- register natives before the preload so a Turmeric body wins.
- See [docs/archive/turi-cek-stackless-reentry-plan.md](../archive/turi-cek-stackless-reentry-plan.md),
  which says native HOF re-entry is gone.

## Resolution (2026-10-21)

Fix direction 1.

- **Fix.** `native_option_map` and `native_result_map` are deleted
  (`src/turi/interpreter_natives.c`), with their capability and
  handle-signature rows (`src/turi/native_caps.c`). Both stdlib bodies are a
  plain `match` (`stdlib/option.tur:166`, `stdlib/result.tur:158`), which the
  stackless driver runs without nesting. A comment where `native_option_map`
  was records why there is no native.
- **The audit.** The rest of the `turi_call` natives in
  `interpreter_natives.c` stay. `result-map-err`, `result-flat-map`,
  `result-or-else` and `result-or` have no stdlib definition, so the native is
  their only body. `result-eq?`'s stdlib body is inline C that fat-dispatches
  its comparators, which the interpreter cannot run. The seq, comparator and
  R7RS bridges have no Turmeric counterpart. Each still nests a C frame per
  callback, as `turi-interpreter-internals-guide.md` says.
- **The panic site the native supplied.** A native's leaf call was the only
  thing that recorded the program's call site for a panic, so with
  `option-map` running its Turmeric body, `panic-site-partial-through-stdlib-hof`
  (a partial of `vec-get` handed to `option-map`) named `option.tur:167`
  instead of the program's line. The driver now records the site of a call
  from program source into a stdlib Turmeric body, and a partial's
  forwarding call made from stdlib source keeps that site
  (`src/turi/eval.c`, `leaf_panic_site` and the closure-call prologue) -- the
  compiled path's `tur_site_keep` rule. Both engines name line 11 again.
- **Not chosen:** registering natives before the preload. The late
  registration is deliberate -- it is what lets a native win over an
  inline-C body the interpreter cannot execute -- so the wrong natives went
  instead.
- **Pinned** by `turi-option-result-map-deep-recursion`: the repro above and
  the same through `result-map`, 100,000 deep, plus a `None` and an `Err`
  case, on both engines. `tur --interpret` prints `100000` where it used to
  overflow the stack under ASan.

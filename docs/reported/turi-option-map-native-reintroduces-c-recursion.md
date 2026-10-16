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

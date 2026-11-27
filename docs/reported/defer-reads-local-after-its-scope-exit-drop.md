# A `defer` that reads a local of its own `let` runs after that local is dropped

**Severity:** high (a use after free in a program the compiler accepts).

A `let` releases its owned locals at scope exit by appending a drop to the
end of its body. Scope exits fire last-registered-first, so that drop runs
before every `defer` the body declared -- including one that reads the local.

Found 2026-11-27 while fixing
[colored-fn-early-return-skips-scope-exits](../archive/colored-fn-early-return-skips-scope-exits.md).
The pre-#1486 `main` (`d4ad1b0`) behaves the same.

## Repro

`r/main.tur`:

```turmeric
(defn f [c : bool] : int
  (let [r (ref 5)]
    (defer (println @r))
    (if c (return 1) nil)
    2))

(defn main [] : int
  (println (f false))
  0)
```

```sh
tur run r/main.tur                          # a garbage number, then 2
TUR_SANITIZE_PROGRAMS=1 tur run r/main.tur  # heap-use-after-free
tur --interpret r/main.tur                  # 5  2
```

`(f true)`, the early return, prints `5` and `1`: a `return` fires the
registered defers first and the lets' injected drops after them.

| Local | Ordinary scope exit |
|---|---|
| `r (ref 5)`, `(defer (println @r))` | use after free |
| `a (rc/of 7)`, `(defer (println @a))` | null dereference (SEGV) |
| a Drop opaque `h`, `(defer (println (peek h)))` | `Drop.drop` runs first, then the defer uses the released handle -- compiled and interpreted both |

## Root cause

`elab_let` appends the injected drops after the body
(`src/compiler/elab_forms.c:2832`, "Add defer expressions for each ref
binding at the end"), as `(defer (drop! r))` items. A `do` registers its
defers as control reaches them and fires them LIFO, so an item appended last
fires first. The binding itself is older than every defer in the body; its
release should be the last thing the scope does.

The early-return path already has the right order (`emit_stmt.c`,
`EX_RETURN`: the frame chain, then `emit_frame_run_pending_drops`), which is
why the two exits of one function disagree.

## Fix directions

- Fire the trailing injected drops after the frame's defers at an ordinary
  exit, not as part of the LIFO: the do-emitters already know where the
  trailing drop glue starts (`emit_do_trailing_drop_glue`). The CPS IR's
  `cps_do_thread_defers` threads them in the same LIFO and needs the same
  change; the interpreter runs a `do`'s defers in `eval.c`.
- Or have the elaborator place the drops first in the body rather than last.
  They capture the binding, which is in scope from the body's first item, so
  LIFO then runs them last with no emitter change. Check what reads "trailing
  drop glue" by position first: `emit_do_trailing_drop_glue`, the O1-b plan
  in `cps_ir.c`, `elab_tail_leaves`.
- Fixture: the table's three rows on both exits, under
  `requires.leak-check`.

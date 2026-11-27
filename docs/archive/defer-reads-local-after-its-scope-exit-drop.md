# A `defer` that reads a local of its own `let` runs after that local is dropped

> **Status: fixed 2026-11-27.** A `let` whose body declares a defer puts its
> scope-exit drops on an outer block, so they run after the body's defers.
> See [Resolution](#resolution).

**Severity:** high (a use after free in a program the compiler accepts).

A `let` releases its owned locals at scope exit by appending a drop to the
end of its body. Scope exits fire last-registered-first, so that drop runs
before every `defer` the body declared -- including one that reads the local.

Found 2026-11-27 while fixing
[colored-fn-early-return-skips-scope-exits](colored-fn-early-return-skips-scope-exits.md).
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

## Resolution

Fixed 2026-11-27, in the elaborator, by neither direction as written.

Moving the drops to the front of the body (the second direction) was ruled
out: a drop registered when the body starts is on the frame at every early
`return`, which then cannot skip the drop of a local it is handing on
(`(return r)` would free `r` and return it). Reordering the fire in each
emitter (the first) meant three direct do-emitters, the CPS IR and the
interpreter, and would still have left the panic path, which fires a frame
as one LIFO.

Instead the nesting does it. `elab_body_under_scope_drops`
(`src/compiler/elab_forms.c`): when a `let` body declares a `(defer ...)` of
its own, the body becomes the single item of an outer `do`, and the three
passes that append scope-exit drops (`ref<T>` / Drop / per-spec, `rc<T>`,
owning struct fields) append to that one. The inner block has its own frame,
so its defers fire as it ends; the drops fire after them. That holds for an
ordinary exit, a panic and an early `return`, in the direct emitter, the CPS
IR and the interpreter, with no change to any of them. A body with no defer
of its own is left alone, so no other `let` compiles differently.

A side effect: a block that mixed an explicit `defer` with a `ref<T>`
local's drop was one the CPS backend refused ("no lowering here: unsupported
form: EX_DEFER"). Nested, both blocks are shapes it lowers, so
`(let [r (ref 5)] (defer (println @r)) (let [a (perform (Ask))] ...))` now
compiles, and prints what its plain twin prints.

Fixtures, both leak-checked:

- `defer-reads-let-local`: the table's three rows on the ordinary exit, the
  `ref<T>` and `rc<T>` rows on an early `return`, two defers over two
  locals, a `ref<T>` left by a panic, and the effect-function shape above.
  Compiled and interpreted agree.
- `defer-reads-let-local-drop-exits`: a Drop local left by an early `return`
  and by a panic. Compiled only: the interpreter runs no injected drop on
  either exit
  ([turi-early-return-skips-injected-drops](../reported/turi-early-return-skips-injected-drops.md)).

`bash tests/run.sh`: 4351 passed, 0 failed.

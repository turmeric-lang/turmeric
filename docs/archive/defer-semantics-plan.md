# Plan: `defer` runs at the end of its block and sees the variables as they are then

> **Status:** DONE 2026-10-23 -- archived (from control-forms-gaps items 2 and 3).
> Decisions taken: end-of-block scope on both engines; a deferred expression reads
> its variables when it runs. See "Outcome" at the end.

## Part A -- end of block, on the interpreter too

```turmeric
(defn f [] : int
  (do (defer (println "d")) (println "body"))
  (println "after") 0)
```

Compiled: `body`, `d`, `after` -- a `defer` fires when its enclosing `do` /
`when` / `while` pass / `let` body ends. Interpreted: `body`, `after`, `d` -- at
function end. In a `while` body compiled fires per pass (`0 d 1 d 2 d after`),
the interpreter at the end (`0 1 2 after d d d`). A `defer` directly in a `let`
body or the function body agrees. `tests/fixtures/defer-in-loop` pins only the
compiled side. Decision: the compiled behaviour (block scope, as Zig and Swift
`defer`; Go's is function scope).

The interpreter keeps one defer stack per activation (`env->defer_stack`, fired by
`fire_defers_to_mark_by_scope`). Give each block a mark: record the stack depth
when a `do` / `when` / `while` body / `let` body starts and fire down to it when
that block ends, on the normal path and on every non-local exit through it
(`return`, `break`-style loop exits, a panic unwinding through, an effect clause
that does not resume). Both evaluators need it: the tree walker (`eval_expr_impl`)
and the work-stack driver (`eval_drive_ex`), which has its own frames for `do`,
`let` and loops (`DK_DO_SEQ`, `DK_LET_BODY`, `DK_WHILE`). The fiber-handle defer timing report
([turi-fiber-handle-defers-fire-at-the-wrong-time](../reported/turi-fiber-handle-defers-fire-at-the-wrong-time.md))
is the same mechanism and should be closed by this.

Fixtures: the program above and the `while` shape, on both engines; drop the
compiled-only assumption from `defer-in-loop`.

## Part B -- a deferred expression reads its variables when it runs

```turmeric
(let [^mut x 10]
  (defer (println x))   ; prints 10 today, on both engines
  (set! x 20))
```

`tests/fixtures/defer-mutated-binding` says "Should print 20, not 10" and has no
`expected.stdout`, so nothing asserts either. Decision: 20 -- the cleanup is code
that runs at the end of the block, and reads the block's state then.

### Prototype (2026-10-21, reverted)

The compiled path snapshots captured variables into the defer's environment when
the `defer` is registered. A `^mut` scalar a `fn` (or, since 2026-10-21, a `gen`)
names is moved into a shared heap cell (`elab_let_mut_to_cell`, triggered by
`cell_fn_mentions` in `elab_forms.c`), so every capture sees one location. Adding
`defer` to that trigger -- one line,
`if (h == e->sym_gen || h == e->sym_defer) in_fn = true;` -- gave:

- `defer-mutated-binding`: 20 on `tur run`, `tur jit` and `--interpret`.
- `defer-in-loop` (a `defer (println i)` in a `while` that then increments `i`):
  `1 2 3` instead of `0 1 2` -- each pass's cleanup now reads `i` after that pass's
  increment, which is what the decision means; its snapshot moves and it gains an
  `expected.stdout`.
- `tests/run-leak-check.sh`: both fixtures leak the 8-byte cell. The cell is freed
  at scope exit only when `mut_cell_escapes` (`emit_expr.c`) proves every capture
  dead, and an `EX_DEFER` that mentions the cell is not a case it knows, so it
  falls to "escapes". A defer runs inside the scope that owns the cell, so it is
  not an escape: add an `EX_DEFER` arm that walks the body for real escapes.
- Every other `defer|mut|cell|scope|region` fixture passed compiled and
  interpreted.

Only scalar `^mut` variables (numbers, bool, cstr, sym, payload-free opaques) take
the cell. An aggregate `^mut` captured by a `defer` keeps the snapshot; say so in
the docs, or extend the cell to aggregates (the generator plan has the same
limit).

## Steps

1. Part B: the trigger line, the `mut_cell_escapes` `EX_DEFER` arm, fixtures
   (`defer-mutated-binding` gets `expected.stdout` 20 and `requires.leak-check`;
   `defer-in-loop`'s output updates), snapshots.
2. Part A in the interpreter's two evaluators; fixtures on both engines.
3. `defer` entry in `stdlib/forms/control.tur` (scope and read time), the
   control-forms guide text if any; archive control-forms-gaps' items with this
   plan.

## Outcome (2026-10-23)

- **Part B** landed as prototyped: `cell_fn_mentions` (`elab_forms.c`) treats a
  `defer` body as a capturing scope, and `mut_cell_escapes` (`emit_expr.c`) has an
  `EX_DEFER` arm that walks only the body. `defer-mutated-binding` prints 20 and
  `defer-in-loop` `1 2 3` on `tur run`, `tur jit` and `--interpret`, both clean
  under `tests/run-leak-check.sh`. An aggregate `^mut` keeps the snapshot; the
  `defer` entry in `stdlib/forms/control.tur` says so.
- **Part A** needed one evaluator, not two: the tree walker hands every `do` and
  `let` to the work-stack driver. A non-tail `do` that directly registers a defer
  now stores its scope marker's parent as a mark on its `DK_DO_SEQ` frame
  (`defer_scope`) and fires down to it when the block ends; an early exit leaves
  the entries to the enclosing `DK_CALL_RET`'s by-scope walk, as `DK_LET_BODY`
  does. A tail `do` keeps leaving them to `DK_CALL_RET`: nothing else in the
  activation runs after it, and a tail call in its last item must run before
  them. `tests/fixtures/defer-block-scope` (block end, per-pass `while`, `when`,
  nesting, `return` through nested blocks, a block's value) runs on both
  harnesses.
- The fiber-handle report
  ([turi-fiber-handle-defers-fire-at-the-wrong-time](../reported/turi-fiber-handle-defers-fire-at-the-wrong-time.md))
  is not closed by this: its remaining case is the fiber runtime's single-shot
  limit (a multi-shot resume over a defer-bearing body), which is about capture,
  not scope.

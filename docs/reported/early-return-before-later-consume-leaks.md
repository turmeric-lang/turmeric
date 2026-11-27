# An early `return` leaks a local that a later statement would have consumed

**Severity:** medium (a leak, and a Drop instance that never runs, on one
path of an ordinary shape; no diagnostic).

A `let` decides once, after its whole body, whether to append a scope-exit
drop for a local: it does not when the body hands the local on. An early
`return` that leaves before that hand-off has nothing pending to run, and the
`return` itself was left nothing either, so on that path the local is never
released.

Found 2026-11-27 while fixing
[owned-drop-parameter-is-never-dropped](../archive/owned-drop-parameter-is-never-dropped.md);
an owned parameter has the same gap. The pre-#1486 `main` (`d4ad1b0`) behaves
the same.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn take [w : Widget] : int (rel w) 5)

(defn f [c : bool] : int
  (let [h (mk)]
    (if c (return 9) nil)
    (take h)))

(defn g [c : bool] : int
  (let [r (ref 4)]
    (if c (return 9) nil)
    (let [v @r]
      (drop! r)
      v)))

(defn main [] : int
  (println (f true))
  (println (f false))
  (println (g true))
  (println (g false))
  0)
```

```
  mk
9
  mk
  rel
5
9
4
```

`(f true)` never prints `rel`. `(g true)` prints nothing wrong, and
`TUR_SANITIZE_PROGRAMS=1` reports its cell: 8 bytes in one allocation.

The shapes #1486 covers are the ones where the hand-off is in the `if`
itself: `(if c (return 0) (rel h))` gives the `return` an owed drop. Here the
hand-off is a later statement, which no join sees.

## Root cause

`elab_let` (`src/compiler/elab_forms.c`) appends the drop only for a binding
that is not moved, consumed or explicitly dropped when the body ends. `h`
is moved by `(take h)` and `r` by its `drop!`, so nothing is appended, and
`emit_frame_run_pending_drops` finds nothing at the `return`.
`elab_return` records what the path has already handed on (`return_.moved`)
and what a join left it (`return_.owed_drops`); it does not ask what the path
still owns.

## Fix directions

- Make a `return` release what its own path still owns: in `elab_return`,
  for each local of the enclosing function with a scope-exit drop
  (`scope_drop_inst`, an owned `ref<T>`) that this path has not handed on,
  add the drop to `owed_drops` and the binding to `moved` -- the second so a
  drop the `let` does append later is skipped at this `return` rather than
  run twice. Stop at the function boundary: a `return` in a lambda must not
  touch the enclosing function's locals.
- The value being returned is evaluated first and may hand a local on;
  compute the list after elaborating it, as the `moved` list already is.
- Fixture: the repro, plus the same two shapes with an owned parameter in
  place of the local, under `requires.leak-check`.

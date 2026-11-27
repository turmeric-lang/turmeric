# A `return` in a `match` arm does not return

> **Status: fixed 2026-11-27**, the day it was found. See
> [Resolution](#resolution).

**Severity:** high (a silently wrong result, in compiled code only).

A `match` arm whose body is `(return x)` did not leave the function unless
the match was the function's last expression. The arm stored `x` as the
match's value and execution carried on after the match.

Found 2026-11-27 writing fixtures for
[linear-consumption-not-reconciled-at-loops-and-literal-match](linear-consumption-not-reconciled-at-loops-and-literal-match.md).
The pre-#1486 `main` (`d4ad1b0`) behaves the same.

## Repro

```turmeric
(defn mid [c : int] : int
  (match c
    0 (return 100)
    _ 1)
  7)

(defn main [] : int
  (println (mid 0))
  0)
```

`tur run` printed `7`; `tur --interpret` prints `100`.

| Shape | Compiled result |
|---|---|
| `(match c 0 (return 100) _ 1)` then `7` | `7`: the `return` is skipped |
| the same over constructors | the same |
| `0 (do (println "zero") (return 100))` | invalid C: `__t = ((void)0);` |
| the match as the function's last expression | right, by accident: the arm's value is the result either way |
| a returning arm that hands on a `ref<T>`, in a match that ends a `let` | double free: the let's scope-exit drop of the ref ran after the arm had handed it on |

## Root cause

Two places, both in `src/compiler/emit_expr.c`.

- Each of the five match emitters sent an arm through `emit_value` unless
  its type was `!`. A `return` node is typed as the value it returns, and
  `emit_value` on a `return` "in value position, just emit[s] the value
  expression". So the arm assigned and fell through.
- `emit_do_value` asks `expr_contains_return_or_throw` whether a `do` holds a
  `return`, to choose between pushing its defers up front and emitting its
  items in order with the scope-exit drops pending. That predicate does not
  look into a `match`, so a `do` ending in such a match pushed the drops
  first. With the first point fixed alone, the arm's `return` then fired
  them all, including the drop of a value the arm had just handed on.

## Resolution

- `emit_match_arm_yields_value`: an arm that leaves (`expr_tail_diverges`) is
  emitted as a statement, as a `!`-typed arm already was. All five sites use
  it.
- `expr_contains_return_or_throw` looks into a match's arms, so
  `emit_do_value` takes the in-order path for a `do` whose item returns from
  one. This landed in two steps: first as a test local to `emit_do_value`,
  because `emit_if_value` read the shared predicate to decide a branch is a
  statement and would have lost a match's value; then in the predicate
  itself, once
  [if-branch-with-inner-return-loses-its-value](if-branch-with-inner-return-loses-its-value.md)
  was fixed.

Fixtures: `return-in-match-arm` (the table's first four rows, both engines)
and the `leaving-arm*` functions of `linear-join-paths-agree` (the last row,
under `requires.leak-check`).

# An `if` branch that can return early loses its value

**Severity:** high (an uninitialized read, or C that does not compile; in
compiled code only).

An `if` used for its value, with a branch that contains a `return` on some
path and yields a value on another, computes nothing on the yielding path:
the branch is emitted as a statement and the `if`'s result is never assigned.

Found 2026-11-27 while fixing
[return-in-match-arm-does-not-return](../archive/return-in-match-arm-does-not-return.md).
The pre-#1486 `main` (`d4ad1b0`) behaves the same.

## Repro

`r/main.tur`:

```turmeric
(defn f [c : bool d : bool] : int
  (let [v (if c
            (do (if d (return 1))
                5)
            7)]
    (+ v 100)))

(defn main [] : int
  (println (f true false))
  (println (f true true))
  (println (f false false))
  0)
```

```sh
tur run r/main.tur           # 107 1 107
tur --interpret r/main.tur   # 105 1 107
```

`(f true false)` should be 105. The emitted C never assigns the result on
that path:

```c
int64_t __t203;
if (c) {
    if (d) {
        return INT64_C(1);
    } else {
    }
} else {
    __t203 = INT64_C(7);
}
int64_t v_6 = __t203;
```

When both branches can return early there is no result variable at all, and
the C does not compile:

```turmeric
(defn g [c : bool d : bool] : int
  (+ 100 (if c
           (do (if d (return 1))
               5)
           (do (if d (return 2))
               6))))
```

```
return (INT64_C(100)) + (((void)0));
```

The same body under a `let` instead of an `if`
(`(let [x 5] (if d (return 1)) x)` as an initialiser) is right.

## Root cause

`emit_if_value` (`src/compiler/emit_expr.c:5891`) decides how to emit each
branch from `expr_contains_return_or_throw` -- "is there a `return` anywhere
in it" -- and treats yes as "this branch leaves":

```c
if (nil_result || then_has_return_or_throw) {
    emit_stmt(ctx, body, e->as.if_.then_);
```

(`:5949`, and the matching test for the else branch). The result variable is
declared only when at most one branch answers yes, which is the second
failure. The question it needs is "does this branch always leave", which
`expr_tail_diverges` (`src/compiler/emit_core.c:3122`) answers.

## Fix directions

- Emit a branch as a statement only when the `if` is nil-typed or the branch
  always leaves; otherwise through `emit_value`, which already handles a `do`
  with an inner `return` (it is what the `let` form above goes through).
  Declare the result variable unless both branches always leave.
- `expr_contains_return_or_throw` has six other callers
  (`emit_let_value`, `emit_do_value`, `emit_fns.c`, `emit_stmt.c`); each
  should be read for the same confusion. It does not look into a `match`,
  and must not start to before this is fixed.
- Fixture: the repro, with all three calls, on both engines.

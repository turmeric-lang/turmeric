# Forwarding a function-typed parameter to a later-defined defn emits a broken cast

**Severity: medium-high** -- a hard **build failure** in `cc`, not a wrong
answer, so nothing ships broken. What makes it expensive is that the trigger is
**source order**: the same two functions compile or fail depending on which one
is written first, and the error names a C type (`tur_poly_fn_t`) that does not
appear anywhere in the program.

**Status: RESOLVED** 2026-09-12, same day. Found building crdt-spice-plan C3's
`ORMap`, whose `ormap-merge-with` takes the value-merge as a parameter and
forwards it to its fold helpers.

## Repro

```turmeric
(defstruct S [v : int])
(defn add [a : int b : int] : int (+ a b))

(defn caller [g : (fn [int int] int) ^borrow a : S ^borrow b : S] : int
  (let [x (.v a)
        y (.v b)
        r (helper g x y 0 true)]         ;; forwards g to a LATER defn
    r))

(defn helper [g : (fn [int int] int) x : int y : int acc : int first : bool] : int
  (if first (helper g x y (g x y) false) acc))

(defn main [] : int (println (caller add (S 3) (S 4))) 0)
```

```
error: operand of type 'tur_poly_fn_t' where arithmetic or pointer type is required
    (helper((void *)(intptr_t)(g), x_1616, y_1617, INT64_C(0), true));
```

**Move `helper` above `caller` and the identical program prints `7`.** That is
the whole difference.

## Mechanism

Both orderings emit the same forward declaration:

```c
static int64_t helper(tur_poly_fn_t, int64_t, int64_t, int64_t, bool);
```

The call sites differ:

```c
/* helper defined AFTER caller  -- broken */
helper((void *)(intptr_t)(g), x_1616, y_1617, INT64_C(0), true)
/* helper defined BEFORE caller -- fine   */
helper(g, x_1621, y_1622, INT64_C(0), true)
```

So the emitter applies its erasing carrier cast (`(void *)(intptr_t)`) when it
cannot see that the callee's parameter is a real `tur_poly_fn_t`. The signature
it consults is evidently populated as definitions are *emitted*, so a callee
that is only forward-declared at that point falls back to the opaque-pointer
convention -- and `tur_poly_fn_t` is a struct, which no cast to `void *` is
valid on.

The forward declaration already carries the correct parameter type, so the
information is present; the call-site emitter is reading the wrong table.

## Root cause -- measured, and not what the section above guessed

The "Mechanism" reading above was close but wrong about *which* table is stale.
Instrumenting the cast decision in both orders showed identical type kinds and
one difference:

```
BROKEN  (helper after caller):  exkind=EX_VAR         ispoly=1  needs_fn_cast=1
WORKING (helper before caller): exkind=EX_POLY_WRAP             needs_fn_cast=0
```

The emitter already skips the cast for an `EX_POLY_WRAP` argument. The
elaborator inserts that wrapper **only when the callee is already known at the
time the call is elaborated** -- so with the callee defined later, the argument
stays a bare `EX_VAR`. That variable's binding is itself `is_poly_fn`, i.e. it
is *already* a `tur_poly_fn_t`, so it needed no cast either; nothing told the
emitter that.

## Fix

`src/compiler/emit_expr.c`: exclude a bare `EX_VAR` whose binding is
`is_poly_fn` from `needs_fn_cast`, exactly as `EX_POLY_WRAP` is excluded. Both
denote a value that is already a `tur_poly_fn_t`, and a struct is what no
carrier cast is valid on.

Verified on the module the bug came from: `crdt/ormap` was restored to natural
caller-first order and compiles and passes there. The ordering workaround and
its comment have been removed.

## Fixture

`tests/fixtures/fn-param-forwarded-to-later-defn` asserts **both** orders, so a
regression cannot be mistaken for the ordering workaround. Suite: 2962 passed,
0 failed.

## Not this

The bug is not about `^borrow`, the struct parameters, the `let`, or recursion
in the helper -- each was added to the repro separately and none triggers it
alone. Forwarding a function-typed parameter to an **earlier**-defined function
is correct in every combination tried.

## Fixtures owed

- The repro above in both orders, asserting `7` twice, so a fix cannot be
  mistaken for the ordering workaround.

# An `if` arm written as a `do` that ends in `return` is not treated as leaving, so valid code is refused

**Severity:** low (a false rejection with an easy rewrite; nothing unsound).

When one arm of an `if` leaves the function, what it did to a local does not
reach the code after the `if`. #1486 taught the move check that, for an arm
that is a bare `(return ...)`. An arm that does something first and then
returns -- `(do (cleanup) (return 0))` -- is still merged as if it continued.

Listed in #1486 as not changed. Verified 2026-11-27 on `main` at `853f866`.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn peek [^borrow w : Widget] : int 1)

(defn f [c : bool] : int
  (let [h (mk)]
    (if c (do (rel h) (return 0)) 0)
    (peek h)))

(defn main [] : int
  (f true)
  (f false)
  0)
```

```
r/main.tur:10:11: error [TUR-E0201]: cannot copy unique value 'h' -- unique values may be used at most once
```

No path reaches `(peek h)` with `h` released: the only arm that releases it
returns. Moving the release into the returned expression compiles and is
correct, because then the arm is a bare `return`:

```turmeric
(defn f [c : bool] : int
  (let [h (mk)]
    (if c (return (take h)) 0)
    (peek h)))
```

## Root cause

`elab_if` decides whether an arm leaves from the arm's top node only
(`src/compiler/elab_forms.c:4562` for the else arm, and the matching test for
the then arm):

```c
bool else_diverges = (else_->type.kind == TY_NEVER) ||
                     (else_->kind == EX_RETURN) ||
                     (else_->kind == EX_PANIC)  ||
                     (else_->kind == EX_PANIC_WITH);
```

A `do` whose last item is a `return` is an `EX_DO`, and its type is the
returned value's type, not `TY_NEVER`. The linear check next to it
(`:4622`) and `match`'s `ArmMoves` use the same test.

## Fix directions

- One helper, "does this expression always leave": true for `return`,
  `panic`, a `TY_NEVER` expression, a `do` or `let` whose last item leaves,
  and an `if` / `match` whose arms all leave. Use it at the three sites.
- `elab_branch_with_scope_drop` (`:818`) has to agree: an arm that is a `do`
  ending in `return` needs its owed drop attached to that inner `return`,
  not appended after the `do`.
- Typing such a `do` as `TY_NEVER` would do it in one place, and needs a
  check of what else reads a `do`'s type.

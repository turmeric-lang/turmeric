# A fn parameter boxed into a local holder reads as retained

**Severity: low (leak, 24-56 B per call).** A function puts its `(fn ...)`
parameter `f` into a holder that never leaves it, by way of a box: a
`let`-bound `Some`, or a fn-value join stored in a struct's fn field. It only
calls what it put there. The caller keeps the closure it passed, and the
plain-`Some` case also leaks the box.

Found 2026-11-28, measuring the fn-value join branch after merging `main` at
`49aaedc3`.
[let-bound-closure-shared-with-fnfield-struct-leaks](../archive/let-bound-closure-shared-with-fnfield-struct-leaks.md)
(resolved on `main`) frees a closure passed to `(let [s (H 1 f)] ...)`:
`main`'s holder relaxation in the escape walk sees `f` stored bare. It does
not see `f` once it is boxed first.

## Repros

Each is called 50 times from a loop, with a let-bound capturing closure:

```turmeric
(defn s1 [f : (fn [int] int)] : int
  (let [o (Some f)]
    (match o (Some h) (h 10) (None) 0)))
```

- **A let-bound `Some` over the parameter.** `(Some f)` boxes `f` for the
  type-variable slot (EX_POLY_TO_FAT). The sum-wrapped rule
  (`mark_sum_wrapped_boxes_stack`, `elab_call.c`) puts that box on the stack
  only when the scrutinee is written in place. Held in a local, it is not
  asked. Leaks 2800 B in 100 allocations per 50 calls: the 32 B box and the
  caller's 24 B env. On `main` at `5aebfd41`, the same.
- **A join in the `Some`**, `(let [o (Some (if c f a2))] ...)`. The join's
  box is dropped (`sum_payload_join_owned`), but the caller's closure is
  kept: 1200 B in 50 allocations. A join in a `Some` did not work on `main`.
- **A join in a struct's fn field**,
  `(let [s (H 1 (if c f a2))] ((.h s) 10))`. The struct owns and drops the
  join's box, but the caller's closure is kept: 1200 B in 50 allocations.
  The holder mask (`esc_holder_mask_x`, `emit_core.c`) looks for an argument
  that peels to `f` itself, and a join does not. This form did not work on
  `main`.

## Fix directions

- Let `esc_holder_mask_x` count a join argument whose leaves include a box of
  `b` (`esc_join_leaf_boxes`), as it counts `EX_POLY_TO_FAT(b)`. The join's
  box is fresh and the struct owns it (`fnfield_closure_owned`), so it dies
  with the holder.
- For the sum local, run the sum-wrapped stack marking from the let
  elaboration, as `(let [f (ident d)] ...)` already does for the passthrough
  rule: the local is only matched, and every arm only invokes its binder.

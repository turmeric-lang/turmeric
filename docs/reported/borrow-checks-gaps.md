# Borrows: a write through `&mut` of a non-`^mut` variable, an assignment while borrowed, and `&` of a literal in a `let`

**Severity: medium** (the guide's borrow rules are not enforced; one shape is a C
compile error). Split out of type-forms-gaps item 2 on 2026-10-21. The
interpreter's side (its borrows were copies) was
[turi-borrow-is-a-copy](../archive/turi-borrow-is-a-copy.md), resolved
2026-10-22.

## 1. A write through `&mut` of a variable that is not `^mut`

```turmeric
(defn main [] : int
  (let [x 1]                    ; not ^mut
    (let [r &mut x]
      (set! @r 5))
    (println x))                ; 5
  0)
```

Accepted, and `x` changes. borrow-checking-and-lifetimes-guide says a `&mut`
borrow is the exclusive *mutable* access to a mutable place.

Rejecting the `&mut` itself (`elab_borrow_mut`, `elab_structs.c`) was tried on
2026-10-21 and reverted: `borrow-basic` and `errors/borrow-conflict` take `&mut`
of a non-`^mut` binding without writing through it, and they test the conflict
rules, not mutability. Fix: reject the WRITE -- `(set! @r v)` (`elab_set_deref`,
`elab_forms.c`) when `r`'s borrow was taken of a binding (or a field of one) that
is not `^mut`. That needs the borrow to remember its root binding: the
`EX_BORROW_MUT` node has it at the `let` that binds `r`, so record it on `r`'s
binding (as `cont_result_type` rides a continuation binding).

## 2. An assignment to a variable while a `&` of it is live

```turmeric
(let [^mut x 1
      r (& x)]
  (set! x 2)
  (println @r))                 ; 2 compiled
```

Not a conflict. The checker's "N readers XOR 1 writer" (`scope_add_borrow`,
`elab_core.c`) covers taking a second borrow, not assigning the variable itself.
The obvious check in `elab_set` -- refuse while `scope_borrow_conflicts(..., BK_MUT)`
-- would have false positives today: a borrow is recorded in the scope where it
is taken and lasts to the end of that scope, so `(println @(& x))` followed by
`(set! x 5)` in the same body would be refused. Needs borrows that end at their
last use, or at least at the end of the `let` that binds them; until then, check
only borrows held by a `let`-bound name that is still in scope.

## 3. `&` of a literal bound by `let` does not compile

```turmeric
(let [r (& 7.25)] (println @r))
```

C compiler error: `lvalue required as unary '&' operand`. Interpreted it prints
7.25. A literal borrowed as a call argument works and is deliberate
(`saffron-borrowed-struct-key-widened`, `(peek-key m (& 4.25))`): that path
spills to a temporary. Fix: spill the literal to a temporary in the `let` path
too (`EX_BORROW_IMMUT` / `EX_BORROW_MUT` over a literal in `emit_expr.c`), so the
borrow has a place; the temporary lives as long as the `let`.

## Fixtures

Item 1: an `errors/` fixture for the write, and `borrow-basic` still passing.
Item 2: an `errors/` fixture for the `let`-held case. Item 3: the program above
on all three engines.

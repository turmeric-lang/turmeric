# Borrows: a write through `&mut` of a non-`^mut` variable, an assignment while borrowed, and `&` of a literal in a `let`

**RESOLVED 2026-10-22.** All three items; see Resolution at the end.

**Severity: medium** (the guide's borrow rules are not enforced; one shape is a C
compile error). Split out of type-forms-gaps item 2 on 2026-10-21. The
interpreter's side (its borrows were copies) was
[turi-borrow-is-a-copy](turi-borrow-is-a-copy.md), resolved
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
      r      (& x)]
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

## Resolution (2026-10-22)

**Item 1 -- rejected at the borrow, not at the write.** `elab_borrow_mut`
(`src/compiler/elab_structs.c`) refuses `(&mut place)` when the place is a
non-`^mut` local or a field path inside one: "cannot borrow `x` as mutable: it
is not declared `^mut`". The place's root comes from `borrow_place_root`
(`elab_core.c`), which stops at an `rc`, a `:heap` struct, a borrow or a
pointer -- those are interior-mutable, as `elab_set_field` already treats them,
so they need nothing. Rejecting the borrow rather than the write is the rule the
guide states, and it also covers the shape the write check could not see: a
`&mut` handed straight to a writing callee (`(bump! &mut z)`). The 2026-10-21
attempt was reverted only because `borrow-basic` took `&mut` of a non-`^mut`
binding without writing; that fixture declares `^mut` now (it tests the
conflict rules, which are unchanged). Nothing in `stdlib/`, `examples/` or
turmeric-spices takes `&mut` of an immutable place.

**Item 2 -- per place, per last use.** A `let` binding whose initializer is a
borrow records the place (`Binding.borrow_of` / `borrow_place`).
`elab_set` and `elab_set_field` ask `elab_borrow_check_write` whether a `let`
still holds a LIVE borrow of an overlapping place (same root, one field path a
prefix of the other -- `(& (.a p))` does not block `(set! (.b p) 5)`). Liveness
is program order, from a clock every use of a holder stamps
(`elab_borrow_note_use`, at the symbol use in `elab_toplevel.c`):

- a use after the write refuses it -- settled when the holder's `let` ends
  (`elab_borrow_pending_flush`), since the later uses are not yet elaborated;
- a use earlier in an enclosing `while` that began after the holder was bound
  refuses it at once (the loop runs it again after the write);
- a holder a closure captured is live for good.

The scope-wide rule this report's text proposed ("until then, check only
borrows held by a `let`-bound name that is still in scope") would have refused
`wf3-borrow-write-free` and `errors/wf3-borrow-reaches-writer`, which assign a
variable whose `let`-held borrow is dead (never used, or last used before the
write); the last-use rule accepts both, as a borrow checker with non-lexical
lifetimes does. The one known over-approximation is an `if`: a use in the
else-branch reads as after a then-branch write. `borrow-writes-reach-the-place`
(turi-borrow-is-a-copy) asserted `(set! x 2)` under a live `(& x)`; its three
such cases now write through the `&mut` and read afterwards.

**Item 3 -- spilled.** `emit_borrow_spill` (`emit_expr.c`) hands `&` / `&mut` of
a literal the address of a temporary in the current block, which lives as long
as the `let` or call holding the borrow. The interpreter's `&mut` of a literal
borrows a frame temporary too, so `(let [r &mut 3] (set! @r 9) @r)` is 9 on
every engine (it was 3 interpreted).

Fixtures: `errors/borrow-mut-of-immutable`, `errors/borrow-assign-while-borrowed`,
`errors/borrow-field-assign-while-borrowed`,
`errors/borrow-assign-in-loop-while-borrowed`, and
`borrow-literal-and-disjoint-fields` (both harnesses; the literal, the disjoint
field, the ended `let` and the dead holder). The guide's "Borrow Types" section
states the three rules.

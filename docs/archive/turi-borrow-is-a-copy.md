# The interpreter's `&` / `&mut` borrow a copy, so a write through a borrow does not reach the place and a borrow does not see later writes

**RESOLVED 2026-10-22** -- see [Resolution](#resolution-2026-10-22). Pinned by
`borrow-writes-reach-the-place` (both programs below, a nested field, a
`&mut` field handed to a callee, and a float, on `tur run` and `--interpret`).

**Severity: medium** (engine divergence; silent wrong answer under
`--interpret`). Split out of type-forms-gaps item 2 on 2026-10-21.

## Repro

```turmeric
(defstruct P [x : int y : int])
(defn main [] : int
  (let [^mut p (P 3 4)]
    (let [m &mut (.x p)] (set! @m 10))
    (println (.x p)))           ; 10 compiled, 3 interpreted
  (let [^mut x 1
        r (& x)]
    (set! x 2)
    (println @r))               ; 2 compiled, 1 interpreted
  0)
```

## Root cause

The compiled program takes the address of the place (`&p.x`, `&x`). The
interpreter evaluates the borrowed expression to a value and boxes that value
(`EX_BORROW_IMMUT` / `EX_BORROW_MUT` in `src/turi/eval.c`), so the borrow and the
place are two cells.

## Fix directions

Make a borrow a reference to the place: for a variable, its slot in the frame
(the frame outlives the borrow, which the checker already scopes); for a field,
the struct's storage plus the field offset (by-value structs live in the frame
slot or a heap box). `@r` reads through it and `(set! @r v)` writes through it.
Pin both programs above on `tur run` and `--interpret`.

Whether item 2 of [borrow-checks-gaps](borrow-checks-gaps.md)
(assigning a borrowed variable) becomes an error does not change this: a
`&mut` still has to write through.

## Resolution (2026-10-22)

A borrow is a reference to the place, as the fix direction says. In
`src/turi/eval.c`:

- `TURI_REF` now carries the address of the borrowed **slot** (a
  `TuriValue *`) rather than an `EvalBinding *`: a variable's slot is its
  frame binding's value (the global's `EnvBinding` when no frame holds it);
  a field's slot is the one inside the struct's storage, reached through
  whatever the receiver evaluates to -- a by-value struct is one
  `TuriStruct` shared by every reader, so the field slot aliases the place
  `(set! (.x p) v)` writes, and `&mut (.x (.p q))` reaches the nested field
  the same way. `borrow_place_slot` resolves the operand (a variable, a
  field read, a deref of a borrow for `&@r`, an ascription of one); an
  operand that names no place -- a call's result, a literal -- keeps the old
  copy.
- `&` and `&mut` both take that path (`&` used to be transparent, and was a
  unary shim the work-stack driver drove as a value transform; it is driven
  through `eval_expr` now). A variable whose value is already a borrow
  reborrows it, so `(& r1)` is `&int`, not `&&int`.
- `@r` reads through the slot (then through an `__rc` wrapper as before),
  `(set! @r v)` writes it, `(set! (.f r) v)` and `(.f r)` resolve a borrowed
  struct through it.

The borrow checker scopes every borrow inside the frame that owns the slot,
so the slot outlives the reference. `exists-open-witness-dispatch` and the
`borrow-*` family are unchanged under `run-turi.sh`.

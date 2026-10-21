# The interpreter's `&` / `&mut` borrow a copy, so a write through a borrow does not reach the place and a borrow does not see later writes

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

Whether item 2 of [borrow-checks-gaps](borrow-checks-gaps.md) (assigning a
borrowed variable) becomes an error does not change this: a `&mut` still has to
write through.

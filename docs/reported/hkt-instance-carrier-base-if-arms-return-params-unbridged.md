# HKT instance method whose `if` arms return a parameter: carrier base assigns the int64 word to the by-value merge temp

**Severity: medium** -- a hard C compile error ("incompatible types when
assigning to type 'tur_adt_Option__int' from type 'int64_t'") on a plain
shape, but only when the instance is reached through a dictionary. Found
2026-11-07 while pinning `tests/fixtures/dict-clone-pinned-method-returns-arg`
(carrier-sum-option-boxes-have-no-owner); it reproduces on `2916897c`, before
that day's change, so it is not the pinning's.

## Repro

```turmeric
(defclass Pick [^m]
  (pick [x : (m int) y : (m int)] : (m int)))
(definstance Pick [Option]
  (pick [x y] (if (some? x) x y)))
(defn choose [^m] [^Pick m a : (m int) b : (m int)] : (m int)
  (pick a b))
(defn nothing [] : (Option int) (none))
(defn main [] : int
  (println (unwrap-or (choose (some 1) (nothing)) -1))   ; wants 1
  0)
```

`tur build` fails in cc. Drop `choose` and call `(pick (some 1) (nothing))`
directly and it compiles and prints 1: the static call uses the by-value spec
`__inst_Pick_pick_Option__spec__...(tur_adt_Option__int, tur_adt_Option__int)`.
The dictionary route needs the CARRIER BASE, and that is what is wrong.

## Root cause

The carrier base is emitted as

```c
static tur_adt_Option__int __inst_Pick_pick_Option(int64_t x, int64_t y) {
    tur_adt_Option__int __t70;
    ...some?(x) with x bridged carrier->concrete, correctly...
    if (__ps_72) { __t70 = x; } else { __t70 = y; }
    return __t70;
}
```

Two things disagree about the method's ABI:

1. The parameters are int64 carrier words (the base's contract), but the
   `if` merge temp is typed by the RESOLVED result `(Option int)`, and a bare
   parameter in arm position is assigned to it without the carrier->concrete
   readback bridge that the `some?` argument a line above gets. (The result
   itself should not be by value here either, see 2.)
2. The base's return type is the by-value aggregate, and the dictionary
   struct follows it (`tur_adt_Option__int (*pick)(int64_t, int64_t)`), while
   a dict clone calls every slot through an `int64_t (*)(...)` cast and
   bridges the int64 result back to the aggregate at its own call site
   (elab_call.c Route B, `dict_clone_byvalue_result`). Had the body compiled,
   the clone would read a two-word struct return through a one-word cast. The
   stdlib `Monad [Option]` base returns `int64_t`
   (`static int64_t __inst_Monad_bind_Option(int64_t ma, tur_poly_fn_t k)`),
   so whatever decides the base's result spelling treats a user class method
   declared `: (m int)` differently from the stdlib class's.

Where to look: the instance-method carrier-base emission in `emit_fns.c`
(result spelling for a method whose declared result is the class variable
applied), and the `if` merge-temp assignment in `emit_expr.c`, which should
route a carrier-typed arm value through the same bridge an argument gets.

## Workaround

Write the arm through a method or function that takes the carriers:
`(pick [x y] (alt-or x y))` compiles and runs correctly, through the dict
clone included. That is what the fixture does.

## Fix directions

- Make the carrier base of an HKT instance method return the int64 carrier
  whenever its declared result is the class variable applied, as the stdlib
  bases do, and spell the dictionary slot the same way; then the dict clone's
  cast and the slot agree.
- Independently, bridge a bare carrier parameter in `if` / `match` arm
  position to the merge temp's type (or type the temp as the carrier when the
  base returns the carrier).
- Pin with a fixture that calls such a method both statically and through a
  constrained generic, with the arms returning each parameter in turn.

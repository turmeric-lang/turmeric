# A plain fn parameter used in a join evicts a function that performs

**Severity: medium (false rejection, misleading diagnostic).** If a function
performs an effect, a plain `(fn ...)` parameter cannot be one arm of an `if`
or `match` whose value is then used. The function is refused with the CPS
backend's "this effect operation has no lowering here".

Found 2026-11-28 while fixing
[fn-value-through-if-or-match-loses-its-representation](../archive/fn-value-through-if-or-match-loses-its-representation.md).
The gap is not new. `main` at `5aebfd41` refuses the repro below with the same
message. It refuses a `let`-bound join, `(let [h (if c f a2)] ...)`, earlier,
as "'h' has type :ptr<void> (a raw pointer), which is not directly callable".

## Repro

```turmeric
(defmodule m
  (defeffect Ask [] : int)
  (defn a2 [x : int] : int (* x 2))
  (defn pick [c : bool f : (fn [int] int)] : int
    (+ (perform (Ask)) ((if c f a2) 10)))
  (defn main [] : int
    (println (handle (pick true (fn [x : int] : int (+ x 5)))
               (Ask [] k) (resume k 1000)))
    0))
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset ...
[EVICT] SIG-REJECT             eff=1 pick
```

`tur --interpret` prints `1015`. The same function with `^fat f` compiles and
prints `1015`; on `main` that variant segfaulted.

## Root cause

A plain parameter with a scalar signature rides the two-word poly carrier
(`tur_poly_fn_t`). The CPS backend's signature gate (`fn_sig_ok`,
`src/compiler/emit_cps_ir.c`) admits a carrier parameter only when it is
called and never used as a value (`fatparam_only_called`). A value use would
spill the two-word struct into a one-word slot.

The join now converts a carrier arm into a fat box (EX_POLY_TO_FAT). That box
is one word, but making it reads the parameter as a value, so `ptc_walk`
counts a value use and the gate evicts the function.

## Fix directions

- Do not count a use inside an EX_POLY_TO_FAT as a value use in
  `ptc_walk`. Then check that the CPS lowering delegates the conversion to
  the direct emitter, where the carrier is spelled `tur_poly_fn_t` in both
  ABIs. The box is a one-word handle, so it can cross a slot.
- Or box carrier parameters at entry in a CPS-colored function, so the body
  only ever sees the fat handle.

## Workaround

Mark the parameter `^fat`, or call each arm in its own branch:
`(if c (f 10) (a2 10))`.

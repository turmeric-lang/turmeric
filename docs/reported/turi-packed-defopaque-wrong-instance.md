# The interpreter dispatches an `open`ed existential on the runtime value, so two `defopaque` types over `:int` get the wrong instance

**Severity: medium** (engine divergence; silent wrong answer under
`--interpret`). Split out of type-forms-gaps item 6 on 2026-10-21.

## Repro

```turmeric
(defclass Shape [a] (area [x : a] : int))
(defopaque Sq :int)
(defopaque Rect :int)
(definstance Shape [Sq]   (area [x : Sq]   (* (:: x :int) (:: x :int))))
(definstance Shape [Rect] (area [x : Rect] (+ 1000 (:: x :int))))
(defn area-of [e : (exists [a] [(Shape a)] a)] : int (open e [a v] (area v)))
(defn main [] : int
  (println (area-of (pack (:: 5 :Sq) (exists [a] [(Shape a)] a))))   ; 25 compiled, 1005 interpreted
  0)
```

`tests/fixtures/exists-open-witness-dispatch` carries `requires.compiled` for
this. Two packed values of different machine types (`int`, `cstr`) work on both.

## Root cause

Compiled, `pack` stores the witness's dictionary with the value (the constraint
`(Shape a)` is resolved at the `pack` site, where `a` is known to be `Sq`), and
`area` inside `open` calls through it. The interpreter erases the existential:
`pack` keeps only the value, and `area` inside `open` dispatches on the runtime
value's tag. A `defopaque` over `:int` is an int at run time, so `Sq` and `Rect`
look alike and the last-matching instance wins.

## Fix

Make the interpreter's `pack` record what the compiled one does: the witness
type (or the resolved instance per constraint) beside the value, and have `open`
bind the type variable to it so a method call on `v` dispatches on the recorded
witness rather than the value's tag. Then drop `requires.compiled` from
`exists-open-witness-dispatch`.

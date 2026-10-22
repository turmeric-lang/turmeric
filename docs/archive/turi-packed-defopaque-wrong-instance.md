# The interpreter dispatches an `open`ed existential on the runtime value, so two `defopaque` types over `:int` get the wrong instance

**RESOLVED 2026-10-22**; see Resolution at the end.

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

## Resolution (2026-10-22)

As the fix says. The pack node already carries the witnesses the elaborator
resolved (`exists_pack_.witnesses`, one `TypeClassInstance` per constraint);
the interpreter used to throw the node away and keep the value. Now a
constrained `pack` (`turi_exists_pack_wrap`, `src/turi/eval.c`, on both the
`eval_expr` arm and the explicit-stack driver's `eval_unary_post`) returns an
`__tur_exists` pair of the value and the pack node. `open` unwraps it, binds
`v` to the value, and binds each witness on the open's frame as a `DictBind`
keyed by the `v` binding and the witness index -- the same two things the
compiled `EX_EXISTS_DISPATCH` reads from the record. The dispatch arm looks the
witness up (`frame_lookup_open_witness`) before it falls back to structural
matching, which now runs only for a value that reached `open` unpacked. The
ordinary dictionary lookups skip these entries, so a constrained call inside
the body still binds its own dictionaries. An unconstrained pack has nothing to
dispatch on and stays the bare value, as before.

`exists-open-witness-dispatch` lost `requires.compiled`;
`exists-open-opaque-witness` pins this report's repro, a `Vec` of packed
opaques, two constraints on one pack, and nested opens, on both harnesses.

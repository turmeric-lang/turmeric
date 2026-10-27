# Interpreter: Show[Set]/Show[Map] over cstr keys renders the carrier pointer

> **RESOLVED 2026-07-10 -- archived.** Fixed via direction 2: the interpreter
> now grounds an `(:: e A)` ascription's tyvar `A` through the frame's tyvar
> bindings before the primitive re-tag runs (`ascribe_effective_kind` in
> `src/turi/eval.c`, applied in both the `EX_ASCRIBE` and `EX_REINTERPRET`
> post-operand cases). The set/map show-loops recover the cstr key / non-int
> value on the tree-walking path exactly as monomorphization does on the
> compiled path, so `(show (set-of "x" "y"))` prints `#set{x y}` under
> `--interpret`. The coverage fixture `tests/fixtures/show-collections-content-hamt/`
> lost its `requires.compiled` marker and now runs under both `tests/run.sh`
> and `tests/run-turi.sh` (both green). No open items remain.

**Severity:** low (interpreter-only display defect; the compiled path is
correct). Blocks full interpreter parity for the "containers are Show-able"
requirement, not the requirement itself.

## Summary

On the tree-walking interpreter (`tur --interpret` / the `run-turi.sh` path),
`(show (set-of "x" "y"))` and `(show (map-assoc ... "k" "v"))` print the raw
carrier pointers of the cstr keys instead of the strings:

```
#set{91328184765720 91328184768504}     ; want #set{x y}
#map{91328184751744 91328184765424}     ; want #map{k v}
```

The compiled path (`tur run`, default) is correct after the
containers-eq-show-element-dispatch fix -- it prints `#set{x y}` / `#map{k v}`.
`Show[Vec]` and `Eq[Vec]` over cstr are also correct on both paths; only the
Set/Map *HAMT-key* read diverges.

## Minimal repro

```turmeric
(load "stdlib/typeclass.tur")
(defn main [] : int
  (println (show (set-of "x" "y")))                                  ; interp: pointers
  (println (show (map-assoc (:: (map-new) (Map cstr int)) "k" 7)))   ; interp: pointer key
  0)
```

`tur run <f>`  -> `#set{x y}` / `#map{k 7}` (correct).
`tur run --interpret <f>` -> pointer words for the cstr keys.

## Root cause

The compiled fix threads a `(Set A)` / `(Map K V)` witness parameter into
`set-show-loop` / `map-show-loop` (`stdlib/typeclass-show.tur`) so that
*monomorphization* can ground the element type `A`/`K` and specialize the
element `show` dispatch. The element read itself is
`(show (:: (hamt/iter-cur-key iter) A))`.

The interpreter does not monomorphize -- it dispatches a typeclass method on the
**runtime value** handed to `show`. `hamt/iter-cur-key` returns the HAMT key as
an untagged `int64` carrier word, so the interpreter's `show` dispatch sees an
`int64` and selects `Show[int]`, printing the pointer. The static
`(:: ... A)` ascription carries no runtime tag for the interpreter to consult.

`Show[Vec]` works in the interpreter because the vec element read
(`vec-get`) surfaces a value the interpreter can still resolve to its element
type; the untyped `ptr<void>` HAMT iterator loses that.

## Fix directions

Give the interpreter a way to recover the key/value type at the `show` site.
Candidates, cheapest first:

1. Have `hamt/iter-cur-key` / `map-iter-cur-val-as` return an interpreter value
   that carries the element's runtime tag (box the key with its type at insert,
   or stamp the iterator with the container's element type so the accessor can
   re-tag on read).
2. Make the interpreter honor an `(:: e A)` ascription where `A` is a concrete
   type in scope by re-dispatching the method on the ascribed type rather than
   the runtime carrier tag (a targeted `EX_ASCRIBE` case in
   `src/turi/eval.c`'s method-dispatch path).

## Coverage

`tests/fixtures/show-collections-content-hamt/` reproduces this; it carries a
`requires.compiled` marker so `run-turi.sh` PASS-skips it (the compiled path is
asserted). `tests/fixtures/show-collections-content/` (Vec cstr/bool/float/int)
runs on both paths.

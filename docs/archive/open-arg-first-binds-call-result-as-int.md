# An open argument before the one that fixes `A` typed the call's result as `int`

**Severity: high** -- a silent wrong answer.  **Status: RESOLVED 2026-10-02**,
same day as filed.  Found while probing forward calls to generic callees
([forward-call-to-generic-callee-typed-as-placeholder](forward-call-to-generic-callee-typed-as-placeholder.md)).

## Repro

```turmeric
(defn get-or [A] [o : (Option A) d : A] : A
  (match o (Some v) v (None) d))

(defn main [] : int
  (println (get-or (none) 1.5))   ; printed 1
  0)
```

| Call | Printed |
| --- | --- |
| `(get-or (none) 1.5)` | `1` |
| `(ok-or (err "e") 2.75)`, `[r : (Result A cstr) d : A]` | `2` |
| `(first-or (vec-new) 9.5)`, `[v : (Vec A) d : A]` | `9` |
| `(get-or (none) "dflt")` | the string's address, as an integer |
| `(get-or (none) (Pt 7.1 3.25))` then `.y` | "no typeclass method found for 'y'" |

The concrete-argument-first order (`(get-or2 1.5 (none))`) was always right,
and so was the interpreter.  `check-emitted-float-conversions.py` flags the
`(long long)(double)` in `println` -- the corpus never had the shape.

## Mechanism

The spec was right: `get_or__spec__double_..._double(..., 1.5)` returned 1.5.
The call's RESULT type was wrong.  `(none)` has no argument to fix its `A`, so
its type is `(Option A)` with `none`'s own free `A`.  Collecting the call's
bindings in argument order, `(none)` bound the callee's `A` to that free
variable first.  When `d : A` then met `float`, `call_collect_type_bindings`
kept the existing binding -- through the m5 rule ("a prior TYVAR binding
accepts a concrete actual without overwriting"), which is about the ENCLOSING
signature's own variable and fired only because `get-or`'s variable and
`none`'s are both named `A`.  The result stayed `A`, collapsed to `int`, and
`println` printed the double through `%lld`.  With the callee's variable named
anything else the same call was "expected A, got float".

## Fix

`src/compiler/elab_call.c`:

- `call_collect_type_bindings`: a binding to a variable nothing fixed -- an
  open constructor slot, or a variable that is not the enclosing signature's
  own -- is provisional, and a later concrete actual replaces it.  The m5
  treatment of the signature's own variables is unchanged.
- The argument that bound it provisionally is grounded once every argument is
  in, as the nullary-generic-call-under-tyvar-expectation block already did for
  one checked AFTER its sibling: the substitution is recorded on the
  `(none)` / `(vec-new)` call so emit monomorphizes it.  Without this second
  half, `(option-eq? (none) (none) (fn [a b] (= a b)))` bound `A := int` from
  the lambda while both arguments still said `(Option A)`, and a CPS caller
  named a spec that was never emitted (`option-basic`, link error).

Two snapshots moved, both to the better code: `(unwrap-or (none) 42)` and
`(option-map (none) ...)` now call `none__spec__tur_adt_Option__int()` and pass
the by-value Option to a by-value spec, instead of the carrier `none()`, a
NULL-checked unbox and a region free (`option-consumers-byvalue-arg`,
`option-map-literal-none-unannotated-lambda`).

## The same axis, probed

A throwaway probe crossed open producer (`(none)`, `(err 0)`, `(ok 1)` with
the variable on the error side, `(vec-new)`, `(map-new)`, a let-bound `(none)`
/ `(vec-new)`, a three-argument call with the open one in the middle) x
position (before / after the concrete argument) x every type the
generic-spec matrix uses, each cell compiled, interpreted and float-linted
(304 cells).  Beyond the shape above it found three, all fixed here:

- **A let-bound open argument after the concrete one was refused**: `(let [nn
  (none)] (cx 7.1 nn))` against `[d : A o : (Option A)]` was "expected (Option
  float), got (Option A)" at every type -- and in the other order it printed
  `7` for 7.1 before this fix.  A variable that is not the enclosing
  signature's own now agrees with a concrete binding, as an open constructor
  slot already did.
- **`(map-new)` before the argument that fixes `A` was refused** -- `[m : (Map
  int A) d : A]`: the map's `K` met the concrete `int` before anything bound
  `A`.  A return-only generic call that cannot be checked yet only because its
  grounding siblings come later is accepted provisionally and checked by the
  grounding pass, which reports the same mismatch if it still does not fit
  (`(cx (vec-new) 7.1)` there now says "expected (Map int float), got (Vec
  A)").
- **A compiler abort at unsigned payloads**: `(let [nn (none)] (cx nn (:: 200
  uint8)))` died with "a representation decision disagrees with repr_of at
  binding" -- `repr_form_from_cty` classified C types by SUBSTRING, so the
  aggregate `tur_adt_Option__uint8` (it contains "uint") read as a scalar.
  uint8/uint16/uint32 only; the signed names carry no `_t`.  Exact spellings
  now.  Pre-existing.

Pinned by `tests/fixtures/open-arg-first-binds-call-result` (every shape
above, both engines).  Suite 3489/0, turi 2521/0, float-conversion corpus 2551
programs / 0 findings, generic-spec matrix 4066 cells / 0 failing.

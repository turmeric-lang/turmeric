# `[(some 3) (none)]` is rejected; `[(none) (some 3)]` is accepted

> **RESOLVED 2026-11-05.** The root cause was not the literal: any
> `(vec-push! v (none))` with `v : (Vec (Option int))` was refused. A return-only
> generic call (`(none)`, `(vec-new)`) handed to a BARE type-variable parameter
> (`val : A`) that a sibling already bound was compared with its own open
> `(Option A)`; the grounding the call checker does for an applied parameter
> (`(Option A)`) did not run for a bare one. It does now
> (`src/compiler/elab_call.c`): the parameter is instantiated with the
> siblings' bindings, the nullary call is pinned to it (recorded so emit
> monomorphizes it) and takes the grounded type. Pinned by
> `vec-literal-none-after-some` on all three engines.

**Severity:** low (a refusal of a well-typed literal; the other order and an
explicit `vec-of` with an ascription work). Found 2026-10-29 while testing the
REPL's Vec display ([repl-show-uses-wrong-instance-for-vec-elements](../archive/repl-show-uses-wrong-instance-for-vec-elements.md)).

## Repro

```turmeric
(defn main [] : int
  (let [v [(some 3) (none)]]
    (println (vec-len v)))
  0)
```

```
error [TUR-E0001]: function 'vec-push!' arg 2: expected (Option int), got (Option A)
```

`tur run`, `tur jit` and the REPL all refuse it. Swap the elements,
`[(none) (some 3)]`, and it compiles and prints `2`.

## Root cause (not investigated)

The literal lowers to a `vec-of` / `vec-push!` sequence. The first element
fixes the element type to `(Option int)`. The second, `(none)`, is
elaborated without that expected type and keeps its open `(Option A)`, which
the argument check then refuses instead of unifying. The other order works
because `(Option A)` first, then `(Option int)`, unifies.

## Fix directions

- Elaborate each later element of a `[...]` literal with the element type
  the earlier ones fixed as its expected type, as an annotated `let` would
  give it.
- Or have the `vec-push!` argument check unify an open `(Option A)` with the
  element type instead of comparing.

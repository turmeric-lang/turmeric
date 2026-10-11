# A parameter of an applied type accepted a value of any struct or `defdata` type

**Severity: high** (unsound type check; the callee reads the argument's words
as the declared type -- a wrong answer or a crash, no diagnostic).

**Status: RESOLVED 2026-10-11.** Found the same day while working
[forward-call-typed-list-arg-cast-to-int64](forward-call-typed-list-arg-cast-to-int64.md):
passing a struct where `(Cons int)` was expected type-checked, in either
definition order. Filed and fixed together; it was never indexed as open.

## Repro

```turmeric
(defstruct P :heap [v : int])
(defn f [v : (Vec int)] : int (vec-len v))
(defn main [] : int
  (println (f (P 3)))   ; type-checked; read P's words as a Vec
  0)
```

Any parameter whose declared type is an application -- `(Vec int)`,
`(Cons int)`, `(Option int)` -- accepted a value of any user `defstruct`
(`:heap`, `:copy` or plain) or `defdata`. Scalars, `cstr` and a mismatched
application (`(Vec cstr)`, `(Cons int)` at `(Vec int)`) were rejected
correctly, which is why it went unnoticed.

## Root cause

`elab_call.c`'s argument check, "Phase HKT/G4: Allow passing a TY_ADT where
TY_APP is expected". It was written for a GADT constructor value -- `Refl`,
typed `TY_ADT Equal`, at an `(Equal a b)` parameter -- but set `arg_ok`
for every `TY_ADT`. The GS2 check that follows compares the full applied
structure only when the parameter names a type variable, so for a ground
parameter nothing compared the heads.

## Fix

The allowance keeps a `TY_ADT` argument only when it is the parameter's own
type constructor: the argument's `AdtDef` is the head of the parameter's
recorded full type (`type_adt_app_def`), by pointer or by name. A parameter
with no recorded full type keeps the old allowance.

Pinned by `errors/adt-arg-at-applied-param` (`TUR-E0001: expected (Vec int),
got P`) and, from the accepting side, `gadt-ctor-at-applied-param` (`Refl` at
`(Equal int int)`, `(None)` at `(Option int)`).

## Not covered

A caller written ABOVE its callee still sees the pass-1 forward decl, which
types a compound parameter as the `:int` placeholder (see
forward-call-typed-list-arg-cast-to-int64), so `(f (P 3))` above `(defn f
[xs : (Cons int)] ...)` is accepted as an erasure to `:int`. Closing that
needs the forward decl to carry closed parameter types in typed files, which
`elab_fwd_param_full_types` deliberately does only for dynamic ones.

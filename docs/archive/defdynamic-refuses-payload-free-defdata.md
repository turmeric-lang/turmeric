# `defdynamic` refuses a payload-free `defdata` as move-only

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by `defdynamic-payload-free-defdata` and `errors/dynvar-defdata-with-payload`.

**Severity: low (expressiveness; a located error with a workaround).** A
`defdata` whose constructors carry nothing is still move-only by default, so
`defdynamic` refuses it with `TUR-E0603`. No constructor holds anything that
could be duplicated or freed twice, so "move-only" is stricter than the rule
needs. It is what keeps
[dynvar-adoption-plan](../upcoming/dynvar-adoption-plan.md) ADO-1 from typing
`*log-level*` as an enum; the plan uses a `defopaque` newtype instead. Split
out on 2026-10-18 from
[defdynamic-generic-adt-root-emitted-before-its-type](defdynamic-generic-adt-root-emitted-before-its-type.md),
whose `(Option T)` half is fixed.

## Repro

```turmeric
(defdata LogLevel (Debug) (Info) (Warn) (Error))
(defdynamic *lv* :LogLevel (Info))
(defn main [] : int 0)
```

```
error [TUR-E0603]: defdynamic: '*lv*' -- dynamic vars may not hold
substructural (linear or move-only) types
```

## Root cause

`elab_defdynamic` (`src/compiler/elab_dynvars.c`, the `TUR-E0603` check)
refuses any value type whose `copy_kind` is `CK_LINEAR` or `CK_UNIQUE`. A user
`defdata` gets `CK_UNIQUE` whatever its constructors hold.

## Fix directions

1. Treat a `defdata` whose constructors all have no fields as copyable, either
   in general or for this check only. Its value is a tag, so a read that
   copies it out of the frame cannot alias anything.
2. Or keep the rule and name the workaround in the diagnostic: a `defopaque`
   newtype over `:int`, or an `(Option T)` of one.

## Resolution (2026-10-21)

Fix direction 1, for this check only. `elab_defdynamic`
(`src/compiler/elab_dynvars.c`) admits a move-only type when
`adt_is_payload_free` holds: a `TY_ADT` with at least one constructor, none
of which has a field, not declared `:linear` or `:affine`. The type's own
`copy_kind` is unchanged, so nothing else about such an ADT moves; only the
dynvar check stops treating "move-only by default" as "owns something".

The repro compiles. `defdynamic-payload-free-defdata` reads a `LogLevel` and a
`Color` dynvar, overrides them with nested and two-var `binding`s, `set!`s one
inside a binding, and passes the value to a function and a `match`, on both
engines. `errors/dynvar-defdata-with-payload` keeps the boundary: one
constructor with a field and `TUR-E0603` still fires.

[dynvar-adoption-plan](../upcoming/dynvar-adoption-plan.md)'s `*log-level*`
bullet now recommends the enum, and the dynamic-vars guide says which
`defdata`s a dynvar can hold.

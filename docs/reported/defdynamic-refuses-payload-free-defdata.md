# `defdynamic` refuses a payload-free `defdata` as move-only

**Severity: low (expressiveness; a located error with a workaround).** A
`defdata` whose constructors carry nothing is still move-only by default, so
`defdynamic` refuses it with `TUR-E0603`. No constructor holds anything that
could be duplicated or freed twice, so "move-only" is stricter than the rule
needs. It is what keeps
[dynvar-adoption-plan](../upcoming/dynvar-adoption-plan.md) ADO-1 from typing
`*log-level*` as an enum; the plan uses a `defopaque` newtype instead. Split
out on 2026-10-18 from
[defdynamic-generic-adt-root-emitted-before-its-type](../archive/defdynamic-generic-adt-root-emitted-before-its-type.md),
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

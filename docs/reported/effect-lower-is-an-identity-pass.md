# `effect_lower` lowers nothing: look into deleting the pass

**Severity:** low (dead pass). Split out on 2026-10-21 from item 6 of
[stale-comments-build-driver](../archive/stale-comments-build-driver.md), whose
other items, and this one's comment fixes, have landed.

## What the code does

`PASS_EFFECT_LOWER`'s arm in `run_core_pass` builds the `EffectEnv` (builtin
`Unsafe` and the capabilities), which later passes read, and then calls
`effect_lower`. That call lowers nothing. `effect_lower` returns its input when
the program contains no effect node; otherwise `lower_expr` rebuilds the tree
arm by arm and returns `EX_PERFORM`, `EX_HANDLE` and `EX_RESUME` unchanged
(`src/passes/effect_lower.c`, the `case EX_PERFORM:` arms). The CPS/DK backend
lowers effects.

The comments in `run_core_pass`, `pass.h` and `effect_lower.c` now say so.

## The one difference removing it would make

The rebuild is not quite an identity: `lower_discontinue` rebuilds
`EX_DISCONTINUE` with `TYPE_NIL`, not with the node's own type. Removing the
pass would keep the original type. If something downstream has come to depend
on `TYPE_NIL` there, that dependency is a bug in its own right.

## Decided 2026-10-17: investigate deleting it

An investigation first, not yet a decision to delete:

1. `run_core_pass` is the only caller. Check that the tests and the turi /
   WASM builds do not link or call `effect_lower`.
2. Find out whether anything downstream reads an `EX_DISCONTINUE` node's type.
3. Delete the call, then run the suite and the CPS/effects fixtures under both
   JIT builds.

If all of that comes back clean, delete `effect_lower.{c,h}`, keep the
`EffectEnv` construction, and consider renaming the pass `PASS_EFFECT_ENV`
(it is named in four places; see the recipe above `core_passes[]`). If
something does depend on `TYPE_NIL`, file that dependency as its own report and
keep the pass until it is fixed.

`src/passes/cps.c` (the `EX_PERFORM` / `EX_HANDLE` arms) and
`src/compiler/emit_stmt.c` carry comments saying effects are "already lowered
to shift/reset by effect_lower"; correct them in the same change.

# Plan: `coerce` takes its result type from the proof; retire `type-app`

> **Status:** PROPOSED 2026-10-21. Decisions taken (type-forms-gaps items 4 and
> 5): give `coerce` the proof's `b` when it is a type variable; retire the
> `type-app` form. Not started.

Two small type-form cleanups, grouped because both touch how a type application
`(F A)` is spelled and read.

## Part A -- `coerce` returns the proof's right-hand type

### Now

`(coerce eq x)` with `eq : (Equal a b)` returns `x` at its own type (`elab_coerce`,
`src/compiler/elab_structs.c`), although its comment says `x : a -> x : b` and
gadts-cookbook shows it that way. So the canonical use does not type-check:

```turmeric
(load "stdlib/equal.tur")
(defn conv [A B] [eq : (Equal A B) x : A] : B (coerce eq x))   ; TUR-E0707 today
```

Since 2026-10-21 the applied annotation `(Equal int int)` is accepted and its `a`
is checked against `x`'s type when both are concrete.

### Change

Read the proof's second argument `b` (the application is `((Equal a) b)`;
`elab_coerce` already peels it):

- `b` a type variable, or `a` and `b` the same type: the result has type `b`.
  The value is unchanged -- a type variable is carried as the same word whatever
  it is instantiated at, so no conversion is emitted, only the node's type
  changes. Use an `EX_ASCRIBE`-style retyping node (or set the type on a copy of
  `x`'s node) rather than `call_wrap_reinterpret`, which would emit a bit
  reinterpret.
- `a` and `b` different concrete types (`(Equal int float)`): an error, `coerce:
  the proof (Equal int float) relates different types` -- `Refl` cannot be built at
  that type, so the only callers are dead code, and a reinterpret there would be
  a silent bit cast if one ever ran. (Until 2026-10-22 it could: `(:: (Refl)
  (Equal int float))` was accepted, because `Refl`'s inferred index `(Equal ?
  ?)` agreed with any claim. The claim is now checked against the constructor's
  declared result as well; `errors/equal-refl-false-claim`.)
- The bare `Equal` annotation (no arguments) keeps today's behaviour: result at
  `x`'s type.

### Steps

1. The change above; fixtures: `conv` on all three engines, called through
   `(conv (Refl) 5)`; `errors/coerce-proof-relates-different-types`.
2. Check the interpreter: it ignores types at run time, so the value passes
   through unchanged; only the elaborator's result type matters.
3. Update the `coerce` entry in `stdlib/forms/types.tur` and gadts-cookbook.

## Part B -- retire `type-app`

### Now

`(type-app F A)` is TUR-E0012 in a type annotation (`cannot apply a type of kind
'*' as a type constructor`) for `Option`, `Result` and a user `defdata`; `(Option
int)` is the spelling that works. As a statement it checks and does nothing. The
only program that writes it is `tests/fixtures/type-app` (a bare top-level
statement). `tests/fixtures/hkt-type-app-kind` claims in a comment that `(type-app F
A)` is "the canonical syntax at type-annotation positions" and does not use it.

Diagnostics also print a type application as `(type-app Box int)` (`type_name` for
`TY_APP`), and several fixture comments quote `(type-app ? ?)` from them.

### Change

1. Make `type-app` in any position an error that names the replacement:
   `type-app has been removed: write the applied type (Option int)`. Keep it a
   reserved word for a release, then drop it (`stdlib/forms/types.tur`,
   `tests/forms-undocumented.txt` if listed, syntax-guide's reserved-word table).
2. Print `TY_APP` as `(Box int)` in diagnostics. Check what keys on the
   `type_name` string first: the `any` type-id registry hashes `type_name(r)`
   ("`type_name` renders a TY_APP per instantiation ("(type-app Box int)")",
   `emit_module.c`), so changing the rendering changes those ids -- harmless
   (they are hashes compared within one build) but it moves snapshots; regenerate
   them in the same change. Fixtures whose `expected.diag` / `expected.stderr`
   quote the old rendering change too.
3. `tests/fixtures/type-app` becomes an `errors/` fixture for the message; fix
   the comment in `hkt-type-app-kind`.
4. Guides: compiler-internals, elaborator-internals-guide and
   value-representations-guide describe `TY_APP` as "(type-app F A)"; say
   "`(F A)`" and keep `TY_APP` as the internal name.

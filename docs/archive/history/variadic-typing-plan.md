# Plan: Typed Variadic Rest Parameters

> **Status:** Implemented (V0-V7)
> **Last Updated:** 2026-05-30
> **Type:** Compiler Fix + Type System Tightening
> **Related:**
> - `src/compiler/elab_fns.c` (parses `& rest :type`)
> - `src/compiler/elab_call.c` (variadic call-site check)
> - `src/compiler/types.h` (`Type.as.fn.rest_kind`)
> - `docs/archive/history/si4-repl-auto-show-plan.md` (introduced `defopaque`)

---

## TL;DR

`& rest :T` does not actually type-check `T` when `T` is anything other than
a primitive (`:int`, `:cstr`, `:bool`, ...). User-defined types -- opaque
newtypes (`defopaque`), structs, ADTs, GADTs -- silently degrade to `:int`
at the parse step, and the call-site check then accepts arbitrary
`int64_t`-carried values. The net effect is "variadic is untyped beyond
primitives," which is unacceptable: it makes typed DSLs impossible to
express cleanly (the symptom that surfaced this -- the `tourist` macro
takes a mix of `Route` and `Middleware` handles and has to declare them
as `:int`).

This is a three-layer bug; this plan fixes all three.

---

## Reproduction

```turmeric
(defopaque Route :int)
(defopaque Middleware :int)

(defn route!  [] : Route       0)   ;; pretend constructor
(defn make-mw [] : Middleware  0)
(defn server  [port : int & items : Route] : int 0)

(server 3000 (route!) (make-mw) 42)   ;; SHOULD error: items 2 and 3
                                      ;; are not :Route
                                      ;; CURRENTLY: accepted silently
```

The call type-checks today because:

1. **Parse step** -- `& items :Route` is parsed by
   `typekind_from_symbol("Route")` which returns `TY_UNKNOWN` (it only
   recognizes primitives). The parser at `elab_fns.c:503-504` then
   silently defaults to `TY_INT`:
   ```c
   TypeKind rk = typekind_from_symbol(type_p->as.sym->name);
   rest_kind = (rk != TY_UNKNOWN) ? rk : TY_INT;   // <-- silent demotion
   ```
2. **Storage** -- `Type.as.fn.rest_kind` is a bare `TypeKind` enum (a
   ~30-element discriminator), not a full `Type *`. Even if step 1 were
   fixed, two different opaque types (`Route` and `Middleware`) both
   reduce to `TY_STRUCT` and become indistinguishable.
3. **Call-site check** -- `elab_call.c:1397-1418` only compares
   `TypeKind`s for equality, plus a permissive coercion:
   ```c
   /* `:int` rest accepts ADT/APP/STRUCT values (all int64_t at runtime) */
   if (!rest_ok && rk == TY_INT &&
       (ak == TY_STRUCT || ak == TY_ADT || ak == TY_APP)) rest_ok = true;
   ```
   So even raw `:int` rest accepts opaque/struct/ADT values.

---

## Design

### Storage: full `Type *` for rest, not `TypeKind`

Mirror the pattern already used for ordinary parameters
(`arg_full_types`, `result_full_type`):

```c
/* types.h -- Type.as.fn */
bool      is_variadic;
TypeKind  rest_kind;       /* keep for fast-path equality (back-compat) */
Type     *rest_full_type;  /* NULL for primitive rest; non-NULL for
                               opaque / struct / ADT / GADT / app rest */
```

Initialize both fields wherever a function `Type` is built
(`types.h:867-873`, `elab_fns.c:1274`, `elab_fns.c:1697`,
`elab_fns.c:2273`). When the rest annotation is a primitive,
`rest_full_type` stays `NULL` and existing fast-path code keeps working.

### Parse step: route through the general type parser

Replace the `typekind_from_symbol` call at `elab_fns.c:503` (and the
identical one at `:2023`) with the same path used to resolve other
parameter annotations -- i.e. the elaborator that looks up user-defined
types in the type scope and produces a full `Type` (handles primitives,
opaques, structs, ADTs, `rc<T>`, `ref<T>`, `list<T>`, etc.).

If the symbol resolves to an unknown name, emit a real error
(`TUR-Exxxx: unknown rest type 'Foo'`) instead of demoting to `TY_INT`.

### Call-site check: compare full types, drop the int-coercion fallback

At `elab_call.c:1397-1418`, when `rest_full_type` is non-NULL, use
`type_unify` / `type_equal` (whichever already exists for parameter
matching) instead of the bare `ak == rk` check. Keep the `TY_TYVAR`
acceptance (for polymorphic rest). Remove the
`TY_INT → STRUCT/ADT/APP` coercion for variadic rest specifically --
if the user wrote `& rest :int`, they should pass `:int`s, not opaque
handles. (The coercion was a workaround for the missing
`rest_full_type` and is no longer needed once typed rest works.)

Diagnostic format stays the same shape:
```
variadic call to 'server': rest arg 2 has wrong type
  (expected Route, got Middleware)
```

### Edge cases to nail down up front

| Case                                           | Behavior |
|------------------------------------------------|----------|
| `& rest :int` with `:int` args                 | accepted (no change) |
| `& rest :int` with opaque/struct arg           | **rejected** (today: accepted) |
| `& rest :Route` with `Route` args              | **accepted** (today: silently demoted) |
| `& rest :Route` with `Middleware` arg          | **rejected** (today: accepted) |
| `& rest :Route` with raw `:int` arg            | **rejected** (today: accepted) |
| `& rest :A` inside a `[A]` polymorphic defn    | accepts any (today: same, via `TY_TYVAR`) |
| `& rest :list<T>`                              | accepted iff each arg is `list<T>` |

The "raw int rejected where opaque expected" line is the one most likely
to break existing user code. Audit needed before flipping it; see V2.

---

## V0 Audit Results

Grepped the repo for `& rest` / `& args` / `& items` annotations. Every
`& rest :T` site in `stdlib/` and `tests/fixtures/` uses the primitive `:int`
element type (9 occurrences); no site declares a user-defined rest type, and no
site relies on the dropped `TY_INT -> STRUCT/ADT/APP` coercion (none pass an
opaque/struct/ADT value into an `:int` rest). The sibling `../turmeric-spices/`
checkout is absent in this environment, so no spice migration was applied. Net
effect: V5's migration sweep is a no-op for the current tree -- the behavior
change in V3 does not touch any existing working code here.

## Implementation Phases

| Step | Task |
|---|---|
| V0 | **Audit existing `& rest :T` sites in repo + sibling spices.** Grep for `& rest` / `& args` / `& items`; classify each as `:int`-primitive (no migration needed) vs. user-type (becomes typed). Land audit notes in this doc before code changes. |
| V1 | **Storage**: add `Type *rest_full_type` to `Type.as.fn` (`types.h`). Initialize to `NULL` in every constructor / copier. Add to debug-printer. No behavior change yet. |
| V2 | **Parse**: replace `typekind_from_symbol` at `elab_fns.c:503` and `:2023` with the general type-keyword elaborator. Populate `rest_full_type` for non-primitive rest. Hard-error on unknown rest type instead of silently demoting. Compiler smoke test: every existing `& rest :int` in the repo + spices still parses. |
| V3 | **Check**: at `elab_call.c:1397-1418`, dispatch on `rest_full_type != NULL` to call the full-type unifier; otherwise keep the fast `TypeKind` path for primitives. Drop the `TY_INT → STRUCT/ADT/APP` coercion fallback. Update the error message to print the full type name. |
| V4 | **Test fixtures**: add positive/negative cases under `tests/fixtures/variadic-types/` -- `:int` rest, `:Route` rest (defopaque), mixed-opaque-rejection, `list<T>` rest, polymorphic `[A]` rest. Each fixture has `expected.c` (positive) or `expected.err` (negative). |
| V5 | **Migration sweep**: any `& rest :int` site in repo+spices that was *secretly* relying on the dropped coercion (i.e. passing structs into `:int` rest) gets corrected. Most likely: `tourist/app.tur`'s `tourist` switches from `& items :int` to two explicit list params (`mws :list<Middleware>`, `routes :list<Route>`) -- which was the trigger for this plan in the first place. |
| V6 | **CLAUDE.md update**: rewrite the "Function Arity Style Guide / Genuine variadic interfaces" section to document that the rest type now type-checks user-defined types, and that the workaround "pass `:int` and cast inside" is no longer needed. |
| V7 | **Docs**: short note in `docs/guides/developing-spices-guide.md` showing the new pattern: `(defn launch [& routes :Route] ...)`. |

---

## Out of scope (deliberately)

- **Multiple rest groups** (`& mws :Middleware & routes :Route`). Keep
  the one-`&`-per-paramlist rule. For the `tourist` use case, the right
  answer post-fix is two explicit `list<T>` parameters, not two rest
  groups.
- **Heterogeneous variadic** (sum-type rest like `& items :(Or Route Middleware)`).
  If a use case needs that, the user can write a tagged ADT and pass it
  through a single typed rest. Adding a built-in `Or` here is a much
  larger language change.
- **Curried partial application into the rest slot.** Already excluded
  by CLAUDE.md's variadic rules; this plan does not change that.

---

## Risk + Rollback

- The drop of the `TY_INT → STRUCT/ADT/APP` coercion (V3) is the only
  behavior change visible to *existing* working code. V0's audit + V5's
  migration are how we make it land safely.
- Fast-path for primitive rest stays a bare `TypeKind` compare, so the
  hot path of variadic arithmetic / printing functions is untouched.
- Rollback: revert V3 alone; V1+V2 are additive and safe.

---

## Validation

After V4 lands, the original reproduction:

```turmeric
(server 3000 (route!) (make-mw) 42)
```

emits two errors:

```
variadic call to 'server': rest arg 1 has wrong type
  (expected Route, got Middleware)
variadic call to 'server': rest arg 2 has wrong type
  (expected Route, got int)
```

And the `tourist` API can finally be written as it should be:

```turmeric
(defn tourist [port : int
               mws    : list<Middleware>
               routes : list<Route>] : ptr<void>
  ...)
```

with full type-checking, no more `:int`-shaped handle soup.

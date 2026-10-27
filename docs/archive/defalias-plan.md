# Plan: `defalias` — Language-level type aliases (TA Phase 1)

> **Status:** Draft
> **Last Updated:** 2026-05-25
> **Type:** Compiler feature / Spice maintenance
> **Companion to:** [signal-spice-blocking-issues-plan.md](signal-spice-blocking-issues-plan.md)
> **Resolves:** Issue 2 Option C in `signal-spice-blocking-issues-plan.md`

---

## Overview

Introduce a `(defalias Name :primitive-type)` top-level form that creates
a named alias for any primitive `TypeKind` (`:int`, `:float`, `:bool`,
`:cstr`, etc.).  The immediate motivator is removing the float-as-int
type mismatch in `turmeric-spices/spices/signal/src/signal/synth.tur` by
letting `Sample` be declared as an alias for `:int` at the top of that
file, replacing ad-hoc `:int` annotations on every `Sample`-typed
parameter with the more readable `:Sample`.

---

## Syntax

```turmeric
(defalias Sample :int)
```

After this declaration, any type annotation that names `Sample` resolves
to `TY_INT` — both in parameter position and in return-type position:

```turmeric
(defalias Sample :int)

(defn make-sample [raw : Sample] : Sample
  raw)
```

Sweet-exp form (indented):

```turmeric
defalias Sample :int
```

---

## Scope and limitations (Phase TA1)

| Supported | Not supported |
|-----------|---------------|
| Aliases for primitive `TypeKind`s (`:int`, `:float`, `:bool`, `:cstr`, `:nil`, `:ptr-void`, fixed-width numerics) | Aliases for ADTs, structs, or parameterized types |
| Module-scope aliases (top-level `defn`/`def` visibility) | Local (let-bound) aliases |
| Multiple aliases in the same file | Cross-module alias export/import via `(export ...)` |
| Sweet-exp / s-expression source | Compile-time type-level alias application |

Future phases can extend `defalias` to non-primitive targets once the
need arises; Phase TA1 is strictly limited to the signal spice use case.

> **Superseded for the first column.** Phase TA2 (2026-07-30) extended
> `defalias` to ADTs, structs, type applications, function types and
> refinements — see [Phase TA2](#phase-ta2--composite-targets-2026-07-30)
> at the end of this document. Alias *type parameters* remain
> unsupported.

---

## Implementation plan

### Compiler side — `turmeric` repo

#### Step 1 — Add alias storage to `Elab` (`elab_internal.h`)

Add the following fields to the `Elab` struct (near the `sym_deftype`
and `sym_defkind` fields, around line 322):

```c
/* Phase TA1: defalias — primitive type alias declarations */
const Symbol *sym_defalias;

const Symbol **type_alias_names;  /* interned alias name symbols */
TypeKind      *type_alias_kinds;  /* resolved target TypeKind */
uint32_t       n_type_aliases;    /* number of declared aliases */
uint32_t       cap_type_aliases;  /* allocated capacity */
```

Also add the forward declaration for the handler near the other
`elab_def*` declarations at the bottom of `elab_internal.h` (around
line 822):

```c
Expr *elab_defalias(Elab *e, const Form *call);
```

#### Step 2 — Initialize in `elab_core.c`

In `elab_init_state` (around line 1141, just after `e->sym_deftype`):

```c
/* Phase TA1: defalias */
e->sym_defalias      = intern_cstr(st, "defalias");
e->type_alias_names  = NULL;
e->type_alias_kinds  = NULL;
e->n_type_aliases    = 0;
e->cap_type_aliases  = 0;
```

#### Step 3 — Dispatch in `elab_call.c`

Add the dispatch line just after `sym_deftype` (around line 408):

```c
/* Phase TA1: defalias */
if (name == e->sym_defalias) return elab_defalias(e, call);
```

#### Step 4 — Implement `elab_defalias` in `elab_types.c`

Add after the existing `elab_defkind` function (around line 68):

```c
/* Phase TA1: (defalias Name :primitive-type)
 *
 * Declares a type alias for a primitive TypeKind.  The alias is stored in
 * e->type_alias_names / e->type_alias_kinds and consulted during type-
 * annotation resolution in type_expr_from_form and elab_fns parameter parsing.
 *
 * Syntax: (defalias Sample :int)
 *         (defalias Timestamp :int64)
 */
Expr *elab_defalias(Elab *e, const Form *call) {
    /* Minimum: (defalias Name :keyword) — 3 elements */
    if (call->as.list.len < 3) {
        diag_emit(DIAG_ERROR, call->span,
                  "defalias requires (defalias Name :primitive-type)");
        return NULL;
    }

    /* Parse alias name */
    Form *name_form = call->as.list.items[1];
    if (name_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, name_form->span,
                  "defalias: name must be a symbol");
        return NULL;
    }
    const Symbol *alias_name = name_form->as.sym;

    /* Parse target type — must be a primitive keyword */
    Form *type_form = call->as.list.items[2];
    if (type_form->tag != F_KEYWORD) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "defalias: target type must be a keyword (e.g. :int, :float)");
        return NULL;
    }
    TypeKind target_kind = typekind_from_symbol(type_form->as.sym->name);
    if (target_kind == TY_UNKNOWN) {
        diag_emit(DIAG_ERROR, type_form->span,
                  "defalias: '%s' is not a recognised primitive type",
                  type_form->as.sym->name);
        return NULL;
    }

    /* Grow the alias table if needed */
    if (e->n_type_aliases >= e->cap_type_aliases) {
        uint32_t new_cap = e->cap_type_aliases ? e->cap_type_aliases * 2 : 8;
        const Symbol **new_names = (const Symbol **)arena_alloc(e->arena,
            new_cap * sizeof(const Symbol *));
        TypeKind *new_kinds = (TypeKind *)arena_alloc(e->arena,
            new_cap * sizeof(TypeKind));
        if (e->n_type_aliases > 0) {
            memcpy(new_names, e->type_alias_names,
                   e->n_type_aliases * sizeof(const Symbol *));
            memcpy(new_kinds, e->type_alias_kinds,
                   e->n_type_aliases * sizeof(TypeKind));
        }
        e->type_alias_names = new_names;
        e->type_alias_kinds = new_kinds;
        e->cap_type_aliases = new_cap;
    }
    e->type_alias_names[e->n_type_aliases] = alias_name;
    e->type_alias_kinds[e->n_type_aliases] = target_kind;
    e->n_type_aliases++;

    /* defalias has no runtime effect — return nil */
    Expr *result = (Expr *)arena_alloc(e->arena, sizeof(Expr));
    memset(result, 0, sizeof(Expr));
    result->kind = EX_NIL_LIT;
    result->type = TYPE_NIL;
    result->span = call->span;
    return result;
}
```

> **Note on arena growth:** The arena allocator does not support `realloc`.
> Each capacity doubling allocates a fresh block.  This is acceptable
> because the alias table is small (single-digit entries per file) and
> the old memory is simply abandoned in the arena — the same pattern
> used elsewhere in the elaborator for growing arrays.

#### Step 5 — Resolve aliases in `type_expr_from_form` (`elab_types.c`)

In the `F_KEYWORD` branch (around line 1197), add an alias lookup
between `typekind_from_symbol` returning `TY_UNKNOWN` and the scope
walk:

```c
} else if (form->tag == F_KEYWORD) {
    const Symbol *sym = form->as.sym;
    TypeKind k = typekind_from_symbol(sym->name);
    /* ... existing TY_ANY gate ... */
    if (k != TY_UNKNOWN) {
        Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
        *t = type_from_kind(k);
        return t;
    }
    /* Phase TA1: check defalias table before struct/ADT fallback */
    const Symbol *alias_sym = symtab_intern(e->st, strslice(sym->name, sym->len));
    for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
        if (e->type_alias_names[ai] == alias_sym) {
            Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
            *t = type_from_kind(e->type_alias_kinds[ai]);
            return t;
        }
    }
    /* ... existing scope walk for ADT / struct ... */
```

#### Step 6 — Resolve aliases in `elab_fns.c` parameter path

**6a — parameter type annotation** (around line 520, inside the `else`
branch for unrecognised keywords after the constraint-env lookup):

```c
} else {
    /* Phase G3: Try constraint env */
    TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
    if (ck != TY_UNKNOWN) {
        param_kinds[n_params - 1] = ck;
        params[n_params - 1]->type = type_from_kind(ck);
        params[n_params - 1]->type.copy_kind = typekind_default_copy_kind(ck);
    } else {
        /* Phase TA1: check defalias table */
        const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
        TypeKind ak = TY_UNKNOWN;
        for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
            if (e->type_alias_names[ai] == ksym) { ak = e->type_alias_kinds[ai]; break; }
        }
        if (ak != TY_UNKNOWN) {
            param_kinds[n_params - 1] = ak;
            params[n_params - 1]->type = type_from_kind(ak);
        } else {
            /* existing ADT lookup ... */
```

**6b — return type annotation** (around line 684, in the `else` branch
after the constraint-env lookup for return type, before the ADT scan):

```c
TypeKind ck = gadt_skolem_lookup(&param_constraint_env, kw->name);
if (ck != TY_UNKNOWN) {
    return_kind = ck;
} else {
    /* Phase TA1: check defalias table */
    const Symbol *ksym = symtab_intern(e->st, strslice(kw->name, kw->len));
    for (uint32_t ai = 0; ai < e->n_type_aliases; ai++) {
        if (e->type_alias_names[ai] == ksym) {
            return_kind = e->type_alias_kinds[ai];
            break;
        }
    }
    if (return_kind == TY_UNKNOWN) {
        /* existing ADT / struct lookup ... */
```

---

### Spice side — `../turmeric-spices/spices/signal/src/signal/synth.tur`

#### Step 7 — Declare the alias at the top of `synth.tur`

Add as the first top-level form after the module imports:

```turmeric
;;; Sample -- bit-cast alias: float64 values stored as int64 bitfields.
(defalias Sample :int)
```

#### Step 8 — Annotate `Sample`-typed parameters

Replace every untyped (or implicitly `:int`) parameter that carries a
`Sample` value with an explicit `:Sample` annotation.

Identification: grep `synth.tur` for `defn` forms whose docstrings or
bodies deal with `sample`, `sig`, `amp`, `freq`, `phase`, `env`, or
`val` at DSP level. All `float`-literal arguments passed at call sites
that trigger the current `expected :int, got :float` diagnostic are
the ones requiring annotation.

Example transformation:

```turmeric
; Before — untyped, defaults to :int; mismatches float literal call sites
(defn make-voice [freq amp] ...)

; After — explicitly :Sample; typechecker accepts float literals that
; are bit-cast at the C level
(defn make-voice [freq : Sample amp : Sample] : Sample ...)
```

Run `tur check src/signal/synth.tur` after each batch of annotations and
address remaining diagnostics until it exits 0.

---

## File change summary

| File | Change |
|------|--------|
| `src/compiler/elab_internal.h` | Add `sym_defalias`, `type_alias_names`, `type_alias_kinds`, `n_type_aliases`, `cap_type_aliases` fields; add `elab_defalias` declaration |
| `src/compiler/elab_core.c` | Initialize the five new fields in `elab_init_state` |
| `src/compiler/elab_call.c` | Add `if (name == e->sym_defalias) return elab_defalias(e, call);` |
| `src/compiler/elab_types.c` | Implement `elab_defalias`; add alias lookup in `type_expr_from_form` F_KEYWORD branch |
| `src/compiler/elab_fns.c` | Add alias lookup in param and return-type annotation `else` branches |
| `../turmeric-spices/spices/signal/src/signal/synth.tur` | Add `(defalias Sample :int)`; annotate `Sample`-typed params |

---

## Test plan

### Minimal reproducer (new fixture)

Add `tests/fixtures/defalias/basic.tur`:

```turmeric
(defalias Sample :int)
(defn id-sample [x : Sample] : Sample x)
(defn test [] : int
  (id-sample 42))
```

Expected: `tur check` exits 0; `tur run` prints 42.

Add `tests/fixtures/defalias/float-literal.tur` to cover the original
signal-spice symptom:

```turmeric
(defalias Hz :float)
(defn make-osc [freq : Hz] : Hz freq)
(defn test [] : float
  (make-osc 440.0))
```

Expected: exits 0.

Add `tests/fixtures/defalias/error-bad-target.tur`:

```turmeric
(defalias Bad :not-a-type)
```

Expected: `tur check` exits non-zero with a diagnostic naming `:not-a-type`.

### Signal spice acceptance check

```sh
cd ../turmeric-spices/spices/signal
tur check src/signal/core.tur
tur check src/signal/dsp.tur
tur check src/signal/envelope.tur
tur check src/signal/synth.tur   # was failing; must now exit 0
```

All four must exit 0, then:

```sh
rm requires.typecheck-skip
```

---

## Work plan

| Step | File(s) | Status |
|------|---------|--------|
| 1 | `elab_internal.h` — add `Elab` fields + declaration | Done |
| 2 | `elab_core.c` — initialize fields in `elab_init_state` | Done |
| 3 | `elab_call.c` — add dispatch | Done |
| 4 | `elab_types.c` — implement `elab_defalias` | Done |
| 5 | `elab_types.c` — alias lookup in `type_expr_from_form` | Done |
| 6a | `elab_fns.c` — alias lookup in parameter path | Done |
| 6b | `elab_fns.c` — alias lookup in return-type path | Done |
| 7 | `synth.tur` — add `(defalias Sample :int)` | Done |
| 8 | `synth.tur` — annotate `Sample`-typed params | Done |
| 9 | Add fixture tests | Done |
| 10 | Delete `requires.typecheck-skip`; confirm CI passes | Done (no skip marker present; all tests pass) |

---

## Open questions

- **Export semantics.** Should `(export defalias-name)` work so that a
  consumer who imports the module can use `:Sample` in their own code?
  For Phase TA1 the alias table is per-compilation-unit and not
  propagated through `(import ...)`.  If the spice's consumers need to
  annotate cross-module function types with `:Sample`, they must
  re-declare `(defalias Sample :int)` in their own file (or the export
  machinery must be extended in a future phase).

- **Docstring rendering.** `tools/gendocs.py` currently ignores
  `defalias` forms.  A follow-up can add a `;;;`-docstring block to the
  alias declaration and render it on the module HTML page.

- **`deftype` overlap.** *Answered in Phase TA2* (see below).  `deftype`
  is the recursive type binder and always produces a `TY_REC`;
  `defalias` is the transparent alias.  The two forms are deliberately
  disjoint, not interchangeable, and `defalias` now covers every target
  `deftype` was being mistaken for.

---

## Phase TA2 — composite targets (2026-07-30)

The "Not supported" row above ("Aliases for ADTs, structs, or
parameterized types") anticipated a later phase; this is it.  The
motivating finding was that between `defalias` (primitives only) and
`deftype` (always `TY_REC`) there was **no transparent-alias spelling
for a composite type at all** — see
[composite-type-alias-gap.md](composite-type-alias-gap.md).

`defalias` now accepts any type expression the elaborator can resolve:

```turmeric
(defalias Sample    :int)                           ; TA1, unchanged
(defalias IntList   (Cons int))                     ; type application
(defalias Point     P)                              ; struct / ADT name
(defalias Backtrack (fn [] int))                    ; function type
(defalias NonZero   #refine{ q : int | (not= q 0) }) ; refinement
```

Still not supported: **alias type parameters**.
`(defalias Name [a] body)` is a hard error naming the restriction — a
parameterised alias would need type-level substitution at every use
site, which nothing in the resolver does today.  The target must be
fully applied.

Implementation: `Elab` gained `type_alias_types` (a full `Type *` per
alias) beside the existing `type_alias_kinds`; `elab_defalias` keeps the
TA1 primitive-keyword fast path and routes everything else through
`type_expr_from_form`; the five lookup sites (two in
`type_expr_from_form`, three in the `elab_fns` param/return ladders) copy
the full target type instead of rebuilding one from a bare `TypeKind`.
Two new guards reject a self-referential alias and an unresolved target
name, so `(defalias Bad :not-a-type)` stays an error rather than
silently aliasing a type variable.

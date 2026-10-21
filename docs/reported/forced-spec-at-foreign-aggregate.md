# A cross-module specialization at a struct the owner does not define is emitted at `int64_t`

**Severity:** low-medium (cc failure, separate compilation only). Narrowed
2026-10-21 from
[forced-spec-loses-aggregate-type](../archive/forced-spec-loses-aggregate-type.md),
which fixed the case where the generic's module also defines the struct.

## Repro

Separate compilation: a bare directory (no `build.tur`), `tur build --shared`,
or `tur emit-c --output-dir`. A single-`main` project inlines every module into
one TU and builds fine.

```turmeric
;; util.tur
(defmodule util
  (export ident)
  (defn ident [A] [x : A] : A x))
```

```turmeric
;; app.tur -- the struct is the CALLER's
(defmodule app
  (import util :refer [ident])
  (defstruct Q [x : int y : int])
  (defn main [] : int
    (let [p (ident (Q 3 4))] (println (.y p)))
    0))
```

```
$ tur build .
app.c: error: incompatible type for argument 1 of 'util__ident__spec__tur_adt_Q_tur_adt_Q'
```

The same failure when the struct lives in a third module both import
(`types` exports `Pos`; `util` and `app` import it).

## Root cause

`util` owns every clone of `ident` and emits `ident` at `Q` for `app` (a
forced spec, `ForcedAbiSpec`). The record now carries the ADT name at a
`TY_ADT` position, and `forced_spec_pos_type` (`src/compiler/emit_module.c`)
rebuilds it -- but only from the owner's own `EX_DEFDATA` / `EX_DEFGADT` items.
`Q` is `app`'s, and an imported `Pos` is not among `util`'s items either, so
both fall back to `emit_type_from_kind`: the `int64_t` carrier. `app` declares
the clone with the by-value `tur_adt_Q`.

For `app`'s own struct, `util.c` cannot name `tur_adt_Q` at all: it does not
include `app.h`.

## Fix directions

- **A third module's struct:** the owner's header already includes the
  defining module's header, so the C type is visible. What is missing is the
  `AdtDef`: hand the owner the imported defs (the elaborator has them in
  `Elab.adt_defs`), or resolve the name through the import's elaborated
  module.
- **The caller's own struct:** the owner cannot emit it. Let the borrower own
  that clone (`static`, as a no-owner prelude spec is) when a position names a
  type the owner does not define. That needs the generic's body in the
  borrower's program, which separate compilation does not carry today.
- Or refuse the call with a diagnostic naming the type, instead of a cc error.

# A cross-module specialization at a struct type is emitted at `int64_t`

**Severity:** medium (cc failure). When one module calls another module's
generic at a struct type, the owning module emits the specialization with a
carrier parameter, and the caller's call does not compile.

## Repro

```turmeric
;; util.tur
(defmodule util
  (export ident Pos)
  (defstruct Pos [x : int y : int])
  (defn ident [A] [x : A] : A x))
```

```turmeric
;; app.tur
(defmodule app
  (import util :refer [ident Pos])
  (defn main [] : int
    (let [p (ident (Pos 3 4))]
      (println (.x p)))
    (println (ident 2.5))
    0))
```

```
$ tur build .
app.c: error: incompatible type for argument 1 of 'ident__spec__tur_adt_Pos_tur_adt_Pos'
```

## Root cause

A specialization the owner must emit for another unit is recorded in a
`ForcedAbiSpec` / `BorrowSpecInfo` (`src/compiler/emit_module.c`), which keeps
only each position's **TypeKind**. The owner rebuilds the types with
`emit_type_from_kind`, which turns a struct back into the `int64_t` carrier.
The caller declares the same symbol with the by-value `tur_adt_Pos` type.

## Fix directions

- Carry the full type identity (or the resolved C type name) in the forced
  spec record, not just the kind.

## Resolution (2026-10-21)

The forced-spec record carries the ADT name at every `TY_ADT` position, beside
its kind: `BorrowSpecInfo.result_adt` / `arg_adts[]` (malloc'd,
`forced_spec_adt_name`) and `ForcedAbiSpec.result_adt` / `arg_adts[]`
(borrowed; `forced_spec_from_borrow` in `src/main.c` now builds both driver
copies). The owner rebuilds each position with `forced_spec_pos_type`
(`src/compiler/emit_module.c`), in `emit_header_inner` and
`emit_implementation_inner` alike: a named ADT among its own items becomes
`type_adt(def)`, so `util.h` declares
`tur_adt_Pos util__ident__spec__tur_adt_Pos_tur_adt_Pos(tur_adt_Pos)` and the
caller's call compiles. Other positions keep `emit_type_from_kind`.

Pinned by `tests/run-forced-spec-aggregate.sh` (ctest
`tur_forced_spec_aggregate`, fixture `forced-spec-aggregate-multi-module`):
the repro plus a two-generic-parameter `pick` at `int`/`Pos` and
`Pos`/`Box`, built from a bare directory and through `emit-c --output-dir`.

Left open: a struct the owner does not define -- the caller's own, or a third
module's -- still reaches the owner as a bare kind, filed as
[forced-spec-at-foreign-aggregate](forced-spec-at-foreign-aggregate.md).

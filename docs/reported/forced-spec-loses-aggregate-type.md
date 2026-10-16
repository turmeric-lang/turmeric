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

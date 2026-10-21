# A `defalias` name was rejected as a struct field type

**RESOLVED 2026-10-21** -- found and fixed in the same change, rewriting
`examples/datalog/indexed.tur` onto typed data. Pinned by
`defalias-struct-field` (all three engines).

## The bug

```turmeric
(defalias Db (Vec Datum))
(defstruct Idb :copy [db : Db idx : EavtIdx])
;; error: defdata: field has unrecognized type :Db
```

A `defalias` name resolved in a parameter or result annotation
(`type_expr_from_form` consults the alias table) but not in a `defstruct` /
`defdata` field: `resolve_ctor_field` (`src/compiler/elab_structs.c`) read a
bare field type name as a primitive or a declared struct/ADT only, and
reported anything else as unrecognized.

## The fix

The named-type path of `resolve_ctor_field` consults the alias table before
giving up: a primitive target is stored as that primitive, a nominal ADT /
struct target continues down the named-type path as that type's name, and
anything else -- a type application such as `(Vec Datum)`, a fn type, an
existential -- is stored exactly as the compound form itself would be. The
compound tail is one helper now, `ctor_field_set_compound`, shared with the
`(Vec Datum)` field form. The fixture covers a type application, two
primitives (one a float, read back as 7.25), a struct and a fn type.

# A struct with a `defopaque` field and an aggregate field lowers the aggregate to `int64_t`

**Severity: medium** (compile failure in the emitted C; no miscompile seen).

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)). Found
2026-10-10 implementing
[xml-html-parsers-plan](../upcoming/spices/xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/dom-core/src/dom/core.tur`:
`ParseError` holds `line`/`col` instead of a `SourcePos`, and its
`ParseErrorKind` field is a `:heap` sum (a pointer word) instead of a by-value
one.

## Repro

````turmeric
(defopaque S :ptr<void>)
(defstruct Pos :copy [line : int col : int])
(defstruct Er :copy [msg : S pos : Pos])
(defn __s [] : ptr<void>
  ```c
  return (void*)"x";
  ```)
(defn mk [] : Er (Er (:: (__s) S) (Pos 3 4)))
(defn main [] : int
  (let [e (mk)]
    (println (.line (.pos e))))
  0)
````

```
error: request for member 'line' in something not a structure or union
```

The emitted `tur_adt_Er` is `{ int64_t msg; int64_t pos; }`. A by-value
`defdata` field in the same position fails the same way (`invalid
initializer` when it is read into a `tur_adt_K`). It needs the sibling to be a
`defopaque` -- a `ptr<void>` or `cstr` field beside the same `Pos` is fine --
and it does not matter whether the struct is `:copy`, whether the opaque is
`:non-null`, or which field comes first.

## Fix direction

The struct's C layout is computed as if every field were a word once one field
is an opaque; the aggregate field should keep its own C type (as it does with
a `ptr<void>` sibling).

## Resolution

The layout is deliberate, not the bug. An opaque field is a `TY_ADT` that is
not an inline aggregate, so `adt_is_byvalue_product_d` keeps the struct on the
carrier ABI: a heap box held as the int64 word, every slot a word, and a
by-value aggregate field stored BOXED. A `ptr<void>` sibling is a scalar kind
and leaves the struct by value, which is why it worked. Moving every struct
with an opaque field onto the by-value ABI (the report's fix direction) would
change the C signature of every such struct crossing an inline-C boundary, so
the carrier layout stays and its accessors are made to honour it:

- **Read** (`EX_GET_FIELD`, `src/compiler/emit_expr.c`). The constructor
  boxed the field (the CONV-S1/B3 field-store crossing) and the match binder
  unboxed it (B3), but `(.pos e)` read the slot raw. A carrier record's
  by-value aggregate field is dereferenced out of its box now. The test is
  the constructor's: a field is inline only when the OWNER is a by-value
  product (`adt_field_is_inline_byval` alone answers for the field).
- **Write** (`emit_set_field_stmt`, `src/compiler/emit_stmt.c`), found on the
  way and broader: `set!` of ANY field of such a struct emitted `(r).n = ...`
  on the word -- a compile error for a plain `int` field too. It writes
  through the box now, boxing a by-value aggregate value as the constructor
  does. A `:copy` record's box may be shared by its copies, so it is copied
  first and the variable re-pointed (a struct keeps value semantics); a
  unique one cannot have been copied, so its box is written in place. Both
  fresh boxes are region-noted.

A by-value `defdata` field in the same position (the report's second shape)
is covered by the same read.

**Revisited 2026-10-16.** Keeping these structs on the carrier ABI left every
construction's box without an owner
([opaque-field-struct-box-never-freed](opaque-field-struct-box-never-freed.md)).
Measured then, the inline-C ABI cost this resolution declined was nil: no
inline-C `defn` in stdlib or turmeric-spices takes or returns such a struct. So
a struct with an opaque field is a by-value product now, except beside a closure
field. That struct still takes the carrier layout and the accessors above.

**Pinned by** `struct-aggregate-field-beside-opaque` (the repro, both field
orders, a `:non-null` opaque, a by-value `defdata` field, the field passed on
and `let`-bound) and `struct-beside-opaque-set-field` (`set!` of a scalar and
an aggregate field, and a copy that keeps its values).

# A struct with a `^heap` ADT field stays on the carrier ABI, and its box is never freed

**Severity: low-medium (leak, one 16-32 B box per construction).** A
`defstruct`, `^copy` or not, with a field whose type is a `^heap` ADT is
built as a heap box behind the `int64_t` carrier (`ctor_<T>_<T>` returns
`int64_t`). Nothing frees that box: not a `let` local of it, not the payload
of a `Result` or `Option` it rides in. The same struct without the `^heap`
field is laid out by value and leaks nothing.

Found 2026-11-28 tracing turmeric-spices' `xml` test leaks under
`TUR_SANITIZE_PROGRAMS=1`. `dom-core`'s
`(defstruct ^copy ParseError [kind : ParseErrorKind msg : String line : int col : int])`
has a `^heap` `ParseErrorKind` field, so every parse error leaks its 32 B
`ParseError` box (`ctor_ParseError_ParseError`, 26 of them across the `xml`
tests). The `String` field is not the cause: opaque fields have been by value
since [opaque-field-struct-box-never-freed](../archive/opaque-field-struct-box-never-freed.md).
Reproduces on `main` at `d00f2f4b`.

## Repro

```turmeric
(defdata ^heap ^copy K (A) (B))
(def a-node : K (A))
(defstruct ^copy PE [kind : K line : int])
(defstruct PM [kind : K line : int])
(defn main [] : int
  (let [p (PE a-node 3)] (println (.line p)))
  (let [q (PM a-node 4)] (println (.line q)))
  0)
```

Built as `tests/run-leak-check.sh` builds:

```
SUMMARY: AddressSanitizer: 32 byte(s) leaked in 2 allocation(s).
```

The emitted constructors are `static int64_t ctor_PE_PE` and `static int64_t
ctor_PM_PM`. Drop the `kind` field, or give it an opaque type instead, and
both become `static tur_adt_PE ctor_PE_PE` (by value) and nothing leaks. The
same holds for the struct as an `(Err ...)` payload matched by a reader.

## Root cause

`adt_is_byvalue_product_d` (`src/compiler/types.c:4199`) admits a field as
by-value when it is an inline by-value aggregate, an `(F Self)` application,
or an opaque. Otherwise it rejects every ADT-typed field
(`fk == TY_ADT ... return false`, `types.c:4238`). A `^heap` ADT is not an
aggregate, though: it is a typed pointer, one word. `adt_field_is_inline_byval_d`
says so ("a :heap ADT is a typed POINTER, not an inline aggregate -- store it
as the pointer carrier"). So the owner could store that word in place, the way
an opaque field is stored; instead the whole struct falls to the carrier ABI,
whose box has no owner.

That is the same mistake opaque-field-struct-box-never-freed fixed for opaque
fields, at `types.c:4234`.

## Fix directions

1. In `adt_is_byvalue_product_d`, admit a `TY_ADT` (or `TY_STRUCT`) field
   whose def `is_heap`, as one word, beside the opaque case. A field read
   then yields the pointer, and the struct copies by value; the `^heap` node
   itself is unchanged (still region-managed, per the memory-usage guide).
2. Watch what moved the last time: the opaque fix changed the inline-C ABI
   of structs passed to C, and `:heap` receivers' field reads. Measure the
   fixture corpus and turmeric-spices (`dom-core`, `xml`, `html`), as
   opaque-field-struct-box-never-freed did, before landing.

## Workaround

Store the `^heap` value behind an opaque handle field, or keep the kind as a
by-value tag (a non-`^heap` `defdata`).

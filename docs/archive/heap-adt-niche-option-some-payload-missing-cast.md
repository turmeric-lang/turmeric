# `(Some x)` into a niche `(Option S)` field of a `:heap` constructor is passed as `int64_t`

**Severity: medium** (clang rejects the emitted C; gcc warns).

**Status: RESOLVED 2026-10-11** -- the niche case first, then the non-niche
case and the wider `:heap` by-value field family it belonged to; see
[Resolution](#resolution). Found 2026-10-10 implementing
[xml-html-parsers-plan](../upcoming/spices/xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/dom-core/src/dom/event.tur`:
`SaxEvent`'s `Doctype` carries its public and system identifiers as `String`s
(empty when absent) rather than `(Option String)`.

## Repro

````turmeric
(defopaque S :ptr<void> :non-null)
(defdata Ev :heap :copy (A int) (C S (Option S)))
(defn __raw [] : ptr<void>
  ```c
  return (void *)"hello";
  ```)
(defn s-of [] : S (:: (__raw) S))
(defn mk [s : S] : Ev (C s (Some s)))
(defn main [] : int
  (match (mk (s-of)) (A x) (println x) (C s o) (println "c"))
  0)
````

`CC=clang tur run` fails with `incompatible integer to pointer conversion
passing 'int64_t' to parameter of type 'void *'`: the constructor's parameter
is the niche `void *`, the call site casts the `Some` to `int64_t`.
`(C s (None))` compiles. Without `:non-null` on `S`, `(Option S)` is a
by-value struct and the same constructor fails differently (`aggregate value
used where an integer was expected`) -- a `:heap` sum cannot carry an
`(Option S)` either way.

## Narrowed 2026-10-11: the niche case

Not just a missing cast. `emit_type_is_byvalue_adt` answered "by-value
aggregate" for a NICHE `(Option S)`, which is spelled as the payload pointer
itself (SR3), so the `:heap` constructor call heap-boxed `(Some s)` and passed
the box's address into the `void *` field, and the match binder dereferenced
the field (`*(void * *)(intptr_t)(__scrut->as.C._1)`). `(None)` took a
different path and stored the bare NULL. Under gcc (which only warns) the
`Some` round trip happened to work, and reading a `None` back was a NULL
dereference -- a segfault.

`emit_type_is_byvalue_adt` (`src/compiler/emit_expr.c`) now returns false for a
niche option, as it already did for a `:heap` ADT: one word, never boxed into a
field slot or unboxed out of one. Both arms of the repro compile under clang and
read back correctly. **Pinned by** `heap-adt-niche-option-field` (`Some` and
`None` through a `:heap` constructor and back out through a nested `match`).

## What was left after the niche fix

Without `:non-null` on `S`, `(Option S)` is the by-value `tur_adt_Option__S`.
The `:heap` constructor lays the field out INLINE (`ctor_Ev_C(int64_t _0,
tur_adt_Option__S _1)`), but the call site still boxes the argument
(`adt_field_is_inline_byval` says no for this field) and the match binder
dereferences it. The typedef/ctor emitter and the inline-field predicate
disagree about one field's layout. Clang: `passing 'int64_t' to parameter of
incompatible type 'tur_adt_Option__S'`. Workaround as before: carry the value
as something one word wide (a `String`, a `:non-null` opaque).

## Resolution

The non-niche half was one member of a family: **any** `:heap` ADT field
whose type is a by-value aggregate (a `:copy` struct, a non-niche `Option`).
The `:heap` header is laid out by value (`hdr_byval = byval || def->is_heap`
in the typedef/ctor emitter), but three readers and writers still asked
`adt_byval` alone -- which is false for a `:heap` owner -- and so treated the
field as a boxed word:

- **The constructor call** (`field_inline` in `emit_expr.c`) boxed the
  argument and passed its address to a `tur_adt_Pos` parameter. It now takes
  the inline path for a `:heap` owner too.
- **Both match-binder sites** (`inline_byval`) dereferenced the field as a box.
  They now read it in place for a `:heap` owner, gated per field on
  `adt_field_is_inline_byval`.
- **A parametric `:heap` owner** (`(defdata Bx :heap [A] (Bx A))`,
  `(defstruct HB :heap [A] [v : A ...])`) stores a WIDE by-value element
  boxed in its monomorph (B4 slice 2, `wide_box`), but its constructor
  assigned the aggregate straight into the `int64_t` word, and `.v` read the
  word back as the value. The monomorph constructor in `types.c` now mallocs
  the box, stores its address and notes it for regions
  (`TUR_REGION_NOTE_WORDS`, per the region store hook rule), and the
  `GET_FIELD` heap-receiver branch derefs it
  (`*(T *)(intptr_t)(recv->field)`). A narrow element (`int32` pair) stays
  inline and is untouched.

**Pinned by** `heap-adt-byvalue-field` -- a `:heap` sum with a `Pos` field, a
single-ctor `:heap` with one, the report's non-niche `(Option S)` (`Some` and
`None` both read back), a `:heap` struct with a by-value field read and
`set!`, and both parametric shapes at a wide (`Pos`), a narrow (`Sm`) and an
`Option` element. Passes under gcc, clang and the JIT; the niche fixture
`heap-adt-niche-option-field` still passes.

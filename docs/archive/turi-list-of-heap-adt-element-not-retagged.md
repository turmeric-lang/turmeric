# `tur --interpret`: a `:heap` defdata through `list-of` comes back as a bare word -- RESOLVED

**Severity: medium (interpreter-only wrong behaviour).** Under
`tur --interpret`, `(list-first (list-of (B "a")))` with a `:heap` `Box`
fails with `eval: match: no arm matched`. The compiled path prints `a`. Found
2026-10-17 while fixing
[typed-list-byvalue-adt-element-unboxed](../archive/typed-list-byvalue-adt-element-unboxed.md),
which fixed the same shape for a non-`:heap` defdata; reproduces on `main` at
`8c539640` too.

## Repro

```turmeric
(load "stdlib/list-typed.tur")
(defdata Box :heap (B cstr))
(defn box-str [b : Box] : cstr (match b (B s) s))
(defn main [] : int
  (let [ys (list-of (B "a") (B "b"))]
    (println (box-str (list-first ys)))   ; turi: match: no arm matched
    0))
```

`(list-cons (B "a") (list-nil))` works; only the variadic rest loses it.

## Root cause

- The interpreter's `EX_CONS_LIST` (`src/turi/eval.c`) stores each rest
  element's raw word in a `{head, tail}` cell, matching the compiled ABI. A
  constructor value is a `TuriStruct` (`adt_ctor_native`), so the cell keeps
  its pointer and drops the tag.
- `list-first`'s `(:: w A)` re-tags through `try_retag_carrier_struct`.
  Since the by-value fix it recognises any non-`:heap` defdata constructor
  value. It deliberately skips `:heap` types, because a `:heap` type's word
  may be a native handle rather than a `TuriStruct` (`MutableMap`: reading
  its header as a struct was an ASan heap-buffer-overflow in `mutmap-eq`).

## Fix directions

- Tell a `TuriStruct` apart without reading the pointee: for example, check
  `turi_prov_check(env, TURI_HK_STRUCT, p)` with provenance noting always on
  for rest cells, or keep a small set of struct pointers `EX_CONS_LIST` gave
  up the tag of.
- Or have `EX_CONS_LIST` box a `TURI_STRUCT` element the way the carrier
  re-tag can recognise safely.

## Resolution

Resolved 2026-10-17 with the first fix direction: the interpreter now knows
which words are struct pointers without reading them.

- `EX_CONS_LIST` records each struct head whose tag it strips in
  `env->tagless_structs`. It is a pointer set like the provenance registry
  and shares its code (`pset_add` / `pset_has` / `pset_forget`, factored out
  of `turi_prov_*`), but it is always on.
- `pool_reclaim_one` forgets a struct when it goes back to the free list, so
  a reused block is never taken for the value that used to live there.
- `try_retag_carrier_struct` re-tags a `:heap` type's word only when the set
  holds it. It then still checks name, field count and constructor, as for a
  by-value type. A `MutableMap` handle, which is never in the set, is not
  read.

**Pinned by** `list-of-heap-adt-elements`: `list-of` over a one-field, a
multi-constructor and a parametric `:heap` defdata and a `:heap` defstruct,
read back with `list-first` / `list-rest`. It passes on both engines; the old
interpreter fails with "match: no arm matched". `mutmap-eq` and
`mutmap-typed-consumer`, the fixtures the first attempt overran, still pass
under ASan, and so do `tur_eval_sandbox` and `tur_security_driver`.

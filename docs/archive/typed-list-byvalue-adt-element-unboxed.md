# A by-value `defdata` as a `(List A)` element is never boxed -- RESOLVED

**Severity: medium-high.** `list-map` producing a `(List T)` of a by-value
(default) `defdata` segfaults at runtime with no diagnostic, and `list-of`
with such elements fails in cc. The same programs work with the type
declared `:heap`. Found 2026-10-16 prototyping hiccup vector rewriting
(`docs/upcoming/spices/xml-html-parsers-plan.md`, D-1); reproduced on the
v0.65.2 release and on `main` at `d60c40f6`.

## Repro

```turmeric
(load "stdlib/list-typed.tur")
(defdata Box (B cstr))
(defn box-str [b : Box] : cstr (match b (B s) s))
(defn main [] : int
  (let [xs (list-of "a" "b")
        ys (list-map xs (fn [r : cstr] : Box (B r)))]
    (println (box-str (list-first ys)))   ; SIGSEGV
    0))
```

`(list-of (B "a") (B "b"))` in place of the `list-map` fails in cc instead:
`error: aggregate value used where an integer was expected`. A
multi-constructor `defdata` behaves the same. Adding `:heap`
(`(defdata Box :heap (B cstr))`) makes both work.

## Root cause (from the emitted C)

- The lambda goes to `list-map` (`stdlib/list-typed.tur:223`, generic
  `[A B]`, not specialized here) as a fat closure whose shim returns the
  struct by value: `static tur_adt_Box __tur_fatshim_tur_adt_Box_const_char__(void *, const char *)`
  (shim emitted from `src/compiler/emit_module.c`, around line 1657).
- The generic `list_hymap` calls it through the int64 carrier ABI, so the
  struct result is read as a word and stored in the cons cell unboxed.
- `list_first__spec__tur_adt_Box_int64_t` then reads the cell as a boxed
  pointer, `*(tur_adt_Box *)(intptr_t)word`, and faults.
- `list-of` (`stdlib/list-typed.tur:44`) collects its `& xs : A` rest
  into int64 cons cells, and an aggregate cannot be assigned to one.

## Fix directions

- Box a by-value aggregate where a closure result crosses into a generic
  `(fn [A] B)` carrier parameter (malloc and copy, the shape the
  `list-first` spec already reads), or specialize the HOF for an aggregate
  `B`.
- Box aggregate elements in the variadic rest collector the same way.
- See `docs/archive/aggregate-carrier-abi-plan.md` for the existing
  carrier-ABI work this belongs to.

Also seen in the same prototype, not minimized: with a `:heap` `Html`, a
`list-foldl` specialization passed a `const char *` where it declared
`tur_adt_Html *` (`-Wincompatible-pointer-types`, a warning under gcc 13
and an error under gcc 14). The program ran correctly.

## Resolution

Resolved 2026-10-17. Three separate crossings each lost the box, and each now
heap-boxes the aggregate (malloc, copy, region note) where it enters the
carrier, which is the shape `list-first`'s spec already read:

- **A bare function at a type-variable result** (`(list-map xs wrap)`, and
  a lambda that captures nothing). The `EX_FN_TO_FAT` box's slot 0 is spelled
  from what the selected callee reads (`fat_box_sink_type`). The generic
  `list-map` reads `B` as the word, and the scalar call adapter declines an
  aggregate result, so the typed shim answered the struct by value through
  the word-returning cast. `ensure_boxres_word_adapter`
  (`src/compiler/emit_module.c`) now calls the function at its own signature
  (converting scalar arguments as the call adapter does), boxes the result
  and returns the pointer.
- **A capturing closure at the same sink.** The elaborator wraps a closure
  in a `{ adapter, handle }` box for a sink the callee reads as words
  (`fnsan-fat-closure-at-tyvar-sink`). That wrap now also fires for a
  closure answering a by-value aggregate where the sink's result is a bare
  type variable, and `fat_closure_tyvar_sink_adapter` falls back to the
  same boxing adapter (handle mode) when the sink reads the result erased.
  A spec clone that reads `B` at its concrete type gets the closure as
  before. Like the existing wrap, this applies only at a sink proven not to
  keep the closure (`list-map`'s `f` is one), since the wrapper borrows the
  handle; a generic that stores such a closure is not covered.
- **`list-of`'s variadic rest.** `EX_CONS_LIST` routed only a by-value
  multi-constructor sum through the escaping carrier bridge (SR1); a
  one-constructor product hit the `(int64_t)(intptr_t)` cast. Products take
  the bridge too. `vec-of` was already right and is unchanged.

Under `tur --interpret`, `(list-of (B "a"))` failed differently ("match: no
arm matched"): the rest cell keeps a constructor value's struct pointer, and
`(:: w A)`'s re-tag (`try_retag_carrier_struct`, `src/turi/eval.c`)
recognised only multi-field records. It now also recognises any non-`:heap`
defdata constructor value whose name, field count and constructor match the
type's own. The `:heap` case still fails the same way under the interpreter;
it is filed as
[turi-list-of-heap-adt-element-not-retagged](../reported/turi-list-of-heap-adt-element-not-retagged.md).

**Pinned by** `list-byvalue-adt-elements`: `list-map` to a one-field
defdata through a lambda, a named function and a capturing closure;
`list-of` over one-field and multi-constructor defdata; a multi-constructor
result through `list-map` read back by `list-foldl`. It passes on both
engines and under `tests/run-fnsan.sh`; the old compiler fails it in cc.

**Not this bug:** `(list-map floats (fn [f : float] : Shape ...))` still
segfaults, and `(list-map floats (fn [f : float] : int 7))` answers
`4619679907765970534` (the bits of the 7.1 left in xmm0). There the inner
`list-map-onto__` clone binds `B := float`, its caller's `A`, and calls `f`
as `double (*)(void *, double)`. That is
[nested-generic-tyvar-name-capture](../reported/nested-generic-tyvar-name-capture.md),
and possibly also behind the unminimized `list-foldl` warning noted above.

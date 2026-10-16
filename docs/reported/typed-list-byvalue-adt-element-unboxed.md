# A by-value `defdata` as a `(List A)` element is never boxed

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

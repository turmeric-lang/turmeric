# `(Some x)` into a niche `(Option S)` field of a `:heap` constructor is passed as `int64_t`

**Severity: medium** (clang rejects the emitted C; gcc warns).

**Status: OPEN.** Found 2026-10-10 implementing
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

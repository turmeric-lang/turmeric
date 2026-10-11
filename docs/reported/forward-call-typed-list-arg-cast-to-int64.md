# Calling a later-defined function with a `(Cons N)` parameter casts the argument to `int64_t`

**Severity: medium** (clang rejects the emitted C; gcc warns). It bites the
most natural code: a helper that walks the result of `node-children` or
`find-all`, written below its caller.

**Status: OPEN.** Found 2026-10-10 implementing
[xml-html-parsers-plan](../upcoming/spices/xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/xml/tests/dom_tree.tur` by
defining the helper first.

## Repro

````turmeric
;; src/geo/n.tur -- exports N and two
(defmodule geo/n (export N two)
(defopaque N :ptr<void>)
(defn __p [] : ptr<void>
  ```c
  return (void *)"x";
  ```)
(defn two [] : (Cons N) (tcons-of (:: (__p) N) (tcons-of (:: (__p) N) (tnil)))))

;; main.tur
(defmodule main (export)
(import geo/n :refer [N two])
(defn child [i : int] : N
  (let [r (nthx (two) i)] r))           ;; nthx is defined below
(defn nthx [xs : (Cons N) i : int] : N
  (if (= i 0) (thead xs) (nthx (ttail xs) (- i 1))))
(defn main [] : int
  (child 1)
  0))
````

`CC=clang tur run main.tur`: `incompatible integer to pointer conversion
passing 'int64_t' to parameter of type 'tur_adt_Cons__N *'`. Moving `nthx`
above `child` compiles. The call site seems to use a provisional (carrier)
signature for the not-yet-elaborated callee.

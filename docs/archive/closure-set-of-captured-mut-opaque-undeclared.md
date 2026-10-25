# `set!` of a captured `^mut` opaque inside a closure emits an undeclared identifier

**Severity: medium** (compile failure in the emitted C).

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)). Found
2026-10-10 implementing
[xml-html-parsers-plan](xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/xml/tests/sax_events.tur`
(the retained String is stashed through C).

## Repro

````turmeric
(defopaque S :ptr<void>)
(defn __mk [] : ptr<void>
  ```c
  return (void *)"x";
  ```)
(defn mk [] : S (:: (__mk) S))
(defn call [f : (fn [int] int)] : int (f 1))
(defn main [] : int
  (let [^mut kept (mk)]
    (call (fn [x : int] : int (do (set! kept (mk)) x)))
    (println "ok"))
  0)
````

```
error: use of undeclared identifier 'kept_8'
```

The same shape with `^mut n 0` (an `int`) promotes `n` to a shared cell and
works -- the SAX tests count events that way. With an opaque, the promotion
does not happen and the closure body refers to the original local.

## Resolution

The `^mut`-into-a-shared-cell conversion
([compiled-closure-copies-a-captured-mut](compiled-closure-copies-a-captured-mut.md))
admitted scalars, `cstr`, `sym` and `any` only (`cell_elem_type_ok`,
`src/compiler/elab_forms.c`). Every other type kept the copying behaviour, and
a closure that only WRITES a variable does not capture it at all
(`collect_free_vars` does not count a `set!` target as a read), so the lambda
named the outer local.

A `defopaque` handle is one word with nothing for scope exit to release -- the
same as a `cstr` -- so it joins the cell types. An opaque with drop glue keeps
the copying behaviour, like every owning type.

**Pinned by** `closure-captured-mut-opaque` (the report's write-only closure,
and a second closure reading what the first wrote).

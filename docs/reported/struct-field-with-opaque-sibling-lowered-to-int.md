# A struct with a `defopaque` field and an aggregate field lowers the aggregate to `int64_t`

**Severity: medium** (compile failure in the emitted C; no miscompile seen).

**Status: OPEN.** Found 2026-10-10 implementing
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

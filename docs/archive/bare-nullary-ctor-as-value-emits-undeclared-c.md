# A bare nullary constructor used as a value emits an undeclared C identifier

**Severity:** low (C compile error naming a generated identifier; `(Red)`
works).

Found 2026-10-25 writing `spices/lmdb`'s flag vectors, where `[DbiCreate]`
reads naturally and `[(DbiCreate)]` is what compiles.

## Repro

```turmeric
(defdata Color :copy (Red) (Green))
(defn main [] : int
  (let [c Red]
    (do (println (match c (Red) 1 (Green) 2)) 0)))
```

```
error: 'Red_4' undeclared (first use in this function)
```

`tur check` accepts it. Same inside a vector literal, `[Red Green]`.

## Root cause

The symbol resolves to the constructor's binding and elaborates as a variable
reference typed `Color`, so the emitter writes the binding's C name -- which
no declaration ever introduces (a nullary constructor is emitted as a
function or a constant initializer, not a variable).

## Fix directions

- Elaborate a bare reference to a nullary constructor as its application,
  `(Red)` -- what a reader means, and what Haskell / OCaml do.
- Or reject it with a diagnostic pointing at `(Red)`.

## Resolution (2026-10-29)

Fixed the first way the report suggested. In `elab_form`'s `F_SYM` arm
(`src/compiler/elab_toplevel.c`), a symbol whose global binding is a nullary
constructor elaborates as its application `(Red)`. The interpreter had its own
face of the same bug: `(let [c Red] (match c ...))` panicked `match: no arm
matched` under `--interpret`. Both engines now agree. A local that shadows
the name is not global, so it is left alone.

Pinned by `tests/fixtures/bare-nullary-ctor-value`: `let`, a vector literal
mixing bare and applied constructors, a direct argument, a non-`^copy` ADT,
and a parametric ADT's nullary constructor at a pinned type.

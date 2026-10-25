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

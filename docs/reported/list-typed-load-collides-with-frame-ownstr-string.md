# Loading `stdlib/list-typed.tur` in a module collides with tur-frame's `String`

**Severity: low-medium (two ordinary dependencies cannot be used together).**
Found 2026-10-29 executing stats-formula-plan.

## Repro

In turmeric-spices `spices/frame`, with `tests/zzc/lt.tur`:

```turmeric
(load "stdlib/list-typed.tur")
(defmodule zzc/lt
  (export one)
  (defn one [] : (List int) (list-of 1)))
```

and `tests/zzc/main.tur`:

```turmeric
(defmodule zzc/main
  (export main)
  (import frame/print :refer [print-frame])
  (import zzc/lt :refer [one])
  (defn main [] : int (println (list-count (one))) 0))
```

`tur run -I tests tests/zzc/main.tur`:

```
stdlib/string.tur:32:22: error: defopaque: 'String' is already defined
```

list-typed loads stdlib/string.tur (for `list->string`), whose global
`String` meets the module-defined `String` of `frame/ownstr`. The same
`load` at the top of the *main* file, before the import, compiles. Reproduces
on `main` (1ecdacc9), after `module-type-names-collide-across-modules` was
resolved -- that fix covers two modules' types, not a `load`ed global type
against a module's.

## Fix directions

- `frame/ownstr` is a self-declared mirror of stdlib `String`, kept only
  until `compiled-string-return-int-conversion` was fixed; that report is
  archived (resolved 2026-07-22), so ownstr can now collapse onto stdlib
  `String` and the collision disappears for tur-frame users.
- Generally: a `load`ed file's types should not collide with a module's
  same-named private type, as module types no longer collide with each other.

## Workaround

Use the auto-loaded `(Cons A)` instead of `(List A)` (what
spices/stats-formula does; its `formula/seq` has the few helpers needed).

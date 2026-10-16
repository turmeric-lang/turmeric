# `int64` / `float64` are rejected as return-type annotations

**Severity:** low-medium. Documented type names
([sized-primitives-guide](../guides/sized-primitives-guide.md) lists `int64` as
the long form of `i64`) work for parameters but not for return types.

## Repro

```turmeric
(defn h [x : int] : int64 x)
(defn main [] : int (println (h 3)) 0)
```

```
error: unsupported return type keyword 'int64': it is not a built-in type and
is not bound by any parameter; declare it (e.g. `[int64]` type params) ...
```

`: i64` works, as does a parameter typed `int64`. `float64` behaves the same.

## Root cause

`src/compiler/elab_fns.c:elab_defn`'s return-type ladder asks
`typekind_from_symbol`, which maps `int64` to `TY_INT` (and `float64` to
`TY_FLOAT`). The sized-kind branch then excludes it as not a sized kind, and
the exact-name branch matches only `"int"` / `"float"`. The name falls through
to the implicit-type-variable path, which the KB-026 check rejects.
(`typekind_from_symbol`'s separate `int64 -> TY_INT64` and
`float64 -> TY_FLOAT64` arms are unreachable for the same reason.)

## Fix directions

- Accept the alias names explicitly in the return-type ladder, using the
  same table the parameter path uses. Better, share one resolver between
  parameters and returns.

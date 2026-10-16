# Non-symbol items in an effect row are silently dropped

**Severity:** low-medium. A typo in an effect annotation silently makes the
row **empty**. That is the most permissive-looking and least checked
annotation there is, and nothing warns.

## Repro

```turmeric
(defeffect Ask [] : int)
(defn f [] #fx{:Ask} : int 0)
(defn g [] #fx{"Ask" 7} : int 0)
(defn main [] : int 0)
```

`tur check` succeeds, and `tur --dump-effects check` shows:

```
defn f : #{}
defn g : #{}
```

## Root cause

The row parsers keep only `F_SYM` items and skip everything else without a
diagnostic. The filter appears in `elab_defn`, `elab_fn`,
`type_expr_from_form`, and the struct-field and method row parsers
(`src/compiler/elab_*.c`).

## Fix directions

- Raise TUR-E0026 (unresolved effect) or a new code on any non-symbol item,
  pointing at the item's span. Share one row-item parser between the sites.

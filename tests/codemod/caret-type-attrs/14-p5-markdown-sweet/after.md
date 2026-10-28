# Attributes

A fence:

```turmeric
(defeffect ^capability ^extends IO FS [] : nil)
(defn ^no-unwind f [] : int 1)
```

A sweet-exp fence:

```sweet-exp
defeffect ^capability ^extends IO FS [] : nil
defn ^used g [] : int 2
```

A code span: `(defeffect ^extends IO Write [s : cstr] : nil)` and `(defn ^export-as "c" h [] : int 3)`.

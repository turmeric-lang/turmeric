# Attributes

A fence:

```turmeric
(defeffect FS [] : nil ^extends IO ^capability)
(defn #[no-unwind] f [] : int 1)
```

A sweet-exp fence:

```sweet-exp
defeffect FS [] : nil ^extends IO ^capability
defn #[used] g [] : int 2
```

A code span: `(defeffect Write [s : cstr] : nil ^extends IO)` and `(defn (export-as "c") h [] : int 3)`.

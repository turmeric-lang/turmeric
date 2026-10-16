# Drop glue frees a closure field with a bare `free()` -- invalid pointer

**Severity:** high. The program crashes on an ordinary (non-sanitized) build:
glibc aborts with `free(): invalid pointer` when a struct or ADT holding a
capturing closure is dropped.

## Repro

```turmeric
(defstruct H [f : (fn [int] int) n : int])

(defn main [] : int
  (let [k 10
        r (rc/of (H (fn [x : int] : int (+ x k)) 1))]
    (println (.n r)))
  0)
```

```turmeric
(defdata Chain [] (Link [f : (fn [int] int) tl : Chain]) (End))

(defn run [c : Chain x : int] : int
  (match c
    (Link f tl) (run tl (f x))
    (End) x))

(defn main [] : int
  (let [k 10
        c (Link (fn [x : int] : int (+ x k)) (Link (fn [x : int] : int (* x 2)) (End)))]
    (println (run c 1)))
  0)
```

`tur run` on either: `free(): invalid pointer`, then `Aborted`. Under ASan
the report is `attempting free on address which was not malloc()-ed`.

## Root cause

`src/compiler/emit_module.c:emit_adt_glue_field_drops` releases a boxed
fn-typed field with a plain `free(field)`. A capturing closure handle points
**past** an 8-byte header, so the pointer is not the allocation's start. The
right release is `TUR_CLOSURE_DROP`, which the `drop_fnfields_<T>` glue
already uses for the same kind of field.

## Fix directions

- Emit `TUR_CLOSURE_DROP(field)` for fn fields in
  `emit_adt_glue_field_drops`, as `drop_fnfields_` does.
- Check that static (non-heap) fat-shim boxes stored in such a field are safe
  under `TUR_CLOSURE_DROP`. They must not be freed at all.
- Add both repros as fixtures, with a `requires.leak-check` twin.

Found while writing
[emitted-code-ownership-guide](../guides/emitted-code-ownership-guide.md).

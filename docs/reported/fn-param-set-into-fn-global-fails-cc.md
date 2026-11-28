# Storing a closure parameter into a fn-typed global fails in cc

**Severity: low (compile error; works through a struct field).** Found
2026-11-28 writing negatives for
[let-bound-closure-shared-with-fnfield-struct-leaks](../archive/let-bound-closure-shared-with-fnfield-struct-leaks.md);
reproduces on `main`.

## Repro

```turmeric
(def ^mut keep : (fn [int] int) (fn [x : int] : int x))
(defn stash [f : (fn [int] int)] : int (set! keep f) 0)
(defn main [] : int
  (stash (fn [x : int] : int (+ x 1)))
  (println (keep 1))
  0)
```

`tur run` fails in cc: `incompatible types when assigning to type 'void *'
from type 'tur_poly_fn_t'` at `keep = f;`. A closure-typed parameter rides
the 16-byte poly carrier (`tur_poly_fn_t`), the global is the one-word fat
handle, and the `set!` emits a plain assignment. The same `set!` from inside a
lambda that captured a let-bound closure warns `-Wint-conversion` instead
(an error under GCC 14).

## Fix directions

Box the carrier into a fat handle at the store, as a constructor's fn-field
argument already does (`EX_POLY_TO_FAT`, carrier-fn-param-refused-by-
constructor-fn-field), and give the box an owner -- the global's next `set!`
or nobody, as for any other value stored in a global.

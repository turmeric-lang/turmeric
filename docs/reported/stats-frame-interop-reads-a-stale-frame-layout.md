# stats reads and builds frames with a layout frame does not use: `ols-frame` segfaults

**Severity:** medium (crash). Every stats function that takes or returns a
`frame` handle -- `ols-frame` (stats/regress), the frame-shaped helpers in
stats/cov and stats/fmt (`fit-coefs-frame`) -- casts it to a struct of
stats' own, and that struct's field order is not frame's. Compiled or in a
notebook cell, `ols-frame` on a frame `read-csv-string` returned dereferences
garbage.

Filed 2026-10-10, found running the notebook examples
([notebook-cells-cannot-call-inline-c-spices](notebook-cells-cannot-call-inline-c-spices.md)).

## Repro

In `turmeric-spices/spices/stats`, as `tests/zz_probe_test.tur`:

```turmeric
(defmodule stats/tests/zzprobe
  (import frame/csv     :refer [read-csv-string])
  (import stats/regress :refer [ols-frame])
  (defn main [] : int
    (let [df  (read-csv-string "x,y\n1,2.1\n2,3.9\n3,6.2\n4,8.1\n5,9.8\n" 0 0 1 0 "")
          fit (ols-frame df "y" (cons (:: "x" :int) 0) 1)]
      (println fit)
      0)))
```

```
tur run tests/zz_probe_test.tur
Segmentation fault
```

Adding `(import stats/fmt :refer [print-fit])` to the same module does not
get that far: the program fails to compile, `conflicting types for
'__test_result_t'` (`src/stats/fmt.tur:23` and `src/stats/test.tur:78`,
which fmt imports, both typedef it at file scope).

## Root cause

frame defines its handle as (`spices/frame/src/frame/frame.tur`, the
`struct __frame_frame` blocks):

```c
struct __frame_frame  { int64_t schema; int64_t n_cols; int64_t n_rows; int64_t *columns; };
struct __frame_schema { int64_t n_fields; int64_t *fields; };   /* fields[i] -> __frame_field * */
struct __frame_field  { char *name; int64_t type_tag; int64_t nullable; };
```

stats re-declares it, in `src/stats/regress.tur:25`, `cov.tur:34`,
`fmt.tur:43` and three more files, as:

```c
struct __frame  { int64_t nrows; int64_t ncols; int64_t schema_ptr; int64_t *columns; };
struct __schema { int64_t arity; struct __field *fields; };     /* an inline array */
struct __field  { int64_t name_ptr; int64_t type_tag; int64_t nullable; };
```

Two disagreements: the first three words are in a different order (stats
reads `n_rows` where frame keeps `schema`), and stats indexes the schema's
fields as an array of structs where frame stores an array of pointers. So
`ols-frame`'s `fr->schema_ptr` is frame's `n_rows`, and `_FIND_COL` walks
that as a schema. The frames stats *builds* (cov's correlation matrix,
fmt's `fit-coefs-frame`) have the same wrong shape, so frame's own
accessors misread them in turn.

The buffers disagree as well as the headers. frame allocates a column's
`values` 64-byte aligned behind a 16-byte header that holds the raw
pointer, and `frame-free` / `column-free` read it back from `values - 16`
(`spices/frame/src/frame/column.tur:78`, `frame.tur:298`). stats builds its
columns over a plain buffer -- `rnorm`'s values start 8 bytes into it
(`src/stats/dist.tur:696`) -- so freeing a stats column or frame with
frame's free hands libc a pointer it never returned. Swapping the struct
fields into frame's order would fix the reads and leave this.

The same layout is declared in `sample.tur:24`, `summary.tur:36` and
`test.tur:66` too; every stats function that touches a frame is affected,
and no stats test builds one with frame, which is how it drifted.

Every handle here is `:int`, so the type checker never sees a frame cross
the spice boundary -- the hazard CLAUDE.md's "No lazy `:int` stand-ins" rule
describes.

## Fix directions

1. **Use frame's accessors and constructors, not its struct.** stats
   depends on frame (`:spices`); `frame-nrows`, `frame-column`,
   `frame-schema` and the schema field accessors are exported. Reading
   through them removes the duplicate layout outright, and building columns
   and frames through frame's `column-*` / `frame` / `schema` / `field`
   gives them frame's buffer protocol, which a layout fix alone does not.
   A stats test that reads a CSV with frame and passes it to each
   frame-taking function would have caught both halves.
2. **At least share the declaration.** If the C path has to stay, one header
   both spices include (frame shipping it) keeps the layouts from drifting
   again; the `__test_result_t` collision wants the same treatment.
3. **Typed handles.** A `defopaque Frame` in frame, used in stats'
   signatures, would have made the mismatch a type error at the first
   cross-spice call.

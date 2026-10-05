---
title: Frame Guide
category: Data Structures and Libraries
description: In-memory columnar dataframes with tur-frame -- building, querying, joining, and exporting via the Arrow C Data Interface
---

# Frame Guide

`tur-frame` is a pure-Turmeric spice that provides in-memory columnar
dataframes modeled on R's `data.frame`, pandas `DataFrame`, and Racket's
`data-frame`. Columns are stored in **Apache Arrow's in-memory format**
(C Data Interface compatible), which enables zero-copy hand-off to Python,
R, DuckDB, and Polars without serialization.

> The "arrow" in `arrow-export` / `arrow-import` / `arrow-export-column`
> refers to **Apache Arrow** (the columnar format), not the `Arrow`
> typeclass. For Turmeric's arrow typeclass hierarchy (`Category`, `Arrow`,
> `ArrowZero`, `Kleisli`, signal graphs), see
> [arrows-guide.md](arrows-guide.md).

## Installing

```sh
tur add https://github.com/turmeric-lang/turmeric-spices \
  --ref frame-v0.1.0 --subdir spices/frame --name frame
tur fetch
```

`tur add` writes the dependency into your `build.tur`:

```turmeric no-check
(defpackage myapp
  :name "myapp"
  :version "0.1.0"
  :spices #map{
    "frame" #map{:url    "https://github.com/turmeric-lang/turmeric-spices"
                 :ref    "frame-v0.1.0"
                 :subdir "spices/frame"}
  })
```

## Importing

```turmeric
(import frame/frame  :refer [frame frame-from-cols frame-from-rows
                               frame-nrows frame-ncols frame-schema
                               frame-column frame-head frame-tail
                               frame-slice frame=?])
(import frame/column :refer [column-int64 column-float64 column-utf8
                               column-type column-length column-get
                               column-int64-at column-float64-at
                               column-utf8-at])
(import frame/type   :refer [type-int64 type-float64 type-utf8 type-name])
(import frame/schema :refer [schema])
(import frame/select :refer [select-cols drop-cols rename with-col
                               map-col mutate])
(import frame/filter :refer [filter drop-nulls distinct sample])
(import frame/sort   :refer [arrange])
(import frame/group  :refer [group-by agg agg-sum agg-mean agg-count
                               agg-min agg-max summarize])
(import frame/join   :refer [inner-join left-join join])
(import frame/reshape :refer [melt])
(import frame/csv    :refer [read-csv write-csv default-csv-opts])
(import frame/interop :refer [arrow-export arrow-import])
(import frame/print  :refer [print-frame frame-describe])
```

```sweet-exp
import frame/frame  :refer [frame frame-from-cols frame-from-rows
                              frame-nrows frame-ncols frame-schema
                              frame-column frame-head frame-tail
                              frame-slice frame=?]
import frame/column :refer [column-int64 column-float64 column-utf8
                              column-type column-length column-get
                              column-int64-at column-float64-at
                              column-utf8-at]
import frame/type   :refer [type-int64 type-float64 type-utf8 type-name]
import frame/schema :refer [schema]
import frame/select :refer [select-cols drop-cols rename with-col
                              map-col mutate]
import frame/filter :refer [filter drop-nulls distinct sample]
import frame/sort   :refer [arrange]
import frame/group  :refer [group-by agg agg-sum agg-mean agg-count
                              agg-min agg-max summarize]
import frame/join   :refer [inner-join left-join join]
import frame/reshape :refer [melt]
import frame/csv    :refer [read-csv write-csv default-csv-opts]
import frame/interop :refer [arrow-export arrow-import]
import frame/print  :refer [print-frame frame-describe]
```

---

## Building a Frame

### From parallel columns

```turmeric
;; Build columns from value lists.  Pass 0 (nil) for validity to mean "all valid".
(let [names (column-utf8    (list "Alice" "Bob" "Carol") 0 0)
      ages  (column-int64   (list 30 25 35) 0 0)
      score (column-float64 (list 8.5 7.0 9.1) 0 0)

      s (schema (list (field "name"  (type-utf8)    0)
                      (field "age"   (type-int64)   0)
                      (field "score" (type-float64) 0)))

      df (frame s (list names ages score))]

  ;; quick sanity check
  (print-frame df)
  ;; =>  name   age  score
  ;;    Alice    30    8.5
  ;;      Bob    25    7.0
  ;;    Carol    35    9.1
  )
```

```sweet-exp
let [names column-utf8(list("Alice" "Bob" "Carol") 0 0)
     ages  column-int64(list(30 25 35) 0 0)
     score column-float64(list(8.5 7.0 9.1) 0 0)

     s schema(list(field("name"  type-utf8()    0)
                   field("age"   type-int64()   0)
                   field("score" type-float64() 0)))

     df frame(s list(names ages score))]

  print-frame(df)
```

### From an association list

`frame-from-cols` infers the schema from the column types:

```turmeric
(let [df (frame-from-cols
           (vec-of (cons "name" (column-utf8  (list "Alice" "Bob") 0 0))
                   (cons "age"  (column-int64 (list 30 25) 0 0))))]
  (print-frame df))
```

```sweet-exp
let [df frame-from-cols(vec-of(cons("name" column-utf8(list("Alice" "Bob") 0 0))
                               cons("age"  column-int64(list(30 25) 0 0))))]
  print-frame(df)
```

### From rows

`frame-from-rows` accepts a schema and a list of rows. Each row is a list
of values in schema order. This is slower than column-wise construction and
is intended for small literals and tests:

```turmeric
(let [s  (schema (list (field "x" (type-int64)   0)
                       (field "y" (type-float64) 0)))
      df (frame-from-rows s
           (list (list 1 1.5)
                 (list 2 2.5)))]
  (print-frame df))
```

```sweet-exp
let [s  schema(list(field("x" type-int64()   0)
                    field("y" type-float64() 0)))
     df frame-from-rows(s list(list(1 1.5)
                               list(2 2.5)))]
  print-frame(df)
```

### From CSV

`read-csv` infers column types from the first 100 rows by default:

```turmeric
(let [df (read-csv "data/sales.csv" (default-csv-opts))]
  (match df
    [(ok f)  (print-frame f)]
    [(err e) (do (println "error:") (println e))]))
```

> `println` takes exactly one argument (there is an overload per argument type,
> each arity 1), so a label and a value are two calls -- or one
> `(println (str-concat "error: " e))` when the value is already a `cstr`.

```sweet-exp
let [df read-csv("data/sales.csv" default-csv-opts())]
  match df
    [ok(f)  print-frame(f)]
    [err(e) do(println("error:") println(e))]
```

The type inference order is int64 -> float64 -> date32 -> timestamp -> bool -> utf8.
Use `read-csv-typed` to supply an explicit schema and skip inference:

```turmeric
(let [s (schema (list (field "id"    (type-int64)   0)
                      (field "name"  (type-utf8)    0)
                      (field "price" (type-float64) 0)))
      df (read-csv-typed "products.csv" s (default-csv-opts))]
  ...)
```

```sweet-exp
let [s schema(list(field("id"    type-int64()   0)
                   field("name"  type-utf8()    0)
                   field("price" type-float64() 0)))
     df read-csv-typed("products.csv" s default-csv-opts())]
  ...
```

Write a frame back to CSV with `write-csv`:

```turmeric
(write-csv df "output.csv" (default-csv-opts))
```

```sweet-exp
write-csv(df "output.csv" default-csv-opts())
```

---

## Selecting, Filtering, and Mutating

Frames are **immutable values**. Every transformation returns a new frame;
unchanged columns are shared with the original (no copying).

### select-cols and drop-cols

```turmeric
;; Keep only named columns
(let [small (select-cols df (list "name" "score"))]
  (print-frame small))

;; Drop named columns
(let [no-age (drop-cols df (list "age"))]
  (print-frame no-age))
```

```sweet-exp
let [small select-cols(df list("name" "score"))]
  print-frame(small)

let [no-age drop-cols(df list("age"))]
  print-frame(no-age)
```

### rename and with-col

```turmeric
;; Rename a column
(let [df2 (rename df "score" "rating")]
  (print-frame df2))

;; Add or replace a column (length must match nrows)
(let [bonus (column-float64 (list 100.0 80.0 120.0) 0 0)
      df2   (with-col df "bonus" bonus)]
  (print-frame df2))
```

```sweet-exp
let [df2 rename(df "score" "rating")]
  print-frame(df2)

let [bonus column-float64(list(100.0 80.0 120.0) 0 0)
     df2   with-col(df "bonus" bonus)]
  print-frame(df2)
```

### filter

Pass a predicate `(fn [frame row-index] :int)` -- return non-zero to keep the row:

```turmeric
;; Keep rows where age >= 30
(let [seniors (filter df (fn [f i]
                           (let [age (column-int64-at (frame-column f "age") i)]
                             {age >= 30})))]
  (print-frame seniors))
```

```sweet-exp
let [seniors (filter df (fn [f i]
                          (let [age (column-int64-at (frame-column f "age") i)]
                            {age >= 30})))]
  print-frame(seniors)
```

### drop-nulls and distinct

```turmeric
;; Drop rows with any null in the named columns; pass 0 for all columns
(let [clean (drop-nulls df (list "score"))]
  ...)

;; De-duplicate by named columns; 0 = all columns
(let [uniq (distinct df (list "name"))]
  ...)
```

```sweet-exp
let [clean drop-nulls(df list("score"))]
  ...

let [uniq distinct(df list("name"))]
  ...
```

### mutate

`mutate` derives a new column by calling `row-fn` once per row:

```turmeric
;; Add a "grade" :utf8 column derived from score
(let [df2 (mutate df "grade" (type-utf8)
            (fn [f i]
              (let [s (column-float64-at (frame-column f "score") i)]
                if {s >= 9.0} "A"
                if {s >= 7.0} "B" "C"))))]
  (print-frame df2))
```

```sweet-exp
let [df2 (mutate df "grade" (type-utf8)
           (fn [f i]
             (let [s (column-float64-at (frame-column f "score") i)]
               (if {s >= 9.0} "A"
               (if {s >= 7.0} "B" "C")))))]
  print-frame(df2)
```

---

## Sorting and De-duplicating

`arrange` sorts by one or more columns. The `keys` argument is two parallel
lists: column names and sort directions (0 = ascending, 1 = descending):

```turmeric
;; Sort by score descending, then by name ascending
(let [sorted (arrange df
               (list "score" "name")
               (list 1       0))]
  (print-frame sorted))
```

```sweet-exp
let [sorted arrange(df
              list("score" "name")
              list(1       0))]
  print-frame(sorted)
```

`arrange-indices` returns the permutation without applying it, so you can apply
the same ordering to multiple frames with `reorder`:

```turmeric
(let [idx     (arrange-indices df (list "score") (list 1))
      sorted1 (reorder df   idx)
      sorted2 (reorder df2  idx)]
  ...)
```

```sweet-exp
let [idx     arrange-indices(df list("score") list(1))
     sorted1 reorder(df  idx)
     sorted2 reorder(df2 idx)]
  ...
```

`distinct` removes duplicate rows by the named key columns (or all columns if
you pass `0`). Combined with `arrange`, this is the typical "keep first
occurrence" pattern:

```turmeric
(let [top-per-name (distinct (arrange df (list "score") (list 1))
                             (list "name"))]
  (print-frame top-per-name))
```

```sweet-exp
let [top-per-name distinct(arrange(df list("score") list(1))
                           list("name"))]
  print-frame(top-per-name)
```

---

## Group-By and Aggregation

`group-by` returns an opaque *grouped-frame*. Call `agg` on it to produce one
summary row per group. The `agg` argument is three parallel lists:
output column names, input column names, and aggregation tags.

```turmeric
;; Sum and count scores, grouped by grade
(let [grouped (group-by df (vec-of "grade"))
      summary (agg grouped
                (list "total_score" "count")
                (list "score"       "name")
                (list (agg-sum)     (agg-count)))]
  (match summary
    [(ok f)  (print-frame f)]
    [(err e) (do (println "agg error:") (println e))]))
```

```sweet-exp
let [grouped group-by(df vec-of("grade"))
     summary agg(grouped
                 list("total_score" "count")
                 list("score"       "name")
                 list(agg-sum() agg-count()))]
  match summary
    [ok(f)  print-frame(f)]
    [err(e) do(println("agg error:") println(e))]
```

Available aggregation functions:

| Function | Meaning |
|---|---|
| `(agg-count)` | Number of rows |
| `(agg-sum)` | Sum of column values |
| `(agg-mean)` | Arithmetic mean |
| `(agg-min)` / `(agg-max)` | Min / max value |
| `(agg-median)` | Median |
| `(agg-std)` / `(agg-var)` | Sample std / variance |
| `(agg-first)` / `(agg-last)` | First / last value in group |

`summarize` applies aggregations without grouping -- useful for whole-frame
summary statistics:

```turmeric
;; Overall mean and max score
(let [stats (summarize df
              (list "mean_score" "max_score")
              (list "score"      "score")
              (list (agg-mean)   (agg-max)))]
  (match stats
    [(ok f) (print-frame f)]
    [_ 0]))
```

```sweet-exp
let [stats summarize(df
                     list("mean_score" "max_score")
                     list("score"      "score")
                     list(agg-mean() agg-max()))]
  match stats
    [ok(f) print-frame(f)]
    [_ 0]
```

`frame-describe` produces a pandas-style summary -- count, mean, std, min,
25th/50th/75th percentile, and max for every numeric column:

```turmeric
(print-frame (frame-describe df))
```

```sweet-exp
print-frame frame-describe(df)
```

---

## Joining Two Frames

All joins return `result<frame>`. The `keys` argument is two parallel lists
of column names -- left-side key names and right-side key names.

```turmeric
(let [orders   (read-csv "orders.csv"   (default-csv-opts))
      products (read-csv "products.csv" (default-csv-opts))]
  (match (cons orders products)
    [(cons (ok o) (ok p))
     ;; inner join on orders.product_id = products.id
     (let [result (inner-join o p
                    (list "product_id")
                    (list "id"))]
       (match result
         [(ok df) (print-frame df)]
         [(err e) (do (println "join error:") (println e))]))]
    [_ (println "csv read error")]))
```

```sweet-exp
let [orders   read-csv("orders.csv"   default-csv-opts())
     products read-csv("products.csv" default-csv-opts())]
  match cons(orders products)
    [(cons (ok o) (ok p))
     (let [result (inner-join o p
                    (list "product_id")
                    (list "id"))]
       (match result
         [(ok df) (print-frame df)]
         [(err e) (do (println "join error:") (println e))]))]
    [_ (println "csv read error")]
```

The convenience `join` function takes a `how` string and a single key list (for
when the key column has the same name on both sides):

```turmeric
;; Left join on the shared "category" column
(join orders products "left" (list "category"))
```

```sweet-exp
join(orders products "left" list("category"))
```

Available join kinds: `"inner"`, `"left"`, `"right"`, `"full"`, `"semi"`,
`"anti"`, and `cross-join` (no keys needed):

```turmeric
;; Semi-join: keep left rows that have a match in right
(semi-join orders products (list "product_id") (list "id"))

;; Cross join: Cartesian product
(cross-join sizes colors)
```

```sweet-exp
semi-join(orders products list("product_id") list("id"))

cross-join(sizes colors)
```

When both frames have a column with the same name (other than the join key), the
right-frame copy gets a `_r` suffix in the output.

---

## Reshaping with `melt`

`melt` converts a wide frame to long format. The identity columns stay unchanged;
the remaining columns each become a row, with their name stored in `var-name` and
their value in `value-name`:

```turmeric
;; Wide frame:
;;   name   q1   q2   q3
;;   Alice  10   20   15
;;   Bob    12   18   22
;;
;; After melt with id-cols=["name"]:
;;   name   quarter  sales
;;   Alice  q1       10
;;   Alice  q2       20
;;   ...

(let [df    (read-csv "quarterly.csv" (default-csv-opts))
      long  (melt df
              (list "name")      ; identity columns
              "quarter"          ; variable column name
              "sales")]          ; value column name
  (match (cons df long)
    [(cons (ok wide) melted)
     (when melted (print-frame melted))]
    [_ 0]))
```

```sweet-exp
let [df   read-csv("quarterly.csv" default-csv-opts())
     long (melt df
            (list "name")
            "quarter"
            "sales")]
  match cons(df long)
    [(cons (ok wide) melted)
     (when melted (print-frame melted))]
    [_ 0]
```

All non-identity columns must share the same type. `melt` returns `0` if they
do not.

> **Note:** `pivot` (long -> wide) and `transpose` are not included in v0.1.0.
> For those operations, export to Arrow and use the receiving runtime's native
> pivot/transpose.

---

## Arrow C Data Interface

`tur-frame` stores data in the Arrow in-memory format. Zero-copy hand-off to
other runtimes uses the Arrow C Data Interface -- two small C structs
(`ArrowSchema*` and `ArrowArray*`) that any Arrow-aware library can consume
directly.

### Exporting to Python (PyArrow)

```turmeric
;; Export returns a cons pair of (schema-ptr . array-ptr)
(let [ptrs      (arrow-export df)
      schema-p  (head ptrs)
      array-p   (head (tail ptrs))]
  ;; Hand the two raw pointers to Python via ctypes or cffi:
  ;; import pyarrow as pa
  ;; tbl = pa.RecordBatch._import_from_c(array_ptr, schema_ptr)
  (println "schema ptr:")
  (println schema-p)
  (println "array ptr:")
  (println array-p))
```

```sweet-exp
let [ptrs     arrow-export(df)
     schema-p head(ptrs)
     array-p  head(tail(ptrs))]
  println("schema ptr:")
  println(schema-p)
  println("array ptr:")
  println(array-p)
```

The exporting side transfers ownership: the consumer must call the release
callbacks embedded in the structs when done. PyArrow does this automatically.

### Exporting to R (nanoarrow) and DuckDB

- **R / nanoarrow**: `nanoarrow::as_nanoarrow_array(array_ptr, schema_ptr)`
- **DuckDB**: `duckdb.arrow_scan(schema_ptr, array_ptr)`
- **Polars**: `pl.from_arrow(pa.RecordBatch._import_from_c(array_ptr, schema_ptr))`

### Importing from another runtime

```turmeric
;; Import an (ArrowSchema*, ArrowArray*) pair produced by another runtime.
;; Turmeric takes ownership and calls the release callbacks on GC.
(let [df (arrow-import schema-ptr array-ptr)]
  (match df
    [(ok f)  (print-frame f)]
    [(err e) (do (println "import error:") (println e))]))
```

```sweet-exp
let [df arrow-import(schema-ptr array-ptr)]
  match df
    [ok(f)  print-frame(f)]
    [err(e) do(println("import error:") println(e))]
```

### Column-level export

For single-column hand-off (e.g. passing a vector to a numeric routine):

```turmeric
(let [col  (frame-column df "score")
      ptrs (arrow-export-column col "score")]
  ...)
```

```sweet-exp
let [col  frame-column(df "score")
     ptrs arrow-export-column(col "score")]
  ...
```

---

## Column Type Reference

| Type tag function | Arrow format | Turmeric element accessor |
|---|---|---|
| `(type-int32)` | `"i"` | `(column-int32-at col i)` -> `:int` |
| `(type-int64)` | `"l"` | `(column-int64-at col i)` -> `:int` |
| `(type-float32)` | `"f"` | `(column-float32-at col i)` -> `:float` |
| `(type-float64)` | `"g"` | `(column-float64-at col i)` -> `:float` |
| `(type-bool)` | `"b"` | `(column-bool-at col i)` -> `:int` (0 or 1) |
| `(type-utf8)` | `"u"` | `(column-utf8-at col i)` -> `:cstr` |
| `(type-date32)` | `"tdD"` | `(column-int32-at col i)` -> days since epoch |
| `(type-timestamp)` | `"tsu:"` | `(column-int64-at col i)` -> us since epoch |
| `(type-null)` | `"n"` | all rows null |

Typed fast-path accessors (`column-int64-at`, etc.) are undefined if called on
the wrong column type or out of range. Use `column-type` to check first, or call
`column-get` for a bounds-checked option-typed result.

---

## What's Coming in v0.2

- `pivot-agg` -- long-to-wide reshape with an explicit reduction function
- Lazy query plans (`tur-frame-lazy`)
- Parquet I/O (`tur-frame-parquet`)
- Line-delimited JSON I/O (`tur-frame-stream`)
- Zero-copy Arrow export (share column buffers via refcount rather than deep-copy)

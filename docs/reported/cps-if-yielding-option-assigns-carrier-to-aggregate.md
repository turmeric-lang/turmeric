# An `if` yielding an `Option` inside a CPS function emits an int-to-struct assignment

**Severity:** medium (cc error: the program does not build).

Found 2026-11-28 writing `tests/fixtures/cps-threaded-fn-value-in-if-arm`.
Reproduces on `main` at `d01ed924`, with and without that fix.

```turmeric
(defeffect Ask [] : int)
(defn f [i : int] : int
  (match (if (= (mod i 2) 1) (some i) (none))
    (Some x) (+ x (perform (Ask)))
    (None) 0))
(defn main [] : int
  (println (handle (f 3) (Ask [] k) (resume k 100)))
  0)
```

```
error: incompatible types when assigning to type 'tur_adt_Option__int'
       from type 'int64_t'
```

`f` performs, so it is lowered by the CPS emitter. The `if`'s join parameter
is declared at the `Option`'s by-value aggregate type, and an arm assigns it
the int64 carrier word. Moving the `if` into a helper that does not perform
(`(defn odd-opt [i : int] : (Option int) ...)`, then `(match (odd-opt i) ...)`)
builds and runs.

## Fix directions

- The join param's C type and the arm's delivered value disagree on the
  representation: find where `emit_deliver_ty` (or the inline-join
  assignment in it, `join_param(...) = v`) renders a sum-typed value as its
  carrier, and render it at the aggregate type, or declare the join param as
  the carrier and unbox on read.
- `binder_ctype_full` and the delivered atom's type are the two ends to
  compare.

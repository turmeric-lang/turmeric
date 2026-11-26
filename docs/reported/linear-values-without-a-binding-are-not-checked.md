# A fresh `^linear` value lent to a `^borrow` parameter, or stored in a non-linear aggregate, is dropped without a diagnostic

**Severity:** medium (silent leaks from programs the linearity check accepted).
`^linear` promises "used exactly once". That holds for a value bound with
`let` or passed as a parameter, and -- since 2026-11-26 -- for a call result
written as a statement. Two shapes of a linear value that never gets a name
are still not followed: a temporary handed straight to a borrowing function,
and a field of an aggregate that is not itself linear.

Found 2026-11-26 prototyping `spices/manifold` in turmeric-spices on `^linear`
handles; that spice's operations consume their operands, and its README lists
these shapes. First verified on `main` at `d4ad1b0`. This report originally
carried four more findings; they are fixed, and listed at the end.

All repros share one 16-byte heap handle:

```turmeric
(defopaque ^linear H :int)

(defn h-new [n : int] : H
  ```c
  #include <stdlib.h>
  int64_t *p = malloc(sizeof *p);
  *p = n;
  return (intptr_t)p;
  ```)

(defn h-free [h : H] : void
  ```c
  #include <stdlib.h>
  free((void *)(intptr_t)h);
  ```)

(defn h-peek [^borrow h : H] : int
  ```c
  return *(int64_t *)(intptr_t)h;
  ```)

;; Consumes its argument, returns a fresh handle holding one more.
(defn h-bump [h : H] : H
  ```c
  #include <stdlib.h>
  int64_t *o = (int64_t *)(intptr_t)h;
  int64_t *p = malloc(sizeof *p);
  *p = *o + 1;
  free(o);
  return (intptr_t)p;
  ```)
```

## The two open shapes

Prelude plus, as `l1/main.tur`:

```turmeric
(defstruct Box [h : H n : int])

(defn temp-to-borrow [] : int (h-peek (h-new 7)))

(defn in-a-struct [] : int
  (let [b (Box (h-new 7) 3)]
    (.n b)))

(defn main [] : int
  (println (temp-to-borrow))
  (println (in-a-struct))
  0)
```

```sh
tur check l1/main.tur                      # no diagnostic
TUR_SANITIZE_PROGRAMS=1 tur test l1
```

```
SUMMARY: AddressSanitizer: 16 byte(s) leaked in 2 allocation(s).
```

| Function | Shape | Why nothing consumes it |
|---|---|---|
| `temp-to-borrow` | a fresh linear value passed straight to a `^borrow` parameter | the callee only borrows; the caller never named it |
| `in-a-struct` | a linear value as a field of a non-linear struct (a vector literal does the same) | the aggregate is not linear, so dropping it is allowed |

Binding the value first turns the first into the expected TUR-E0100:
`(let [t (h-new 7)] (h-peek t))` is rejected as "linear value 't' dropped
without being consumed".

For `^affine` + `Drop` the first shape is handled -- the temporary is hoisted
and dropped after the call
([borrowed-drop-temporary-leaks-outside-the-hoist](borrowed-drop-temporary-leaks-outside-the-hoist.md)
tracks what is left of that). The same hoist covers a `^linear` type that has
a `Drop` instance. A linear type without one has no drop to synthesise, so the
only correct outcomes are a diagnostic or an owner.

## Why the borrowed temporary is not simply an error

It was tried (2026-11-26): TUR-E0100 for a call of linear type, with no `Drop`
instance, in a `^borrow` argument slot. `tests/run.sh` stayed green, but a
`tur check` sweep of every source and test in turmeric-spices found seven new
errors in three files, and none of them is a leak:

| Where | Code | What it is |
|---|---|---|
| `spices/postgres/src/postgres/typed.tur` | `(rows-count (__int->rows (trows-handle r)))` | a re-wrap: an int handle the caller does not own, cast to the linear `Rows` the borrower's signature wants |
| `spices/sqlite/src/sqlite/typed.tur` | `(stmt-bind-int (__int->stmt (tstmt-handle s)) i v)` | the same, for `Stmt` |
| `spices/ecs/tests/sized-defsystem.tur` | `(set-Pos! (:: (make-write-cap Pos-cid) (WriteCap Pos)) ...)` | a capability token: nothing to release |

A call that allocates and a call that re-wraps look the same to the checker,
and the re-wrap has no correct spelling under the strict rule -- binding it
first is TUR-E0100 too, because a view must not be consumed. So the check was
taken back out.

### Fix directions

- Give the re-wrap a spelling first: a way to say a function's result is a
  borrowed view of its argument, not a fresh owner (a `^borrow` result, or a
  `borrow-as` cast form). The lifetime machinery already distinguishes owned
  from borrowed for `&T`; an opaque handle has no equivalent. With that in
  place the temporary-to-borrow shape can be TUR-E0100.
- Short of that, a warning rather than an error, which the three files above
  would need a way to silence.
- For the struct field: either make an aggregate with a linear field linear
  itself, or reject a linear field in a type that is not declared `^linear`.
  stdlib has `^linear` handles in non-linear containers today (a `Result`
  payload is the common one), so this needs its own sweep.

## Resolved 2026-11-26

Four findings filed here are fixed (`src/compiler/elab_forms.c`,
`elab_fns.c`):

- **A discarded linear call result.** `(h-bump h)` written as a statement is
  TUR-E0100, "linear value of type H is discarded here without being
  consumed" (`elab_note_discarded_linear`, called where `do`, `let`, function
  and `while` bodies are sequenced). The same sweep of turmeric-spices found
  no instance; one fixture in this repo had one
  (`size-arith-guards`, `(do (bytes-alloc -1) 0)`), now bound and freed.
- **`^linear` with a `Drop` instance double-freed on every explicit consume.**
  The scope-exit drop honoured a move and not a linear consumption; it now
  skips a binding whose `is_linear_consumed` is set.
- **`(set! h (h-bump h))` on a `^mut` linear binding was TUR-E0101** at the
  next use, because the old value's "consumed" mark stayed on the cell.
  `elab_set` clears it: the cell holds a value nobody has consumed. A loop can
  accumulate into a linear value, and into an `^affine` + `Drop` one (whose
  last value the scope exit drops).
- **The complement:** `set!` on a linear binding whose current value was not
  consumed is TUR-E0100, "overwritten without being consumed". A `ref<T>`,
  which is only inferred linear and is auto-dropped, keeps its old rule.

Pinned by `tests/fixtures/linear-set-reinitialises` (counted allocations and
frees, also under `run-leak-check.sh`),
`errors/linear-call-result-discarded` and
`errors/linear-set-overwrites-unconsumed`.

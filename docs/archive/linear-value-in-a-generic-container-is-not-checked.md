# A `^linear` value stored in a generic container is dropped without a diagnostic

> Status: resolved 2026-11-28. A struct or data type applied to a linear
> argument whose parameter reaches a field is linear; a linear argument to a
> parameter that reaches no field (`Vec`'s) is TUR-E0110. See "Resolved" below.

**Severity:** medium (silent leaks from programs the linearity check accepted).

`^linear` promises "used exactly once". A linear value in a field whose
declared type is linear is followed: the type must be declared `^linear` too
(TUR-E0100, since 2026-11-28 --
[linear-values-without-a-binding-are-not-checked](linear-values-without-a-binding-are-not-checked.md)).
A linear value in a slot whose declared type is a **type variable** is not:
the container's type is not linear, so dropping it is allowed and the value
is never released. Verified on `main` at `c878a702` plus that fix.

```turmeric
(defopaque ^linear H :int)
(defn h-new [n : int] : H
  ```c
  #include <stdlib.h>
  int64_t *p = malloc(sizeof *p);
  *p = n;
  return (intptr_t)p;
  ```)

(defstruct Duo [A] [a : A n : int])

(defn vec-lit [] : int (let [v [(h-new 1) (h-new 2)]] 0))
(defn generic [] : int (let [p (Duo (h-new 7) 3)] (.n p)))
(defn opt [] : int (let [o (some (h-new 7))] 0))

(defn main [] : int (vec-lit) (generic) (opt) 0)
```

`tur check` reports nothing; all four handles leak.

| Shape | Why nothing consumes it |
|---|---|
| a vector literal of linear elements | `(Vec H)` is not linear |
| a generic struct instantiated at a linear type | `(Duo H)` is not linear; the field's declared type is `A` |
| `(some h)`, `(ok h)` | `(Option H)` / `(Result H E)` are not linear |

## Fix directions

- Make an application linear when an argument is: `(Duo H)`, `(Option H)`
  carry `CK_LINEAR` whenever a type argument that reaches a field does. That
  is the principled answer, and the expensive one: stdlib's `Result` /
  `Option` returns of linear handles (fallible constructors) would then need
  every caller to consume the wrapper by `match`, which is mostly what they
  do already -- a sweep says how much is not.
- Short of that, reject a linear argument to a type parameter that is not
  declared to accept one, the way Rust's `?Sized` opts in. Needs a kind or
  bound on the parameter.
- `vec-of` with a linear element type can be refused outright: a `Vec` has no
  way to hand its elements back one at a time under the checker.

## Resolved 2026-11-28

The first fix direction, with the third for `Vec`.

- **A container holding a linear value is linear.** `ty_carries_linear`
  (`src/compiler/elab_structs.c`) says a type holds a linear value when it is
  linear, or applies a struct / data type to a linear argument whose
  parameter reaches a field (`adt_param_reaches_field`). A binding of such a
  type is linear -- `let`, parameters, match-arm fields, tuple destructuring
  -- and so are the discarded-statement and lent-to-`^borrow` checks and the
  concrete-field check of the archived report above. The type's own
  `copy_kind` is left alone: codegen reads it, and `Binding.is_linear` is what
  the exactly-once checks follow. An opaque head is phantom by definition and
  carries nothing.
- **A phantom slot refuses one.** A linear argument to a parameter that
  reaches no field -- `(Vec H)`, whose elements live behind `data :
  ptr<void>` -- is the new TUR-E0110 at the `let` or parameter that holds it
  (`elab_check_linear_phantom_slot`). A `Vec` cannot hand an element back
  under the checker: `vec-get` copies it out and `vec-free` drops them all.
- **Reading one field drops the others.** `(.n p)` consumes `p`, so a field
  read that leaves another linear field behind is TUR-E0100 (the field-read
  path in `src/compiler/elab_typeclasses.c`). This held for a non-generic
  `defstruct ^linear` too, which the previous report's fix had told people
  to write.
- **The tag predicates borrow.** `ok?`, `err?`, `some?`, `none?` take
  `^borrow`, so `(? r)` -- lowered to `(if (err? q) (return ...) (ok-val q))`
  -- and `(if (ok? r) (ok-val r) (err-val r))` consume `r` once per path.

### What it caught

- In this repo: `schan-recv`'s `(Pair T (SChan R))` was read with `pair-fst`
  and then `pair-snd`, two consumptions of a pair holding a linear channel.
  `schan-roundtrip`, `schan-worker-pool`, the `schan-recv` docstring and the
  channels and session-types guides now take it apart with `match`.
- In turmeric-spices, against a baseline sweep of every `.tur` file: one
  library line in `lmdb/env.tur` and two in `lmdb/cursor.tur`
  (`(if (ok? r) (ok (ok-val r)) (err ... (err-val r)))`, now a `match`), and
  test code across lmdb, secret, nng and tourist-session-valkey using the
  `ok?` / `ok-val` idiom, `(assert-true (err? (mint ...)))` with nothing to
  release an unexpected `Ok`, and one Option pair in `lmdb/tests/bytes_test`
  that leaked the key when only the value was missing.

Pinned by `tests/fixtures/linear-in-generic-container` (counted
allocations, also under `run-leak-check.sh`), `errors/linear-in-option-dropped`,
`errors/linear-in-vec-literal` and `errors/linear-field-read-drops-other`.

### Not covered

A generic function's own body still sees its parameter `[o : (Option A)]` as
non-linear, so a generic helper may drop what a caller handed it at
`(Option H)`; that is parametricity, and every caller-side obligation above
holds. Reading a linear field out of a `^borrow` parameter duplicates it.

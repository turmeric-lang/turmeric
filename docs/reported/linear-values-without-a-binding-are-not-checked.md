# The `^linear` check follows named bindings only: an unbound linear value is dropped without a diagnostic (and three related gaps)

**Severity:** medium (silent leaks from programs the linearity check accepted;
one of the related gaps is a double free). `^linear` promises "used exactly
once". That holds for a value bound with `let` or passed as a parameter. A
linear value that never gets a name -- a temporary, a discarded result, a
struct field -- is not followed at all, so it can be dropped with no TUR-E0100.

Found 2026-11-26 prototyping `spices/manifold` in turmeric-spices on `^linear`
handles; that spice's operations consume their operands, and its README lists
the shapes below, because of section 1. Verified on `main` at `d4ad1b0`.

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

## 1. Unbound linear values leak silently

Prelude plus, as `l1/main.tur`:

```turmeric
(defstruct Box [h : H n : int])

(defn temp-to-borrow [] : int (h-peek (h-new 7)))

(defn discarded-result [] : int
  (let [h (h-new 7)]
    (h-bump h)
    1))

(defn in-a-struct [] : int
  (let [b (Box (h-new 7) 3)]
    (.n b)))

(defn main [] : int
  (println (temp-to-borrow))
  (println (discarded-result))
  (println (in-a-struct))
  0)
```

```sh
tur check l1/main.tur                      # no diagnostic
TUR_SANITIZE_PROGRAMS=1 tur test l1
```

```
SUMMARY: AddressSanitizer: 24 byte(s) leaked in 3 allocation(s).
```

One leak per function:

| Function | Shape | Why nothing consumes it |
|---|---|---|
| `temp-to-borrow` | a fresh linear value passed straight to a `^borrow` parameter | the callee only borrows; the caller never named it |
| `discarded-result` | a call returning a linear value, in statement position | the result is thrown away |
| `in-a-struct` | a linear value as a field of a non-linear struct (a vector literal does the same) | the aggregate is not linear, so dropping it is allowed |

Binding the value first turns each of these into the expected TUR-E0100:
`(let [t (h-new 7)] (h-peek t))` is rejected as "linear value 't' dropped
without being consumed".

For `^affine` + `Drop` the first shape is handled -- the temporary is hoisted
and dropped after the call
([borrowed-drop-temporary-leaks-outside-the-hoist](borrowed-drop-temporary-leaks-outside-the-hoist.md)
tracks what is left of that). A linear type has no drop to synthesise, so the
only correct outcomes are a diagnostic or an owner.

### Fix directions

- Treat a call whose result type is linear, in a position where the result is
  not bound, returned, or passed to a consuming parameter, as TUR-E0100. That
  covers the first two rows.
- For the third: either make an aggregate with a linear field linear itself,
  or reject a linear field in a type that is not declared `^linear`.

## 2. `^linear` with a `Drop` instance: every explicit free is a double free

Prelude plus:

```turmeric
(definstance Drop [H] (drop [x] : void (h-free x)))

(defn main [] : int
  (let [h (h-new 7)
        v (h-peek h)]
    (h-free h)
    (println v)
    0))
```

```
==314==ERROR: AddressSanitizer: attempting double-free on 0x502000000010 in thread T0:
```

The program is the textbook linear usage -- bind, borrow, consume once -- and
it compiles. The `Drop` instance makes the scope exit drop `h` as well, even
though `h-free` already consumed it. The same program with `^affine` runs
clean, so the scope-exit drop honours an affine move and not a linear
consumption.

Either the combination should be rejected at the `definstance` ("a `^linear`
type is consumed explicitly; it cannot also have `Drop`"), or the drop glue
should treat a linear consumption as a move.

## 3. A `^mut` linear binding cannot be reassigned

Prelude plus:

```turmeric
(defn main [] : int
  (let [^mut h (h-new 7)]
    (set! h (h-bump h))
    (let [v (h-peek h)]
      (h-free h)
      (println v)
      0)))
```

```
error [TUR-E0101]: linear value 'h' used after being consumed
```

`(set! h (h-bump h))` consumes the old value and stores a fresh one, so `h`
holds a live handle again -- but the checker still has it marked consumed. The
consequence is that a loop cannot accumulate into a linear value
(`(while ... (set! acc (step acc)))`); `spices/manifold` works around it with a
borrowed vector that the loop pushes into. A recursive function whose
accumulator is a parameter is accepted.

Fix direction: a `set!` whose right-hand side type-checks should clear the
consumed mark on its target (and, if the target was *not* consumed, the old
value is being overwritten without a consumer -- that is a TUR-E0100).

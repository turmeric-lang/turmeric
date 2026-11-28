# A `^linear` value stored in a generic container is dropped without a diagnostic

**Severity:** medium (silent leaks from programs the linearity check accepted).

`^linear` promises "used exactly once". A linear value in a field whose
declared type is linear is followed: the type must be declared `^linear` too
(TUR-E0100, since 2026-11-28 --
[linear-values-without-a-binding-are-not-checked](../archive/linear-values-without-a-binding-are-not-checked.md)).
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

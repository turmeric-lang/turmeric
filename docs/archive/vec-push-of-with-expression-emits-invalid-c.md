# `(vec-push! v (with x [...]))` emits C that does not compile

**Severity: low (compile failure, easy workaround).** Pushing the result of a
`with` straight onto a `Vec` of a `:copy` struct fails in `cc`:

```
error: incompatible types when assigning to type 'tur_adt_N' from type 'long int'
```

Binding the `with` result in a `let` and pushing the binding works. Found
2026-10-16 writing the CommonMark block parser (markdown-library-plan M1),
which does exactly that at its two push sites.

## Repro

```turmeric
(defstruct N :copy [a : int b : int c : int])
(defn new-ns [] : (Vec N) (vec-new))
(defn main [] : int
  (let [v  (new-ns)
        n0 (N 1 0 0)]
    (do (vec-push! v (with n0 [b 2]))
        (println (.b (vec-get v 0)))
        (vec-free v)
        0)))
```

The workaround is `(let [m (with n0 [b 2])] (vec-push! v m))`.

## Root cause

The element is boxed twice. The emitted C spills the `with` result into a
box, then boxes the box:

```c
tur_adt_N *__t208 = (tur_adt_N *)malloc(sizeof(tur_adt_N));
*__t208 = __t206;                                  /* the with result      */
tur_adt_N *__t209 = (tur_adt_N *)malloc(sizeof(tur_adt_N));
*__t209 = (int64_t)(intptr_t)(__t208);            /* the box, as a struct */
vec_hypush_ex(..., (int64_t)(intptr_t)(__t209));
```

So two sites each apply the container-element boxing (`repr_of` at
`REPR_POS_CONTAINER_ELEM`): `vec-push!`'s argument bridge, and the spill of
the `with` expression's value. A let-bound value reaches the push already
spelled as `tur_adt_N` and is boxed once.

## Fix directions

1. **Box once.** Have the bridge check whether the operand is already the
   boxed carrier word: either the `with` lowering returns the by-value
   aggregate and leaves boxing to the bridge, or the bridge skips an operand
   the spill already boxed.
2. **Guard it with a fixture:** push a `with` result, a constructor call and a
   function result, each inline, into a `Vec` of a three-field `:copy` struct,
   and read them back. The fixture should be leak-checked, since the
   double-boxed form would also leak its outer box.

## Resolution (2026-10-18)

The first fix direction, on the bridge side. The argument loop of a call
through a uniform-carrier `fn` (`src/compiler/emit_expr.c`) has two arms that
bridge a by-value operand to its carrier word. The by-value-tail arm, which a
`(with n0 [...])` operand takes, spills the aggregate into a box and sets
`arg_carrier_boxed`. The uniform-carrier arm after it did not check that flag,
so it boxed the box pointer again. It now skips an operand the earlier arm
already bridged, as every other carrier seam in that loop does. One box, and
the outer cell no longer leaks.

Pinned by `vec-push-inline-copy-struct`. It pushes a `with` result, a
constructor call, a function result, and a `with` over a call, each inline,
into a `(Vec N)` of a three-field `:copy` struct, and reads each back. It is
leak-checked (`requires.leak-check`).

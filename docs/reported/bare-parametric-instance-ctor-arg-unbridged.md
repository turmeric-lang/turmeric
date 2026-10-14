# An instance on a bare parametric `:heap` type passes a carrier word to its typed constructor

**Severity: low-medium.** A `-Wint-conversion` warning under gcc 13 (which
`tests/run.sh` treats as a failure) and a hard error under clang 15+ and gcc
14. The interpreter is unaffected.

**Discovered:** 2026-10-14, executing
[generic-zipper-plan](../archive/generic-zipper-plan.md) (ZP3). `zip.tur`'s
own `Plated [Rose]` instance avoids it by building the node through the
generic helper `zip/rose` instead of `make-struct`.

## Repro

```turmeric
(defclass Pl [a] (pr [x : a kids : (Cons a)] : a))
(defstruct Rose :heap :copy [A] (label A) (kids (Cons (Rose A))))

(definstance Pl [Rose]
  (pr [r ks] (make-struct Rose :label (.label r) :kids ks)))

(defn main [] : int
  (let [r (make-struct Rose :label 1 :kids (tnil))]
    (println (.label (:: (pr r (tcons-of r (tnil))) (Rose int)))))
  0)
```

`CC=clang tur run`:

```
error: incompatible integer to pointer conversion passing 'int64_t' to
parameter of type 'tur_adt_Cons__Rose__int *'
```

in `__inst_Pl_pr_Rose`, at `ctor_Rose_Rose__int(..., ks)`.

## Root cause

The instance is for the bare `Rose`, so its method is emitted once with the
erased carrier signature (`ks` is `int64_t`), but the `make-struct` in its
body selects the representative monomorph constructor `ctor_Rose_Rose__int`,
whose `kids` parameter is the typed pointer. The argument is passed without
the `(T *)(intptr_t)` bridge the ordinary call-argument path applies.

## Fix directions

1. In the constructor-argument loop (`emit_expr.c`, the monomorphised-ctor
   arm), bridge a carrier-word argument into a typed-pointer field parameter,
   as `bridge_control_result_int_ptr` does for merge temps.
2. Or specialize an instance method on a bare parametric type per
   instantiation, so `ks` arrives typed.

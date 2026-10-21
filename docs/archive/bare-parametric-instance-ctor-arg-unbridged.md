# An instance on a bare parametric `:heap` type passes a carrier word to its typed constructor

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`bare-parametric-instance-ctor-arg`.

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

## Resolution (2026-10-21)

Fix direction 1. The monomorph-ctor arm already bridged an int64<->pointer
straddle at a field slot -- it compares the slot's recorded C type
(`emit_sig_lookup_param_ctype`) with the argument's -- but it found the
argument's C type only for an explicit cast, an active spec's parameter, a
closure, or a bare LOCAL (`emit_localvar_lookup_ctype`). The localvar table
never holds parameters, so `ks` -- a parameter of the erased method -- had no
known C type and went across bare. The bare-identifier lookup is now
`emit_ident_emitted_ctype`, which answers a parameter from the enclosing
function's recorded signature (the same lookup `bridge_control_result_int_ptr`
uses), and the existing cast fires:
`ctor_Rose_Rose__int(..., (tur_adt_Cons__Rose__int *)(intptr_t)(ks))`.

The defect is now invisible to a default `tur build`: the base
`__inst_Pl_pr_Rose` is unreferenced (the call goes to its spec), so the
link-time prune drops it before cc sees it. It was still in `tur emit-c`
output and in any unpruned build. `bare-parametric-instance-ctor-arg` is
therefore a `hook.sh` fixture that builds with `TUR_CC_NO_PRUNE=1` and fails
on cc's `-Wint-conversion` / `-Wincompatible-pointer-types`; it fails on the
pre-fix compiler and passes on both engines after.

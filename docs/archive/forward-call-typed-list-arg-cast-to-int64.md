# Calling a later-defined function with a `(Cons N)` parameter casts the argument to `int64_t`

**Severity: medium** (clang rejects the emitted C; gcc warns). It bites the
most natural code: a helper that walks the result of `node-children` or
`find-all`, written below its caller.

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)). Found
2026-10-10 implementing
[xml-html-parsers-plan](../upcoming/spices/xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/xml/tests/dom_tree.tur` by
defining the helper first.

## Repro

````turmeric
;; src/geo/n.tur -- exports N and two
(defmodule geo/n (export N two)
(defopaque N :ptr<void>)
(defn __p [] : ptr<void>
  ```c
  return (void *)"x";
  ```)
(defn two [] : (Cons N) (tcons-of (:: (__p) N) (tcons-of (:: (__p) N) (tnil)))))

;; main.tur
(defmodule main (export)
(import geo/n :refer [N two])
(defn child [i : int] : N
  (let [r (nthx (two) i)] r))           ;; nthx is defined below
(defn nthx [xs : (Cons N) i : int] : N
  (if (= i 0) (thead xs) (nthx (ttail xs) (- i 1))))
(defn main [] : int
  (child 1)
  0))
````

`CC=clang tur run main.tur`: `incompatible integer to pointer conversion
passing 'int64_t' to parameter of type 'tur_adt_Cons__N *'`. Moving `nthx`
above `child` compiles. The call site seems to use a provisional (carrier)
signature for the not-yet-elaborated callee.

## Resolution

Not specific to an imported element type: `(defn child [xs : (Cons int)] :
int (nthx xs))` above `(defn nthx [xs : (Cons int)] ...)` in one file fails
the same way. The report's guess was right. A typed file's pass-1 forward decl
keeps a compound parameter as the `:int` placeholder (`fwd_decl_scan_params`;
only a dynamic file gets closed parameter types, `elab_fwd_param_full_types`),
so the caller's argument is wrapped in the elaborator's erasing `:int`
ascription (`elab_call.c`, no `type_form`). By emit time the callee binding
carries the real parameter type, and the call-argument arm that casts a heap
argument to a concrete typed parameter (`emit_expr.c`, "value-preserving
whether raw is currently int64 or already the pointer") is exactly the one
that should fire -- but it asked the *ascription's* type (`int`) whether the
argument was a heap pointer.

The arm now looks through an elaborator-made erasing ascription (no
`type_form`, typed `int`) to the argument it wraps. The emitted call is the
one the defined-first order always produced,
`nthx((tur_adt_Cons__int *)(intptr_t)((int64_t)(intptr_t)(xs)))`. The
recorded-signature arm further down, which had now begun to cast a second time,
skips an argument already cast to exactly that parameter's type.

Not changed: the forward decl itself. Committing closed parameter types for
typed files is what `elab_fwd_param_full_types` deliberately does not do ("a
typed file's int64 hatches are untouched"); widening it is a larger change.

**Pinned by** `forward-call-typed-heap-param` (a tail call, a non-tail call
and an opaque element type, all above their callee; the old compiler warns
`-Wint-conversion` three times, which the run.sh ratchet fails).

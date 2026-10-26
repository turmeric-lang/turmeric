# A `let`/`do` block evaluating to a `Result` is unboxed a second time

**Status:** RESOLVED 2026-08-18.
**Severity:** high (emits C that cannot compile, with no `.tur` attribution;
hits the most natural way to write "compute, clean up, return the result")
**Found:** 2026-08-18, building `spices/secret` (`secret/kdf`, `secret/hex`)
against `tur` v0.35.0 (`2748f5e8a`).

## Summary

When a `let`/`do` block **evaluates to** a `(Result A B)` and is returned
through an `if` branch, the emitter applies the boxed-carrier-to-struct
conversion to a value that is already a struct:

```c
tur_adt_Result__H__cstr __t96;
{
    ...
    __t96 = __t101;          /* already the struct */
}
tur_result_box_t *__t106 = (tur_result_box_t *)(intptr_t)(__t96);   /* <-- */
__t94 = (tur_adt_Result__H__cstr){.is_ok = __t106->is_ok, ...};
```

```
error: operand of type 'tur_adt_Result__H__cstr'
       (aka 'struct tur_adt_Result__H__cstr')
       where arithmetic or pointer type is required
```

Note the Result here is built in **Turmeric** with `ok`/`err`, so no unbox is
warranted at all. This is the mirror image of
[`cps-result-unbox-dropped.md`](cps-result-unbox-dropped.md), where the
conversion is *omitted* where it is needed: in both cases the emitter is
deciding "does this value need unboxing" from something other than the value's
actual representation.

## Minimal repro

```turmeric
(defmodule b5

(defopaque H :int)

(defn mkh [n : int] : H
  ```c
  return n;
  ```)

(defn mk [n : int] : (Result H cstr)
  (if (< n 1) (err "bad") (ok (mkh n))))

(defn side [x : int] : int x)

;; A let/do block that EVALUATES TO a Result, returned through an `if`
;; branch whose other arm is a plain (err ...).
(defn broken [n : int] : (Result H cstr)
  (if (= n 0)
    (err "zero")
    (let [r (mk n)
          _ (side n)]
      (do
        (side n)
        r))))

(defn main [] : int (do (broken 3) 0)))
```

```
$ tur run b5.tur
.../b5_tur.c:7264:70: error: operand of type 'tur_adt_Result__H__cstr' ...
1 error generated.
```

## Why it matters more than it looks

This is the shape of every "acquire, compute, release, return" function --
which is most fallible resource code:

```turmeric
(let [h (scratch-alloc n)]
  (if (= h 0)
    (err "allocation failed")
    (let [_  (compute-into (scratch-ptr h))
          r  (wrap (scratch-ptr h) n)      ;; the Result
          _2 (scratch-free h)]            ;; cleanup AFTER producing it
      r)))
```

The cleanup has to happen after the Result exists, so the Result has to be
the block's value. There is no natural rewrite that keeps the release in the
same function.

## Workaround in use

Hoist the block into its own `defn` so each `if` branch is a single call:

```turmeric
(defn wrap-scratch [h : int n : int] : (Result Secret cstr)
  (let [r  (secret-of-bytes (scratch-ptr h) n)
        _2 (scratch-free h)]
    r))

(defn secret-hkdf-sha256 [...] : (Result Secret cstr)
  (let [h (scratch-alloc n)]
    (if (= h 0)
      (err "allocation failed")
      (hkdf-then-wrap ikm salt info n h))))
```

`spices/secret/src/secret/kdf.tur` and `.../hex.tur` both carry
`*-then-wrap` / `wrap-*` / `fail-*` helpers that exist only for this, with
comments so they are not "simplified" back inline.

## Related

Same session, same area:
- [`cps-result-unbox-dropped.md`](cps-result-unbox-dropped.md) -- the unbox
  omitted where it *is* needed (CPS-transformed functions).
- [`cps-join-point-emits-invalid-assignment.md`](cps-join-point-emits-invalid-assignment.md)

There is also a fourth symptom, not separately filed because the workaround
is the same: passing a Turmeric-built `(Result T E)` as a **function
parameter** produces the identical double-unbox error at the callee.

## RESOLVED (2026-08-18)

Fixed in `src/compiler/emit_expr.c`: `expr_emits_byvalue_carrier_abi` now
reports true for a bare `EX_VAR` whose binding is a lowered Option/Result
monomorph, checked BEFORE the `type_uses_carrier_abi` guard that was
rejecting it.

The scope of that check matters. A first attempt keyed on "any by-value
product ADT" and regressed **10 fixtures** (the vec/map multiword-struct
element paths and assoc-type returns): a plain non-parametric product like
`tur_adt_Point` genuinely DOES need the carrier treatment at those seams, so
reporting it as already-by-value suppressed a bridge it required. The landed
predicate is narrowed to Option/Result apps -- which is also exactly the
family `emit_carrier_bridge`'s canonical readback special-cases.

Regression fixture:
`tests/fixtures/result-byvalue-tail-var-no-double-unbox/`, verified to FAIL
against the pre-fix compiler.

Full fixture suite after both fixes: 2616 passed, 0 failed.

### Follow-up: the narrowing left a residue, now reported

The scope note above said the predicate was "narrowed to Option/Result apps".
Re-checking that on 2026-08-18 showed it was two steps too tight in one
direction and correct in the other:

- **Too tight:** every OTHER parametric by-value product (`(Box2 int cstr)`)
  hit the identical bug. The predicate now keys on `adt_app_is_byvalue_product`
  rather than the names `Option`/`Result`, which fixes them all and regresses
  nothing (suite: 2628 passed, 0 failed). Pinned by
  `tests/fixtures/byvalue-product-tail-var-parametric/`.
- **Correct:** the 10 regressions came specifically from the NON-parametric
  `TY_ADT` arm, not from dropping the name check. That case is still broken,
  and is not widenable -- the same type rides the carrier at the vec/map
  element seams, so it needs a position-sensitive predicate. Filed as
  [`byvalue-product-tail-var-double-unboxed-nonparametric`](../reported/byvalue-product-tail-var-double-unboxed-nonparametric.md).

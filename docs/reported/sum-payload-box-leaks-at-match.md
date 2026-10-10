# A `match` on a fresh `(Result T S)` / `(Option S)` never frees the struct payload's box

**Severity: medium** (unbounded memory growth on the default build; no
miscompile, no use-after-free).

**Status: OPEN.** Found 2026-10-10 measuring the error path of the
turmeric-spices XML/HTML parsers on `tur` v0.64.0. It belongs to the class
[value-struct-payload-sum-monomorph-box-has-no-owner](../archive/value-struct-payload-sum-monomorph-box-has-no-owner.md)
closed for its 9 fixtures. Those were all freed at a reader call, an argument
to a non-retaining callee, or statement position. The consumer here is the
one that report never measured: a `match` written at the use site.

## Repro

```turmeric
(defstruct Er :copy [line : int col : int])
(defopaque Doc :ptr<void> :non-null)

(defn parse [i : int] : (Result Doc Er)
  (Err (Er i 7)))

(defn main [] : int
  (let [^mut i   0
        ^mut acc 0]
    (while (< i 1000)
      (set! acc (+ acc (match (parse i) (Ok d) 0 (Err e) (.col e))))
      (set! i (+ i 1)))
    (println acc))
  0)
```

Under valgrind (gcc build; clang's optimizer can delete the allocation, so its
zero proves nothing):

```
7000
definitely lost: 16,000 bytes in 1,000 blocks
```

The emitted C copies the payload out of the box and drops the pointer:

```c
tur_adt_Er *__t179 = (tur_adt_Er *)malloc(sizeof(tur_adt_Er));   /* in parse */
*__t179 = (__ps_178);
...
case 1: {
    tur_adt_Er e_22 = *(tur_adt_Er *)(intptr_t)(__scrut->as.Err._0);  /* never freed */
```

## Which consumers leak

Same producer, 1000 iterations, valgrind:

| Consumer | Result |
| --- | --- |
| `(match (parse i) (Ok d) 0 (Err e) (.col e))` | 16,000 B lost |
| `(match (parse i) (Ok d) 0 (Err _) 7)` | 16,000 B lost |
| `(let [r (parse i)] (match r ...))` | 16,000 B lost |
| `(ok? (parse i))` | freed |
| `(col-of (parse i))`, where `col-of` matches its parameter | freed |
| any consumer of `parse-via-let`, `(defn parse-via-let [i] (let [r (parse i)] r))` | 16,000 B lost |

So a `match` inside a callee is handled (the callee's
`nonretain_sum_param_mask` is inferred, `box_uses_confined` counts a
scrutinee as a read), and a `match` at the consumer itself is not.

## Root cause

Three separate gaps, all in the drop analysis rather than the layout:

1. **A fresh scrutinee has no owner.** `(match (parse i) ...)` hoists the call
   into `__scrut_v` (emit_expr.c, `case EX_MATCH`). Nothing on the drop side
   looks at a match scrutinee, so no drop is pushed for it.
2. **A let-bound scrutinee reads as an escape.** `let_binding_vsp_box_freeable`
   asks `sum_box_binding_escapes(body, b)`. That walk
   (`binding_escapes_impl_x`, emit_core.c `case EX_MATCH`) pushes the
   scrutinee, reaches `EX_VAR b`, and reports an escape. For a by-value
   monomorph that is too strict. Every binder of a box-held arm is a
   deref-copy (`match_field_is_ros_pointer_box` takes the
   `*(T *)(intptr_t)` branch before the SR4 borrow branch is reached). So
   only a whole-value variable arm can alias the box.
3. **Freshness stops at a returned let binding.** `fresh_sum_walk`
   (elab_fns.c) answers `EX_LET` from its body, and a body that is the
   binding (`(let [r (parse i)] r)`, or `(do (cleanup) r)`) is an `EX_VAR`,
   which is "not fresh". The real parsers' producers end that way: they free
   the C parser and then return the result.

## Impact

Any program that loops over a fallible call returning a struct error, and
handles it with a `match`, which is the documented idiom. In turmeric-spices
this blocks a by-value `ParseError` from being leak-free. One
`(defstruct ParseError [kind msg : cstr pos : SourcePos])` per failed parse
would lose 32 bytes, plus the 32 the rest of this class already costs.

## Fix directions

1. Free the live arm's box when the `match` commits to an arm whose
   scrutinee is a fresh producer call. That point is after the binders'
   deref-copies and the guard, and before the body, so it covers the value
   and tail (`match_tail`) emitters alike. Skip the free when any arm binds
   the whole scrutinee.
2. Let the payload-box let-scope walk accept a `match` on a bare `b` with no
   whole-value arm. The tail-call admission (`tco_let_refusal`) has to accept
   the same shape for a binding whose only release is this box, or the
   newly-freed `let` falls off the TCO path.
3. Make `fresh_sum_walk` see through `(let [r <fresh>] ... r)` when `r` does
   not otherwise escape the `let`.

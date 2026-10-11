# A `match` on a fresh `(Result T S)` / `(Option S)` never frees the struct payload's box

**Severity: medium** (unbounded memory growth on the default build; no
miscompile, no use-after-free).

**Resolved 2026-10-10** -- all three gaps, below. Found the same day measuring
the error path of the
turmeric-spices XML/HTML parsers on `tur` v0.64.0. It belongs to the class
[value-struct-payload-sum-monomorph-box-has-no-owner](value-struct-payload-sum-monomorph-box-has-no-owner.md)
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

## Resolution (2026-10-10)

All three, each keyed on `adt_field_is_ros_pointer_box`, the one predicate the
typedef, the constructor and every existing drop of this class already use.

1. **A fresh scrutinee** (`match_scrut_owns_vsp_box`, emit_expr.c). When the
   scrutinee is a fresh producer call (`emit_init_owns_fresh_sum`, so a
   copying reader of one counts too), the scrutinee is a by-value aggregate
   the match bound itself (not pbp, not niche, not bridged from the carrier
   word), and no arm binds the whole scrutinee, then the arm that answers frees
   the live arm's box (`boxed_struct_payload_walk` over `__scrut`). The free
   runs after the binders' deref-copies and the guard, and before the body.
   A failing guard falls through to an arm that reads the box again, and a
   tail-position arm ends in its own `return` or backedge. Both arm paths
   (if-chain and switch) carry it.
2. **A let-bound scrutinee** (`vsp_box_binding_escapes`, emit_core.c). The
   payload-box let-scope walk counts `(match b ...)` with no `is_var` arm as a
   read (`match_copies_out_of_b`). Only this walk does: the carrier-sum walk's
   binders may borrow, so it still treats the scrutinee as an escape. The tail
   admission does the same for a binding whose only release is this box
   (`let_binding_scope_free_is_vsp_only` gating `tco_drop_use_ok`). Without
   that, a newly-freed `let` in a self-recursive function fell off the TCO
   path.
3. **A returned let binding** (`fresh_let_binding_returned`, elab_fns.c).
   `(let [r <fresh>] ... r)`, with the tail peeled through `do` and
   ascriptions, is fresh when `r`'s init is, `r` is not `^mut`, and
   `sum_box_binding_escapes_except` (the tail set aside) finds no other
   escape. `(let [r (parse i)] (vec-push! store r) r)` stays not-fresh.

The repro now reports `All heap blocks were freed`, as do all six leaking rows
of the consumer table. It also holds inside a function that performs an effect
(`(let [a (perform (Ask))] (+ a (match (parse i) ...)))` was 16,000 B lost on
v0.64.0) and inside one that calls through a closure.

**Not freed, by design:** an arm that binds the whole scrutinee,
`(match (parse i) (Ok d) ... r (keep r))`. The binder then holds the box,
and nothing yet follows where `r` goes.

Pinned by `tests/fixtures/sum-payload-match-consumer-freed`
(`requires.leak-check`). It covers every freed shape, a guard fall-through,
tail recursion 100000 deep through both a direct and a let-bound match (the
latter with a struct tail value), and the refused shapes (a stored binding, a
stashing producer), whose values are read back through a global `Vec` under
ASan. Before the fix it reported 3,200,128 bytes leaked in 200,008
allocations. `tests/fixtures/sum-payload-match-whole-value-arm-kept` asserts
the value read through a whole-value arm. Suite 3703/0, leak-check 169/0 (3
known-open, unchanged).

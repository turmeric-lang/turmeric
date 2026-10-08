# JIT (x86-64): a struct-valued `({ ... })` clobbers a sibling local

**Severity: medium (JIT engine, x86-64 only).** Filed 2026-09-10 while
driving the Saffron dynamic-surface PR to green.

**Status: RESOLVED 2026-09-29.** The engine fix below landed as
[rjungemann/mir#5](https://github.com/turmeric-lang/mir/pull/5), merged into the
fork's master as `96c34860`, and `TUR_MIR_GIT_TAG` in `cmake/mir.cmake` now
pins that commit (with a line in the pin notes above it). MIR carries the
regression test (`c-tests/new/stmtexpr-struct-slot-overlap.c`: both reported
shapes plus sibling independence, under `-eg` and `-ei`).
`tests/fixtures/jit-inline-c-struct-stmtexpr-slot` pins it on this side: the
emitter no longer produces the shape, but user inline C can, and the
fixture's inline-C bodies (extracted from the `TUR_JIT_DUMP_C` output) print
708 on the old pin and 304 on the new one, as gcc does. Checked on Linux: a
fresh `-DTUR_JIT=ON` build fetches `96c34860`, and `tests/run-jit.sh` passes
on it.

The status note from 2026-09-28, before the fork merge:

- **Fix direction 2 is complete.** The last three sites that still emitted a
  struct-holding `({ ... })` -- the union widen (`__tur_ua`),
  `dyn_widen_to_any`'s by-value box (`__tur_fb`), and `emit_agg_box`'s
  `__tur_pbox` -- now build their value with statements and leave a plain
  expression, like every site before them (emit_expr.c).  `__tur_fb` is built
  inside the dynamic field read's tag-checked branch, since the read it boxes
  is only valid once the tag matches.  The emitter no longer produces the
  shape anywhere; the fixtures that reach the three sites pass under cc and
  in the engine.
- **Fix direction 1: reduced to plain C, and the defect is c2mir's front end,
  not MIR-gen.** Built against the pinned fork (79cb2905) on x86-64 Linux,
  `c2m -eg` AND the interpreter `c2m -ei` both answer wrong; gcc is right:

  ```c
  #include <stdio.h>
  #include <stdint.h>
  typedef struct { int64_t a, b; } T;          /* 16 bytes */
  typedef struct { int64_t x, y, z; } S;       /* 24 bytes */
  int64_t use(T f) { return f.a * 100 + f.b; }
  int64_t k(T f, int64_t p) {
    S s = ({ *(S *)(intptr_t)p; });            /* shape 3 */
    return use(f) + s.x * 0;
  }
  int main(void) {
    S v = {7, 8, 9}; T f = {3, 4};
    printf("%lld\n", (long long)k(f, (int64_t)(intptr_t)&v));  /* gcc 304, c2m 708 */
    return 0;
  }
  ```

  Shape 1 reduces the same way: `g(acc, ({ int64_t q = p; q ? *(T *)q : NIL; }))`
  with two `{tag, double}` boxes gives 4.5 for 3.75, the report's exact
  symptom.

  **Root cause** (c2mir/c2mir.c).  The N_STMTEXPR check reserves the
  struct/union result slot at `func_block_scope->size` *while the function
  body is still being checked* -- but a function's stack variables are laid
  out only after the whole body is checked, by
  `process_func_decls_for_allocation`, which places the top scope's
  variables from offset 0 and then overwrites that scope's `size`.  So the
  reserved slot is the frame's first bytes, and it coincides with the first
  non-scalar stack variable -- a by-value struct parameter (`f` above, and
  shape 3's `f`), which the statement expression's copy-out then
  overwrites.  The layout code is target-independent, so why arm64 never
  showed it is not established here -- most likely the aarch64 ABI code does
  not materialize the aggregate parameter as a frame variable at offset 0 --
  and the fix does not depend on the answer.

  **The fix** records each struct/union statement expression during the
  check and assigns its slot in a new `process_func_stmtexprs_for_allocation`,
  called right after `process_func_decls_for_allocation`, at the end of the
  frame that pass computed (which already covers every nested scope) and
  before the call-arg area is added.  Every slot is still a fixed frame slot,
  so the "no dynamic ALLOCA in a loop" property the original code was after
  is kept.  Verified: all four reductions answer as gcc does under `-eg` and
  `-ei`, and MIR's own c2mir suites (`c-tests/runtests.sh` with
  `use-c2m-interp` and `use-c2m-gen`, 1087 tests each) give identical
  results before and after -- the same two pre-existing failures either way.

  **To finish (done 2026-09-29, see the status above):** land the patch
  below on rjungemann/mir, bump `TUR_MIR_GIT_TAG` in cmake/mir.cmake (with a
  line in the pin comment's fix list, like every fork fix before it), and
  archive this report.  The patch is against 79cb2905:

  ```diff
  --- a/c2mir/c2mir.c
  +++ b/c2mir/c2mir.c
  @@ struct check_ctx {
     VARR (decl_t) * func_decls_for_allocation;
  +  VARR (node_t) * func_stmtexprs_for_allocation;
     VARR (node_t) * possible_incomplete_decls;
  @@
   #define func_decls_for_allocation check_ctx->func_decls_for_allocation
  +#define func_stmtexprs_for_allocation check_ctx->func_stmtexprs_for_allocation
  @@ after process_func_decls_for_allocation
  +/* Place the struct/union result slot of every statement expression in the function after all
  +   of its stack variables, i.e. after the frame size process_func_decls_for_allocation computed
  +   (which already covers every nested scope): */
  +static void process_func_stmtexprs_for_allocation (c2m_ctx_t c2m_ctx, node_t block) {
  +  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  +  struct node_scope *ns = block->attr;
  +
  +  for (size_t i = 0; i < VARR_LENGTH (node_t, func_stmtexprs_for_allocation); i++) {
  +    node_t r = VARR_GET (node_t, func_stmtexprs_for_allocation, i);
  +    struct expr *e = r->attr;
  +    mir_size_t size = type_size (c2m_ctx, e->type);
  +    mir_size_t align = var_align (c2m_ctx, e->type);
  +
  +    ns->size = round_size (ns->size, align);
  +    e->c.u_val = ns->size;
  +    ns->size += size;
  +    ns->stack_var_p = TRUE;
  +  }
  +}
  @@ case N_FUNC_DEF (check):
       VARR_TRUNC (decl_t, func_decls_for_allocation, 0);
  +    VARR_TRUNC (node_t, func_stmtexprs_for_allocation, 0);
  @@
       process_func_decls_for_allocation (c2m_ctx);
  +    process_func_stmtexprs_for_allocation (c2m_ctx, block);
       /* Add call arg area */
  @@ case N_STMTEXPR (check):
  -    if (func_block_scope != NULL && (t1->mode == TM_STRUCT || t1->mode == TM_UNION)) {
  -      struct node_scope *fns = func_block_scope->attr;
  -      mir_size_t size = type_size (c2m_ctx, t1);
  -      mir_size_t align = var_align (c2m_ctx, t1);
  -
  -      fns->size = round_size (fns->size, align);
  -      e->c.u_val = fns->size;
  -      fns->size += size;
  -      fns->stack_var_p = TRUE;
  -    }
  +    if (func_block_scope != NULL && (t1->mode == TM_STRUCT || t1->mode == TM_UNION))
  +      VARR_PUSH (node_t, func_stmtexprs_for_allocation, r);
  @@ context_init:
       VARR_CREATE (decl_t, func_decls_for_allocation, alloc, 1024);
  +  VARR_CREATE (node_t, func_stmtexprs_for_allocation, alloc, 64);
  @@ context_finish:
     if (func_decls_for_allocation != NULL) VARR_DESTROY (decl_t, func_decls_for_allocation);
  +  if (func_stmtexprs_for_allocation != NULL)
  +    VARR_DESTROY (node_t, func_stmtexprs_for_allocation);
  ```

The body below is the report as it stood before this update.

## Summary

Under the MIR engine (`tur jit`, `tests/run-jit.sh`) on x86-64 Linux, a GNU
statement expression whose value is a by-value struct -- `tur_tagged_t` (the
`any` box, 16 bytes) or a by-value ADT -- is miscompiled in at least two
positions: as an argument in a call's argument list, and as the initializer of
a local. The observed effect is that a *sibling* value is overwritten: another
argument of the same call, or another parameter of the enclosing function. The
same C is correct under gcc and clang, and the same program is correct in the
engine on arm64 (the macOS runner).

The emitter no longer produces any of the shapes below, so no fixture hits it
today; the engine defect itself is open. It is a MIR / c2mir bug, not a
codegen one, and the Turmeric-side work is to keep the emitter out of the
shape until the engine is fixed upstream.

## Repro (three shapes, all pinned by fixtures that now run in the engine)

1. **Argument position, struct-valued `?:` or if/else inside `({ ... })`.**
   The carrier -> `any` bridge, spelled as
   `f(acc, ({ int64_t p = ...; p ? *(tur_tagged_t *)p : TUR_TAG(nil, 0); }))`,
   reached the callee with the *first* argument replaced by the second:
   `saffron-prelude`'s `(vec-fold [1.5 2.25] 0.0 (fn [a x] (+ a x)))` printed
   `4.5` (2.25 + 2.25) for 3.75; `saffron-container-param-cast-shape` printed
   `1` for `4.25`. Rewriting the bridge's interior as if/else into a local
   changed nothing -- the position is the trigger, not the conditional.
2. **Argument position, malloc'd box.**
   `each(TUR_TAG(fn_id, &fatbox), ({ T *b = malloc(...); *b = xs; TUR_TAG(id, b); }))`
   reached `each` with the first argument's tag wrong
   (`cannot call a unknown value -- it is not a function`).
3. **Initializer position, by-value ADT cast.**
   `tur_adt_Lst s = ({ tur_tagged_t c = xs; __tur_any_cast_check(...); *(tur_adt_Lst *)TUR_UNTAG(c); });`
   as a `match` scrutinee clobbered the function's other parameter `f`, so
   `(defn app [f xs] (match xs (Cons h t) (f 5) (Nil) 0))` panicked at `(f 5)`
   -- while the same match with `(f 5)` moved before it, or the same call
   without the match, was fine.

Minimal program for shape 3 (`tur jit` on x86-64 panics; `tur run` prints 5):

```turmeric
#lang saffron
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn app4 [f xs]
  (match xs
    (Cons h t) (f 5)
    (Nil)      0))
(defn show [x] (println x))
(defn main [] : int
  (app4 show (Cons 1 (Nil)))
  0)
```

`TUR_JIT_DUMP_C=<path>` now writes the exact text handed to c2mir on the
no-split path too, so the shapes can be read off the engine's own input.

## What the emitter does now

Every site that produced one of those shapes builds the value with statements
in the enclosing body and leaves a plain expression behind:

- `emit_core.c` carrier -> `any` bridge: a call to a per-TU `static inline`
  helper `__tur_any_of_carrier(int64_t)` (`ensure_any_carrier_bridge`,
  emit_module.c), emitted on demand.
- `emit_expr.c` dynamic call: the callee box is bound to a fresh temp by a
  statement, the check runs there, and the call is a bare prototype-cast
  call (this also dropped the `(tur_tagged_t)(a)` identity struct casts
  `TUR_APPLYn_T` made, which c2mir rejects outright -- see the JIT guide).
- `emit_expr.c` EX_UNION_INJECT by-value aggregate: the malloc'd box is built
  by statements; the expression is `TUR_TAG(id, tmp)`.
- `emit_expr.c` by-value ADT cast out of `any`: the box read and the tag
  check are statements; the expression is the dereference.

**Shape 4, found 2026-09-10 exactly as the paragraph below predicted: the
`any` -> SCALAR cast.** The note here used to add that "the scalar arms beside
this one yield a word and are fine". They do yield a word -- and that is not
what the engine trips over. What matters is what the statement expression
CONTAINS: `__tur_c` is a 16-byte `tur_tagged_t` bound from a call that itself
takes `tur_tagged_t` arguments. In argument position that call reached its
callee with the first argument replaced by the second, which is shape 1's
symptom arriving through a scalar-valued `({ ... })`:

```turmeric
#lang saffron
(defn id [x] x)
(defn main [] : int
  (let [g (id (fn [a : float b : float] : float (+ a b)))]
    (println (cast (g 3.5 1.5) float)))
  0)
```

`tur run` prints 5; `tur jit` on x86-64 printed **3** -- 1.5 + 1.5, the first
argument replaced by the second. No adaptor or seam is involved, so this was
reachable before the Saffron work; `saffron-seam-into-typed-fn-param` is simply
the first fixture to run a two-float call through an `any` in the engine.
Fixed by fix direction 2: both scalar arms of `EX_ANY_CAST` now bind the box
and run the tag check as STATEMENTS and leave a plain unbox expression, exactly
as the by-value ADT arm beside them already did. `inner` already emits its own
statements into the body, so this changes no evaluation order.

**Shape 5, found 2026-09-26: the dynamic method call (`__tur_dm`).** It was
the first site on the "none observed to misbehave yet" list. Two `any`
receivers dispatched in one call's argument list --

```turmeric
#lang saffron
(load "stdlib/rc.tur")
(defdata Two [a] (Two [l : a r : a]))
(definstance Foldable [Two]
  (foldl [t init f] (f (f init (.l t)) (.r t)))
  (foldr [t init f] (f (.l t) (f (.r t) init))))
(defn tri [p q r] (+ p (* q r)))
(defn show [t u]
  (println (tri (.foldr t 0.5 (fn [x acc] (- x acc)))
                (.foldl u 1.0 (fn [acc x] (* acc x)))
                2.0)))
(defn main [] (show (Two 1.5 2.25) (Two 7.25 3.5)) 0)
```

-- print 50.5 under `tur run` and `--interpret`; `tur jit` on x86-64
panicked `+: no operator for a value of that type argument`. With a
`(println (diff2 (.foldl t ...) (.foldl u ...)))` ahead of it and a third
dispatch (through a helper) in place of the `2.0`, the second dispatch was
handed a float for its receiver instead: `no instance of Foldable for float
(dispatching .foldl on an any)`. An earlier re-check the
same day called this site clean: its fixture let-bound the receivers, which
resolves `.foldl` to the static instance specialization, so only one
`__tur_dm` ever ran. `saffron-dyn-method-in-argument-position` now routes
every receiver through an unannotated (so `any`) parameter. Fixed by fix
direction 2: `emit_dyn_method` binds the receiver box and the looked-up slot
as STATEMENTS and leaves a bare prototype-cast call, as `emit_dyn_call`
already did for its callee box.

Still emitted as struct-valued statement expressions, none observed to
misbehave yet (all three hoisted 2026-09-28, see the status at the top): the
union widen (`__tur_ua`), `dyn_widen_to_any`'s by-value box (`__tur_fb`),
and `emit_agg_box`'s `__tur_pbox` (emit_expr.c, not emit_core.c). `tests/run-jit.sh` passes
the fixtures that reach them (`union-to-any-widen-aliases-box` and
`docs-any-guide-examples` for `__tur_ua`, `forall-dict-byvalue-receiver` and
`hkt-constrained-byvalue-carrier` for `__tur_pbox`) -- but, as shape 5 shows,
a fixture passing is only evidence if it puts SEVERAL of these values in one
argument list. If a fixture that reaches one of them starts answering
differently in the engine on Linux only, this is the first thing to suspect.

## Fix directions

1. Upstream: reduce shape 3 to plain C (a 16-byte struct returned from a
   `({ ... })` initializer next to another struct local) against c2mir +
   MIR-gen on x86-64 and file it on rjungemann/mir. Likely a temp-slot reuse in
   the statement-expression lowering when the value is a block (BLK) type.
2. Meanwhile, keep the emitter out of the shape (above), and if one of the
   remaining sites is implicated, hoist it the same way.

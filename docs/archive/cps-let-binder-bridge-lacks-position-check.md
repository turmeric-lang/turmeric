# The CPS let-binder bridge claims parity with a site that has since outgrown it

**Severity: low** -- LATENT. The divergence is real and verified by reading;
**no repro was found**, and the gate is inert for every shape reached so far.
Filed for the next person who hits a double-unbox in a CPS-transformed body, so
they start here instead of re-deriving it.

**Status: RESOLVED 2026-09-02** -- both fix directions landed, and the missing
repro was replaced by a corpus-wide measurement that explains why there is not
one. See the Resolution section at the bottom.

Filed 2026-09-02 by the sweep that
[control-form-around-if-double-unboxes-carrier-arms](../archive/control-form-around-if-double-unboxes-carrier-arms.md)
asked for in its "Related" section -- "worth a sweep for the remaining
`fn_body_tail_emits_byvalue_carrier_abi` callers rather than a fifth round of
this."

## The finding

`emit_cps_ir.c:6507` is an explicit mirror of the direct emitter's
`init_carrier_to_byval` bridge, and says so:

> `cps-result-unbox-dropped`: mirror the direct emitter's `init_carrier_to_byval`
> bridge (emit_expr.c, EX_LET). ... **Same gate as the direct site**, so it is
> inert whenever the init already yields the aggregate.

It is no longer the same gate. The direct site (`emit_expr.c`, the EX_LET
binding loop) carries a **position check** the mirror does not:

```c
/* emit_expr.c -- the direct site */
bool init_val_recorded_byval_agg = false;
if (emit_str_is_bare_ident(iv)) {
    const char *lvty2 = emit_localvar_lookup_ctype(iv);
    init_val_recorded_byval_agg =
        lvty2 && strcmp(lvty2, bind_c) == 0 &&
        strcmp(lvty2, "int64_t") != 0 && strchr(lvty2, '*') == NULL;
}
bool init_carrier_to_byval = !bind_is_ptr_repr &&
    strcmp(bind_c, "int64_t") != 0 &&
    init_bv.kind != TY_UNKNOWN &&
    !init_val_recorded_byval_agg &&                        /* <-- absent in the mirror */
    !fn_body_tail_emits_byvalue_carrier_abi(ctx, init);
```

```c
/* emit_cps_ir.c:6507 -- the mirror */
if (rhs && bct && strcmp(bct, "int64_t") != 0 &&
    strchr(bct, '*') == NULL &&
    init_bv.kind != TY_UNKNOWN &&
    !fn_body_tail_emits_byvalue_carrier_abi(ce->ctx, t->as.letraw.e)) {
```

`fn_body_tail_emits_byvalue_carrier_abi` is an **Expr-level** predicate -- "what
would this Expr naturally emit". The missing term is the **position-level**
question -- "what representation does the value in hand have HERE". That is
precisely the distinction four resolved reports in this family have each moved
one more site onto, and this mirror is a site that has not moved.

## Why no repro was found

Two conditions must hold together, and the shapes tried satisfied only one:

1. The binder's C type (`bct`) must be the by-value aggregate rather than the
   carrier -- otherwise the gate is inert before the predicate is consulted.
2. The init value must be a value something ALREADY bridged (a merge temp), so
   bridging again is the second unwrap.

An effect-`perform` body does route through the CPS path (143 `__cps` functions
emitted for a three-line program), and its env struct really does hold
`tur_adt_Result__Handle__int f0` -- but the `let` binder itself came out
`int64_t r_1433`, the carrier, so condition 1 failed and no bridge was
attempted. A `^fat` higher-order call did not reach the `letraw` path at all.
Getting both conditions at once is what the next investigation needs.

## Repro attempts that did NOT trigger it

Recorded so the next person does not repeat them:

```turmeric
; CPS path taken, binder is the carrier -- gate inert, output correct.
(defeffect Ask [] : int)
(defn go [take-ok : bool] : int
  (let [r (if take-ok (mk-ok 7) (mk-err 11))]
    (+ (perform (Ask)) (if (ok? r) 100 200))))
```

```turmeric
; A ^fat higher-order call does not reach the letraw path.
(defn apply1 [^fat f : (fn [int] int) x : int] : int (f x))
```

## Fix direction

Add the missing term, which means giving `emit_cps_ir.c` access to the same
position question the direct site asks. `emit_arm_is_recorded_byval_agg` is
`static` in `emit_expr.c`, and the direct site hand-rolls the check inline
rather than calling it -- so there are really two cleanups here, and doing them
together is what makes the mirror stay a mirror:

1. **Extract the check once.** The direct site's `init_val_recorded_byval_agg`
   is `emit_arm_is_recorded_byval_agg` with the binder's C name in place of the
   type lookup. One shared non-static helper taking `(value, expected C type)`
   would serve the `emit_if` arms, `bridge_control_value_to_byvalue_temp`, the
   direct let-binding init, and this mirror.
2. **Then add the term to the mirror**, and the comment's "same gate as the
   direct site" becomes true again.

Do NOT add the term speculatively without a repro: the gate currently guards
`cps-result-unbox-dropped`'s fixtures, and a change to a path with no failing
case is unverifiable in the direction that matters.

## Related

Every one of these moved one site from the type/Expr question to the position
question. This report is the remaining site:

- [`control-form-around-if-double-unboxes-carrier-arms`](../archive/control-form-around-if-double-unboxes-carrier-arms.md)
  -- the sweep that found this; note its own fix needed a second half the
  report did not anticipate (the `emit_if` merge temp was never RECORDED).
- [`byvalue-product-tail-var-double-unboxed-nonparametric`](../archive/byvalue-product-tail-var-double-unboxed-nonparametric.md)
- [`result-block-value-double-unboxed`](../archive/result-block-value-double-unboxed.md)
- [`cps-result-unbox-dropped`](../archive/cps-result-unbox-dropped.md) -- the
  report that ADDED the mirror being questioned here.

## Guides to update when fixed

- [docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
  -- its closed-cells table is where this family is tracked.

## Resolution (2026-09-02)

Both fix directions landed, in the order the report specifies.

### 1. The check is extracted, and there were more copies than the report knew

`emit_value_is_recorded_as(v, want_ctype)` is now the single answer to "does the
value in hand already HAVE the by-value aggregate representation, here?". It
takes the wanted C type as a STRING, because that is what the binder sites hold
(`bind_c`, `bct`); `emit_arm_is_recorded_byval_agg` is a thin Type-taking wrapper
over it for the arm sites.

The report says the direct site "hand-rolls the check inline rather than calling
it". There were **two** such inline copies, not one -- two separate let-binding
init sites, each with its own `lvty2` block spelling the same three comparisons.
With the CPS mirror having none, that is four sites and three different answers
to one question. There is one copy now.

### 2. The term is added, justified by measurement rather than by a repro

The report warns: *"Do NOT add the term speculatively without a repro ... a
change to a path with no failing case is unverifiable in the direction that
matters."* That is the right instinct, and the way past it was not to find a
repro but to make the change **provably inert**, which is a different and
achievable bar.

Instrumenting the bridge and sweeping **all 2131 fixtures**: only 33 reach it
with a by-value init type at all, and in every one of them either

- `tailabi=1` -- the existing Expr-level predicate already suppresses the bridge,
  or
- the init is recorded as `int64_t`, a pointer, or nothing -- never as the
  aggregate, so the new term answers false and changes nothing.

The single fixture that hands the bridge a **recorded by-value merge temp**
(`option-construct-byvalue-return-spec`, `rhs=__t211`,
`recorded_lv=tur_adt_Option__int`) has `tailabi=1`, so it never fires. And
`cps-result-carrier-unbox` -- the fixture the gate exists for -- fires with
`recorded_lv=int64_t`, so the term leaves it alone, which is what keeps that
regression pinned.

Result: **the emitted C is byte-identical across the corpus.** Suite 2752 passed
/ 0 failed with zero snapshot churn, and the five fixtures nearest this gate were
checked individually for matching OUTPUT, not merely building.

Be clear about what that does and does not establish: this is a **consistency
repair**, not a fix for an observed miscompile. It restores the "same gate as the
direct site" claim the mirror's own comment makes, so the two cannot drift a
third time. It is not evidence that anything was broken.

### Why there is no repro, which is the substantive finding

Two conditions must hold together, and they appear to be **structurally
exclusive** in the CPS path rather than merely rare:

The dangerous shape needs the init to be an `if` whose arms are carrier
producers -- that is what makes the Expr-level predicate answer false while
`emit_if` has already bridged the arms into a recorded by-value merge temp. But
when such an `if` is a `let` init inside a CPS-transformed body, the CPS
transform restructures it before the bridge sees it: a targeted repro built to
that recipe (an inline-C `(Result H cstr)` producer in each arm, plus a
higher-order call to force the transform, modelled on
`cps-result-carrier-unbox`) reached the bridge as **two separate hits** with
`rhs=__ps_174` / `__ps_175`, each recorded `int64_t` -- one per branch, the `if`
already split. Never a merge temp.

So the guard's absence was not reachable through the shape that motivated
looking for it. Recorded here so the next person does not repeat the search: the
question to answer first is whether the CPS transform can ever leave an `EX_IF`
as a `letraw` init, and the evidence above says it does not for this shape.

### What would still be worth doing

Nothing in this report. If a double-unbox ever does surface in a CPS-transformed
body, the term is already there and the shared helper is the place to look.

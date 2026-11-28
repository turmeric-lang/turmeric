# A `match` lowered by the CPS backend never frees the scrutinee's struct payload box

**Narrowed 2026-11-28: the repro below is fixed.** The CPS backend lowers
this `match` as a direct dispatch: it binds the scrutinee to a fresh binder
and hands the match to the direct emitter. That emitter's rule ("a fresh
producer's payload box is freed once the arm has copied its binders out",
`match_scrut_owns_vsp_box`, and the carrier-cell twin
`match_scrut_owns_carrier_cell`) then saw a variable, not the call.

`CT_MATCH.fresh_scrut` keeps the original call, and `emit_direct_match` passes
it through `MatchTailCtx`. It does so only when that call was delegated to
the direct emitter (`safe_to_delegate`), so it runs once per match and its box
is fresh each time; the rule then asks the call, as on the direct path.
Pinned by `tests/fixtures/cps-match-fresh-payload-freed` (leak-checked, both
engines; 48 B leaked before). turmeric-spices: `html/tests/sax_tokens.tur` and
`stats-formula/tests/formula_expand_test.tur` gain the free, and their tests
under `TUR_SANITIZE_PROGRAMS=1` show no memory-safety error (`html` 416 B
less leaked).

**Still open:** a producer the CPS backend lowers. Under a multi-shot resume
it can deliver the SAME box to the match twice: `early` in
`tests/fixtures/cps-match-payload-multishot` builds it, then performs, then
returns it. So a free here would be a double free, and that box is left
alone (known-leak). Freeing it needs a deduplicating reap
(`__dk_reap_ptr_once`, kind 0, like `__dk_reap_closure_once`). That is a
preamble change, so every snapshot moves.

**Severity: low-medium (leak, one box per match).** A by-value struct inside a
`(Result T E)` or `(Option T)` rides a heap box. A `match` on a fresh producer
owns that box and frees it once the arm has read its binders, on the direct
path. The same `match` in a function the CPS backend lowers, with an arm that
performs, does not.

Found 2026-11-24 while executing
[cps-byvalue-struct-with-closure-field](../archive/cps-byvalue-struct-with-closure-field.md).
Reproduces on `main` at `1227412b`; it has nothing to do with closure fields.

## Repro

```turmeric
(defeffect Log [x : int] : nil)
(defstruct ^copy P [raw : int handler : int])
(defn parser-new [n : int h : int] : (Result P int)
  (if (< n 0) (Err n) (Ok (P n h))))
(defn main [] : int
  (handle
    (match (parser-new 100 7)
      (Ok p)  (do (perform (Log (.raw p))) (println (+ (.raw p) (.handler p))))
      (Err e) (println e))
    (Log [x] k) (do (println x) (resume k nil)))
  0)
```

Built as `tests/run-leak-check.sh` builds:

```
SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s).
```

Remove the `handle` and the `perform` and the same `match` leaks nothing.

## Root cause

The direct emitter frees the box in the arm, after the binder copies out
(`match_scrut_owns_vsp_box`, `src/compiler/emit_expr.c:7361`, from
[sum-payload-box-leaks-at-match](../archive/sum-payload-box-leaks-at-match.md)
fix 1):

```c
tur_adt_P p_8 = *(tur_adt_P *)(intptr_t)(__scrut->as.Ok._0);
...
if (__scrut->as.Ok._0) free((void *)(intptr_t)__scrut->as.Ok._0);
```

The CPS emitter's `emit_match` (`src/compiler/emit_cps_ir.c:11264`) emits the
binder copy and no free. A match with a control op in an arm cannot be
delegated whole to the direct emitter, so it never reaches the code above.

## Why it matters

The SAX parser constructors in turmeric-spices return
`(Result SaxParser ParseError)`. Measured on a probe of that shape (a
constructor returning `(Result P int)`, matched inside a `handle` body), the
leak is 16 B per parser, before and after the struct moved to the by-value
layout on 2026-11-27: it was the struct's carrier box, and is the Result's
payload box now. A capturing handler's env hangs off it (24 B more in the
probe).

## Fix directions

1. In `emit_match`, apply the direct emitter's rule: when the scrutinee is a
   fresh temp the match alone reads, register the arm's box with the boundary
   reap after the binders are read (`__dk_reap_ptr`). A reap and not a `free`:
   the arm's continuation can run again under a multi-shot resume and read the
   box a second time.
2. Check `match_field_is_ros_pointer_box` for the binder-copy precondition the
   direct rule depends on, and refuse an `is_var` arm the same way.

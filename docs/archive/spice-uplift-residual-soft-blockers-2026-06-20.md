---
title: Residual soft blockers found during Track C plot paydown (2026-06-20)
category: Spice-uplift soft blockers — to be filed as turmeric reports
status: RESOLVED (W1/W2 fixed by #463; W3 fixed by #464)
reported-by: turmeric-spices Claude (Track C, branch claude/track-c-turmeric-xaky6y)
verified-on: turmeric 0.21.0, main @ 48e99d9 (post #462 / #463 / #464; built from source)
---

# Residual soft blockers (Track C plot paydown)

While paying down the S4–S7 workarounds in `spices/plot/src/plot/core.tur`,
three soft workarounds surfaced. **All three are now fixed in turmeric main:
W1 and W2 by #463, W3 by #464.** Their spice-side workarounds have all been
retired. This doc is kept for the paper trail.

W3 never blocked: the fold stood as a top-level `defn` until #464 landed; it is
now a local `letrec` closure again (see `renderers-bbox` / `anyrenderers->legacy`
in `spices/plot/src/plot/core.tur`).

---

## W3 — `letrec` self-recursive closure that *captures* an existential-element vec and `open`s it mis-emits the captured binding in C — FIXED (#464)

Fixed by turmeric #464 ("Fix closure capture of bindings referenced inside
open/pack/dispatch"): `collect_free_vars` now traverses the existential
`open`/`pack`/`dispatch` forms, so a captured binding referenced inside an
`open` body is added to the closure's capture set and resolves to its env slot
instead of mis-emitting an undeclared outer C name. Verified: the repro below
now checks **and** runs (returns 12), and the plot folds `renderers-bbox` /
`anyrenderers->legacy` are local `letrec` closures again, with the pure-Turmeric
`any_renderer`/`bbox` tests green.

Severity: Low. Type-check passes; codegen emits a reference to the captured
binding (`<name>_NNNN`) that is never declared in the lifted closure's C
function, so `cc` fails with `'<name>_NNNN' undeclared`. Worked around by
hoisting the loop to a top-level recursive `defn` threading the vec as an
explicit parameter — so it does not block, but it defeats the local `letrec`
form for the heterogeneous-renderer fold.

Relationship to fixed work: this is adjacent to S5
(`letrec-self-recursive-closure-misemits-undeclared-binding`, which fixed the
self-*call* box name) but distinct: S5 was the closure's own binding; W3 is a
*captured outer* binding referenced inside the lifted body. A plain `(Vec int)`
capture emits fine — only an existential-element vec dereferenced via `open`
inside the closure body trips it, so the `open`/witness-projection lowering is
the likely culprit (it loses the capture-env rewrite for `rs`).

Repro (verified on this tree, main @ f7bc09f):

    (defmodule capexists
      (defclass Sz [a] (sz [x : a] : int))
      (defstruct Wm :copy [w : int])
      (definstance Sz [Wm] (sz [x] (.w x)))
      (defn mk-vec [] : (Vec (exists [a] [(Sz a)] a))
        (let [v (:: (vec-new) (Vec (exists [a] [(Sz a)] a)))]
          (vec-push! v (pack (make-struct Wm 5) (exists [a] [(Sz a)] a)))
          (vec-push! v (pack (make-struct Wm 7) (exists [a] [(Sz a)] a)))
          v))
      ;; letrec closure CAPTURES the existential vec `rs`, opens it in the body
      (defn total [rs : (Vec (exists [a] [(Sz a)] a))] : int
        (let [n (vec-len rs)]
          (letrec [go (fn [i : int  acc : int] : int
                        (if (>= i n) acc
                          (go (+ i 1) (+ acc (open (vec-get rs i) [a v] (sz v))))))]
            (go 0 0))))
      (defn main [] : int (total (mk-vec))))   ;; expect 12

    ;; tur check => exit 0
    ;; tur run   => /tmp/.../capexists_tur.c:3003:86:
    ;;              error: 'rs_1013' undeclared (first use in this function)

Control (passes): the same shape with a plain `(Vec int)` capture and
`(vec-get rs i)` (no `open`) builds and runs — so the trigger is the
existential `open` capture, not vec-capture in a letrec per se.

Workaround (in landed spice code): `__renderers-bbox-go` and
`__any-to-legacy-go` in `spices/plot/src/plot/core.tur` stay top-level `defn`s
that take `rs` as an explicit parameter, rather than local `letrec` closures
inside `renderers-bbox` / `anyrenderers->legacy`. Folding them into `letrec`
closures (the tighter form, unblocked on the typing side by #463/W1) is blocked
on W3.

Fix direction: ensure the closure-conversion capture-env rewrite reaches the
`open`/witness-projection lowering, so a captured binding referenced inside an
`open` body resolves to the env slot (the same way a plain `vec-get` reference
on the captured binding already does) rather than the outer-scope C name.

---

## Fixed in #463 (retired this pass) — kept for the paper trail

### W1 — `letrec` self-recursive closure with a struct / `(Vec T)` accumulator typed its self-call at the int carrier — FIXED (#463)

Was the `letrec`-closure analogue of S6 (top-level-`defn` case fixed by #460).
Verified fixed: the repro below now checks **and** runs (exit 7).

    (defstruct Box :copy [lo : int  hi : int])
    (defn mk [n : int] : Box (make-struct Box n n))
    (defn go-letrec [n : int] : Box
      (letrec [go (fn [i : int acc : Box] : Box
                    (if (>= i n) acc (go (+ i 1) acc)))]
        (go 0 (mk 7))))

### W2 — a bare `(vec-new)` passed directly as a concrete `(Vec T)` argument did not unify its element tyvar — FIXED (#463)

Verified fixed: `(sink (vec-new))` against `sink : (Vec int) -> int` now checks
clean; the plot `anyrenderers->legacy` seed dropped its `(:: (vec-new) (Vec int))`
ascription to a bare `(vec-new)`.

---

## Also retired earlier this session (json)

`spices/json/src/json/encode.tur` (`derive-json-sum` / `derive-json-sum-decode`)
no longer routes its `(ok <adt>)` tail through a generated standalone `defn`;
the `Decode` instance boxes `(ok ...)` directly in the method body now that the
ADT-typed result-slot box is correct inline. Full json suite green (15 files).
Closed, not open.
</content>

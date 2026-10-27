# Cross-function resume (item c) -- design + scope

**Severity: low (design note -- item (c) of the resuming-shift plan is the deep
first-class-continuations piece; this records the tractable path and why it is a
substantial effort, not a bounded increment).**

## The gap

After the unification (items A/B), one `shift`/`reset` pair covers abortive,
resumable (lexically scoped), single-shot, and cross-function **abort**. The one
remaining hole is cross-function **resume**: a resuming shift whose receiver
invokes the continuation, delimited by a reset in a *caller*.

```turmeric
(defn ck [k : cont] : int (+ (k 1) (k 2)))
(defn inner [] : int (shift ck 0))          ; resumes k
(defn outer [] : int (reset (+ 10 (inner)))); reset is in the caller
```

This is rejected (`TUR-E0016`, now with a tailored message pointing here). The
reified-context lowering is **lexically scoped** -- the continuation is reified
by walking the reset body syntactically, which cannot cross a function boundary.
Cross-function *abort* works because it never reifies (it aborts dynamically);
cross-function *resume* fundamentally needs a dynamically-captured continuation.

## Key finding -- the machinery already exists (effects)

`perform` / `handle` / `resume` **already do cross-function resumable
continuations**, on both paths (compiled `dk_invoke`; interpreter fibers).
Verified:

```turmeric
(defeffect Yield [] : int)
(defn producer [] : int (+ 100 (perform (Yield))))   ; performs cross-function
(defn run [] : int
  (* 10 (handle (producer) (Yield [] k) (resume k 5))))  ; handler resumes k
;; direct == turi == 1050
```

So cross-function resume is not missing from the runtime -- it is missing only
from the *shift/reset surface*. The tractable design is therefore a
**shift/reset -> effect desugar**, not new DK-subk plumbing across three
backends.

## The design -- shift/reset as a synthetic effect

A resuming shift/reset maps onto one synthetic effect `__Shift` carrying the
receiver:

- `(reset BODY)` (when it binds a cross-function resuming shift) ->
  `(handle BODY (__Shift [recv] k) (recv <k-as-cont>))` -- the handler receives
  the shift's `recv` as the effect payload and the delimited continuation `k`,
  and applies `recv` to it.
- `(shift recv _)` -> `(perform (__Shift recv))`.

This inherits the effect machine's cross-function capture on both paths.

## Blocker 1 -- resume representation mismatch -- RESOLVED

The two surfaces spelled "resume" differently: a shift receiver `(fn [k : cont]
(k v))` uses the `(k v)` sugar (cloneable resume), while an effect handler
continuation used `(resume k v)` and was NOT applicable with `(k v)`.

**Landed:** `(k v)` now works on an effect handler continuation too -- a call
through an `is_continuation` binding produces an `EX_RESUME` (shared
`elab_make_resume`, factored out of `elab_resume`; `elab_call.c` routes the
`(k v)` sugar there). So a receiver `(fn [k] (k v))` resumes a handler
continuation uniformly. Verified cross-function (`perform` in a callee, `handle`
+ `(k 5)` in a caller): `direct == turi == 1050` (fixture
`handler-cont-kv-sugar`). This is the foundation the desugar needs -- the handler
body can now apply the shift's receiver to the handler continuation directly.

Note: this reuses the handler continuation's existing int64 representation (no
`CONT_EFFECT` type flavor needed) -- the `is_continuation` flag is the hook, and
`resume` already accepts an arbitrary continuation *value*.

## Blocker 2 -- reset must catch a cross-function perform -- the remaining work

A `(shift recv _)` in a callee lowers to `(perform (__Shift recv))`; the matching
handler must be at the enclosing `reset`, which is in a *caller* and is
elaborated separately. So **every `reset` must install a `__Shift` handler**
around its body:

```
(reset BODY)  ->  <abortive-or-reified-reset> ( handle BODY
                    (__Shift [recv] k) (recv k) )      ; recv resumes k via (k v)
```

A lexical abortive/reified shift bypasses the handler (it is `EX_SHIFT` /
`EX_CLONEABLE_SHIFT`, not a `perform`) and hits the outer reset unchanged; only a
cross-function resuming shift performs `__Shift` and is caught here. `(recv k)`
applies the receiver to the handler continuation (now possible via blocker 1).

The cost/risk: wrapping every reset in a handler changes reset codegen for the
whole corpus (fixture snapshots, and the shift0 / nested / substrate shapes the
reset-alias experiment showed are fragile). The safe way to introduce it is a
**whole-program pass** that wraps resets in the `__Shift` handler *only when the
program contains a cross-function resuming shift* -- so the common case (no such
shift) is byte-for-byte unchanged. A same-elaboration global flag will not do
(a reset can be elaborated before the callee's shift sets it); it needs a
post-elaboration transform.

### Mechanism VERIFIED; the auto-desugar remains

The reset -> handle + shift -> perform mechanism is proven end to end -- a
*hand-written* `__Shift`-style desugar of a cross-function resuming shift runs
`direct == turi == 15`, and the capturing, single-resume shape is pinned by
fixture `cross-function-resume-via-effect` (`= 11`). So cross-function resumable
continuations WORK today via the effect surface (blockers 1 + 3), with the
ergonomic `(k v)` resume. What is left is purely the **automatic desugar** of the
`shift`/`reset` *surface* onto this working machinery -- a substantial multi-part
change, not a bounded increment:

1. **Synthesize the `__Shift` effect** (program-level, carrying the receiver as a
   boxed fn payload -- now supported).
2. **`shift` -> `perform`** for the cross-function resuming case (the site that
   today raises `TUR-E0016`).
3. **`reset` -> handle-wrapping** whole-program pass (gated as above).
4. **The receiver-convention crux.** The user writes `(fn [k : cont] (k v))`,
   whose `(k v)` is *cloneable* resume; cross-function needs *effect* resume.
   Four ways to bridge, each a real change: (a) a `CONT_EFFECT` cont flavor whose
   `(k v)` lowers to `EX_RESUME` (consistent with the existing
   cloneable/serial/escape flavors; but the user's bare `cont` must be routed to
   it for the cross-function case); (b) a Form-level rewrite of the receiver's
   `(k v)` -> `(resume k v)` in the desugar (fiddly: identify continuation
   applications, handle shadowing); (c) a runtime cloneable-cont *bridge* wrapping
   the effect continuation so cloneable resume forwards to effect resume;
   (d) runtime-polymorphic `(k v)` dispatch on the continuation's flavor.

Blocker 1 (`(k v)` on `is_continuation` bindings) already solves the *handler*
side; the crux (4) is the *receiver* side, where the param is an ordinary `cont`,
not an `is_continuation` binding.

## Blocker 3 -- effects carry a callable fn payload -- RESOLVED

Effects now carry a callable fn payload, **capturing or not**. Three fixes:

1. `defeffect` dropped the full param `Type` for a `TY_FN` param (only
   ADT/APP/STRUCT were preserved), so the handler saw a 0-arity uncallable fn.
   Preserve `TY_FN` too, and mark it **`boxed`** -- an fn payload is a boxed
   closure: a one-word `void *` to a heap `{thunk, env}` box, which carries a
   captured env yet fits the one-word effect slot. (`elab_effects.c`)
2. The handler declared an fn-value payload as `f_<id>` but the call site uses
   the raw id-less name (`name_for_binding`'s `TY_FN`-non-boxed path) -- the
   declaration now matches. (`emit_effects.c`) *(Superseded in practice by (1)'s
   boxed spelling, but kept for the general fn-value case.)*
3. `elab_perform` boxes each non-boxed fn arg (`EX_FN_TO_FAT`) so a *non*-capturing
   fn (a bare pointer) becomes the same boxed representation a capturing closure
   already is -- uniform, so the handler's boxed dispatch is always right.

Verified `direct == cps == turi`: non-capturing (`effect-fn-payload` = 1050) and
capturing on both the receiver and handler sides (`effect-fn-payload-capturing`
= 1107; a receiver closing over two locals resumes correctly). This is exactly
the `__Shift` receiver shape (`(fn [k] (k v))` closing over the shift's locals),
so the effect-desugar path is no longer blocked on the payload representation --
what remains for full cross-function resume is blocker 2, the reset-wrapping
whole-program transform.

### (historical) the original blocker probe

Attempting the reset-wrapping transform surfaced a prerequisite the effect
desugar *depends on but does not have*: the `__Shift` effect must carry the
receiver as an **fn-value payload**, and the handler must apply it (`(recv k)`).
Effects cannot do this today. Probed directly (not via the desugar):

```turmeric
(defeffect ShiftE [recv : (fn [int] int)] : int)          ; fn-typed payload
(defn producer [] : int
  (+ 100 (perform (ShiftE (fn [k : int] : int (resume k 5))))))
(defn run [] : int
  (handle (producer) (ShiftE [recv] k) (recv k)))          ; apply the payload
```

```
error [TUR-E0002]: function 'recv' returns ?, which is not callable
  -- did you mean to pass all 0 argument(s)?
```

The handler param `recv` is bound from `eff->constructor->param_full_types[j]`,
but a fn-typed effect payload loses its arity/param types across the effect
constructor -- `recv` reads as a 0-arity fn, so `(recv k)` is uncallable. An
`unsafe (reinterpret recv (fn [int] int))` does not recover it either (still
"type int, not callable"). So **effect payloads do not preserve callable fn
types**, and the `__Shift` desugar is blocked here before the reset-wrapping
even runs.

The receiver's own resume mechanism is a second latent issue: `(fn [k : cont]
(k v))` bakes in *cloneable* resume at definition time, which is wrong for a
handler (dynamic) continuation; the desugar would need the receiver to resume via
the effect continuation. Blocker 1 fixed `(k v)` on `is_continuation` bindings,
but a receiver's own `k` param is an ordinary param, not `is_continuation`.

## Status

- **Foundation landed (blocker 1 resolved):** unified resume surface -- `(k v)`
  on handler continuations (`handler-cont-kv-sugar`).
- **Blocked (blocker 3, newly found):** the `__Shift`-effect desugar cannot be
  built until effects can carry a *callable* fn payload -- itself a distinct
  compiler change to the effect-constructor param typing. Plus blocker 2 (the
  reset-wrapping whole-program transform) and the receiver-resume-mechanism issue.
  Cross-function resume is therefore a multi-prerequisite change, not a single
  transform; the two viable architectures are (a) this effect desugar once fn
  payloads are callable, or (b) DK-subk threading in the CT-IR backend with a
  DK-flavored continuation (avoids the fn payload, but needs interp support and a
  dynamic-resume `cont` flavor).
- Until then the tailored `TUR-E0016` message steers users to a lexical reset or
  to hand-written effects (fixture `errors/shift-crossfn-resume`), which already
  give cross-function resumable continuations -- now with `(k v)` sugar.

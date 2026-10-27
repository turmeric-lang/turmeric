---
title: Signal Phase 0 Spike -- Findings
category: Planning
description: Spike report on the Phase 0 :float sample migration from signal-primitives-expansion-plan. Documents a compiler-level blocker and proposes three alternative paths.
---

# Signal Phase 0 Spike -- Findings

## Resolution (2026-06-05) -- SUPERSEDED; no Option-B work landed

The recommendation below (Option B: a centralised `__sig_call` bit-cast
macro in `dsp.tur`) has been **overtaken by events** and is intentionally
**not** executed:

- **The signal spice was removed wholesale.** There is no
  `../turmeric-spices/spices/signal/src/signal/dsp.tur` to add a
  `__sig_call_f` / `__sig_call` macro to, and no existing SFs to refactor.
  The spice is being rebuilt from scratch by
  [tur-signal-rebuild-plan](../../upcoming/tur-signal-rebuild-plan.md), which
  **explicitly rejects** the int64-bit-cast sample ABI that Option B was
  designed to centralise. Adding the macro now would re-entrench exactly
  the pattern the rebuild exists to delete.
- **The Phase 0 exit-criteria edit is moot.** Its target,
  `signal-primitives-expansion-plan.md`, is archived under
  `docs/archive/history/` and superseded by the rebuild plan. There is no
  live `src/signal/` to grep-gate.
- **Option A is subsumed -- do not file `closure-float-return-abi-plan.md`.**
  The compiler-side capability Option A asked for (`:float`-returning
  fat closures, so samples flow as native `double` through closure and
  arrow boundaries with no `memcpy`) has since landed via the typed
  closure-invocation work, not as a separate plan:
  - [closure-typed-invocation-abi-plan](../closure-typed-invocation-abi-plan.md)
    threads declared arg/return types to the C invocation site.
  - The typed `:fn` carrier (phase F5) now round-trips `float`/`cstr`/`ptr`
    through `tur_poly_fn_t` -- see the RESOLVED report
    `docs/reported/fn-first-class-float-carrier-gap.md`.
  - The companion bare-`^fat` result-type and struct-return fixes
    (`docs/archive/bare-fat-result-type-inference-plan.md`,
    `docs/reported/fat-closure-dispatch-does-not-handle-struct-return.md`)
    close the remaining float/aggregate boundary gaps.
  These are reflected in G1/G4/G7 going green in
  [language-readiness-for-typed-signal-plan](language-readiness-for-typed-signal-plan.md).
  Filing a separate `closure-float-return-abi-plan.md` would duplicate
  work already done.

Net: the typed-`:float` closure ABI that Option A wanted exists today, so
the rebuild gets the clean path *for free* and never needs Option B's
interim macro. The findings below are retained for historical context.

---

Spike on Phase 0 ("finish the `:float` sample migration") from
[signal-primitives-expansion-plan.md](signal-primitives-expansion-plan.md).
**Result: blocked.** Phase 0 as written requires changes the compiler does
not support today. This doc captures what I found and three options for
unblocking it.

## TL;DR

The plan's goal -- *"SF type is Time -> Sample end-to-end: `(fn [t] ...)`
returns `:float`, not `:int`-bit-cast-of-`:float`"* -- is not implementable
without a Turmeric compiler change. Closures (`fn`) in Turmeric have a
hard-coded `int64_t`-only ABI. Until that changes, samples cannot flow
through `__arrow_call1` / SF→SF boundaries as native `:float`.

## What's currently in place

### Layer 1: closure ABI (compiler)

`src/compiler/emit_module.c:1988-2005` emits the fat-closure dispatch
macros. The signatures are hard-coded to `int64_t`:

```c
#define TUR_CLOSURE_FN(f)  ((int64_t *)(intptr_t)(f))[0]
#define TUR_APPLY1(f, a) \
    (((int64_t (*)(void *, int64_t))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f), (int64_t)(a)))
```

Every anonymous `(fn [...] ...)` form lowers to a thunk of this exact
shape. There is no codegen path that produces a closure returning
`double`. `defn`-level functions can declare `:float` return and emit a
C function with `double` return -- but `fn` closures cannot.

### Layer 2: arrow ABI (stdlib)

`stdlib/arrow.tur:84` defines:

```turmeric
(defn __arrow_call1 [f x] : int
  ```c return ((int64_t(*)(int64_t))(intptr_t)f)(x);
  ```)
```

Note: this casts to `(int64_t(*)(int64_t))`, not the fat-closure
`(int64_t(*)(void*, int64_t))` shape -- so SFs are being treated as
**thin** function pointers, not fat closures. This works today because
`__arrow_call1` is only ever fed names of top-level `defn`s (whose
codegen produces a plain C function), not anonymous `fn` closures.

That subtlety matters: a "Phase 0" that touches `__arrow_call1` to use
`double` does **not** automatically need a closure-ABI change; it only
needs the arrow consumers to be `defn`s (which they already are after
codegen specialises them). But the SF *bodies* are `(fn [t] ...)` --
those are closures, and the moment you return one from a `defn` and
then call it through `__arrow_call1`, you're calling a fat closure
through a thin function-pointer cast.

In other words: the current code is already cheating, and any change
that exposes the closure return type to the compiler will get the
hard-coded `int64_t` from layer 1.

### Layer 3: signal SF idiom (signal spice)

`signal/src/signal/dsp.tur:104-118` (`low-pass`) is representative:

```turmeric
(defn low-pass [alpha : float]
  (let [av alpha]
    (fn [sig]
      (let [sv    sig
            state (__dsp_alloc_state)]
        (fn [t]
          ```c
          double *prev = (double *)(intptr_t)state;
          int64_t sig_val = ((int64_t(*)(int64_t))(intptr_t)sv)(t);
          double x; memcpy(&x, &sig_val, 8);
          double y = av * x + (1.0 - av) * (*prev);
          *prev = y;
          int64_t ret; memcpy(&ret, &y, 8);
          return ret;
          ```)))))
```

Every SF body:
1. Receives `t` as `int64_t` bit-pattern of a `double`.
2. Calls the upstream SF through a `(int64_t(*)(int64_t))` cast.
3. `memcpy`s the returned `int64_t` to a `double` to compute on it.
4. `memcpy`s the result back to `int64_t` to return.

The plan's grep-gates require steps 3 and 4 to disappear. They cannot
disappear while the closure ABI returns `int64_t`.

## Why the existing `:float`-returning helpers don't help

`__dsp_sin`, `__sample-add`, `__sample-mul`, `__sample-clip`,
`__adsr_fixed_sample` are top-level `defn`s with `:float` return. They
emit C functions with `double` return and work fine. The cast at the
call sites (`(:: ... :float)` / `(:: ... :int)`) is the Turmeric type
system telling itself "trust me, this `int64_t` is really a `double`
bit-pattern" -- it is *not* a runtime conversion.

So today's code already mixes:
- Real `double` arithmetic inside `defn` bodies that take/return `:float`.
- `int64_t`-bit-pattern smuggling at every closure boundary.

The plan is asking us to make the closure boundary speak `double`. That
is a compiler change.

## Cross-spice blast radius

`__arrow_call1` lives in **this repo** at `stdlib/arrow.tur`, not in a
spice. Consumers:

```
stdlib/arrow.tur                               (definer + intra-stdlib users)
../turmeric-spices/spices/signal/src/...       (target of this plan)
../turmeric-spices/spices/signal/tests/...     (target of this plan)
```

(Plus vendored copies under `notebook/` and `plot/` workspaces; those
follow whatever the source `signal/` does.)

So `__arrow_call1`'s ABI is effectively owned by the signal spice -- no
other production caller depends on it returning `int64_t` specifically.

The seven `(int64_t (*)(void *, ...))` cast sites in
`src/compiler/emit_module.c` are the harder coupling: those are how
every closure in the language is invoked.

## Three options

### Option A -- compiler change: closures can return `:float`

Teach `emit_module.c` to emit a second flavour of `TUR_APPLYn` (or
generalise the existing one) when the closure's declared return type
is `:float`. Codegen for `(fn [...] :float ...)` then emits a thunk
with `double` return.

Then `__arrow_call1` gets a `:float`-returning sibling (or its own
return type changes), and the signal SF bodies can finally return
`double` directly.

**Cost**: real compiler work. Need to:

1. Plumb the closure return type through the closure-allocation site
   so the runtime knows which kind of thunk it is. Either tag the
   closure box, or have two separate APPLY macros and let the type
   system pick at the call site. The type-system pick is much cleaner
   if Turmeric's monomorphisation already specialises by return type.
2. Update `arrow.tur` to provide both `__arrow_call1_i` and
   `__arrow_call1_f` (or make `__arrow_call1` itself polymorphic on
   return type). Existing callers in `stdlib/arrow.tur` (`>>>`,
   `__arrow_pair_*`) must pick the right one.
3. Update `signal/core.tur:__signal_call1` to the `:float` variant.
4. Rewrite all dsp/synth SF bodies to use `:float` end-to-end.

**Value**: unblocks the entire plan as written. New primitives ship
clean.

**Risk**: codegen change. Needs fixtures regenerated (per CLAUDE.md
strict rule). Closure ABI changes can show up in subtle ways through
the arrow/tuple/free-monad/etc. stdlib code.

### Option B -- redefine "Phase 0" to mean "samples are conceptually `:float`"

Drop the grep-gate ("no `memcpy` in DSP code") from Phase 0. Replace
with a softer goal:
- `:float` is the *declared* sample type everywhere -- the `(:: ... :float)` /
  `(:: ... :int)` casts mean "interpret these 8 bytes as a double" and
  are tolerated as a known idiom because they reflect a compiler
  limitation, not a code-quality problem.
- The `int64 sig_val; memcpy` dance is replaced with a single inline-C
  helper macro (e.g. `SIG_CALL(sv, t)` -> `double`) defined once at
  the top of `dsp.tur`, used everywhere. Same bit pattern, much less
  boilerplate.
- New Phase 1+ primitives use the macro from day one, so no new
  `memcpy` ever appears in DSP bodies.

**Cost**: a few hours to define the macro and refactor existing SFs to
use it. No compiler changes.

**Value**: removes Phase 0 as a blocker. New primitives can land with
clean-looking bodies. The "samples are `:float`" docstring at
`synth.tur:7` becomes mostly true (declared type) without being
literally true (closure ABI).

**Risk**: leaves a known cheat in place. The plan's stated motivation
("get cleanup right *before* adding 12x more code") is partially
sacrificed. But Option A is also unblocked later -- the macro
abstracts the boundary, so swapping it for a real `:float` ABI later
is a one-file change.

### Option C -- ignore Phase 0, build Phase 1 + 1.5 in the existing idiom

Land `scale`, `triangle`, `offset`, `invert`, `abs-sf`, `multiply`,
`clip`, `noise-burst` using the same `memcpy`-and-int64-bit-pattern
pattern that `gain`/`mix`/`add` already use. Phase 0 stays an open TODO.

**Cost**: lowest.

**Value**: gets the user-asked-for primitives shipped.

**Risk**: when Phase 0 lands later (via Option A or B), every primitive
we add now has to be touched again. We multiply the eventual
migration cost by however many primitives we add in the meantime.
The plan explicitly tries to avoid this trap (line 50: *"finish it
before adding any new inline-C, otherwise every primitive in this
plan inherits the bit-cast idiom and the cleanup gets twelve times
harder"*).

## Recommendation

**Option B**. Concretely:

1. Update the plan doc to acknowledge the closure-ABI constraint and
   redefine Phase 0's exit criteria. The grep-gate becomes:
   `grep -nE 'int64_t sig_val|memcpy\(.*&.*sig' src/signal/` empty
   (i.e. no *ad-hoc* bit-cast dances; the centralised macro is fine).
2. Add a `__sig_call_f` macro / helper at the top of `dsp.tur`:

   ```turmeric
   ;; Calls an SF at time t (both encoded as int64 bit-patterns of
   ;; doubles) and returns the sample as a real double for arithmetic.
   ;; This is the *only* sanctioned bit-cast in DSP bodies; closure ABI
   ;; limitations make it unavoidable until tur supports :float-returning
   ;; closures. See docs/upcoming/signal-phase-0-spike.md.
   ```

   The actual helper can be an inline-C `defn __sig_call [sv :int t :int] :float`
   that takes/returns `:float` and does the bit-cast once inside.

3. Refactor existing SFs to use it. Verify tests still pass.
4. Then proceed with Phase 1 + 1.5 primitives, all of them using
   `__sig_call` instead of inline `memcpy`.
5. File Option A as a separate compiler-side plan
   (`closure-float-return-abi-plan.md`); it can land independently
   later and the signal spice gets cleaner for free when it does.

## What I did NOT find (and is therefore not a blocker)

- I did not find any cross-spice consumer of `__arrow_call1` outside
  signal+vendored copies. So the arrow ABI is effectively the signal
  spice's to redefine, once the compiler allows it.
- I did not find missing-build-machinery issues for spice-internal
  `.c` files (relevant to the later Phase 14 / KissFFT discussion --
  see `spices-c-sources-plan` when it lands).

## Files inspected

- `stdlib/arrow.tur` (this repo)
- `src/compiler/emit_module.c:1980-2005` (this repo)
- `../turmeric-spices/spices/signal/src/signal/{core,dsp,envelope,synth}.tur`
- `../turmeric-spices/spices/signal/examples/03_dsp.tur`
- `../turmeric-spices/spices/signal/tests/signal/arrow_tests.tur`

## Next step

Confirm Option B (or pick another). If Option B, the next deliverable
is an edit to `signal-primitives-expansion-plan.md` updating Phase 0's
exit criteria and Open Questions section.

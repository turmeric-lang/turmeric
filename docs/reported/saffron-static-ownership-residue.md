# Saffron without its collector: the `any` boxes no static owner reaches

**Severity: low, by design.** Only a `#lang saffron` program built with the
collector OFF (`TUR_SAFFRON_GC=0` / `--no-saffron-gc`, or a `--shared` build)
leaks these; a default build reclaims them (the collector is the allocator of
a compiled single-unit Saffron program since 2026-09-28). This report is the
residue three earlier ones left behind when they were closed by that change,
and it is what `tests/run-leak-check.sh` still measures -- that gate builds
Saffron fixtures with the collector off, because the collector's heap is
invisible to LeakSanitizer.

## The shapes

All three are "a box whose every holder is reached through an opaque dynamic
call", which no AST walk can settle:

1. **Argument- and return-position `any` widens around a recursive walker**
   (`tests/fixtures/saffron-higher-order`, `known-leak`: 560 bytes in 14
   allocations since 2026-10-21, when a dynamic call stopped disqualifying the
   parameters it never sees -- 880 in 22 before; see
   [any-widen-deep-drop-frees-borrowed-children](any-widen-deep-drop-frees-borrowed-children.md)).
   A by-value payload widened into an `any` argument is a box
   the callee reads but may pass to `(f h)`; a result widened on the way out
   carries values `(f h)` produced. Attribution and the static fixes that
   landed first are in
   [any-widen-stored-in-an-adt-field-has-no-owner](../archive/any-widen-stored-in-an-adt-field-has-no-owner.md).
2. **A capturing closure returned as `any`** -- its env. See
   [dynamic-returned-closure-env-is-never-freed](../archive/dynamic-returned-closure-env-is-never-freed.md).
   **Narrowed 2026-09-28:** a `let` that holds the fresh closure now drops
   it statically (at scope exit, or at the DK entry boundary in a CPS body),
   self-application included, so `tests/fixtures/tailcall-dyn-leak` runs
   clean with the collector off and lost its `known-leak` marker. What stays
   here is a closure whose single owner the compiler cannot prove: stored,
   returned onward, or handed to a callee that may keep it.
3. **A capturing closure built in a CPS-lowered function and passed to a
   dynamic call** -- its env, one per call. See
   [cps-capturing-closure-env-leaks-through-dyn-call](../archive/cps-capturing-closure-env-leaks-through-dyn-call.md).
   **Narrowed 2026-09-28:** when the callee's `any` parameter is inferred
   non-retaining (it only invokes the value), the lambda is hoisted into a
   `let` and released the same way
   (`tests/fixtures/saffron-lambda-arg-env-freed`, leak-checked). A callee
   that may keep it still leaves the env here.

## Re-examined 2026-09-28: the collector cannot cover `--shared`

The one build that meets this residue without asking for it is `--shared`, so
the natural question is whether the collector can go there too.  It cannot, and
not for the reason `r7rs_gc_active` records ("each unit its own heap") -- that
part is fixable by compiling `r7gc.c` once into the library.  The blocker is
root discovery: the collector finds live objects by scanning the EXECUTABLE's
data and bss (`__data_start` .. `_end`) and each registered thread's stack.  A
shared library's own data segment is outside that range, and worse, the HOST
that loads it keeps Turmeric values -- the handles an exported function returns
-- in memory the collector never scans (the host's heap, a Python object, a
Godot variant).  A conservative collector there frees objects the host still
holds.  Making `--shared` collect would need an explicit root API
(register/unregister a handle) at the FFI boundary, which is a different
design, not a switch.

The shapes were also rechecked against the shared-copies ownership work
(byvalue-recursive-shared-copies-leak): none is a by-value spine or an rc-field
struct, so neither the result-alias walk nor the lent-temporary free reaches
them.  What remains of each after the same-day narrowing above is a box whose
every holder sits behind an opaque dynamic call.

## Why it stays open rather than being fixed

The two static answers each report ended on -- a move discipline for `any`
arguments, or a reference count on every `any` box -- are either not
decidable across an opaque call or cost a count on every widen and copy. The
collector settles all three without either. Close this only if a no-collector
Saffron build becomes a supported target in its own right; until then the
`known-leak` markers are what keep the static drops in those fixtures honest
(a double free or use-after-free there still fails the gate).

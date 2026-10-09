# Carrier-riding sum Option/Result boxes have no automatic owner

**Severity: low-medium -- downgraded from medium 2026-10-08** (see the
re-measurement at the end). What is left is 408 B across 9 fixtures, from three
consumers the compiler cannot see through. Only one of them grows with a
running program: a user-written HKT-constrained generic (`[^Monad m ...]`)
called from a concrete caller, at ~40-72 B a call. Nothing in `stdlib/` is
such a generic (no `^Monad` / `^Applicative` / `^Functor` constraint on any
stdlib defn); 40 fixtures are. The other two are code that erased the box
itself (`:int` inline-C readers, a `ptr<void>` closure), which owns it by
CLAUDE.md's own rule.

**Original severity:** medium (memory growth in long-lived carrier-path programs).
Filed 2026-08-27 during SR2b.

**Narrowed 2026-08-27 (SR3 slice A):** `(none)` no longer allocates -- the
carrier None is the null pointer (`adt_ctor_is_null_none`, types.c).  The
report now covers `(some x)` / `(ok x)` / `(err e)` boxes only.

**Narrowed again 2026-08-27 (SR2a graduation), and this is most of it.** A
CONCRETE `(Option T)` / `(Result T E)` monomorph now flows by value, so its
constructor is a struct literal and there is no box to own.  What remains is
the ERASED path only: a generic base (`some`, `ok`, an instance method's
carrier base) whose element is still a type variable mallocs the tagged layout
and hands back the pointer.  `arc-weak-upgrade`, the repro below, is by-value
now -- its explicit `option-free` calls had to be REMOVED, because by value
they free a stack slot.  The residue shrinks further with each site that
monomorphizes; end-to-end monomorphization is where it reaches zero.

**Narrowed a third time 2026-08-30 (RM1), and this is most of what was
left.**  The erased residue now HAS an owner for the audited consumer set:
`returns_fresh_sum_box` (a per-callee freshness analysis -- every value path
mints a fresh box or NULL) plus two drop mechanisms (free-after-accessor-call
and free-at-scope-exit) close the `(ok? (ok 1))` / instance-body shapes, which
the corpus sweep showed were the bulk: 8324 -> 7364 bytes across every
erased-base caller in the tree, with the `hkt-stdlib-*` fixtures leaving the
leak list entirely.  What remains open is exactly the unstampable residue: a
box handed to a consumer OUTSIDE the audited read-only allowlist (user-defined
readers, dictionary-dispatched `bind`/`fmap` chains -- a user instance may
retain its argument, so those can never be stamped by name).  That residue
reaches zero where this report always said it would: end-to-end
monomorphization.  Mechanism and measurements:
[reclamation-plan.md](../archive/reclamation-plan.md), RM1.

## Summary

SR2b made stdlib Option/Result real sums.  On the default path a `(some x)` /
`(ok x)` / `(none)` construction mallocs a tagged monomorph box and hands back
the pointer as the int64 carrier -- and nothing frees it unless the caller
calls `option-free` / `result-free` by hand.  Before SR2b `(Option A)` /
`(Result A B)` over word-sized elements were BY-VALUE record products: no
allocation, so nothing needed an owner, and almost no caller frees today.

## Repro

`tests/fixtures/arc-weak-upgrade` (links ASan-built libturi, so LeakSanitizer
runs on the fixture binary): before the fixture added explicit
`(option-free (:: u :int))` calls, each `arc-upgrade` leaked its 16-byte
Option box.  Any fixture that links `-lturi` and constructs Options in a loop
shows the same growth.

## Root cause

`ctor_Some__*` / `tur_box_some` / `none()` malloc the tagged layout
(emit_module.c preamble, types.c monomorph ctor emission); the elaborator's
release machinery does not track sum ctor boxes the way it tracked the
(non-allocating) record path, so the box escapes with no release point.

## Fix directions

The second of the two original directions -- move the default path to by-value
flow so the box never exists -- is DONE for concrete monomorphs (the SR2a
graduation, 2026-08-27).  For the erased residue the options are unchanged:
teach the release pass to treat a carrier sum box like other compiler-owned
temporaries (free at the end of the binding scope unless it escapes), or
monomorphize the site so it stops being erased.  The second is where the track
is already heading.

## Workaround

Callers that care (long-lived processes, leak-checked binaries) free the box
explicitly: `(option-free (:: o :int))` / `(result-free (:: r :int))` after
the payload has been read out.

## Narrowed again: bind chains (2026-09-02)

The erased residue's largest rows were `bind` / `fmap` chains over the stdlib
`Result` / `Option` instances. They are owned now, at statically resolved
dispatch sites only: instance methods carry the same inferred non-retaining
masks a defn does, freshness is tracked through a continuation parameter
(`fresh_sum_via_param_mask`), a fresh producer read back by value marks its
carrier owned for the bridge to free, and the closure-argument hoist reaches
dispatch calls. Corpus sweep 7200 -> 5643 B (with the SR4 flip and the comparator shim-box
fix); both `result-monad-*-bind-typed-boundary` fixtures, `result-typed-basic`
and `typed/result-basic` are fully clean. The residual attribution is in
[leak-sweep-decomposition.md](../artifacts/leak-sweep-decomposition.md): what
is left is fixture scaffolding, recursive spines, and dictionary-dispatch
sites. A dynamic dispatch (abstract receiver inside a constrained
generic) is freed only after the emitter re-resolves the instance per
monomorph -- the first round had read a representative instance's flag there,
which was unsound. Details in
[reclamation-plan.md](../archive/reclamation-plan.md), RM1.

## Re-measured 2026-09-05, and two rows re-attributed

The sweep reproduces byte for byte (1790 B, same per-fixture split), so the
figures above are stable.  Re-reading it moved two rows, both toward "smaller
than recorded":

- **`zipper-basic`'s 64 B was a test rig**, not the compiler's -- the fixture
  frees the zipper it started with and not the fresh one `zipper-move-right`
  hands back.  Fixed; the fixture now carries `requires.leak-check`.  That makes
  **two** of the sweep's fifteen rows fixtures leaking their own scaffolding,
  1240 B between them.  (`stdlib/zipper.tur`'s `zipper-free-raw` got the null
  guard it was missing in the same change: the API returns the null handle at
  the end of the tape and this fixture's own `unwrap-or ... (:: 0 (Zipper int))`
  idiom hands it straight back to `zipper-free`.)
- **The `__tur_aggrspill_*` rows are this report's erased path**, not a separate
  "poly aggregate-spill" category.  Verified rather than inferred: write the same
  `bind` with its dispatch statically resolved and *no shim is emitted at all* --
  the instance specializes to a by-value return and the closure needs no box.
  The 16-48 B boxes appear only when the dispatch goes through the dictionary,
  which is exactly where this report has always said the residue lives, and they
  go to zero at monomorphization for the same reason everything else here does.

Why the existing freshness machinery does not reach those boxes -- it is one
specific gap, not a general limit -- is written up in
[rm1-leak-sweep-decomposed-2026-09-04.md](../artifacts/rm1-leak-sweep-decomposed-2026-09-04.md#the-__tur_aggrspill_-rows-are-the-erased-dict-path-not-a-category):
freshness is a per-monomorph property recorded once per binding, stamped on the
generic body where the dispatch is not static.  Closing it costs an
all-instances-agree stamp or a per-spec pre-pass, with double-free as the
failure mode, for ~48 B that monomorphization deletes outright.  Deliberately
not taken up.

### A third row was a real compiler leak, and is fixed

Chasing the same sweep found one entry that was neither scaffolding nor erased
residue: the **niche** arm of `emit_carrier_bridge` consumed an inline-C
producer's carrier box without freeing it, while the by-value readback beside it
and the CE_WORD Vec-slot store above it both did. 16 bytes per call, on the arm
every inline-C `: (Option T-shaped-as-a-pointer)` producer reaches. Invisible to
`bash tests/run.sh`, which compiles fixtures without sanitizers.

Fixed, with `tests/fixtures/option-niche-inline-c-box-freed` (leak-checked, both
readback positions, both Some and None paths) pinning it. Write-up appended to
[inline-c-option-carrier-box-leaks.md](../archive/inline-c-option-carrier-box-leaks.md).
`option-niche-crossings` 183 -> 151 B, `option-niche-string` 38 -> 22 B.

**The largest remaining category is not this report's.** Of the 1678 B left,
~990 is the per-node recursive spine (`ctor_Cons_Cons__*`, `stdlib/re.tur`'s
`RxCons` cells), which is RM2 -- and RM2's own assessment is that it gets
unblocked by RM3 (regions), not by a better analysis.  RM1's own residue is
~240 B.

## Re-measured 2026-09-19, and one row added on purpose

The erased-base sweep re-run against the current compiler over the same
fixture list (`docs/artifacts/rm1-leak-sweep-after.txt`'s rows, built with
`-fsanitize=address`, `detect_leaks=1`, attributed by first non-allocator
frame):

| | bytes |
|---|---:|
| last recorded (2026-09-05) | 1678 |
| today | 1630 |

The split is the one already recorded: `re-string` 516 and
`constrained-defn-cons-return-monomorphize` 432 are the recursive spine
(RM2); `option-niche-crossings` 151 / `option-niche-string` 22 /
`httpd-req-string-opt` 109 are `tur_string_from_bytes` payloads; `zipper-basic`
is 0 (its rig leak was fixed); `coerce-carrier-to-struct` no longer exists.
RM1's own rows (`some` / `ok` / `err` / `ctor_Option_Some` / `tur_box_err`
under `hkt-*`, `conv-defstruct-option-hkt-instance-bodies`,
`option-map-capturing-closure`) sum to ~320 B including the ~48 B of
`__tur_aggrspill_*`, unchanged. Nothing has regressed and nothing has moved;
the attribution above stands.

**One row is new, and deliberate.** Closing
[colored-generic-tyvar-elemented-param-sig-rejects](../archive/colored-generic-tyvar-elemented-param-sig-rejects.md)
made `(peek (ok 1))` -- an erased `(ok 1)` box handed to a COLORED generic --
compile and run. Its 16 bytes are not freed: `elab_call.c` stamps a fresh sum
argument for the drop-after free only when the callee's effect row is EMPTY,
because a callee that suspends may capture its continuation and resume after
the caller has freed the box (a use-after-free, the one failure mode this
whole report refuses). The CPS deferred-drop table's cps->cps tail arm would
be the right place to fire it -- `f__cps` runs the callee and the rest of the
computation before returning -- but only when the continuation is provably
one-shot and non-escaping, which elab does not know at the stamp. So the box
joins the "consumer outside the audited set" residue this report already
owns. `tests/fixtures/colored-generic-erased-carrier-param` carries
`requires.leak-check` plus a `known-leak` marker naming this report, so the
gate turns red the day the box is freed and the marker must go.

## Re-measured 2026-09-28, attributed row by row, and two directions tried

**Still open.** Executed as a report on 2026-09-28. Nothing here could be
closed soundly short of the monomorphization this report has always pointed
to. What follows is the measurement, where each byte comes from, and why the
two cheap-looking fixes are not taken.

The sweep over `docs/artifacts/rm1-erased-base-callers.txt` reads
**1766 B**. Each fixture was built with `-fsanitize=address` and run with
`detect_leaks=1`, with each leak attributed by its first non-allocator frame.
The total is byte-identical on the pre-session compiler (`bf31e725`,
v0.56.2) and after that day's changes, so none of them moved a row. It is
higher than 2026-09-19's 1630 mostly because two rows live under
subdirectories and were read as absent or zero there:
`typed/zipper-basic` (64 B) and `typed-slots/coerce-carrier-to-struct`
(32 B). The rest is small drift in the `hkt-*` rows before 2026-09-28.

| Category | Rows | Bytes |
|---|---|---:|
| **This report: a fixture's own `:int` inline-C reader or producer** | `hkt-stdlib-result-ok-biased` 64, `conv-defstruct-option-hkt-instance-bodies` 40, `hkt-stdlib-option-result-instances` 40, `typed-slots/coerce-carrier-to-struct` 32 | 176 |
| **This report: dictionary dispatch inside a constrained generic** | `hkt-constrained-byvalue-bind-pure` 72, `hkt-constrained-hole-headed-instance-head` 48, `hkt-constrained-spec-reresolves-instance` 48, `hkt-constrained-byvalue-carrier` 32 | 200 |
| **This report: payload type erased through a `ptr<void>` closure** | `option-map-capturing-closure` | 16 |
| Recursive spine (RM2) | `re-string` 516, `constrained-defn-cons-return-monomorphize` 432, `refined-nonempty` 80 | 1028 |
| `tur_string_from_bytes` payloads | `option-niche-crossings` 151, `httpd-req-string-opt` 109, `option-niche-string` 22 | 282 |
| Test rig | `typed/zipper-basic` (the twin of the `zipper-basic` rig leak fixed 2026-09-05) | 64 |

So this report's residue is **392 B**, plus the deliberate 16 B of
`colored-generic-erased-carrier-param` (outside the sweep, `known-leak`
marked). Each category is a consumer the compiler cannot see through:

- **`:int` readers.** `(defn res-ok [r : int] : int ```c ... ```)` takes the
  box as a machine word, and inline-C bodies are never inferred
  non-retaining (`elab_infer_nonretain_masks`). The
  `conv-defstruct`/`hkt-stdlib-option` rows also leak the 24 B env of a
  capturing lambda stored in `(some ...)`, which nothing drops.
- **Dictionary dispatch.** A monomorph that knows its instance still
  dispatches through the dictionary pointer.
  `bind_then_pure__dict_19__spec_..._tur_adt_Option__int` spills its by-value
  `x` into a malloc'd carrier (16 B) and builds the lambda's env (24 B). It
  then calls `bind` as `((void **)__dict_20)[0](...)`, and the result box
  that `pure` mints (16 B) is read back by value at the caller. None of the
  three can be freed without knowing which instance ran, and a user instance
  may retain. Devirtualizing the call per monomorph is the fix, and it is
  the monomorphization this report names.
- **`option-map-capturing-closure`.**
  `(unwrap-or-carrier (option-map (some 5) scale4) 0)` with
  `scale4 : ptr<void>` leaves option-map's `B` unresolved, so the call's type
  is `(Option ?)`.

**Tried, not taken: 1. reap a fresh box handed to a colored callee at the DK
entry boundary.** This would close `colored-generic-erased-carrier-param`.
The boundary itself is sound: the runtime already frees the continuation
frames that hold the box there (`__pfe0` in `peek__..._cps`), and a
continuation is dead once the outermost entry settles (the B7 stored-chain
comment in `emit_cps_ir.c`). The blocker is upstream of it. The callee's
`(perform (Tick))` is an escape to `binding_escapes_impl_x`, so
`box_uses_confined` never sets `peek`'s `nonretain_sum_param_mask` bit, and
the stamp has nothing to key on. Setting that bit would not stay local: the
same mask admits scope-exit frees in every caller that hands `peek` a
let-bound box. A multi-shot handler that re-enters the callee's
continuation would run such a caller's trailing free twice. Doing this
safely needs a second mask meaning "non-retaining except through a
continuation", read only by a boundary-reap stamp. That is a lot of new
machinery for 16 bytes, and a double free is the failure mode.

**Tried, not taken: 2. admit `unwrap-or-carrier` as an audited reader.** It
reads arg 0 and returns the payload word or its default, never the box. But
for a boxed value-struct payload, that word points into the arm box that the
deep drain (`emit_carrier_sum_free`) frees, and inside a generic body the
drain resolves a type variable to such a payload. So it is sound only over a
statically word-scalar payload. The one row it could reach has an unresolved
payload type (above), so the guarded rule measured zero, and it was
reverted.

Also found while re-measuring, and filed separately: rewriting
`hkt-stdlib-result-ok-biased` with typed readers segfaults on an ascribed
`catch-error`
([catch-error-ascribed-result-types-handler-by-value](../archive/catch-error-ascribed-result-types-handler-by-value.md)).
That bug predates this work.

## Re-measured 2026-10-08 -- unchanged, and the one growing row scoped

Same sweep, same procedure (`docs/artifacts/rm1-erased-base-callers.txt`,
built with `TUR_RUNTIME=source` and `-fsanitize=address`, run with
`detect_leaks=1`, attributed by first non-allocator frame): **1857 B**, against
2026-09-28's 1766.

| Category | Rows | Bytes | vs 2026-09-28 |
|---|---|---:|---|
| **This report: a fixture's own `:int` inline-C reader or producer** | `hkt-stdlib-result-ok-biased` 64, `conv-defstruct-option-hkt-instance-bodies` 40, `hkt-stdlib-option-result-instances` 40, `typed-slots/coerce-carrier-to-struct` 32 | 176 | same |
| **This report: dictionary dispatch inside a constrained generic** | `hkt-constrained-byvalue-bind-pure` 72, `hkt-constrained-hole-headed-instance-head` 64, `hkt-constrained-spec-reresolves-instance` 48, `hkt-constrained-byvalue-carrier` 32 | 216 | +16 (one more dict-clone spill in `hole-headed`) |
| **This report: payload type erased through a `ptr<void>` closure** | `option-map-capturing-closure` | 16 | same |
| Recursive spine (RM2) | `re-string` 516, `constrained-defn-cons-return-monomorphize` 432, `refined-nonempty` 80 | 1028 | same |
| String payloads + rig | `option-niche-crossings` 226, `httpd-req-string-opt` 109, `option-niche-string` 22 | 357 | +75 |
| Test rig | `typed/zipper-basic` | 64 | same |

The `option-niche-crossings` growth is not this report's: 112 B of it is the
fixture's two let-bound `Vec`s (`probe-vec`, `probe-push`), which it never
`vec-free`s. Containers are not freed at scope exit, by design
(memory-usage-guide). The `fn-value-call-cps-frames-held-until-outer-entry`
fix of the same day frees DK join frames only and moves no row here.

So this report's residue is **408 B** (392 + the extra `hole-headed`
spill), plus the deliberate 16 B of `colored-generic-erased-carrier-param`,
which `run-leak-check.sh` still reports as KNOWN against this file.

### The dictionary-dispatch row: what a fix needs, measured against the C

The rows come from Route B (`elab_call.c`, "constrained-hkt-lifted-lambda-
keeps-representative-instance"). A direct call to an HKT-constrained generic at
a concrete type constructor goes through a DICT CLONE that loads every method
from a dictionary parameter. That is correct by construction, and it is
exactly what hides the instance from the ownership analyses. In
`bind_then_pure__dict_19__spec_..._Option__int` the clone allocates three
things it cannot release:

- a spill box for the by-value `x` handed to `bind` as a carrier;
- the env of the `(fn [v] (pure (+ v 1)))` closure handed to `bind`;
- and `bind`'s result carrier, which the caller's ascription bridge reads back
  by value.

**One fact makes a fix tractable that the 2026-09-28 note did not record:
the clones are per CALL SITE, not shared.** Two calls with identical
instances get `__dict_19` and `__dict_30`. At its Route B site each clone
is called with exactly the instances in `insts[]`, so those could be recorded
on the clone binding ("pinned"). Its dict-slot calls could then be resolved to
the instance method at emit, the way `emit_reresolve_method_fndef` already
resolves a monomorph's dispatch for `sum_box_drop_after_dyn`.

Pinning alone does NOT make the frees sound, which is why it was not landed
here:

- The closure env can go once the pinned `bind`'s `nonretain_param_mask`
  covers its fn parameter. Option's does (it only calls `f`); a State or Free
  instance's `bind` returns a closure capturing `f`, so the mask is the only
  safe key.
- The spill box cannot simply be freed after the call either. A method may
  hand its argument straight back as its result. Option's `bind` does not (it
  answers a fresh `(none)`), but Option's own `alt-or` returns `x`, and a
  user `bind` may return `ma`. So the free has to be keyed on the pinned
  method's `nonretain_sum_param_mask`. That mask is set only when the method's
  result is a non-pointer scalar, and a `bind`'s never is. The result's own
  free belongs to the caller's bridge, which needs the clone to export a
  freshness bit (`returns_fresh_sum_box`) it does not compute today.
- A clone made by the nested-mapper lowering (`make_dict_clone` from the
  `poly_wrap` path) takes AMBIENT dictionaries and must never be pinned.

That is three new pieces of ownership plumbing, with a double free as the
failure mode, for ~40-72 B a call on a path no stdlib function takes. It stays
where this report always put it: end-to-end monomorphization, which deletes
the boxes rather than owning them.

## Investigated further 2026-10-08 -- the sweep under-counted; the rest is attributed

**1. LeakSanitizer's default roots under-count this sweep.** Its default
scans every stack and register, and a stale copy of a pointer in a dead frame
keeps a leaked block "reachable". With `LSAN_OPTIONS=use_stacks=0:use_registers=0`
the same sweep reads **1984 B** against 1857:

| row | default | stack/register roots off |
| --- | ---: | ---: |
| `hkt-constrained-byvalue-bind-pure` | 72 | **112** (the second call's closure env and result box) |
| `re-string` | 516 | 548 |
| `httpd-req-string-opt` | 109 | 126 |
| `option-niche-crossings` | 226 | 245 |
| `option-niche-string` | 22 | 41 |

Every other row is unchanged. `tests/run-leak-check.sh` runs with stack and
register roots off from now on. Its 127 fixtures pass either way, so the gate
cost nothing to tighten. Earlier rows in this report were measured with the
default and may be low by the same mechanism.

**2. The two `:int`-reader rows with a 24 B closure env are erasure after all.**
A first pass on 2026-10-08 moved them to a new report. That was wrong:
`conv-defstruct-option-hkt-instance-bodies` and
`hkt-stdlib-option-result-instances` both build
`(:: (some (:: (fn [x : int] : int (+ x bump)) int)) (Option int))`. The
fixture erases the closure to `int` itself, to exercise the int carrier, so
its env is the fixture's to own, like the `:int` readers beside it. The same
investigation did find a real, separate gap: a closure held TYPED in a
by-value Option/Result local was never released. It is
[sum-closure-payload-never-dropped](sum-closure-payload-never-dropped.md),
now fixed for the shapes ordinary code uses, but it does not touch these rows.

So this report's own residue, measured with the stricter roots, is **448 B**:

| Category | Bytes |
| --- | ---: |
| a fixture's own `:int` inline-C reader / erasing ascription | 176 |
| dictionary dispatch inside a constrained generic | 256 |
| `ptr<void>`-erased closure (`option-map-capturing-closure`) | 16 |

plus `colored-generic-erased-carrier-param`'s known 16 B. Of these, only the
dictionary-dispatch row is a compiler omission on code that erased nothing.

**3. The dictionary-dispatch row, attributed per allocation** (bind-pure,
roots off). Each Route B clone call leaks:

- the spill box copying the by-value `x` for the dict-dispatched `bind`;
- the env of the `(fn [v] (pure ...))` continuation, when `bind` takes the
  `Some` path;
- `pure`'s `Some` box, which `main`'s bridge reads back by value.

The clones share one body (`make_dict_clone`, `cf->body = orig->body`, dict
param bindings memoized on the original), so pinning a clone to its instances
means either copying the body per clone or resolving each dict-slot call at
emit. Neither existing mask can then key a free: `bind` returns a pointer, so
its `nonretain_sum_param_mask` is never set, and its result is "fresh through
its continuation param", which the clone does not export. The design note
above stands. One more constraint is now on record: a sum-param mask says
nothing about closures inside the sum (see the split-out report), so it
cannot be reused for the continuation's env either.

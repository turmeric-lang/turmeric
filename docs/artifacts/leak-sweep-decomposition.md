---
title: What the corpus leak sweep actually contains
category: Artifact
description: The 8.3 KB the RM1 measurement surfaced is four unrelated classes, only one of which is RM1's. Two are product defects (one newly filed), one is RM2's, and the largest single entry is fixture scaffolding. Recorded so the headline number is not quoted as one problem.
---

# Corpus leak sweep -- decomposition

Swept 2026-08-30 (v0.42.2 + this branch) while looking into the leaks the
[RM1 measurement](../archive/reclamation-plan.md) surfaced. The headline
"8.3 KB of leaks in emitted programs" is **four unrelated classes**, and
reading it as one number overstates the product problem in one direction while
hiding a real default-path defect in the other.

| class | example | bytes | whose problem |
|---|---|---:|---|
| fixture scaffolding | `httpd-req-string-opt`'s `fake-conn-cookie` / `fake-conn-body` | ~3.5 K | **nobody's** -- the fixture's own `calloc`'d `HttpdConn` and `strdup`'d headers, never freed by the test |
| fixture-owned runtime values | `option-niche-crossings` (`vec_new`, `vec_push_ex`, `tur_string_from_bytes`) | ~0.4 K | **nobody's** -- Vecs and Strings the fixture allocates and never `vec-free`s |
| recursive sum spine | `re-string`'s `ctor_RxCons` / `ctor_REmpty` / `ctor_RxNil` | ~0.6 K | **RM2** (the recursive spine), not RM1 |
| **value-struct payload box** | `rational-*`'s `ok__spec__...` / `err__spec__...` | 0.46 K -> 0.13 K | **a defect**, filed: [value-struct-payload-sum-monomorph-box-has-no-owner](../archive/value-struct-payload-sum-monomorph-box-has-no-owner.md); a scope-exit drop covers the let-bound shape, an inferred non-retaining sum-param mask (now admitting `cstr` results) plus a statement-position drain cover the argument shape, and the carrier free is deep for a boxed struct arm (7 of 9 fixtures clean); the remaining 2 are dictionary-dispatched inline-C class methods, which need the glue route |
| erased sum carrier | `hkt-*`, `result-basic` | ~0.7 K remaining | **RM1**, partially closed 2026-08-30; bind chains closed 2026-09-02 (sweep 7200 -> 6856 B; 704 B of SUM-BOX rows left, dominated by inline-C `:int` readers in fixtures and dictionary-dispatched sites) |

## The two things worth carrying forward

**The largest entry is not a compiler problem.** `httpd-req-string-opt` leaks
3756 bytes, and 3528 of them are the fixture's own C scaffolding building fake
`HttpdConn` structures. Any future "the corpus leaks N bytes" claim should
subtract this class first; otherwise a fixture that allocates more test data
looks like a regression.

**The smallest entry is the most serious.** The value-struct payload box is on the
DEFAULT path with no experiment involved, and it falsifies a narrowing claim
that was being relied on -- SR2a's "a concrete monomorph flows by value, so
there is no box to own" is true only for payloads of 8 bytes or less. It is
also the most tractable of the real classes, because the constructor mallocs a
fresh copy and the resulting monomorph is its sole owner: no freshness
analysis needed, unlike RM1's residue.

## Method

`benchmarks/adt-alloc/rm1-erased-base-sweep.sh` finds erased-base callers;
the wide-payload population comes from a static scan for a `*__spec__*` sum
constructor whose body mallocs (11 fixtures), confirmed by building each under
`-fsanitize=address` and requiring a `*__spec__*` frame in the leak trace
(9 of the 11). Per-fixture byte counts are best-of-one runs and are stable,
but they are per-EXECUTION totals for these specific fixtures -- they measure
presence and shape, not severity in a real program, where every one of these
is per-construction and grows in a loop.

## Residual attribution, 2026-09-02 (after the RM1 rounds and the SR4 flip)

Every leak record in the 27-fixture erased sweep, bucketed by the allocation
site's shape rather than by fixture (`ctor_*` frames inlined at -O1 are
attributed by the function that inlined them). Total 5,643 B, from 8,324 B
when RM1 started.

| bucket | bytes | what it is | whose |
|---|---:|---|---|
| fixture scaffolding | 3,630 | `httpd-req-string-opt`'s `fake-conn-*` C helpers | nobody's |
| recursive spine | ~900 | `re-string`'s `RxPos`/`Regex` cells (406, inlined ctors), `constrained-defn-cons-return-monomorphize`'s `Cons` cells (432), `refined-nonempty`'s (64) | RM2, which RM0 closed: no constituency |
| runtime values | 415 | `tur_string_from_bytes`, `vec_new`/`vec_push` -- Strings and Vecs fixtures allocate and never free | fixture-owned |
| dictionary-dispatch sites | ~200 | closure envs and `Some`/`Err` boxes minted inside a constrained generic (`bind_then_pure__dict`, `poly-bind`) -- the callee is a dictionary slot, so no mask or freshness fact exists at the call site | by design (per-spec re-resolution recovers freshness; retention would need a per-instance emit-time mask) |
| `:int` stand-in readers | ~150 | `res-ok [r : int]`, `opt-val`, `unwrap-or-carrier`: inline-C fixture readers that take the carrier as `:int` | fixture scaffolding (the pattern CLAUDE.md forbids in new code) |
| zipper | 128 | `zipper-basic` never frees the zipper `zipper-move-right` returns | fixture-owned |

What was closed on the way here today, in order: value-struct payload boxes
(let-scope drop, inferred sum-param mask, statement-position drain, deep
carrier free), the reinterpret and closure-wrap gaps in the escape walk, bind
chains at static dispatch sites (instance masks, freshness through the
continuation, owned-carrier bridge), the `do-m` route, the inline-C predicate's
unmodeled kinds, the comparator shim boxes (`^borrow` on inline-C sinks, stack
boxes for proven non-retaining sinks, a fixed point for recursive walkers), and
the sum-param result gate widened to every copy-shaped result. There is no
compiler-owned residue left in this sweep that is not either a recursive spine
or a dictionary-dispatch site.

## Corpus-wide sweep, 2026-09-02

The 27-fixture list above was the erased-base callers only. Every fixture
(2,186 built; 7 did not build under the ASan flags) was then run under
AddressSanitizer with leak detection for the first time. Two findings that
are not leaks came first:

- **`hamt-lisp-eq` had a real use-after-free in the fixture itself**: it
  freed the value it had stored in the map, then compared it through the
  map. Fixed by reading before freeing. Pre-existing on `main`.
- **`van-laarhoven-lens-wide-functor-show` reads 8 bytes from a 1-byte
  box**: the aggregate return spill boxes a `(Identity bool)` monomorph at
  `sizeof` 1 and the erased generic reader dereferences it as the int64
  layout. Pre-existing on `main`, right answer by little-endian luck; filed as
  [erased-generic-field-read-overruns-subword-monomorph-box](../archive/erased-generic-field-read-overruns-subword-monomorph-box.md).

Then the leaks: 735 of 2,186 fixtures leaked something, 31.4 MB in total --
and 30.8 MB of that was ONE shape in three fixtures. The stackless
catch-unwind stress loops (`stackless-catch-unwind-panic-caught` and its two
siblings, 200k caught panics each) leaked the caught `Result` box plus its
panic payload and message, 53 B per iteration. The stackless emitter already
had a scope-exit free for a let-bound caught box, gated on the body being
straight-line, because the free named the resume segment's local temp;
naming the machine variable instead (which `gs_save`/`gs_restore` carry
across every descend) lets a body that self-calls qualify. All three are
fully clean and carry `requires.leak-check`.

The rest, by allocation-site shape (record counts; bytes are in the raw
sweep and are small everywhere but `logic-lazy-infinite`'s 219 KB stream
spine):

| shape | records | fixtures | what it is |
|---|---:|---:|---|
| user functions | 1,800 | 453 | dominated by `dk_new` / `seq-new` / `range-new` (generator and continuation frames the fixtures never close), `json/decode` nodes never `json/free`d, `mreturn` (logic stream spine) |
| runtime values | 580 | 137 | `hamt_malloc` (378: persistent maps and sets never freed), `vec-push!`, `tur_string_from_bytes` |
| ADT constructors | 472 | 141 | GADT `Bound`/`Size`/`SizedVec` cells (by design), `Cons`/`Stream` spines (RM2), `Point` products in containers |
| fixture `main` | 258 | 122 | allocations inlined into `main` at -O1, mostly the same runtime values |
| runtime other | 227 | 139 | `tur_cloneable_cont_alloc`/`clone` (multi-shot continuations, process-lifetime by design), `__tur_cons_of`, `tur_box_ok`/`err` from inline C |
| instance bodies | 124 | 90 | `Show`'s `show` returning a malloc'd cstr nobody frees (29), Arrow instances |
| closure envs / spills | 42 | 30 | dictionary-dispatch sites and aggregate spills |
| erased sum boxes | 2 | 2 | the two class-method fixtures from the value-struct payload report |

The compiler-owned classes visible here that no phase owns yet are the
persistent-collection and generator frames the fixtures allocate and never
release (a fixture-hygiene question as much as a compiler one: a HAMT has
`hamt/free`, a Seq has no free at all), and `Show` returning owned strings
through an interface that has no way to say so. Neither is a sum box; both
are recorded here so the next sweep does not rediscover them.

### After the two fixes (re-swept against the final compiler)

| | fixtures leaking | bytes |
|---|---:|---:|
| first sweep | 735 | 31,408,175 |
| after the stackless catch-box free and catch freshness | 731 | 608,023 |

No fixture got worse. Every catch fixture in the corpus (49) now runs under
ASan with no memory errors and no leaks. What remains, by shape and bytes:

| shape | bytes | what it is | whose |
|---|---:|---|---|
| logic stream spine | ~340 K | `logic-lazy-infinite` alone is 220 K: `StCons`/`StInc`/`StNil` cells, `mreturn`/`st-take`/`st-append`, and the lazy thunk envs | RM2: no constituency (RM0) |
| captured delimited continuations | ~120 K | `dk_new` frames and `tur_serial_cont_serialize` blobs held by `serial-shift`/`save-cont!` and the async fibers (`async-capturing-closure` alone 29 K with its futures); the capture APIs have no release call | the continuation runtime's ownership model, not a scope drop; recorded, not built |
| persistent collections | ~108 K | `hamt_malloc`: maps and sets fixtures build and never `hamt/free`; `set-cstr-content`, `data-literal-set-*`, `ghe3-generic-map-key` | fixture-owned (a persistent HAMT is freed explicitly by design) |
| GADT cells, products in containers | ~6 K | `Bound`/`Size`/`SizedVec` constructors | by design (tagged union kept) |
| `Show` strings, Arrow instances | ~5 K | `show` returns a malloc'd cstr through an interface that cannot say who frees it | API design |
| erased sum boxes | 16 B | the two class-method fixtures | glue route |

The sum-box and closure-env classes this branch set out to own are gone from
the corpus-wide picture; what is left is spine, capture, and collection
ownership, each of which is a design decision recorded elsewhere rather than
a missing drop.

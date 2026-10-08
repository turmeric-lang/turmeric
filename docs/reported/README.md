# Open findings -- triage index

Every file in this directory is an **open** finding. Resolved reports move to
`docs/archive/` (and their per-fix paper trail to `docs/archive/history/`) --
see the archiving rule in [CLAUDE.md](../../CLAUDE.md). `docs/reported/history/`
is forbidden and blocked by a `PreToolUse` hook.

This index exists so a triage pass reads one file instead of two dozen. Keep
it current when you file, absorb, or archive a report -- a row here is cheaper
than re-deriving the grouping.

**Last full verification sweep: 2026-08-01.** Every report below had its own
repro re-run against `main` and still reproduces, except where a row says
otherwise. That means a red result you hit today is almost certainly one of
these, not something new -- check here before opening an investigation.

**Two exceptions to that sweep, both added later the same day:** the four rows
under "Windows port" and "Platform-independent, found on a platform sweep"
arrived with `main`'s Windows work and were **not** re-verified here -- their
repros need an MSYS2/UCRT64 box. They are indexed on the authority of their own
filings. The `libedit` CI row *was* verified (three job runs across two heads
and `main`'s tip); it has since been fixed and archived, so it no longer appears
below.

**Four rows were added 2026-08-05** --
`manifest-read-failure-degrades-to-module-not-found`, `mono-specs-header-comment-stale`,
`turi-toplevel-expr-subforms-elaborate-in-global-scope`, and
`fixture-dirs-with-loose-tur-files-pass-without-running` (since resolved and
archived). They were not new filings: all four had been sitting in this
directory **unindexed**, found while archiving
`definstance-constraint-type-defaults-to-int`, which was unindexed too. Their
rows summarise their own filings and were **not** re-verified here, so the sweep
sentence above does not cover them. If you touch this file, check
`ls docs/reported/` against it -- an index that silently omits a quarter of the
directory is worse for triage than no index.

## Found auditing the spices plans (filed 2026-10-07)

Both rows were carried out of `docs/upcoming/spices/` when its resolved plans
were archived. The first was re-run on v0.63.5 and still reproduces; the second
is a `spices/watch` finding recorded as probably permanent.

| Report | Severity | One line |
| --- | --- | --- |
| [match-result-temp-typed-from-first-arm](match-result-temp-typed-from-first-arm.md) | low-medium (compile failure) | A `match` returning closures types its result temporary from the first arm: a non-capturing closure there gives `int64_t` while every arm builds a `void *` fat box, so `tur check` passes and AppleClang rejects the C (`-Wint-conversion`). Workaround: write a capturing arm first |
| [watch-darwin-fresh-name-is-create](watch-darwin-fresh-name-is-create.md) | low | `spices/watch` on Darwin reports a name that arrived by `rename(2)` as `create`, not `rename`, because the tree layer diffs directory snapshots; Linux reports `rename`. The event and path are correct on both |

## Found implementing the notebook docs-and-examples plan (filed 2026-10-05)

The plan ([notebook-docs-examples-plan](../upcoming/notebook-docs-examples-plan.md))
ships five example notebooks that import `plot`, `linalg`, `stats`, and `frame`
from cells. None of them can run: the notebook's embedded evaluator never sets
`module_base_dir`, so every `(import ...)` fails with a generic elaboration
error.

| Report | Severity | One line |
| --- | --- | --- |
| [notebook-eval-no-module-base-dir](notebook-eval-no-module-base-dir.md) | medium (feature gap) | `session-open` calls `turi_env_new()` but never `turi_env_set_module_base_dir`, so no cell can import an external spice; all five C1 example notebooks and the C2 guide examples fail at the first import |

## Found auditing r7rs compile- and run-time cost (filed 2026-10-05)

From [docs/notes/r7rs-performance-audit.md](../notes/r7rs-performance-audit.md),
which measures every `#lang r7rs` fixture on all three back ends. The cold
prelude-variant cost it also found is the existing
[r7rs-prelude-library-object-varies-with-the-program](r7rs-prelude-library-object-varies-with-the-program.md).

| Report | Severity | One line |
| --- | --- | --- |
| [r7rs-imported-libraries-recompiled-every-build](r7rs-imported-libraries-recompiled-every-build.md) | medium (build time) | An imported SRFI or `(scheme eval)` lands in the uncached program unit, not the cached library unit, so 200-375 library functions are compiled at `-O2` on every build: 2-4 s extra warm, and the 25 slowest r7rs builds. Direction 2 (imported libraries in the prelude unit) prototyped 2026-10-08 and not landed: ~2 s faster warm, but a cold prelude compile per import set (32 in the corpus, no CI cache); direction 1, a separate cached unit, is the one to build |
| ~~[turi-immutable-struct-args-copied-per-call](../archive/turi-immutable-struct-args-copied-per-call.md)~~ | medium (interpreter) | **RESOLVED 2026-10-07** (archived): the interpreter copies a by-value argument only if its type (or a by-value struct inside it) has a field write in the program (`AdtDef.field_written`, set at the one `EX_SET_FIELD` site); `any` boxes over symbols and the empty list are no longer copied. `r7rs-srfi-14` peak 132 -> 48 MB. Pinned by `tests/check-turi-frame-reclaim.py` |
| ~~[turi-effect-perform-keeps-its-continuation](../archive/turi-effect-perform-keeps-its-continuation.md)~~ | medium (interpreter) | **RESOLVED 2026-10-07** (archived): a clause that is just `(resume k v)` (inert `v`) resumes its slice in place, re-arms its own prompt and frees the capture: the repro's 200,000 performs peak at 31 MB, from 556 MB. Other clause shapes keep the cloning path. Pinned by `tests/check-turi-frame-reclaim.py` |
| [turi-call-pins-and-side-frames-not-reclaimed](turi-call-pins-and-side-frames-not-reclaimed.md) | low (interpreter) | **Narrowed 2026-10-07:** per-call `TyvarBind`/`DictBind` pins now go back with their frame (a generic call loop no longer grows) and `exists-open`'s frame is owned. Still kept: frames made off the driver's call path that a continuation or defer may hold -- fiber handler frames, `reset`/`shift`, the work-stack perform's handler frame, defer snapshots |
| [r7rs-gc-torture-quadratic-in-live-heap](r7rs-gc-torture-quadratic-in-live-heap.md) | medium (CI time) | `run-r7rs-gc.sh` collects every 31 allocations, re-marking a program's whole live heap each time: O(allocations x live), so one 300,000-closure fixture took 292 s (99 s after the 2026-10-05 mark-path work). An opt-in `TUR_GC_TORTURE_SCALE` (interval = live objects / R) takes it to 0.8 s; whether the gate should use it is open |
| [r7rs-gc-eval-programs-scan-libturi-data](r7rs-gc-eval-programs-scan-libturi-data.md) | low-medium | **Narrowed 2026-10-08:** the root scan tests each word against the heap bounds inline (read once per range) instead of calling the marker per word: `r7rs-eval` under torture went from 15.8 to 3.4 ms a collection (Linux, Debug). Still open: a `(scheme eval)` program scans libturi's 46 MB of data and bss, which holds no collector pointer |
| ~~[turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md)~~ | medium (interpreter) | **RESOLVED 2026-10-05** (archived): `--interpret` now hands an activation's call frame, its let / match-arm frames and their bindings back to a free list when nothing captured them, and compares frame names by pointer first; the four 0.3-0.5 GB r7rs fixtures (Release) dropped to 40-132 MB. Pinned by ctest `tur_turi_frame_reclaim` |

## Found reviewing #1082's deep-recursion fix (filed 2026-10-05)

Follow-ups to PR #1082, which runs a compiled `#lang r7rs` program on a 1 GiB
stack and prints a message when it overflows. The longer-term design, a stack
bounded by memory rather than by the C stack, is on hold in
[r7rs-heap-frames-plan](../upcoming/hold/r7rs-heap-frames-plan.md).

| Report | Severity | One line |
| --- | --- | --- |
| ~~[jit-stack-overflow-has-no-message](../archive/jit-stack-overflow-has-no-message.md)~~ | low-medium (diagnostics) | **RESOLVED 2026-10-07** (archived): the JIT's entry thread has an alternate stack and a SIGSEGV/SIGBUS handler that prints `stack overflow: recursion too deep` (one definition with the cc path, `src/runtime/stack_overflow.h`) and hands any other fault back to the previous handler. Pinned by ctest `tur_jit_stack_overflow_message`. Windows' JIT entry thread still has none |
| ~~[r7rs-deep-stack-size-not-configurable](../archive/r7rs-deep-stack-size-not-configurable.md)~~ | low | **RESOLVED 2026-10-07** (archived): `TUR_MAIN_STACK_MB=N` sizes a compiled r7rs `main`'s stack and the JIT's entry stack (`TUR_JIT_STACK_MB` kept as an alias); a bad or unreservable size is reported. Pinned by ctest `tur_r7rs_main_stack_size`, which also pins the overflow message at 16 MiB |
| ~~[no-public-dynamic-call-for-any-values](../archive/no-public-dynamic-call-for-any-values.md)~~ | low-medium (expressiveness) | **RESOLVED 2026-10-07** (archived): already closed by `868fea84` on `main` -- `(f x)` on an `any` is a dynamic call in every dialect, and its arity / not-a-procedure errors are guardable Scheme errors. Documented in the r7rs guide; the error side pinned by a `tests/run-r7rs-import.sh` case |

## Found fixing #1075's CI (filed 2026-10-04)

Three legs timed out building two new r7rs fixtures. They were not slow
fixtures: one of them sorts first and paid a cold prelude compile that the
suite's warm-up should have absorbed.

| Report | Severity | One line |
| --- | --- | --- |
| [r7rs-prelude-library-object-varies-with-the-program](r7rs-prelude-library-object-varies-with-the-program.md) | medium | **Narrowed 2026-10-07:** 10 library objects across the 129-fixture r7rs corpus -> 2 (one shared, one for `(scheme eval)`'s sanitized link). The "a lambda forks it" cause was the CPS coloring resolving `__cons-fmap`'s parameter `f` to a program's global `f` (the counter shifts followed from it), and the program's own hoisted inline-C `#define`/`#include`s reached the library unit; both fixed and pinned in `check-r7rs-prelude-split.sh`. The cache is now LRU-pruned (24 objects, a day's grace). Still open: `-I`/`-D` flags that reach no header fork the key. Was: The "program-independent" cached prelude object is not: fresh-name counters, whole-program CPS decisions and irrelevant `-I` flags reach its text, so 114 r7rs fixtures need 10 objects at 5-14 s each cold, a lambda alone forks a new one, `run.sh`'s warm-up warms the wrong variant, and the cache is never evicted. Importing a library is not the cause |

## Found writing the memory-usage guide (filed 2026-10-05)

Measured with valgrind (memcheck traffic, massif peak) on small probe
programs; every number is in its report. The arena slab-reuse bug found the
same day was fixed and archived.

| Report | Severity | Summary |
| --- | --- | --- |
| ~~[hamt-nodes-allocated-at-full-array-size](../archive/hamt-nodes-allocated-at-full-array-size.md)~~ | medium | **RESOLVED 2026-10-07** (archived): bitmap and collision nodes are allocated at the size of their own arm (`offsetof`), not a full 32-slot node, so a persistent `Map int int` entry fell from ~360 B to ~66 B of peak heap and insert traffic from ~1650 B to ~716 B |
| [fn-value-call-cps-frames-held-until-outer-entry](fn-value-call-cps-frames-held-until-outer-entry.md) | medium | Calling a function value goes through the `__cps` path and allocates ~200 B of continuation frames per call, reaped only when the outermost direct entry returns, so a tail-recursive loop grows for its whole run (`while` loops stay bounded). `#fx{}` does not help |
| ~~[closure-let-in-self-tail-loop-leaks](../archive/closure-let-in-self-tail-loop-leaks.md)~~ | low (leak) | **RESOLVED 2026-10-07** (archived): the CPS path reaps a closure returned by a `returns_fresh_closure` call as it does a literal, and a self tail call frees an only-invoked closure binder at the backedge (`__dk_reap_closure_now`), so the repro neither leaks nor grows. Pinned by leak-checked fixture `closure-let-in-self-tail-loop` |
| ~~[stdlib-list-null-check-retires-regions](../archive/stdlib-list-null-check-retires-regions.md)~~ | low | **RESOLVED 2026-10-07** (archived): an erasing ascription whose only use is `(= ... 0)`/`not=` against a literal `0` is no longer noted as an escape, so a `with-region` that walks a stdlib list with `tnil?`/`tlength` rewinds. `tcons` (carrier `:int` tail) still erases; build with `tcons-of` |
| ~~[region-rewind-reuses-only-head-slab](../archive/region-rewind-reuses-only-head-slab.md)~~ | medium | **RESOLVED 2026-10-05** (archived): a rewound region refilled only its head slab and malloc'd the rest again, so a region rewound in a loop grew without bound (9.9 MB at 50 rounds vs 267 KB). `Arena` now keeps emptied slabs on a spare list; pinned by `tests/region_unit.c` check (i) |

## Editor integration surfaces (filed 2026-10-03)

All resolved and archived. These are the six findings from bringing
[Trowel](https://github.com/rjungemann/trowel) onto `v0.60.1`: the LSP reader,
the two formatter sweet paths, the `--lang` vocabulary, `:run`/`:reload`
changing the session's reader, and the signature-help trigger. The two found
while resolving them are archived too: cross-file diagnostics published under
one URI, and sibling imports resolved from the LSP's scratch directory. See
`docs/archive/` under their slugs.

## CodeQL critical alerts (filed 2026-10-03)

All 21 open `critical` CodeQL alerts were checked against `main`
(`1a84092bb`). Neither row below is the defect CodeQL described. Both are real
bugs found on the lines it pointed at. Every other critical alert was a false
positive or was already mitigated (the WP3 `printf` format vetting, and the
documented WP2 D-2 decision to run recipe bodies through the shell). The
per-alert verdicts are in the PR that filed these rows.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[justrun-replace-empty-input-returns-unterminated-buffer](../archive/justrun-replace-empty-input-returns-unterminated-buffer.md)~~ | low | **RESOLVED 2026-10-04** (archived): the NUL is written before the fill loop; `tests/run-tur-run-rhs-eval.sh` pins it.  Original row: `replace("", a, b)` in a Justfile returns a 1-byte `malloc` that is never NUL-terminated, a heap over-read (seen as `0xAA` under `MallocScribble`). One-line fix |
| ~~[justrun-shebang-recipe-breaks-when-tmpdir-has-a-space](../archive/justrun-shebang-recipe-breaks-when-tmpdir-has-a-space.md)~~ | low | **RESOLVED 2026-10-04** (archived): the script is `fork`+`execv`'d with no shell; `tests/run-security-driver.sh` case 13 pins it.  Original row: Shebang recipes `system()` a `$TMPDIR`-derived path, so a space gives exit 127 and shell metacharacters are interpreted. Fix: `execv` the script with no shell in between |

## Security (filed 2026-09-30)

Findings from [security-audit-plan](../upcoming/security-audit-plan.md) that
are open in the tree. Section 2 of the plan is the full candidate list; a row
lands here only once a work package has reproduced it and chosen not to fix it
in the same change.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[httpd-residual-request-hardening](../archive/httpd-residual-request-hardening.md)~~ | low-medium | **RESOLVED 2026-10-03** (archived): every item is closed -- the last, item 9, makes `(httpd-set-bind-addr! addr)` name the interface (IPv4 or IPv6) through one listener helper.  Original row: WP4's httpd read-through.  **Narrowed 2026-10-01:** the quadratic header scan, the async writes that parked forever, silently-empty oversize request-line fields (now 400 / 414), the prefix-matched `Connection` header, the Basic-auth example that leaked username validity, and `mw-log`'s raw control bytes are fixed.  **Narrowed 2026-10-03:** multipart parsing is strict and NUL-safe -- a NUL in an uploaded file used to end every `strstr` and lose every part.  Left, each needing a default or an enhancement: no default in-flight cap on `httpd-new-async`, a rate limiter that hashes IPs to 32 bits and fails open when full, IPv4-only binding |
| ~~[lock-tracks-ref-not-resolved-commit](../archive/lock-tracks-ref-not-resolved-commit.md)~~ | medium | **RESOLVED 2026-10-01** (archived): a fetch checks out the lock's `:resolved` commit after the clone -- a bare-SHA fetch, else the branch's full history -- and never falls back to the branch tip; a commit that is gone is an error and no clone is kept.  `tur fetch --frozen` holds a fetch to the lock and never writes it.  Pinned by `tests/run-spice-fetch.sh` cases 11, 11b and 16-18 |

## Refinement contract positions disagree about `#reads` (filed 2026-10-02)

Found looking for an illustrating use case for the `loop-invariants`
experiment and hitting the rejection on the first one tried -- a bounded-index
walk whose invariant mentions the container's length.

| Report | Severity | One line |
| --- | --- | --- |

## Effect rows and capability tags (filed 2026-10-01)

Found investigating two security-guide questions -- whether `--no-proc-macros`
should default on, and whether `--strict-effects` should -- and scoping
`^capability` for `println`. The plan these feed is
[effect-row-honesty-plan](../upcoming/effect-row-honesty-plan.md), which also
answers [security-audit-plan](../upcoming/security-audit-plan.md) open
question 3 as Option A.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[capability-effect-tag-silently-resolves-to-empty-row](../archive/capability-effect-tag-silently-resolves-to-empty-row.md)~~ | medium | **RESOLVED 2026-10-02** (archived): an uppercase name in `#fx{...}` that no `defeffect` declares is now `TUR-E0026` -- naming the tag, a did-you-mean for a near miss, and `(load "stdlib/effects.tur")` for the names it declares -- in every row position.  No allowlist was needed: `Construct`/`ByVal` are plucked before the row is built and `NonExhaustive` never is one.  The check found four rows that had never resolved (a stdlib `#fx{FS}` on a stdout writer, three fixtures' `Write` and `#fx{|e}`), all fixed.  Direction 2 (compiler-known `IO` etc.) is the plan's W2.  Pinned by `tests/fixtures/errors/unknown-effect-in-row`, `unknown-capability-tag-pure-caller`, `loaded-capability-tag-pure-caller` |
| ~~[strict-effects-w0030-names-synthesized-lambdas](../archive/strict-effects-w0030-names-synthesized-lambdas.md)~~ | low (diagnostic quality) | **RESOLVED 2026-10-02** (archived): a synthesized binding carries a `diag_label` set where it is minted -- *anonymous function in 'dfs-or'*, *method 'eq?' of instance Eq [int]*, *default body of method 'greeting' in class Greet* -- and `binding_fn_describe` is how every effect diagnostic (`TUR-E0009`, `W0030`-`W0032`) names a function; corpus-wide no effect diagnostic prints a `__` gensym (a rank-2 `__poly_N` wrapper is skipped: the function it wraps answers for its row).  `TUR-W0030` also spells the row to add in `#fx{}` syntax.  Pinned by `tests/run-flags.sh` `strict-effects-lambda-name` |
| ~~[strict-effects-and-lint-effects-are-indistinguishable](../archive/strict-effects-and-lint-effects-are-indistinguishable.md)~~ | low (flag taxonomy) | **RESOLVED 2026-10-02** (archived): one flag with a real promotion path -- `-Werror=strict-effects` makes `TUR-W0030`/`W0032` errors and implies `--strict-effects`; `--lint-effects` is a deprecated alias (`TUR-W0050`); the duplicated emitter and its wrong comment are gone.  Pinned by `tests/run-flags.sh` `lint-effects-alias`, `strict-effects-werror`, `strict-effects-werror-clean` |
| ~~[module-members-skip-effect-row-checking](../archive/module-members-skip-effect-row-checking.md)~~ | medium | **RESOLVED 2026-10-02** (archived): `effect_check_pass` walks one flattened list -- top-level items with `(defmodule ...)` bodies and top-level `(do ...)` forms spread in place, as the emitter flattens them -- so a module member (or a macro's `(do (defn ...))`) is resolved (`TUR-E0026`), inferred and checked (`TUR-E0009`), and `--dump-effects` lists it.  The binding index that silently dropped functions past slot 1024 -- already live on `main`: `#lang r7rs` programs index 1241-1587 functions, so a late callee's inferred effects never reached its callers -- is a growable hash.  Measured blast radius: 0 of this repo's fixtures; 3 of turmeric-spices' 777 files (the three spice tests the report named, each a printing `#fx{Unsafe}` that needs `IO`).  Pinned by `tests/fixtures/errors/effect-row-module-member`, `errors/effect-row-toplevel-do-defn`, `errors/effect-row-past-1024-fns`, `effect-row-module-member-ok`, and `tests/run-flags.sh` `dump-effects-module-members` |

## Refinement experiments reviewed at the v0.59.0 cut (filed 2026-10-02)

Found reviewing the two live `EXPERIMENTS[]` rows --
[loop-invariants](../archive/loop-invariants-plan.md) (`expires_at 0.58.0`,
so due) and [reflected-measures](../upcoming/reflected-measures-plan.md)
(`expires_at 0.61.0`, not due) -- against what graduating each would freeze.
Every row below was reproduced against a v0.59.0 Debug build. None is a
soundness bug: in all four the runtime check is kept, so the programs are
correct. They are the completeness and diagnostic gaps the `prototype`
lifecycle label is currently covering for, and each one is a thing to decide
before either row graduates.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[loop-invariant-silently-unverified-outside-a-defn](../archive/loop-invariant-silently-unverified-outside-a-defn.md)~~ | medium | **RESOLVED 2026-10-03** (archived): a `definstance` method's loops are analysed like a `defn`'s (`li_analyze_method_loops`, entry facts from its parameter refinements), and a loop no definition analyses -- a top-level lambda's -- is declined with `TUR-W0372` and counted (`li_decline_unanalyzed`).  Pinned by `loop-invariant-definstance-method`, `errors/loop-invariant-definstance-refuted`, `errors/loop-invariant-top-level-lambda-strict` |
| ~~[reflect-fuzz-never-reaches-the-rf3-rf4-encoder](../archive/reflect-fuzz-never-reaches-the-rf3-rf4-encoder.md)~~ | medium | **RESOLVED 2026-10-03** (archived): `shape_reflect` (`--only-shape reflect`) generates recursive measures over a list, fuel-crossing depths, RF4 arm selection, a sabotaged sibling per rung, and the RF1/RF2 rejections.  RF3's `--n 400` at seeds 11 and 23: 0 BUG on the fixed compiler.  Its first run found a float-field selector soundness bug (23 / 26 BUG_soundness unfixed), fixed alongside |
| ~~[reflect-two-provable-facts-report-as-not-holding](../archive/reflect-two-provable-facts-report-as-not-holding.md)~~ | low-medium | **RESOLVED 2026-10-03** (archived): an unknown's note no longer claims the predicate fails (every `TUR-W0372`); a Boolean equality encodes as an `iff`, so `(= r true)` proves; and a crossing in a constructor arm gets the arm's tag/selector facts when the predicate mentions a reflected measure and the arm connects to the argument, so a ground false crossing keeps its refutation.  Dropping path facts about a name rebound below them also fixed nested same-name `let`s "proving" a false crossing |
| ~~[loop-invariant-declines-more-than-soundness-requires](../archive/loop-invariant-declines-more-than-soundness-requires.md)~~ | low | **RESOLVED 2026-10-03** (archived): a nested loop's assigned names are havocked rather than declining the outer loop, so only a later read of one declines; a `return` prunes the paths through it, so initiation and preservation still prove and only the post-loop fact is withheld (`early_return`).  Pinned by `loop-invariant-nested-and-early-return`, `errors/loop-invariant-early-return-refuted`, `loop-invariant-declines` |

## P0 representation confusion (filed 2026-09-30)

The recurring class -- a value crossing a seam in the emitted C at the wrong
representation -- is now watched by a mechanism-level detector (clang
`-fsanitize=function`, armed in all four source fuzzers by
`tests/fuzz_arm.py`) instead of only by shape-enumerating generators.  The
corpus sweep reached zero on 2026-10-02 and the detector gates now (the `fnsan`
CI job, `tests/run-fnsan.sh`; a fuzzer trap fails by default).  Detectors see only programs that exist, so the generic-body shapes the
fixture corpus lacked are generated by `tests/generic-spec-matrix.py` (a
PRODUCER x SINK x TYPE matrix checked compiled, interpreted and linted).

| Report | Severity | One line |
| --- | --- | --- |
| ~~[generic-spec-carrier-crossings](../archive/generic-spec-carrier-crossings.md)~~ | high | **RESOLVED 2026-09-30** (archived): a value typed as the enclosing generic's own `A` crossed the carrier at the wrong representation in five independent paths -- `(defn first-of [A] [v : (Vec A)] : A (vec-get v 0))` at float printed `4.61968e+18`, the cstr twin segfaulted, `(.val (Box x))` and `(vec-push! w x)` value-converted, `(some ...)` over a generic call was invalid C -- plus the interpreter's float32 `gen-unwrap`.  The lint flagged it; no program in the corpus had the shape.  Pinned by `tests/fixtures/generic-spec-carrier-crossings` and `gen-yield-float32-interp` |
| ~~[generic-map-assoc-rejects-sig-tyvar-value](../archive/generic-map-assoc-rejects-sig-tyvar-value.md)~~ | high | **RESOLVED 2026-09-30** (archived): `(map-assoc (map-new) 1 x)` with `x : A` was refused at `tur check`, and once accepted kept `(map-new)`'s unfixed V, which `map-get` read back as `int` -- retyping the generic's `A` result, so at `A := (fn ...)` the caller thin-called a fat handle (SIGSEGV).  Pinned by `tests/fixtures/generic-map-assoc-sig-tyvar-value` and `generic-spec-carrier-crossings-4` |
| ~~[dict-classvar-float-param-value-converted](../archive/dict-classvar-float-param-value-converted.md)~~ | high | **RESOLVED 2026-10-01** (archived): a rank-2 lens applied at a float handed the dictionary method the wrong number -- the carrier call value-converted the argument, and the slot held the raw `sh(double)` called as `(int64_t)`.  Found sweeping fnsan traps.  Pinned by `tests/fixtures/dict-classvar-float-param-value-converted` |
| ~~[rank2-class-result-word-converted](../archive/rank2-class-result-word-converted.md)~~ | high | **RESOLVED 2026-10-01** (archived): a rank-2 class method whose result is the class variable came back as the wrong word -- a float instance's result read as an int (`3.95253e-323` for 8.35), a by-value ADT result invalid C or a segfault.  The dict slot now holds a `__dictwrap_*` returning the word.  Found by the type fuzzer's `x_rank2_class` crossing.  Pinned by `tests/fixtures/rank2-class-result-word-converted` |
| ~~[applied-type-annotations-unchecked](../archive/applied-type-annotations-unchecked.md)~~ | high | **RESOLVED 2026-10-01** (archived): a defn return, a call argument and a let annotation compared applied types by kind or not at all -- `(f (some 7.1))` on `[o : int]` printed a box address, `: cstr (some x)` type-checked, `[o : (Option int) (some 7.1)]` silently took the initializer's type.  One predicate, `applied_type_conflict`, now answers for all three.  Pinned by six `tests/fixtures/errors/applied-*` / `scalar-return-into-applied-rejected` fixtures |
| ~~[instance-method-applied-return-annotation-rejected](../archive/instance-method-applied-return-annotation-rejected.md)~~ | medium | **RESOLVED 2026-10-01** (archived): `(co [x : float] : (Option float) ...)` in a `definstance` was refused as a stray type annotation; only a bare symbol reached the return-annotation path.  Pinned by `tests/fixtures/instance-method-applied-return-annotation` |
| ~~[class-var-applied-result-untyped-in-constrained-generic](../archive/class-var-applied-result-untyped-in-constrained-generic.md)~~ | high | **RESOLVED 2026-10-01** (archived): `(co x)` with a declared `(Option a)` result typed as a def-less `(? ?)` inside a constrained generic; typed honestly, the dict and base clones read each instance's by-value monomorph through the representative's struct.  Slot returns the boxed word; the base clone boxes at production.  Pinned by `tests/fixtures/class-var-applied-result-in-constrained-generic` |
| ~~[constrained-generic-relay-drops-dict](../archive/constrained-generic-relay-drops-dict.md)~~ | high | **RESOLVED 2026-10-01** (archived): a constrained generic relaying to another ran the wrong instance -- the relay's dict clone called the callee's carrier base, a spec borrowed a sibling spec's clone (`(eu 40)` ran the W instance on an int), int64 temps were re-boxed as aggregates, and `(some x)` at A := (Option int) was minted at A := int.  Found by the widened rank-2 fuzz crossing.  Pinned by `tests/fixtures/constrained-generic-relay-word-temp-return`, `construct-in-spec-nested-option-int` |
| ~~[subword-payload-box-read-at-wrong-offset](../archive/subword-payload-box-read-at-wrong-offset.md)~~ | high | **RESOLVED 2026-10-01** (archived): a boxed `(Option float32)` read through the carrier layout answered 0 for 3.25 in both engines -- the float32 word pad fixed the payload's size but left it at offset 4, the carrier's at 8.  Also: a class with only float/float32 instances was refused as ambiguous inside every constrained generic.  Pinned by `tests/fixtures/subword-payload-box-read-at-wrong-offset` |
| ~~[cps-typed-pointer-into-carrier-slot](../archive/cps-typed-pointer-into-carrier-slot.md)~~ | medium | **RESOLVED 2026-10-01** (archived): a CPS-colored generic bound `(vec-new)` to `tur_adt_Vec__float *` and passed it bare into `vec-push-ex`'s `int64_t` slot -- the CPS binder decls recorded no C type.  Found by the type fuzzer under clang.  Pinned by `tests/fixtures/cps-typed-pointer-into-carrier-slot` |
| ~~[generic-fn-element-word-pointer-crossings](../archive/generic-fn-element-word-pointer-crossings.md)~~ | medium | **RESOLVED 2026-10-01** (archived): six generic-spec matrix cells (fn and cstr elements across if/match merges, `some`, a lambda slot, a CPS raw-let) were invalid C under clang and gcc 14 -- the matrix had only ever compiled with gcc 13, which warns.  The matrix now prefers clang.  Pinned by `tests/fixtures/generic-fn-element-word-pointer-crossings` |
| ~~[tvar-float-payload-value-converted](../archive/tvar-float-payload-value-converted.md)~~ | high | **RESOLVED 2026-09-30** (archived): every TVar payload crossed into the slot through `(void*)(intptr_t)v`, so a float was stored truncated -- `(tvar/cas t 7.4 ...)` against 7.1 SUCCEEDED and the read-back printed 3.45846e-323, with the interpreter right throughout.  The fuzzer's tvar seam was a "correct positive control" whose CAS-only oracle could not see it; it now prints the value.  Pinned by `tests/fixtures/tvar-float-payload-value-converted` |
| ~~[fn-field-carrier-shim-read-typed](../archive/fn-field-carrier-shim-read-typed.md)~~ | high | **RESOLVED 2026-09-30** (archived): a typed fn field over a parametric aggregate (`(Result float int)`, `(Option int)`) passed it by value to a shim that takes a box pointer: SIGSEGV, pre-existing.  The call site now asks the shims' own b4box slot question.  Pinned by `tests/fixtures/fn-field-carrier-shim-read-typed` |
| ~~[lambda-thin-fn-result-read-as-fat](../archive/lambda-thin-fn-result-read-as-fat.md)~~ | high | **RESOLVED 2026-09-30** (archived): a captureless lambda returning a function handed back its bare code pointer where every consumer of `(fn [] (fn [int] int))` -- and of a generic `(fn [] A)` -- reads a fat handle: `((f) 41)` segfaulted. Lambdas now get the fn-value-fat-normalization stage-2 treatment defns already had.  Pinned by `tests/fixtures/lambda-thin-fn-result-read-as-fat` |
| ~~[open-arg-first-binds-call-result-as-int](../archive/open-arg-first-binds-call-result-as-int.md)~~ | high | **RESOLVED 2026-10-02** (archived): `(get-or (none) 1.5)` against `[o : (Option A) d : A]` printed `1` -- `(none)` bound `A` to its own free variable first and the later `1.5` did not replace it (the enclosing-signature rule fired on the coincident name), so the result typed `int` while the spec ran at float.  `(err "e")` and `(vec-new)` first did the same; a cstr default printed an address.  A provisional binding now yields to a concrete one, and the open argument is grounded and monomorphized.  Probing the axis also fixed a let-bound open argument (refused one way, `7` for 7.1 the other), `(map-new)` first (refused), and a compiler abort at uint8/16/32 payloads (the repr shadow matched `uint` as a substring of an aggregate's name).  Pinned by `tests/fixtures/open-arg-first-binds-call-result` |
| ~~[match-var-arm-in-generic-spec-byvalue](../archive/match-var-arm-in-generic-spec-byvalue.md)~~ | medium | **RESOLVED 2026-10-02** (archived): a catch-all variable arm over `(Option A)` in a generic emitted `int64_t other = *__scrut;` against the spec's by-value aggregate -- invalid C at every instantiation.  The binder takes the scrutinee's C type.  Pinned by `tests/fixtures/match-var-arm-in-generic-spec-byvalue` |
| [cps-evicts-handle-in-operand-positions](cps-evicts-handle-in-operand-positions.md) | medium | **Narrowed 2026-10-03 (twice):** item 5 (a pure generic applying a lambda literal on the spot) and item 4 (a join over a non-atomic argument to an effectful struct-field call, which must carry the fn-typed field-load callee -- `handle-field-call-joined-arg`) are fixed, plus invalid C for two sequential `handle`s with a closure in the second (`handle-sequential-widened-closure`).  Left: item 3, a GENERIC HOF on the handler-to-performer path -- reduced to five lines; its carrier instantiation has no ABI spec, so there is no mono-template to admit and the base sig-rejects, and the obvious fix (admit the spec-less base) is unsound for an empty-row fn parameter.  Fuzz seeds 3333 / 4444 / 5555 / 6666: 0 `GEN_REJECT`s.  Earlier: **Narrowed 2026-10-01 (second pass):** a reinterpret around a generic call (`EX_REINTERPRET`) and a `handle` in a rank-2 / fn-value call's operand now lower (and a rank-2 argument captured across a `handle` no longer emits invalid C).  Left: a SIG-rejected generic HOF on the handler-to-performer path, a join that would capture the field-load callee (clean refusal), and one fuzz case failing the structural core check.  Seeds 3333+4444: 5 `GEN_REJECT`s -> 2.  Expressiveness gap, not a miscompile |
| ~~[struct-temporary-fn-field-box-leaks](../archive/struct-temporary-fn-field-box-leaks.md)~~ | low (leak) | **RESOLVED 2026-10-03** (archived): a by-value struct with a boxed fn field, used unbound as a field-call receiver, is bound in a synthesized `drops_fn_fields` `let` (direct emitter), and the CPS backend hands each boxed field of such a binder to the entry boundary's reap (`emit_letraw_fnfield_reap`), since a CPS scope can end in a tail call. `struct-temporary-fn-field-released` (leak-checked) covers both |
| ~~[handle-over-effectful-fn-field-in-arg-let-evicted](../archive/handle-over-effectful-fn-field-in-arg-let-evicted.md)~~ | medium | **RESOLVED 2026-10-01** (archived): a `handle` over an effectful fn-field call was refused under a builtin argument, as a method-call operand, or with a non-atomic argument -- the struct-store walker missed EX_BUILTIN/EX_GET_FIELD, method calls kept control operands, the registry paths refused non-atomic args.  Pinned by `tests/fixtures/handle-over-effectful-fn-field-shapes` |
| ~~[emitted-c-indirect-calls-are-not-type-exact](../archive/emitted-c-indirect-calls-are-not-type-exact.md)~~ | high (WASM) / medium | **RESOLVED 2026-10-02** (archived): zero `-fsanitize=function` traps across the corpus, and the detector gates -- the `fnsan` CI job (`tests/run-fnsan.sh`, canary-armed) and `BUG_fnptr_trap` in the fuzzers by default.  The ninth sweep made the `ptr<void>` decision: an untyped `ptr<void>` PARAMETER is the word in slot 0, as a function-typed one already was (a closure keeps `void *` in its definition behind a `__tur_slot0_` entry).  Two traps found on the way fixed (a fn-param closure boxed at words for a typed caller; a narrow fat value packed into a concrete `tur_poly_fn_t` sink).  The one trap left (the next row) was fixed the same day; the gate carries no `known.fnsan` |
| ~~[arrow-instance-closure-erased-to-words](../archive/arrow-instance-closure-erased-to-words.md)~~ | medium | **RESOLVED 2026-10-03** (archived): `((>>> f g) 7.1)` printed `-9223372036854775808` (`>>>` returns `(fn [A] C)` now, and a direct invoke of a generic's returned closure no longer value-converts a carrier-reinterpreted float into the spec clone's double slot); `pipe [^Arrow A]` over `^fat` arrows SIGSEGV'd (the tyvar-parameter shim boxed an already-fat box); and `.>>>` inside the generic ran the erased instance base -- an fnsan trap for float/cstr -- until the emitter's per-spec scan learned to bind `{a := (->), b, c, d}` from spec-resolved arguments (`emit_abi_arrow_spec_bindings`).  run.sh and run-fnsan.sh 3536/0.  Pinned by `arrow-generic-closure-direct-call`, `arrow-instance-in-constrained-generic` |

## Representation gaps (filed 2026-09-09, extended 2026-09-10)

A value whose representation does not fit the one the typed path already chose
for it. The three rows below were found independently and share that shape;
the family the Saffron pass tracked under H7 / M1 / M2 is resolved and lives in
[saffron-dynamic-surface-pass](../archive/saffron-dynamic-surface-pass.md)
(archived 2026-09-19 with every item of the pass resolved).

| Report | Severity | One line |
| --- | --- | --- |
| ~~[fn-typed-tyvar-drops-a-capturing-closure](../archive/fn-typed-tyvar-drops-a-capturing-closure.md)~~ | medium-high | **RESOLVED 2026-09-11** (archived): a lambda whose declared fn result is a capturing closure now marks that result `boxed` like a `defn` does, so `A` instantiates fat; pinned by `tests/fixtures/fn-typed-tyvar-capturing-closure`. Original row: A capturing closure passed through a type parameter instantiated to a FUNCTION type is silently miscompiled: `check`, `emit-c` and `build` all exit 0 with no warning, and the built program takes SIGBUS before printing anything. Needs both halves -- a capture-free lambda in the same position works (it lifts to a bare code pointer, which survives the int64 carrier round trip), and spelling the function type concretely instead of through a type variable works. Confirmed pre-existing at v0.46.0. An adjacent `turmeric-spices` defect first looked like the same gap seen from the other side; it is **not** -- its trigger is arm ORDER (the emitter types the match result temporary from the first arm as written), established by three controls, and it has a one-reordering workaround. This one has no `match` in its repro at all |
| ~~[all-any-fn-param-is-unusable](../archive/all-any-fn-param-is-unusable.md)~~ | medium | **RESOLVED 2026-09-11** (archived): a fn-value callee no longer takes the CPS IR's named-call arm, and the seam into an all-`any` fn target casts against the fat id the widen stamps; pinned by `tests/fixtures/all-any-fn-param`. Original row: A parameter declared `(fn [any] any)` or `(fn [] any)` -- the signature the Saffron dialect produces by default -- is emitted as a bare `int64_t` and then CALLED, so the callee itself does not compile; and an `any`-held function passed into one panics at the cast. The interpreter handles both. Found while fixing the Saffron pass's H7 and masked until then by H7's own fuzzer `KNOWN` row |
| ~~[hkt-carrier-result-loses-payload-types](../archive/hkt-carrier-result-loses-payload-types.md)~~ | high | **RESOLVED 2026-09-17** (archived): the carrier-path dispatch result for a partially-applied instance head is now grounded hole-aware in the elaborator (the receiver with its hole slot replaced by `b`, so T4's erased head cannot swap the arms) and the call hoist bridges the carrier box into the aggregate at production, so every consumer position sees the real payload types; pinned by `tests/fixtures/hkt-carrier-result-payload-types` (Result hole-at-0 heterogeneous, Either leftmost partial + carrier-bodied, and the no-`let` direct-argument shape). Original row: A typed `fmap` on a heterogeneous `(Result int cstr)` / `(Either int cstr)` printed the `cstr` arm's ADDRESS -- the dispatch rode the erased carrier and its result stayed a def-less `(type-app ? ?)`, so the `match` bound every arm as the int carrier |
| ~~[partial-head-ap-calls-fat-closure-as-thin-pointer](../archive/partial-head-ap-calls-fat-closure-as-thin-pointer.md)~~ | high | **RESOLVED 2026-09-25** (archived): the instance body of a hole-at-0 head `(Result _ B)` read `(f X)` as `(Result B X)`, arms swapped, so `ap`'s function was typed as the fixed arm and the ascription that worked around it emitted a thin call on a fat closure; the body typing is now hole-aware, the call site grounds `ap`'s result from the receiver's function, and the head binding no longer runs inside generics. `Applicative [(Result _ B)]` ships; pinned by `hkt-ap-partial-head` and `hkt-ap-result-instance`. Original row: compiled `ap` over a partial head segfaulted; `--interpret` printed the right answer |
| ~~[narrow-closure-result-read-through-int64-carrier](../archive/narrow-closure-result-read-through-int64-carrier.md)~~ | high | **RESOLVED 2026-09-26** (archived): A closure's narrow integer result is widened to int64 at every entry point an erased caller can reach (the carrier shims at an erased sink and slot 0 of a fat closure), and typed callers cast slot 0 to match. Pinned by `tests/fixtures/narrow-closure-result-through-carrier` |
| ~~[defdata-ctor-fn-field-passes-pointer-as-int](../archive/defdata-ctor-fn-field-passes-pointer-as-int.md)~~ | low | **RESOLVED 2026-09-26** (archived): A capturing closure or boxed function passed to a monomorph constructor's carrier field is now cast into the int64 slot. Pinned by `tests/fixtures/hkt-ap-partial-head`'s capturing-closure case |
| ~~[hkt-generic-forwarded-bind-continuation-segfaults](../archive/hkt-generic-forwarded-bind-continuation-segfaults.md)~~ | high | **RESOLVED 2026-09-25** (archived): the call site now boxes a by-value aggregate result in the continuation's slot-0 shim when the parameter's declared result is an HKT-erased `(M b)` -- a result-boxing shim for a bare fn, a wrapping box for a capturing closure. Pinned by `hkt-generic-forwarded-continuation`. Original row: a generic forwarding a continuation parameter to `bind` segfaulted compiled |
| ~~[hkt-dict-generic-byvalue-result-to-typed-param](../archive/hkt-dict-generic-byvalue-result-to-typed-param.md)~~ | medium | **RESOLVED 2026-09-26** (archived): A dict clone's by-value result now reaches a typed parameter of any by-value ADT, and a dictionary-dispatched method result is no longer re-spilled as an aggregate when it feeds another dispatch. Pinned by `tests/fixtures/hkt-dict-generic-byvalue-result-to-typed-param` |
| ~~[generic-category-base-passes-carrier-to-arrow-instance](../archive/generic-category-base-passes-carrier-to-arrow-instance.md)~~ | low | **RESOLVED 2026-09-26** (archived): A carrier argument passed to a `void *` parameter (the function arrow's instances) is now cast. Pinned by `tests/fixtures/stdlib-arrow-generic-entails-category` |
| ~~[hkt-generic-none-to-typed-param-segfaults](../archive/hkt-generic-none-to-typed-param-segfaults.md)~~ | high | **RESOLVED 2026-09-25** (archived): the carrier -> by-value bridge dereferenced `none`'s 0 carrier; for a sum whose tag-0 constructor is nullary it now answers the tag-0 value through a NULL-safe helper, the reading `match` already gives. Pinned by `hkt-generic-none-to-typed-param`. Original row: a `none` from a constrained generic segfaulted at a typed `(Option int)` parameter |
| ~~[hkt-generic-nested-bind-result-type](../archive/hkt-generic-nested-bind-result-type.md)~~ | medium | **RESOLVED 2026-09-26** (archived): A nested `bind` in a `do-m` continuation now dispatches through a captured Monad dictionary, an aggregate captured into a carrier env slot is boxed, and a constrained generic calling another is routed to the callee's dict clone with the caller's dictionaries. Pinned by `tests/fixtures/hkt-generic-nested-bind-result-type` and `tests/fixtures/hkt-generic-calls-generic` |

Both 2026-09-10 rows carry a "do not retry this" that is easy to lose: for
`all-any-fn-param`, it is NOT a gap in H7's fix -- H7's adaptor declines an
all-`any` target correctly, since that IS the representation the box holds, and
lifting the decline was measured to fix neither repro. For
`hkt-carrier-result`, it is the same missing head-tyvar binding as M2, but M2's
fix binds it only where slot-safe (a by-value body, a homogeneous receiver);
grounding a heterogeneous hole-at-0 head instead swaps the arms and turns a
printed address into a miscompiled body. The fix that landed sidesteps the
head entirely -- the result is the receiver with its hole slot replaced --
and bridges at the call hoist rather than per consumer.


## Runtime seams carry payloads as int64 (filed 2026-09-16)

A payload **parked in a runtime data structure** by one piece of emitted code and
read back later by another. The slot has one C type, so the value is cast in and
out -- and a plain C cast of a `double` is a value conversion that TRUNCATES.
Every row below is a silent wrong answer on the compiled path, accepted by the
type checker, correct under `tur --interpret`.

This is one defect *class*, not three coincidences, and it has been paid for
before: `docs/archive/` holds ten resolved instances of the same truncation
found one feature at a time (`ascribe-int-to-float-reinterprets`,
`forall-dict-float-result-truncated`, `fiber-effect-float-result-truncated`,
`method-result-float-spec-return-value-converts`,
`ok-val-untyped-catch-box-loses-float`,
`float32-generic-call-result-printed-as-carrier`,
`int-declared-method-float-body-engine-divergence`, ...). **Before reaching for a
fourth instance, read the fix convention that already shipped**: the direct/fiber
effect path stores through a `union { double d; int64_t i; }` bit-reinterpret, and
`any` sidesteps the slot entirely by boxing with a type tag.

Why it went unfound for so long: four source-level fuzzers all fuzz `float` as a
first-class axis, but every boundary they crossed was one the **compiler owns both
ends of** (pass-through defn, let, ascription, generic identity, HOF, closure
return, typeclass dispatch). None emitted a session, a router send, a `yield`, an
`await`, a `perform` or a `tvar` -- the shapes were absent, not suppressed, and
absence is invisible. `tests/type-fuzz-src.py` grew a **runtime-seam axis** in the
same change as these filings; `--seam-matrix` prints the whole table and
`--known-probes` pins each row's minimal repro.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[generator-yield-payload-is-int64-only](../archive/generator-yield-payload-is-int64-only.md)~~ | high | **RESOLVED 2026-09-17** (archived): `yield` stores the value's bits and `gen-unwrap` is a core form typed by the generator's element kind (through the `(gen-next g)` it consumes or the let binding holding one), so a yielded `7.25` / `"hello"` / `true` read back as themselves on both backends; the interpreter's misaligned `TuriGen` (the second defect) is 16-byte aligned now. Pinned by `tests/fixtures/gen-payload-types`, the fuzzer's generator rows retired. Original row: `yield` parks the payload in a `void *` frame slot and `gen-unwrap` is declared `: int`, so a yielded `cstr` **prints as a raw pointer** and a `bool` prints `1` -- with no cc error anywhere to stop it. Worse than the session seam: the erasure reaches the Turmeric *signature*, so a codegen-only fix still hands back an `int`. Carries a second, separate defect -- the same program trips a UBSan misaligned load on `TuriGen` under `--interpret` (`src/turi/eval.c:11962`), which also blocks using the interpreter as this seam's differential oracle |
| ~~[async-await-payload-is-int64-only](../archive/async-await-payload-is-int64-only.md)~~ | high | **RESOLVED 2026-09-17** (archived): the thunk's declared result rides the async node and the `let`/`def` that binds it, `await` reads the slot back at that type (a float from its bits, the rest by cast), and a non-int64 thunk is spawned through a wrapper at its real prototype -- calling a `double`-returning fn through `int64_t (*)(void)` was an x86-64 ABI mismatch (xmm0 vs rax), not merely a lost reinterpret; pinned by `tests/fixtures/async-await-payload-types`, the fuzzer's await rows retired. Original row: `tur_await_future` returns `int64_t` and the await site binds the result at that type, so a `float`-returning `async` thunk prints `4619848792751996928` -- bit-exact `7.25`. **The bits arrive intact and only the type is lost**, which makes this the cheapest row to fix. `tests/fixtures/async-await-basic` documents the int64 return in its own header, so the erasure was known and simply never contradicted |

Two traps recorded so nobody re-derives them. First, the natural assertion
**hides** the await and generator rows rather than exposing them: `(= (await fut)
7.25)` is a `TUR-E0042` reject, not a wrong answer, so a fuzzer that compares
instead of printing files the defect under "the generator emitted an illegal
program" and never fails. Second, `float` is the only probe that shows these at
all -- per the float rule in [CLAUDE.md](../../CLAUDE.md), an integer literal
cannot show truncation, and every fixture and guide example for all three
features sends an `int`.

A fourth seam, the **binary** session `send`/`recv`
(`session-payloads-are-int64-only`), and the multi-party router seam
(`router-payloads-are-int64-only`) were fixed 2026-09-16
(turi-session-expansion-plan S3.5: floats bit-reinterpreted, pointers cast
through `intptr_t`, by-value aggregates rejected with `TUR-E0212`) and are
archived under `docs/archive/`; the remaining reports above cross-link the
archived binary report. Three seams measured **correct** and kept as the
fuzzer's positive controls: `perform`/`resume`, `any`/`cast`, and `tvar`
write/cas.


## Docs audit sweep (filed 2026-08-20)

Thirty-three reports filed from a full-docs accuracy audit (guides, design
notes, README). These were verified by grep against the source tree, not by
running repros -- the audit container had no built compiler -- so treat each
repro as read-verified unless its file says otherwise. Every report carries a
"Guides to update when fixed" section; updating those guides is part of the
fix, not a follow-up. The two rows marked (spice repo) live in the sibling
`turmeric-spices` checkout and could not be grep-verified at all.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[saffron-dynamic-surface-pass](../archive/saffron-dynamic-surface-pass.md)~~ | -- | **RESOLVED 2026-09-19** (archived; every item of the pass is resolved and pinned). **2026-09-19:** six lows resolved and pinned (Sym `=`, expression call head, `if` join to `any`, `[] [1 2.5]` parse, trailing-keyword body, ctor under an all-`any` slot); fuzzer KNOWN table and probes now empty. M9 (multi-arg / class-variable-result methods on a primitive or non-parametric ADT receiver) resolved via a per-instance witness. The `call/cc` `: any` receiver is resolved too. **Later on 2026-09-19: M7 and the `& rest : any` gap resolved.** stdlib `Cons`'s tail is the recursive occurrence `(Cons A)` (a typed pointer; `tnil` is `[A] [] : (Cons A)`, `tnil?` takes the typed list, `null?` is the `:int` twin), so a `(Cons any)` walks through an `any` with no per-step ascription on both back ends (`saffron-cons-list-walk-through-any`); a `: any` rest list is that `(Cons any)` monomorph, built by the call site through the `Cons` constructor under a per-element `any` ascription, and the callee's `rest` is typed as one (`saffron-variadic-rest-any`, `variadic-rest-any`). No item of the pass remains open. Original row: Differential pass over the Saffron surface (2026-09-09): 11 high and 10 medium findings plus lows, each with a repro. 2026-09-10: H1-H6, H8-H11, M3-M6, M8, M10 resolved and pinned (struck through in the file), and M2 resolved on the INTERPRETER only; H7 and M1 are resolved on both back ends (a marshalling adaptor at the seam and at the dynamic witness's continuation, not the reinterprets the report filed); M2 is resolved on both back ends too: the instance's own head tyvar is now bound at a by-value-bodied dispatch (hole-aware, homogeneous-only for T4's hole-at-0 head), so the by-value spec exists and the dynamic witness resolves to it. Still open: M7's remaining half (the erased self-referential cons tail), M9, and the REMAINING lows. Eight lows are resolved and pinned: string `=` through `any`, `while` truthiness, `defstruct`/`defdata` `any` and `Sym` field types, a dynamic read of an `any` field emitting uncompilable C, the `tvec`/`vec` bounds-message split, and `map-assoc` widening a concrete value into an `any`-valued map (guarded so an annotated `(Map Sym int)` still rejects it), and a leading keyword reading as a Sym VALUE rather than a field name. Two `KNOWN` rows retired from `tests/saffron-fuzz-src.py`. M7's hard error is fixed -- a dynamic field read now reaches generic and `:heap` ADTs. H7, M1 and M2-compiled are ONE shape -- a Saffron `tur_tagged_t` that does not fit the representation the typed path already chose -- and for all three the cheap fix yields a silent wrong answer (M1 already truncates one), so they are parked behind clean declines and want picking up together. `tests/saffron-fuzz-src.py` avoids the open shapes via its KNOWN table and pins them with `--known-probes`. |
| ~~[self-typed-heap-parametric-field-unsupported](../archive/self-typed-heap-parametric-field-unsupported.md)~~ | medium | **RESOLVED 2026-09-19** (archived). `(defstruct Node :heap [A] (val A) (next (Node A)))` compiles end to end on both back ends: five defects in a row (byvalue-product re-entry, the `expected int, got int` diagnostic, the SR2b self-recursive exclusion vs `repr_of`, ctor-signature recording re-entering the registry and freeing a held param ctype, and the typedef dependency pre-pass recursing into a pointer-held dependency). Pinned by `tests/fixtures/heap-parametric-self-typed-field*`, `saffron-heap-parametric-self-typed-field`, `errors/heap-parametric-self-typed-field-int-terminator`. Unblocks M7 / `& rest : any` in the Saffron report; the stdlib `Cons` tail redeclaration itself is still to do |
| ~~[nullary-generic-call-under-tyvar-expectation](../archive/nullary-generic-call-under-tyvar-expectation.md)~~ | low-medium | **RESOLVED 2026-09-19** (archived, same day). A nullary generic call as an argument whose expected type is tyvar-shaped -- `(make-struct W 8 (none))` for `(opt (Option A))`, `(wrap 3 (box-nil))` -- was rejected with `expected (type-app Option tyvar 'A'), got (type-app Option tyvar 'A')`: the sibling-bound `A := int` was compared with the argument's own open tyvar instead of unified. The parameter is now instantiated with the siblings' bindings and the argument unified W2-style, recording the substitution on the nullary call. Pinned by `tests/fixtures/nullary-generic-call-under-tyvar-expectation`; what let `tnil` become `(Cons A)` for M7 |
| ~~[nonparametric-adt-forward-typedef-redefinition](../archive/nonparametric-adt-forward-typedef-redefinition.md)~~ | low | **RESOLVED 2026-09-25** (archived): the early-file ADT pass defines `TUR_TD_<Name>` even when it does not guard the layout, so the forward decl no longer re-typedefs it. Original row: Filed 2026-09-19, pre-existing on `main`. A user ADT held behind `(Option P)` gets a forward `typedef struct tur_adt_P tur_adt_P;` from the registered-app pre-pass AFTER its full `typedef struct tur_adt_P {...} tur_adt_P;`, because that full-layout emitter never defines the `TUR_TD_` guard the forward decl keys on: `redefinition of typedef` under `clang -std=c99` (silent under gcc). ~1 in 40 snapshots. Fix: define the guard there, or introduce the name once and define the tag only, as the parametric path now does |
| ~~sr2-carrier-seam-rotted~~ | -- | **Resolved 2026-09-04**, all three defects. (1) the hatch aborted the compiler unless `TUR_OPTION_NICHE=0` moved with it -- `sr3_option_niche` follows its substrate down; (2) an "invalid initializer" -- the match binder read an Option/Result pointer-box slot inline; (3) the silent wrong answer needed TWO changes and the root-cause note had found one. `EX_MATCH` was missing from `body_has_dispatch_on_app_tyvar`'s walk **entirely**, so every `instance_changes` trigger was blind to the inside of a match -- which is how every sum instance is written, so `Eq[Option]`'s only dispatch was permanently out of reach. Found by instrumenting the walk, not reading it: under the concrete binding set it reached exactly one call, with no dict_arg. With the walk fixed the predicate still declined, because it peels the `(:: vx A)` ascription and asks about the INNER expression (the erased carrier) while `A` is not in `bindings` anyway -- that holds the CLASS var. The added clause asks about the ascription's OWN type, gated on the class-var binding's type ARGUMENT being nominal, which is the over-minting guard: `(Option int)` mints nothing. Suite 2792/0 with **zero snapshot drift**. Minting the spec was only half -- the re-resolved `__inst_Eq_eq_qu_String(void *, void *)` was then handed `int64_t`, a right answer with a `-Wint-conversion` under it, so the argument reinterpret landed too. Also probed and NOT a gap: two payload types sharing an Option C signature do not collide (the clone names disambiguate with `_h1`). Archived to [docs/archive](../archive/sr2-carrier-seam-rotted.md) |
| ~~[defstruct-struct-path-is-dead](../archive/defstruct-struct-path-is-dead.md)~~ | low | **RESOLVED 2026-09-19** (archived): `defstruct_lowers_to_adt` and its helpers are deleted and `elab_defstruct` lowers unconditionally; the five malformed field-list shapes that reached the vague fallthrough now get the record-ADT parser's precise diagnostics. Original row: `defstruct_lowers_to_adt` had been widened until it rejected nothing well-formed, so the `else` in `elab_defstruct` was (nearly) unreachable |
| ~~[serial-shift-colored-receiver-rejected](../archive/serial-shift-colored-receiver-rejected.md)~~ | low | **RESOLVED 2026-09-28** (archived): a colored receiver or leaf is refused only when an effect can ESCAPE it (non-empty declared/inferred row) -- the direct-entry call is how every effect-free colored function is already called, so colored-but-silent code (a template through a fn value) is admitted; a `k : serial-cont` receiver is now handed the chain in its declared C spelling; the remaining refusal names the receiver and the effect. The escaping-effect shape is filed as `serial-receiver-effect-cannot-reach-enclosing-handler`. Original row: a serial-shift receiver (named or lambda) that calls anything colored -- a fn-value call, an effect, an `unsafe` block -- is rejected with TUR-E0706 although it runs once at capture and is never marshalled; keep its callees uncolored. **Fix direction CORRECTED 2026-09-05.** Root cause confirmed by instrumentation (the colored lambda lifts to a global fn VALUE, so the named arm rejects it on `is_global && callee_colored` and the U7 arm rejects it for not being an EX_CLOSURE). But the filed direction -- thread `subk`'s continuation as the receiver's `DK *` -- does NOT fix the soundness problem it names: the driver builds `subk` as [frames up to the prompt] ++ [RE-INSTALLED prompt] ++ [DONE], so a `perform` inside the receiver would escape to root. Same escaped effect as the direct-entry wrapper's fresh DK root, different address. The receiver needs a chain whose tail is `P->next` (outside the enclosing prompt), which the driver computes and **never hands to the body** -- `DKBody` is `(intptr_t env, DK *sub)`. So the work is widening that contract, not per-site plumbing in the emitter, which is why the restriction has stood. `DKK_RESUME_FRAME` in the same switch is the precedent: a callback that needs the downstream chain already gets it |
| ~~[serial-receiver-effect-cannot-reach-enclosing-handler](../archive/serial-receiver-effect-cannot-reach-enclosing-handler.md)~~ | low | **RESOLVED 2026-09-28** (archived): a named receiver an effect escapes is now called as a colored callee on the reset's own continuation -- the reset builds the frames-over-a-prompt chain the shift would have captured and `return recv__cps(frames, <rest as a resume frame>)`, so the effect walks out into the handlers around the reset (no DK runtime change). `fn_sig_ok` admits a `serial-cont` parameter and the stale `k` parameter-name reservation is lifted for one; admitting it exposed a capturing closure receiver that compiled and aborted with "unhandled effect", which is refused again. Residue (an `if` context, a capturing closure receiver, a leaf) filed as `serial-receiver-effect-under-if-closure-or-leaf`. Original row: Filed 2026-09-28, the residue of serial-shift-colored-receiver-rejected: a serial-shift receiver (or leaf) whose effect escapes it, meant for a handler around the reset, is TUR-E0706 compiled (it runs under `--interpret`). Needs three things: `DKBody` widened to hand the body the chain outside the prompt, the serial reset's chain linked to the enclosing `__kont` with its rest lifted into a resume frame (it runs standalone today), and a `serial-cont` parameter made CPS-emittable (`fn_sig_ok` refuses it). Workaround: handle the effect inside the receiver |
| [serial-receiver-effect-under-if-closure-or-leaf](serial-receiver-effect-under-if-closure-or-leaf.md) | low | **Narrowed 2026-10-03:** shapes 1 and 2 are fixed -- an `if` in the context (the pure arm is delivered into the outward call's lifted rest) and a capturing closure receiver (its use makes the lambda threadable, and the reset calls its env-taking `__cps` twin); `serial-shift-receiver-effect-under-if`, `serial-shift-closure-receiver-effect`.  Open: an effect escaping a context callee (a leaf, which runs at RESUME time) -- decide what a resumed continuation's effects mean first (`errors/serial-shift-context-callee-effect`).  Filed 2026-09-28 |
| ~~[serial-cont-chain-never-freed](../archive/serial-cont-chain-never-freed.md)~~ | low (leak) | **RESOLVED 2026-10-03** (archived): a receiver that only READS `k` -- `(k v)`, `serial-resume`, `serial-cont->bytes`, a helper that in turn only reads it -- has its chain freed after it returns (native: `dk_free(__cap)` in the shift body; outward: registered with the entry reap); the closure receiver's own box is dropped after the run, and `resume-cont!` frees the chain it deserializes. The repro and three existing fixtures are leak-clean and carry `requires.leak-check`, plus `serial-cont-receiver-chain-freed`. Residue (a `k` a lambda captures, a `serial-cont` from `bytes->serial-cont`) filed as `serial-cont-escaped-or-deserialized-chain-leaks` |
| ~~[serial-cont-escaped-or-deserialized-chain-leaks](../archive/serial-cont-escaped-or-deserialized-chain-leaks.md)~~ | low (leak) | **RESOLVED 2026-10-07** (archived): `serial-resume-owned` (resume a rebuilt continuation and free it -- a `serial-cont` is consumed by its resume), `serial-cont-free` and `serial-bytes-free` release what a round trip makes. Pinned by leak-checked fixture `serial-cont-roundtrip-freed`. A rebuilt `cstr` env string stays allocated on purpose |
| ~~[cps-reaped-closure-kept-by-callee](../archive/cps-reaped-closure-kept-by-callee.md)~~ | high (use-after-free) | **RESOLVED 2026-10-03** (archived; found and fixed together): the CPS backend reaped every capturing lambda in an operand position at the entry boundary, so one a callee had stored was freed live -- now only a lambda reaching a `^borrow` / inferred non-retaining slot is reaped; and `elab_fn_value_to_fat` double-boxed a `^fat` parameter stored in a fn cell. `cps-closure-arg-kept-by-callee` (leak-checked) |
| ~~perform-inside-loop-has-no-lowering~~ | -- | **Resolved 2026-09-02**: a tail-position `while` now reaches the loop lowering, a conditional `perform` in statement position reifies its join as a DK resume-frame, a conditionally or repeatedly assigned loop-carried `^mut` rides a shared cell, a loop followed by statements reifies its continuation as a join, and `extern-c` callees no longer color their callers. `examples/snake` passes `tur check`. Archived to [docs/archive](../archive/perform-inside-loop-has-no-lowering.md) |
| ~~wss-client-cert-verification~~ | -- | **Resolved (already fixed at filing; docs updated 2026-09-04)**: `ws-client`'s TLS-V0 (turmeric-spices commit `95eae7a4`, 2026-06-23) predates this report and already verifies against the system CA store by default, with `ws-connect-with-ca` / `ws-connect-insecure` opt-outs. Only the guide had drifted. Archived to [docs/archive](../archive/wss-client-cert-verification.md) |
| ~~webkit-sw-controlled-reload-fails-wasm-init~~ | -- | **Resolved 2026-09-09**, and it corrects the report on two counts. The `init-error` was **none of the three** candidates: the worker never started, so `main.js`'s `error` listener rejected first with `[object Event]` over an `ErrorEvent` whose `message` is `undefined` on WebKit. The cached wasm body is **byte-exact** (3616659 = `Content-Length`, `application/wasm` intact), so the truncation and lost-MIME theories are dead -- `/turmeric.wasm` was never the asset at fault. **`/eval-worker.js` was**, and it was in no cache at all: nothing names it, so `PRECACHE_URLS` omitted it and `precacheShellAssets` (which mines markup) cannot see a worker built from a string literal. Root cause, confirmed on **real macOS Safari 27.0** and not only Playwright: inside a service worker WebKit rejects `fetch(request)` with `TypeError: Load failed` when the request carries `destination: 'worker'` **and** that script is already in the HTTP cache -- `new Request(req)` and `cache: 'reload'` fail identically, `fetch(request.url)` succeeds. So the title is backwards: a Cache API **hit is the healthy path**, and the failure needs a **miss** to force that fetch. Severity settled the way the report allowed for: the symptom is **test-environment only** -- against the built site the pre-fix `sw.js` passes on WebKit; only `npm run dev`, which is all CI ever ran, reproduces it. The defect underneath is real, and the shipped site was saved only by `cacheFirst` happening to hit. Fixed by a worker-destination retry in `cacheFirst` plus precaching `/eval-worker.js` and `/lsp-worker.js` (a genuine offline hole: the evaluator was never precached). `clients.claim()` and a CORP header were both tried and **dropped** -- neither changed anything. Pinned by a new spec that fails pre-fix; mobile project 33/33 on dev and on the built site. Archived to [docs/archive](../archive/webkit-sw-controlled-reload-fails-wasm-init.md) |
| ~~c-sources-propagate-only-one-level~~ | -- | **Resolved 2026-09-02**: `:c-sources` / `:c-includes` now propagate across the whole `:spices` closure (worklist + realpath visited set, the `:cmake-deps` shape, sharing its dep resolver), each source linked once by resolved path. Two-hop and diamond fixtures in the spice c-sources harness. Archived to [docs/archive](../archive/c-sources-propagate-only-one-level.md) |
| ~~[gadt-length-index-not-enforced](../archive/gadt-length-index-not-enforced.md)~~ | low | **RESOLVED 2026-09-28** (archived): a GADT constructor application carries its instantiated index beside its (still bare) type -- `(LNil)` is `(LVec Zero)` -- on SZ8's Form channel, through `let` bindings, ascriptions and declared returns, and it is checked wherever a claim is made (call argument, ascription, annotated let, declared return; one substitution per call). A bare argument with a fully known index meeting an indexed parameter is ascribed to it, so the parameter's variables bind. `(vec-head (VNil))` is a `TUR-E0001`; unknown indices stay unchecked, never rejected. Pinned by `gadt-ctor-index-inferred` and three `errors/gadt-ctor-index-rejects-*`. Original row: Narrowed 2026-09-19 -- exhaustiveness read the scrutinee's index, but a constructor application was the bare ADT, so an unannotated empty vector reached an arm-less head |
| ~~[union-tagged-union-c-emission](../archive/union-tagged-union-c-emission.md)~~ | low | **ARCHIVED 2026-09-11**: its four defects are fixed; the per-member union emission it asked for is a deferred enhancement tracked in the union guide. Original row: per-member C union emission still deferred -- and now much less urgent, because the cost it was filed to remove is gone. FOUR defects hid behind it, **all now fixed**: the unowned box at argument position, the aggregate match-arm compile error, float truncation, and (2026-09-05) two more found inside this entry's own "still open" note, which claimed a union bound to a local or returned "still leaks its box". (1a) It did not leak, it did not COMPILE -- `EX_UNION_INJECT` was inserted at call arguments and NOWHERE else, so a let-init was "invalid initializer" and a `defn` declared `: (Wide | int)` was "incompatible types when returning". `elab_coerce_to_union` is the union twin of `elab_coerce_to_any`, reached now from the ascription and from return position (which needed a `return_union_type` capture -- `return_kind` alone is a bare TY_UNION with no member list). (1b) The heap box then had no owner: 32,000 bytes in 1,000 blocks where the same program through `any` is clean, because `let_binding_any_freeable` was a FOURTH rule keyed on TY_ANY. Adding the key would have closed NOTHING -- the drop channel emits `__tur_any_drop`, whose id space (>= 1000) is disjoint from a union's member index, so it is a silent no-op. A union let needs no switch (the inject is in the initializer, so the tag is a compile-time fact); the channels carry drop STATEMENTS now instead of names, which reaches the early exits the tail-recursive repro leaks through |
| ~~tourist-ws-conn-adapter~~ | -- | **Resolved (already fixed at filing; docs updated 2026-09-04)**: the `tourist-ws` spice (`ws-route!` + `tourist-conn`, TOUR-V0, turmeric-spices commit `95eae7a4`, 2026-06-23) predates this report. Only the guide had drifted, still describing it as unbuilt future work. Archived to [docs/archive](../archive/tourist-ws-conn-adapter.md) |

`stdlib-dir-guard-accepts-mismatched-stdlib` was resolved 2026-09-02 and moved
to [docs/archive](../archive/stdlib-dir-guard-accepts-mismatched-stdlib.md) by
fix direction (1). `stdlib/VERSION` is written beside the top-level `VERSION`,
ships with the stdlib, and `resolve_stdlib_root` compares it against
`TUR_VERSION`; a mismatch names both versions and the variable.

It also closed the report's OTHER complaint, which only became visible once the
stamp existed: fix direction (2)'s "differs from the walk-up" notice fired even
when the two stdlibs were the SAME release, telling the user a mismatch "will
miscompile" when it demonstrably would not. The three verdicts are separated
now -- a confirmed MISMATCH gets the definite message, an UNKNOWN (unstamped)
stdlib falls back to the heuristic, and a confirmed MATCH is silent wherever it
sits. That last one is what makes deliberately pointing at another tree usable.

The real risk is the stamp rotting: a stale one makes a mismatched stdlib look
like a match AND makes the correct one warn, so it is worse than no stamp
because it is trusted. `tests/check-stdlib-version-stamp.sh` (ctest
`tur_stdlib_version_stamp`) fails on drift, emptiness, or absence, and the three
`cut-*-release` commands bump it alongside `VERSION`. Note the report's "Guides
to update" names `docs/guides/troubleshooting-guide.md`, which does not exist and
never has (checked against `origin/main`); the material went to
`docs/guides/tvm-guide.md`, since tvm is the tool that SETS the variable.

`spices-carry-pre-sum-option-result-layout` was resolved 2026-08-27 in
`rjungemann/turmeric-spices` ("migrate every spice off the pre-sum
Option/Result layout", merged as #59) and moved to
[docs/archive](../archive/spices-carry-pre-sum-option-result-layout.md). All
46 files across 12 spices were migrated -- the 126 hand-rolled inline-C struct
sites and the 35 `.is-some`/`.value`/`.ok-val` reads alike; see the Resolution
section at the bottom of the archived report. The corruption was reproduced
before it was fixed: `assert-ok` on a genuine `(ok 7)` printed "expected ok,
got err", because the retired layout's `is_ok = true` byte reads back as
`tag == 1`, which is Err. Measured across every spice suite that runs without
external C deps, 102 failing test files -> 75, zero regressions. That
migration is what unblocked the `parametric-sum-byvalue` graduation, whose
soak existed to avoid fixing the ABI before its heaviest client existed.
The hole that made this expensive to find is still open, and is not this
row's to close: `requires.spices` fixtures auto-skip without the sibling
checkout, so nothing in this repo's CI could have caught any of it.

`args-api-int-erased-handles` was resolved 2026-08-21 and moved to
[docs/archive](../archive/args-api-int-erased-handles.md). `ArgSpec` /
`ArgResult` are `defopaque` newtypes now, the option default is
`(Option cstr)`, and `args/sub-result` returns `(Option ArgResult)`; the
`args-defaults` fixture's `cstr->int` reinterpret helper is deleted. Two
notes worth carrying: an inline-C body **cannot** take a by-value
`(Option cstr)` -- it lowers to `tur_adt_Option__cstr`, not the `int64_t`
carrier `tur_is_some` accepts, so the option is peeled in a pure-Turmeric
wrapper -- and a `;;;` block binds to the NEXT definition, so an internal
helper slipped between a docstring and its public `defn` steals the
docstring in `docs/api/`. `argv` / `args/positional` stay `:int` on purpose:
they are the `*args*` cons list, which the elaborator itself declares as a
global `:int`.

`async-panic-task-boundary` was resolved 2026-08-21 and moved to
[docs/archive](../archive/async-panic-task-boundary.md), with its root cause
corrected: the async body runs **inline on the caller's stack**, so there was
no fiber whose `panic_jmpbuf` the fix could arm -- the boundary is a
`tur_handler_chain` node like `tur_catch_unwind_box`'s, and the emitted body's
existing `if (tur_panicking) return ...` checks do the unwinding. The other
half the report did not have: `await` on a rejected future used to `abort()`,
so rejecting the future alone would have deferred the same process death
rather than removing it. Awaiting a rejected task now re-raises the task's own
panic at the await, where a `catch-unwind` can catch it. The spawn-side frame
is the boundary, so a panic after a re-park is still outside it.

`match-nested-constructor-patterns` was resolved 2026-08-21 and moved to
[docs/archive](../archive/match-nested-constructor-patterns.md) -- by a
FORM-level rewrite in front of `elab_match`, not the decision-tree rewrite its
fix direction proposed, so the arm loop, exhaustiveness check, linear/borrow
machinery and codegen are untouched. Depth falls out of recursion: the inner
`match` forms the lowering emits go back through `elab_match`. Two
prerequisites were separate defects with their own repros, both fixed there: a
`!`-typed (`(panic ...)`) arm was rejected as incompatible with its peers, and
match arms did not rewind MOVE state the way `if` branches do, so consuming the
same value in two arms was a spurious TUR-E0005. One limitation found on the way
is filed above as `match-adt-var-arm-does-not-bind`.

`catch-unwind-aggregate-return-miscompiled` was resolved 2026-08-21 and moved
to [docs/archive](../archive/catch-unwind-aggregate-return-miscompiled.md); it
was filed 2026-08-20 and never got a row here. A catch boundary over a thunk
returning a by-value aggregate now calls it through a per-type BOXING
trampoline (`__tur_catchbox_<ctype>`) and `tur_catch_unwind_box_via` /
`tur_catch_panic_of_box_via`, instead of the `TUR_APPLY0` cast that read the
struct's return register as an `int64_t` and handed the consumer garbage to
dereference. The detail its root-cause section lacked: the thunk reaches
codegen as a `ptr<void>` fat handle, so the ascription has to be peeled to find
the `TY_FN` that carries the return type. Two follow-ons: this unblocks
`json-str-result-and-file-readers-missing`, and the INTERPRETER has the same
symptom by a different mechanism, filed as
`turi-catch-unwind-aggregate-payload` above.

`match-adt-var-arm-does-not-bind` was filed and resolved 2026-08-21 (found
while implementing nested patterns, whose group fallthrough emits exactly this
shape) and moved to
[docs/archive](../archive/match-adt-var-arm-does-not-bind.md). Both halves were
needed: the elaborator now binds the var arm in its own scope, and both ADT arm
emitters declare the C variable -- from `*__scrut`, `__scrut` or
`(T)(intptr_t)__scrut` depending on which of the three ways the scrutinee was
bound. No narrowing: a var arm is reached for any remaining variant, so there
is nothing to narrow to.

`ok-val-untyped-catch-box-loses-float` was filed and resolved 2026-08-21. One
branch in the erased-carrier field read: the erased `tur_adt_Result` declares
`int64_t ok_val` and a float payload rides in it as BITS, so reading it and
letting C convert int64 -> double converted the bit pattern numerically. It
reinterprets now, like the typed construction path. Note the `:heap`-ADT branch
two cases above carries a comment about the same trap but fixes it with a CAST
-- correct there (the monomorph cell really has a `double` field), wrong here.
Moved to
[docs/archive](../archive/ok-val-untyped-catch-box-loses-float.md).

`guides-two-arg-println-and-when-body` was filed and resolved 2026-08-21 and
moved to
[docs/archive](../archive/guides-two-arg-println-and-when-body.md). Every
`(println "label:" v)` in `docs/guides/` is gone -- frame-guide's 12 use the
two-call form rather than `str-concat`, since `tur-frame` is a sibling-repo
spice whose error payload type could not be verified here. The doc lint the
report proposes (extract fenced blocks, `tur check` the self-contained ones) is
NOT built; the `no-check` fence marker some guides already carry is the seed of
the opt-in convention it would need.

`type-of-cast-kind-granularity` was resolved 2026-08-21 and moved to
[docs/archive](../archive/type-of-cast-kind-granularity.md). An `any` box now
carries a per-monomorph id for a struct/ADT payload, so `(cast a OtherStruct)`
on a box holding a `Point` panics instead of reinterpreting, and `type-of`
names the type. One correction to the filed direction: the mangled-C-name
intern table it points at is the wrong key -- every carrier ADT's C name is
`int64_t`, so two ADTs would have collided; the key is `type_name()`. Two
mechanism notes: the id->name table cannot live in the preamble (ids are
per-program) NOR be a forward-declared per-program function (the S2 split
runtime compiles the preamble standalone), so it is installed through a
function pointer from `__tur_static_init`; and the interpreter, whose
`type-of` comment said it was deliberately matching the old kind granularity,
was updated in step.

`global-spice-library-consumption` was resolved 2026-08-21 and moved to
[docs/archive](../archive/global-spice-library-consumption.md). `#{:global
true}` resolves a dep through the `tur install` registry (`state.tur`), is
never fetched, and errors clearly when the spice is not installed. Two things
the filing did not anticipate: **four** resolution ladders had to learn the new
shape, not one (pkg.c's plus three in main.c, each carrying its own copy of the
workspace-sibling -> `:path` -> `spices/<name>-<ref>` chain), and the
`:global`+`:url` conflict has to be reported with `diag_emit` -- a bare
`fprintf` leaves the manifest ACCEPTED, since `pkg_manifest_read` judges the
read by `diag_had_error()`. Deliberately not done: `:global-policy`, version
validation / the `tur.lock` SHA (no range syntax to validate against yet), and
library-only installs (`tur install` still requires a `:bin`).

`httpd-mw-recover-unblocked-but-unwritten` was resolved 2026-08-21 and moved
to [docs/archive](../archive/httpd-mw-recover-unblocked-but-unwritten.md).
`mw-recover` ships as MW3 in `stdlib/httpd.tur`, pinned by
`tests/fixtures/httpd-mw-recover/` (two requests -- the panicking one and a
following good one, because "the server survived" is the property that
matters and a single request would pass even with the use-after-free). Three
of its four repros are fixed: (C)/(D) were one defect -- `collect_free_vars`
had **no case** for `EX_CATCH_UNWIND`/`EX_CATCH_PANIC_OF`, so a name used
only inside a catch thunk was never captured and the lifted thunk emitted an
undeclared identifier -- and (B) was the drop glue owning a `^fat` handle the
catch thunk only **borrows** from the frame it is created and dropped in.
Repro (A) did **not** fall out; it is re-filed, narrowed to a three-line
repro with no httpd and no `catch-unwind`, as
[let-returning-noncapturing-lambda-ices-at-merge-temp](../archive/let-returning-noncapturing-lambda-ices-at-merge-temp.md)
-- since resolved (2026-08-21) and archived.

Four rows were removed 2026-08-21 as **stale index entries**, not as new work:
`performance-guide-fictional-stdlib-api`,
`logic-guide-documents-unimplemented-backtracking-api`,
`datalog-examples-do-not-compile` and `tur-run-test-blocked-by-doctest-failures`
were all resolved and archived on 2026-08-20, but their table rows were left
behind -- so a triage pass read four open findings that were not open. Worth
noticing as a class: an index that can drift like this is worse than no index,
because it is trusted. Their archived files carry the resolutions
([performance-guide](../archive/performance-guide-fictional-stdlib-api.md),
[logic-guide](../archive/logic-guide-documents-unimplemented-backtracking-api.md),
[datalog](../archive/datalog-examples-do-not-compile.md),
[tur-run-test](../archive/tur-run-test-blocked-by-doctest-failures.md)).

Two of those archives were hiding live work, which is the reason the drift
mattered rather than just being untidy:

- `datalog-examples-do-not-compile` was archived **partially** resolved, with
  a "Remaining work" list inside it. Its item 2 ("reduce the
  undeclared-identifier codegen bug") turned out to be **two** codegen bugs,
  both now fixed: an inline-C block could not name a `let`-bound local (only a
  parameter), and a lifted lambda could not read a top-level `def` (Pass 1
  forward-declares functions, never global storage, and lifted lambdas are
  prepended to the item list). Both reduced to programs of under a dozen lines
  with no datalog in them; both fixed with **zero snapshot churn**; pinned by
  `tests/fixtures/inline-c-names-let-local/` and
  `tests/fixtures/global-def-read-by-lifted-lambda/`. The report's own guess --
  "a codegen scoping bug inside `sch_hydecode_hyrec_hy`" -- was wrong, and its
  "attribution unverified" note is closed: neither bug was branch-specific.
  The runtime residue is now the open row
  [examples-tree-does-not-run](../archive/examples-tree-does-not-run.md),
  filed and resolved the same day (see the note below).
- `logic-guide-documents-unimplemented-backtracking-api` was archived with its
  narrative sections still a design sketch, labelled as one in the file.

Archiving a PARTIALLY resolved report is what made both invisible. If a
resolution leaves work behind, the leftover belongs in a new `docs/reported/`
file with its own row -- not in a "Remaining work" heading inside
`docs/archive/`.

`examples-tree-does-not-run` was filed and resolved 2026-08-21 (same day) and
moved to [docs/archive](../archive/examples-tree-does-not-run.md). Everything
in `examples/` that compiles now also runs. Both of its runtime items were
example code, not the compiler, and the answers were cheap once anyone
actually looked: the four datalog segfaults were `return (int)vec->data[i];`
in hand-written inline C -- C's `int` is 32 bits, so a 64-bit datum pointer
came back truncated -- and `datalog.tur`'s TUR-E0201 was a `defdata` that
moves by default being used twice, which wants `:copy` (now documented in
`docs/guides/datalog-02-minimal-impl.md`, where a reader meets the trap).
`cli_args_demo.tur` was fictional twice over (`print` and a bare `getenv`,
neither of which exists) and is rewritten against the real `env/get`.
`cellular-automata.tur` checked clean but never linked -- its inline C called
sibling Turmeric functions by their unmangled names instead of
`__TUR_CNAME_<name>__`.

The lesson worth keeping is the ratchet's, not any individual fix:
`tests/check-examples.sh` only ever ran `tur check`, and `tur check` passing
was mistaken for "works" twice in this tree's history. It now RUNS every
example that checks clean and requires exit 0, and it fails on any sanitizer
line the Debug compiler prints while checking one -- which is how a live union
type-confusion in `emit_stmt` (a `(perform ...)` in statement position reading
`is_unsafe_marker` out of a `PerformExpr`) got found and fixed. That one could
not be pinned by a fixture, since whether the garbage byte is non-zero is
uninitialized memory; the sweep pins it instead.

Three rows above (`env-doctests-are-machine-dependent`,
`float-division-aborts-instead-of-ieee-inf`,
`user-defn-named-div-collides-with-libc`) were added 2026-08-21 by
`tests/check-reported-index.sh` doing its job on its first real merge: they
arrived from `main` in PRs #775 and #777 as report FILES with no rows here, and
the lint failed the build naming all three. That is the drift this index has
had twice before and could not previously detect -- it is now caught at the
merge that introduces it rather than at the next triage pass that happens to
notice.

## Value representation (the consolidation campaign)

The scoreboard for this family is the open-cells table in
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
not this file -- these each have a row there, and the guide carries the
matrix, the structural note about which `TypeKind` switch is authoritative, and
the plan links. File a new repr cell there as well as here.

| Report | Severity | One line |
|---|---|---|
| ~~[rc-deref-emits-control-block-pointer](../archive/rc-deref-emits-control-block-pointer.md)~~ | high | **RESOLVED 2026-09-25** (archived): `EX_DEREF` reads an rc's payload through `rc_get_value` in `EX_RC_OF`'s layout, `@` on an `rc<ADT>` keeps the ADT's def, and the interpreter reads the `__rc` pair's value; pinned by `tests/fixtures/rc-deref-reads-payload`. Original row: Filed 2026-09-23, pre-existing on `main`. `@x` on an `rc<T>` falls into `EX_DEREF`'s `ptr<T>` arm and yields the `RcControlBlock *` itself, so `(+ @x 1)` prints an address with no diagnostic. Needs a `TY_RC` arm that reads the payload |
| ~~let-alias-of-fn-param-captured-in-lambda~~ | -- | **Filed and resolved 2026-09-05**, in three lines, and the filing was wrong twice. The LAMBDA is not part of it: the minimal repro has no lambda and no capture -- `(defn use3 [f : (fn [int] Pair2)] (let [g f] (.a (apply2 g 5))))` SEGVs while `(apply2 f 5)` is fine. And the two "cases" are one defect, not two. A fn-typed param is fat either by the `^fat` ANNOTATION (`Binding.is_fat`) or by NORMALIZATION (`fn_param_type_is_fat_normalized`, keyed on the type); the let-alias propagation in elab_forms.c carried only the first, so an alias of a normalized nominal param lost the fact and the call-site pass-through re-shimmed it into a SECOND `__tur_fatshim` box. Its own comment claims to cover "a ^fat parameter (or a let-alias of one)" -- but its normalized arm requires `is_param`, which an alias is not. Carrying the fact on the alias fixes every guard keyed on `is_fat`, not one call site. An int-returning callback never failed, which is why it hid. Archived to [docs/archive](../archive/let-alias-of-fn-param-captured-in-lambda.md) |
| ~~inline-c-carrier-producer-byval-container-element~~ | -- | **Resolved 2026-08-30**: it was a FAMILY of five store sites, not one `vec-of` row -- match scrutinee, call argument, if-merge binding, ctor argument, escaping element -- each now bridged on the value's RECORDED emitted spelling, the mechanism the niche crossings established. The if-merge one was not a missing bridge at all: `emit_if_value` declared its by-value merge temp without recording its C type, unlike its sibling declarer, so the double-deref guard had nothing to look up. Archived to [docs/archive/inline-c-carrier-producer-byval-container-element.md](../archive/inline-c-carrier-producer-byval-container-element.md) |
| ~~option-niche-container-elements-box-at-parity~~ | -- | **Resolved 2026-09-03**: CE1/CE2 built -- `container_elem_form` is the chokepoint, a Vec element-store sink hands the bridge's niche row the payload word and a raw slot read is cast back, so a niche `(Option String)` element costs one 8-byte slot and no malloc. The container row reads 17.8 MB / 0.018 s against 79.7 MB / 0.08 s. TUR-E0714 refuses the one erased store shape that cannot decide the convention. Archived to [docs/archive](../archive/option-niche-container-elements-box-at-parity.md) |
| ~~generic-vec-read-wrapper-spec-returns-carrier-word~~ | -- | **Resolved 2026-09-04** along its own fix direction. `fn_return_needs_carrier_result_bridge` already emitted exactly the readback this wanted; it was gated on one tail shape (`expr_tail_is_catch_box`), so a raw container read in the same position never reached it. It now also answers yes for a raw-slot-read tail whose DECLARED result is a CE_BOX element -- declared, not `fd->body->type`, which reads back as plain `int` because `vec-get`'s own result is already the carrier and would answer about the wrong type. CE_WORD stays excluded (the niche slot word IS the value); niche and scalar controls are in the fixture. Reached past the report: `vec-pop!` had the same failure, and so did a bare by-value struct element. The three raw-slot readers are now one shared `emit_call_is_raw_slot_read`. Archived to [docs/archive](../archive/generic-vec-read-wrapper-spec-returns-carrier-word.md) |
| ~~eq-two-pair-monomorphs-sharing-a-component-cross-bind~~ | -- | **Resolved 2026-09-04**: not instance selection and not the spec KEY -- the callee emitted was right and only the ARGUMENT was bridged wrong. `find_matched_abi_spec`'s cross-spec fallback keys on the source `Expr*` alone, and a dict-dispatched call re-resolves its callee per monomorph, so the entry it grabbed named a clone of a DIFFERENT function: an `Eq[Option]` clone handed to a call whose callee is `Eq[int]`. The Expr* is shared; the callee is not, so the fallback now requires `spec->binding == fn_binding`. A first attempt guarded on "only one candidate" and did not fix it -- one candidate is not safer than two if it names the wrong function, which the trace showed and the count heuristic hid. Archived to [docs/archive](../archive/eq-two-pair-monomorphs-sharing-a-component-cross-bind.md) |
| ~~eq-on-pair-of-niche-option-segfaults~~ | -- | **Resolved 2026-09-04**: the NESTED-dispatch spec path forced only parameter 0 to the resolved receiver -- right for a method whose other params are elements, wrong for a BINARY method like `(eq? [x y])` whose second param is also the class variable, which is not an element tyvar and so kept the erased carrier. One `Eq[Option]` spec came out `(void *, int64_t)`, matching x as a niche and y as a tagged box. A param declared with the receiver's own erased type IS the class var (the tyvar was erased at instance elab) and resolves with it. Never niche-specific: `(Pair (Vec int) int)` minted the identical `(concrete, carrier)` shape and merely WARNED, since both are pointers to one layout -- the crash was the visible tail. Suite 2787/0 with **zero snapshot drift**, which for a default-path spec-minting change is the point of running it. Archived to [docs/archive](../archive/eq-on-pair-of-niche-option-segfaults.md) || ~~pair-eq-macro-applies-comparator-directly~~ | -- | **Resolved 2026-09-04** by none of its three filed directions, all of which assumed the missing thing was a CONTRACT. A direct application needs none: the argument is at the call, and a concrete niche-typed argument IS the payload word, so the convention is read off the value rather than promised by a callee. Marks per PARAMETER, not per lambda -- `pair-eq?`'s two comparators take a niche component and an `int` respectively. Archived to [docs/archive](../archive/pair-eq-macro-applies-comparator-directly.md) || ~~typed-comparator-over-hamt-box-compares-box-addresses~~ | -- | **Resolved 2026-09-04**: the mirror image needed a mirrored mechanism. The word half marks an ERASED param so its ascription reinterprets; the box half now marks a param DECLARED as a niche option (`Binding.arrives_as_carrier_box`) so it arrives as the carrier and unboxes at body entry -- the same shape as the B4 wide-by-value box load, borrowing the same way. Cost the report did not anticipate: the forward declaration needed its own branch, since B4 keeps its two sides in step through a TYPE-level predicate a per-binding mark cannot reuse. Also fixed: `option-eq?`'s `expected (fn [int int] : bool), got (fn [int int] : bool)` was not a convention bug but `type_name_buf` rendering fn params from their KIND alone, collapsing every composite and every tyvar -- it prints full param/result types now, improving every fn-type diagnostic. Archived to [docs/archive](../archive/typed-comparator-over-hamt-box-compares-box-addresses.md) || ~~erased-closure-param-over-niche-vec-slot-reads-box~~ | -- | **Resolved 2026-09-04**, and its own scope claim CORRECTED the same day (the row above): the synthesized `(eq? a b)` comparator was marked for `Vec` alone, so `(eq? (some (some "aa")) (some (some "bb")))` -- the DEFAULT equality operator on a nested niche option -- answered true. `container_helper_passes_slot_words` replaces that name test, and `list-eq?` / `result-eq?` join `vec-eq?` at the user-written site. Both fix directions, each where the other could not reach. A comparator written AT the `vec-eq?` call is minted for that argument and reachable from nowhere else, so it now carries the same `__cmp_slot_` mark the synthesized one does (capturing as an `EX_CLOSURE`, captureless through its lifted `__fn_N` -- `is_lifted_lambda`); the mark is the emitted NAME because the value table is program-scoped, and a flag-plus-`emit_slot_word_mark` would have made every unrelated local named `a` a slot word. A NAMED comparator cannot be marked -- another caller may hand it real boxes -- so erased params there are refused with TUR-E0715, the read-side twin of E0714; it fires on zero of 2784 fixtures. The filed repro understated it: with DIFFERENT payloads the broken path printed `eq`, a wrong answer rather than a right one reached wrongly. Archived to [docs/archive](../archive/erased-closure-param-over-niche-vec-slot-reads-box.md) |
| ~~sr4-byvalue-recursive-sum-walk-copies-per-link~~ | -- | **Resolved 2026-09-05.** The profile the report asked for found the cost was not the copies but a RECURSION GCC could not flatten: the binder copied the boxed link into a local and the self-call took `&rest`, pinning the frame (callgrind: 898k genuine self-calls, ~50 instructions/link, versus the carrier compiled to a loop at ~14). The match binder for a wide boxed recursive field now BORROWS the box as `const T *` and registers as pass-by-pointer, so the tail call is a jump again: walk at n=512 3804 -> 581 ns/op, and `bench-logic-subst`'s by-value/carrier ratio 4.77x -> 1.01x (parity across the sweep). Construction was never the gap (`benchmarks/bench-logic-subst-split.tur`). Archived to [docs/archive](../archive/sr4-byvalue-recursive-sum-walk-copies-per-link.md) |
| ~~typeclass-constrained-param-erases-adt-to-int64~~ | -- | **Resolved 2026-09-02**: the bare `x` in `[^Show a x]` was never the constrained type -- an untyped parameter defaulted to `int`, so the body was one erased int64 function whose `(show x)` bound whichever instance the carrier fallback picked (cc failure at a by-value ADT, silent `int` with an int instance present). Bare parameters directly following a constraint binder now take the binder's type variable, as if written `x : a`, and each aggregate instantiation gets its own specialized clone. Archived to [docs/archive](../archive/typeclass-constrained-param-erases-adt-to-int64.md) |

`option-niche-inline-c-carrier-crossings-incomplete` was resolved 2026-08-28
(same day it was filed) and moved to
[docs/archive](../archive/option-niche-inline-c-carrier-crossings-incomplete.md):
both named positions bridged, plus two more the audit found (`vec-of` first
element heap-promoted to a `P **` cell; `vec-push!` double-boxing an
already-carrier value), capture audited clean, rest args unreachable by the
annotation grammar. Pinned by `tests/fixtures/option-niche-crossings`. The
default-path row above is the one adjacent finding that stayed open.

Both rows that were here before the consolidation campaign are archived
(2026-08-21).

`byvalue-product-tail-var-double-unboxed-nonparametric` was resolved
2026-08-21 and moved to
[docs/archive](../archive/byvalue-product-tail-var-double-unboxed-nonparametric.md),
along the fix direction it filed. `emit_arm_is_recorded_byval_agg()` gates the
carrier->concrete bridge in both `emit_if` arms on what the localvar side table
records the value's representation to be HERE, rather than on what its type is
-- which is why the ten-fixture regression the report measured for a TYPE-level
widening does not occur: at the vec/map element and assoc-type seams the
recorded type IS the carrier, so those fixtures still get the bridge they need.
All ten pass unchanged, no snapshot regenerated, suite 2690 passed / 0 failed.
Pinned by `tests/fixtures/byvalue-product-tail-var-nonparametric/`, which
asserts field values (a double-unbox that type-checked would still read wrong
bytes) and covers the then arm and the parametric half too.

`let-returning-noncapturing-lambda-ices-at-merge-temp` was resolved 2026-08-21
and moved to
[docs/archive](../archive/let-returning-noncapturing-lambda-ices-at-merge-temp.md),
with its diagnosis inverted. The value was **not** a bare fn pointer needing a
shim: the tail already built a proper fat box, and the merge TEMP was declared
thin (`int64_t (*)(int64_t)`), so `repr_of` was right and the declaration was
wrong. Nor was it benign -- the exit-0 repro hid it, but a variant that returns
and CALLS the closure emits `-Wint-conversion` on the temp assignment, i.e. a
hard error under GCC >= 14. Root cause was two sites asking different
questions: `emit_temp_decl` keys fat-vs-thin off `type.as.fn.boxed` (a TYPE
fact), stage-2 tail normalization off `fn_result_type_is_fat_normalized` (a
POSITION fact). The merge temp asks `repr_of` in RESULT position now. `do` and
the direct return escaped only because the shadow check hangs off the `let`
path's bridge, not because they were spelled correctly. Zero fixtures
regenerated (the `void * name` spacing matches the generic path deliberately);
pinned by `tests/fixtures/let-tail-noncapturing-lambda-fat-temp/`.

`mut-map-reassign-missing-spec-link-error` was resolved 2026-08-16 (filed
and fixed the same day, both defects along its own fix directions) and moved
to [docs/archive](../archive/mut-map-reassign-missing-spec-link-error.md):
`emit_abi_scan_expr` gained its missing `EX_SET` case (a generic call in a
`set!` RHS was the one statement position the spec-materialization walk
never descended into), and chokepoint 1's concrete-heap rule was extracted
to `emit_repr_concrete_heap_ptr_c_name` and shared with the merge-temp decl
+ ctype mirror, closing the seam the R3 ICE caught.  Zero snapshot churn --
the respelling fires only for shapes that previously ICE'd.  Pinned by
`tests/fixtures/mut-map-reassign/`.

`poly-result-hof-capturing-closure-sigbus` was resolved 2026-08-16 -- its
LAST row (the effect-row signature), by exactly the CPS increment its own
status bullet specified -- and moved to
[docs/archive](../archive/poly-result-hof-capturing-closure-sigbus.md).
Effect-annotated fn params are now fat-normalized like every other nominal
fn param: the E2a registry call sites dispatch fat (slot 0 = a registered
capturing-lambda entry with an env-taking `__cps` twin, slot 1 = the
fatshim's stashed bare-fn entry), threadable capturing lambdas are
CPS-admitted with the direct thunk's env-unpack preamble, and the
effect_check walkers peel the shim so all five `errors/effect-*` negatives
keep diagnosing.  The fix reached past the report: a capturing PERFORMING
callback -- previously no working spelling at all -- now threads the
handler chain (pinned at value 37 in
`tests/fixtures/effect-capturing-closure-thin-param/`).

`generic-closure-return-type-app` was resolved 2026-08-16 (both defects) and
moved to
[docs/archive](../archive/generic-closure-return-type-app.md): Defect A by the
report's own "narrower change" (a result-graft recovery at the thunk-type
clobber in `elab_call.c`, leaving the grounding gate untouched), Defect B by
making the per-spec inner-closure clone fire for type-app results and be the
thing actually invoked (`inner_app` trigger + clone-body scan in
`emit_module.c`, head-keyed clone resolution via a `closure_head_init` stash).
The parametric backtracking monad it blocked now compiles, links, and runs
cast-free; `docs/guides/logic-programming-guide.md` was promoted to it in the
same change.

`fat-sink-shim-box-leaks-per-call` was resolved 2026-08-13 and moved to
[docs/archive](../archive/fat-sink-shim-box-leaks-per-call.md). It needed no
ownership annotation after all: dropping a fat handle goes through
`TUR_CLOSURE_DROP`, a C macro reachable only from inline-C, and any body with
inline-C already has `nonretain_param_mask == 0` -- so a set bit already means
"neither retains nor drops", which is exactly the fact the proposed annotation
was to supply. Note the report's own measurement conflates two allocations: its
recursive repro also allocates a CPS continuation env per call, so the fix looks
like ~15% there. The `while`-loop form isolates the shim and goes 109 MiB ->
1.3 MiB flat over 4e6 iterations. Two others have
since been resolved and moved to [docs/archive](../archive/):
`macos-int-conversion-carrier-pointer-straddles` (2026-08-01) and
`contract-type-arg-not-peeled-to-base` (2026-08-01, fixed by
`rt_peel_type_arg_contract` + `TUR-W0380`, which also unblocked `TY_CONTRACT`
joining `type_has_concrete_codegen_layout`); both resolution notes are
closed-cells rows in the guide.

`borrow-param-passed-as-unique-mut-undiagnosed` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/borrow-param-passed-as-unique-mut-undiagnosed.md). It
was never part of that campaign -- a uniqueness/borrow-checking gap, not a
representation one. Root cause, which the report left open: the UT2 check reads
`scope_borrow_conflicts`, which sees only borrows registered in THIS frame by an
explicit `(& v)`; a `^borrow` parameter registers nothing there and correctly so,
since its aliasing happened one frame up. Fixed by the narrower of the two
options it weighs -- reject the `^unique ^mut` crossing on a `^borrow`-moded
binding directly, rather than registering `^borrow` params as frame-live borrows,
which would have fed every other borrow check too. No existing fixture changed,
so the rejected shape was not in use anywhere in the corpus.

## Effect handlers

*(No open reports.)*

`cps-body-panic-not-propagated` was found and fixed 2026-09-11 and filed
straight into [docs/archive](../archive/cps-body-panic-not-propagated.md): on
the DK/CPS path every panic-signal check emitted only a comment, so a
CPS-colored function whose callee panicked under `catch-unwind` -- or which
panicked itself -- ran the rest of its body and its whole continuation before
the catch saw the flag. It was the real cause behind the "generic vs mono"
asymmetry in `defer-in-generic-hof-skipped-on-caught-panic`.

`cps-direct-bt-scope-closure-temp-undeclared` was filed and resolved 2026-09-04
and moved to
[docs/archive](../archive/cps-direct-bt-scope-closure-temp-undeclared.md). Its
filed root cause was WRONG and the archive corrects it: not the `cps->direct`
bridge dropping operand statements, but `pap_register_let` classifying the
hoisted closure as an inlinable partial application and dropping it, because the
pap check delegated its "the var appears only as a call callee" proof to
`closure_binding_escapes` -- which answers whether an env may be freed at scope
exit and deliberately clears a value passed to a non-retaining fn param.
`--dump-cps` refuted the emitter hypothesis in one line (the IR itself named a
free variable); reading emitted C and reasoning backwards is what cost the wrong
guess. A second, plausible-looking guard on the closure's arity was tried and
REMOVED -- `FnDef.n_params` counts the lifted env param, so it would have
rejected genuine paps while fixing nothing.

`handler-clause-setbang-enclosing-mut-undeclared` was resolved 2026-08-05 and
moved to
[docs/archive](../archive/handler-clause-setbang-enclosing-mut-undeclared.md).
Two corrections are recorded there: the area was the CPS/DK backend, not
`emit_effects.c`, and the read side the report called "fine" was in fact a
SILENT wrong answer (a clause read the value snapshotted when the handle was
installed -- `5` where the answer is `7`), which is why it archived at high
rather than medium severity. Both directions had one cause: a clause is its own
C function and saw enclosing mutables only by value. The fix widens the existing
B7 cell promotion to any `^mut` a clause touches, types the cell (an `int64_t`
cell truncated a `^mut` float -- `10.1` read back as `9`), and derefs at the two
existing chokepoints (`atom_var` for reads, `emit_set_stmt` for writes) so a
`set!` inside a delegated `while` is covered too. Pinned both-paths by
`tests/fixtures/effect-handler-clause-setbang-enclosing-mut/`.

`handler-clause-statement-if-ices-emitter` was resolved 2026-08-05 in two
landings (statement-position `if`/`when`, then `CT_LOOP` in a handler case --
the multi-shot fold included) and moved to
[docs/archive](../archive/handler-clause-statement-if-ices-emitter.md). Its
archived note records that it was three root causes, none of them the CPS
*coloring* the report pointed at -- the mechanism was CPS *admission*, the
same family as the remaining row's capture admission. One narrow eviction
survives by design (a `perform` of an outer effect inside a loop inside a
clause), with a located diagnostic and its own delete-me-if-admitted error
fixture.

`cps-multishot-nontail-resume-inner-handle-drops-clause-rest` (a multishot
non-tail resume across a nested handle printed `2`/`20` where the answer is
`22`) was resolved 2026-08-05 in two layers -- `dk_invoke` trampoline scoping,
then unifying the handle chain's two spines (the handle-continuation frame is
a `DKK_RESUME_FRAME` whose `next` is the actual, borrowed enclosing chain) --
and moved to
[docs/archive](../archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md).
Both paths now agree on every boundary variant, pinned by
`tests/fixtures/effect-multishot-nontail-resume-inner-handle/`.

`cps-case-reopen-marker-kont-truncates-capture` (the remaining two-spine
instance: a case that RE-OPENS an outer effect got a marker-copy `__kont`,
truncating the outer multishot capture -- `1025` for `2025` -- and letting a
tail-resume longjmp discard the C-stack pending delivery entirely -- silent
exit 14 for `1014`) was resolved 2026-08-05 the same day it was filed: the
case's `__kont` is now the real borrowed chain (`dk_case_enclosing_real`; the
marker variant is deleted) and re-opening cases deliver their own value
through it under the `case_delivers` protocol, so `dk_perform` no longer
delivers `H->next` a second time. Moved to
[docs/archive](../archive/cps-case-reopen-marker-kont-truncates-capture.md);
pinned both-paths by `tests/fixtures/effect-case-reopen-outer-capture/`.

The two LATENT two-spine layouts filed from the same audit were converted the
same day and archived:
[cps-reset-frame-pre-unification-layout](../archive/cps-reset-frame-pre-unification-layout.md)
(the reset continuation frame) and
[cps-await-cont-baked-env](../archive/cps-await-cont-baked-env.md) (the
bounded await continuation; NOT purely mechanical -- the frame rides below a
shift, so the conversion widened the shift's capture extent to a
self-contained copy of the real chain, verified against the async-await-cps*
fixtures). With them, `LH_RESET_CONT` and the `__k` env-slot machinery are
deleted from the emitter: every lifted continuation frame now receives its
downstream chain at run time, and nothing can bake an original-chain pointer
into a frame env again.

## Interpreter (`--interpret` / `tur repl`) divergence

`turi-defer-fires-before-tail-call` was found and fixed 2026-09-11 and filed
straight into [docs/archive](../archive/turi-defer-fires-before-tail-call.md):
the interpreter's frame-reusing tail call fired the activation's defers before
entering the callee, so a `defer` cleanup ran ahead of the tail-called work
(and a defer-based `bt-scope` undid the trail before the body wrote it). The
reuse is now gated on an empty defer chain, as the compiler's "defers break
tail" rule already is.

| Report | Severity | One line |
| --- | --- | --- |
| ~~turi-show-instance-with-inline-c-body-prints-a-pointer~~ | -- | **Resolved 2026-09-09**, and the report had located the wrong bypass: `turi_call_show_named` is the println-on-a-struct route and never ran. The real one is `interpreter_natives.c` registering stdlib's inline-C `Show` instances as natives under the elaborator's MANGLED INSTANCE NAMES (`__inst_Show_show_float`), plus eval.c's "keep native override" branch keeping such a native for ANY same-keyed impl -- so a user `Show`/`show` over `float` never registered its own body and ran stdlib's native, whose owned-`String` handle the user's `cstr` method handed to println as a number. That is the whole isolation table. The branch now keeps an `__inst_` native only for an impl whose defining FILE is under `stdlib/` (not `in_stdlib_load`, false for an explicit load); a user impl falls through to its own closure, so the `%.2f` body reports the clean diagnostic and the `%lld` body is CLAIMED by the simple executor and answers `int:735` -- the other honest outcome, which the harness accepts. Plain-defn natives keep their documented override-by-name. Dedicated runner `tests/run-interp-show-inline-c.sh` (ctest `tur_interp_show_inline_c`): four isolation rows, pure user Show, compiled, and stdlib's own `show-line` on a float. Archived to [docs/archive](../archive/turi-show-instance-with-inline-c-body-prints-a-pointer.md) |
| ~~separator-fold-collides-emitted-c-names~~ | -- | **Resolved 2026-09-02**: ADT and constructor names use the injective mangler (`-` -> `_hy`, `_` -> `_un`), so the `_` / `__` joiners in `ctor_<Adt>_<Ctor>` and monomorph type names are structural only; zero snapshots moved (every in-tree name is plain alphanumerics). Archived to [docs/archive](../archive/separator-fold-collides-emitted-c-names.md) |
| ~~option-niche-graduation-breaks-carrier-some-null~~ | -- | **Resolved 2026-09-02**: the `Some(NULL)` break has its release-notes entry, under `CHANGELOG.md` `[Unreleased]` as an announced-ahead breaking change; the sr3 plan's hold reason 2 records it as satisfied. Archived to [docs/archive](../archive/option-niche-graduation-breaks-carrier-some-null.md) |
| ~~json-str-result-and-file-readers-missing~~ | -- | **Resolved 2026-09-02**: `#json-str?<T>` landed 2026-08-21; `#json-file<T>` / `#json-file?<T>` now expand over `json/decode-file!`, which reads, parses and frees the buffer itself and panics (catchably, with the path) on an unreadable file; interpreter native added. Archived to [docs/archive](../archive/json-str-result-and-file-readers-missing.md) |
| ~~minikanren-example-implements-no-minikanren~~ | -- | **Resolved 2026-09-02**: the example is a miniKanren over `stdlib/logic.tur` now -- `parento` / `grandparento` queried in every direction and `appendo` run forwards, backwards and with both inputs unknown -- and the logic guide's section shows that code. Archived to [docs/archive](../archive/minikanren-example-implements-no-minikanren.md) |
| ~~try-turmeric-browser-suites-green-while-failing~~ | -- | **Resolved 2026-09-02**: the Playwright report is uploaded whenever the job is not cancelled, the desktop/mobile suites write JUnit that `collect-playwright-timings.py` turns into honest `web_desktop` / `web_mobile` rows for `/ci`, the job summary and a warning annotation name a failed non-blocking suite, and the three test-side failures (unscoped minimap selectors, a non-configurable `location.reload` stub) are fixed; the mobile WebKit reload failure is tracked in `webkit-sw-controlled-reload-fails-wasm-init`. Archived to [docs/archive](../archive/try-turmeric-browser-suites-green-while-failing.md) |
| ~~turi-suite-accounting-and-reporting-gaps~~ | -- | **Resolved 2026-09-02**: every marker skip and error-pass skip writes a result, one marker helper serves both passes, `expected.stderr` counts as a needle and a needle-less `errors/` fixture is a loud FAIL, the dead denylist entry is gone and a stale one is a startup error, the async scripts are no longer double-counted, and the summary is a census (`of D discovered`, `TUR_SKIP_PARTIAL`, accounting check) that the CI timings ingest parses. Archived to [docs/archive](../archive/turi-suite-accounting-and-reporting-gaps.md) |
| ~~guestbook-example-has-no-import-graph~~ | -- | **Resolved 2026-09-02**: rewritten as a spice (build.tur + `:c-sources` httpd.c, one `defmodule` per file), in the shipped language, with one serializable continuation per page resumed by `POST /submit?k=TOKEN`; `tests/run-guestbook.sh` walks the flow with curl (10/10); the defmodule serial-prelude defect and the anonymous context rejections fixed on the way. Archived to [docs/archive](../archive/guestbook-example-has-no-import-graph.md) |
| ~~serializable-continuations-aspirational-surface~~ | -- | **Resolved 2026-09-02**: `serial-cont->bytes` / `bytes->serial-cont` (Result-returning, validates frame names against this program's registry) / `serial-resume` built in `stdlib/serial.tur`; the guide's Overview/Surface API/Examples/Error Handling rewritten around the shipped `(serial-shift handler default)` form and `serial-cont` type; web guides updated. Archived to [docs/archive](../archive/serializable-continuations-aspirational-surface.md) |
| ~~image-dumps-globals-registry-missing~~ | -- | **Resolved 2026-09-02**: plan AI3 built -- `defimage-global` + `image/track-globals!` registry, globals snapshot written as a second TSER section (header `globals_offset`) and restored before resume, `TUR-W0706` lint on the `init` root of `with-image-cache-after-init`; stdlib float `deserialize` fixed on the way. Archived to [docs/archive](../archive/image-dumps-globals-registry-missing.md) |
| ~~debugger-and-tracer-only-instrument-main~~ | -- | **Resolved 2026-09-02**: a program with no `main` is now debugged and traced as a top-level program -- the CLI launch arms the debugger around the load itself (the wasm glue already did), so stopOnEntry stops on the first form, breakpoints in called functions fire, and the recorder records steps. Top-level DAP scenario and trace case added. Archived to [docs/archive](../archive/debugger-and-tracer-only-instrument-main.md) |

`turi-catch-unwind-aggregate-payload` was filed and resolved 2026-08-21 and
moved to
[docs/archive](../archive/turi-catch-unwind-aggregate-payload.md). One line:
`turi_ok_result_box` took a bare `int64_t` and always built the 3-int box --
the flattening `native_ok`'s own comment describes and avoids -- so the
catch-unwind boundary lost the tag of every heap payload. It takes a
`TuriValue` and applies the same rule now. Wider than the struct repro it was
filed for: `cstr` came back as a pointer and `float` was tag-flattened too.
Both `catch-unwind-aggregate-thunk` and `schema-reader-json-str-result` dropped
their `requires.compiled` markers.

`interp-hkt-pure-return-dispatch-elab-error` was resolved 2026-08-17, the
day after filing, and moved to
[docs/archive](../archive/interp-hkt-pure-return-dispatch-elab-error.md).
Its root-cause direction was wrong: no elaboration flag was involved -- the
fixture's `mk-box` collides with the stdlib MapKey method of the same name,
and the interpreter's `(load ...)`-based stdlib preload registered every
typeclass with `from_stdlib = false`, so the "user defn overrides a stdlib
method" resolution flipped to the method.  Fixed by marking preload turns
(`g_turi_stdlib_preload`); the whole hand-run hkt-constrained family now
passes under `--interpret`.

The first absorbed two symptom reports on 2026-08-01
(`turi-hkt-constrained-byvalue-bind-pure-wrong-values`,
`turi-hkt-byvalue-bind-pure-wrong-value`, both now in `docs/archive/`). It is
the single red line in `tests/run-turi.sh`.

Another was resolved 2026-08-05:
[turi-multishot-resume-in-while-aborts](../archive/turi-multishot-resume-in-while-aborts.md)
-- turi aborted on a multi-shot resume from inside a `while`, because the loop
was a black box to the work-stack driver and forced the handle onto the
single-shot fiber. `while` is now driven (`DK_WHILE`), and the fiber fallback
reports instead of `abort()`ing when it is genuinely reached.

[turi-ws-capturable-stale-black-box-arms](../archive/turi-ws-capturable-stale-black-box-arms.md)
was that defect's sibling, resolved the same day: the stale arms now recurse,
match scrutinees / perform args / resume's `k` are driven (DK_MATCH_SCRUT /
DK_PERFORM_ARG / DK_RESUME_K), and `TURI_TRACE_FIBER_FALLBACK=1` names the
form behind any remaining fallback.  Its archived note keeps the map of what
is still fiber-only and why -- read it before touching `ws_capturable`; in
particular it separates the safely-descendable forms from the ones whose
frames carry a heap boundary `clone_ws_slice` would double-free (reset /
catch-unwind / atomically), which remain open by design.  Executing it also
exposed a compiled-path miscompile, filed under "Effect handlers" above.

`ascribe-bool-to-int-prints-differently-per-path` was resolved 2026-08-06 and
moved to
[docs/archive](../archive/ascribe-bool-to-int-prints-differently-per-path.md).
It reached all ten numeric ascription targets, not just `:int`. The archived
note records two natural-looking fixes that are wrong: converting at the
ascription loses the element type for later method dispatch (the elaborator
synthesizes an int-carrier ascription for an ordinary push of a bool into a
`(Vec bool)`, and two fixtures printed the wrong instance's answer), and
mirroring the existing int -> float re-tag with float -> int fails 16 fixtures,
because an `:int` ascription over a float is the CARRIER spelling in generic
code rather than a request to expose the bits. Fixed instead at the rendering
site: `println` is overload-resolved by static type, so the elaborated AST
already records which shape `(:: b :int)` selected, and the interpreter's
tag-dispatch now yields to that shape in the one case where it is strictly more
informative. Pinned by `tests/fixtures/ascribe-bool-to-numeric-prints/`.

## Surface / expressiveness

Two open findings, both filed 2026-09-09 while answering saffron-lang-plan D8's
design questions; neither involves Saffron.

| Report | Severity | One line |
| --- | --- | --- |
| ~~typeclass-default-methods-do-not-work~~ | -- | **Resolved 2026-09-09.** The default is no longer elaborated at the class; it is kept as a FORM and spliced in by definstance for an omitted method, so it elaborates as an ordinary instance method with a concrete receiver and resolvable siblings. All four failing shapes pass on both back ends (`typeclass-default-method`), the omitted-method error names the method, Saffron dispatches a defaulted method dynamically (`saffron-dyn-default-method` -- D8 Q2), and the guide says `.lt?`. [Archived](../archive/typeclass-default-methods-do-not-work.md) |
| ~~[fn-typed-param-forwarded-to-a-later-defn-miscasts](../archive/fn-typed-param-forwarded-to-a-later-defn-miscasts.md)~~ | medium-high | **RESOLVED 2026-09-12** (same day). Forwarding an `(fn ...)`-typed parameter to a defn defined LATER in the file emitted a carrier cast onto a `tur_poly_fn_t` (a struct) and failed in cc; the identical program with the two functions swapped compiled. Root cause was NOT the stale-signature-table guess in the original writeup: instrumenting both orders showed the emitter already skips the cast for an `EX_POLY_WRAP` arg, and the elaborator inserts that wrapper only when the callee is known at elaboration time -- so a forward reference left a bare `EX_VAR` whose binding was already `is_poly_fn`. Fixed by excluding that case from `needs_fn_cast` in `emit_expr.c`. Verified by restoring `crdt/ormap` to natural caller-first order. Pinned by `fn-param-forwarded-to-later-defn` (asserts BOTH orders). |
| ~~[defopaque-over-sym-skips-the-ptr-bridge](../archive/defopaque-over-sym-skips-the-ptr-bridge.md)~~ | medium | **RESOLVED 2026-09-16** (archived): `opaque_base_is_ptr` is the "is this carrier a pointer" judgement now, not "is it spelled :ptr" -- `:cstr` and `:Sym` bases take the `void *` spelling and the existing opaque-pointer seams cover both directions; the ascription into the newtype casts a qualified pointer inner explicitly. `:non-null` stays `:ptr`-only. Pinned by `defopaque-over-sym-and-cstr`. Original row: A `defopaque` newtype over `:Sym` stores the symbol POINTER straight into an `int64_t` slot [-Wint-conversion], a hard build failure on the macOS leg; the same with `:cstr`
| ~~[generic-defn-forward-reference-wrong-arity](../archive/generic-defn-forward-reference-wrong-arity.md)~~ | medium-high | **RESOLVED 2026-09-12.** A forward-referenced GENERIC defn was declared with the arity of its TYPE-parameter vector: `(defn f [V] [a b] ...)` called from above reported "returns int, which is not callable -- did you mean to pass all 1 argument(s)?". Defining the callee first avoided it, so it read as "generics cannot be forward referenced". Both pre-passes had it: `elab_toplevel.c`'s arity scan used `name_idx + 1` though its own return-type probe already skipped the type-param vec, and `elab_module.c`'s defmodule half did not know about type-param vectors at all -- getting the RETURN type wrong too. Pinned by `generic-defn-forward-reference` and `-defmodule`. |
| ~~[bare-parametric-heap-base-repr-disagreement](../archive/bare-parametric-heap-base-repr-disagreement.md)~~ | medium | **RESOLVED 2026-09-12.** `repr_of` reported HEAP_PTR for a BARE parametric `:heap` base (`ORMap` with its args dropped, as it appears in a generic body) while the merge-temp emitter declared the erased carrier -- ICE `want=heap-ptr got=carrier-i64`. The `TY_APP` spelling of the same erased-declaration fact had a carve-out; the bare-base spelling did not, and the `TY_APP` test cannot see it since there are no args left to find a tyvar in. Also fixes a duplicate typedef: the forward (`TUR_FWD_`) and full-layout (`TUR_TD_`) guards protected one typedef name, so each let the other through and cc warned -Wtypedef-redefinition on emitted code. Pinned by `heap-parametric-struct-constrained-join`. |
| ~~[constrained-generic-as-fn-value-collapses](../archive/constrained-generic-as-fn-value-collapses.md)~~ | medium-high | **RESOLVED 2026-09-12.** A constrained generic passed as a FUNCTION VALUE (`(fold vjoin a b)`) collapsed every instantiation onto one instance, while calling it directly was correct at each. Three layers: the enclosing generic must specialize though the generic is an ARGUMENT (and both probes missed it -- a poly-slot argument is wrapped in `EX_POLY_WRAP`); the fn value needs per-instantiation clones (`emit_abi_scan_fn_values` bailed on `!abi_changes`, and same-carrier newtypes share an ABI); and the `tur_poly_fn_t` literal named ONE elaboration-time wrapper hardcoding one callee -- now a wrapper variant per clone, chosen by the enclosing spec's type bindings. Also fixed a latent heap-use-after-free in `emit_abi_intern_spec` (it copied caller `bindings`/`arg_types` AFTER growing the array those may point into). Removed the duplicated fold from `crdt/ormap`. Pinned by `constrained-generic-instance-inheritance`; interpreter half recorded on the turi report. |
| ~~[typeclass-constrained-relay-dispatch](../archive/typeclass-constrained-relay-dispatch.md)~~ | high | **RESOLVED 2026-09-12.** A constrained generic that dispatches only INDIRECTLY -- its body calls another constrained generic rather than a class method -- was never specialized, so it called the BASE callee, which bakes the last-declared instance. Two instantiations, one answer. The same callee was correct when called directly, which is what hid it. Fixed by asking the callee the same dispatch question with this specialization's bindings substituted in (depth-capped; requires a type argument to have actually become concrete). Zero codegen snapshot churn across the suite, so it fires only where dispatch was being lost. Pinned by `typeclass-constrained-relay-dispatch` (direct + one hop + two hops). |
| ~~[phantom-constrained-generic-base-body-picks-aggregate-instance](../archive/phantom-constrained-generic-base-body-picks-aggregate-instance.md)~~ | medium-high | **RESOLVED 2026-09-12.** A constrained generic over a PHANTOM type parameter failed to compile -- TUR-E0295 naming a type the function never mentions -- whenever the class had any by-value aggregate instance in scope, because the GENERIC BASE body must give the phantom var some type and picked a representative. The receiver-directed representative search had tiers for `int` and carrier-compatible SCALARS but none for a `defopaque` newtype over int, so it fell through to a generic search that landed on the aggregate -- even though the class had a usable carrier-shaped instance. Fixed by adding that tier (the return-directed twin has had it for some time). Unblocked crdt-spice-plan 2.3's constrained `(ORMap V)`, which now ships. Pinned by `typeclass-opaque-representative-vs-aggregate`. |
| ~~[phantom-type-param-does-not-drive-monomorphization](../archive/phantom-type-param-does-not-drive-monomorphization.md)~~ | high | **RESOLVED 2026-09-12** (same day). A constrained generic whose class variable reached the dispatch only through an ASCRIPTION out of a carrier -- `(:: (.v x) V)`, which is what reading a HAMT value or a phantom-parameterized struct field looks like -- silently ran the representative instance for every instantiation. Root cause was NOT the specialization key (the first guess): the key was never consulted because no specialization was requested. `body_has_dispatch_on_app_tyvar` strips `EX_ASCRIBE` before asking whether the receiver is a bound tyvar, discarding the only thing that said `V`. Fixed by keeping the outermost ascribed type. The field-typed sibling was always correct, which is why this read as being about phantomness -- phantomness is what forces the ascription. Pinned by `typeclass-phantom-tyvar-dispatch` (both shapes, both instantiations; interpreter verified correct too). |
| ~~[clone-struct-app-type-segv-on-null-arg](../archive/clone-struct-app-type-segv-on-null-arg.md)~~ | high | **RESOLVED 2026-09-12**: a compiler **SEGV**, no diagnostic, no output. A parametric `definstance` head whose method recurses into the type parameter (`(definstance JS [(Box A)] (j [x y] (Box (j (.v x) (.v y)))))`) crashed in `clone_struct_app_type` (`types.c:1191`), which dereferenced a `TY_APP`'s `fn`/`arg` unguarded -- while `free_struct_app_type` directly below it had always null-checked both, so the null state was expected and the clone was simply asymmetric with its own free. Fixed by mirroring the guard. **Unblocked more than the crash**: the shape now works, which is the general "container CRDT merges by merging its elements" form crdt-spice-plan C3's `ORMap` needs. Remaining and untouched: two-deep nesting `(Box (Box int))` fails in codegen; the result needs a `(:: ...)` ascription; a caret-spelled instance constraint (`[^JS A]`) is still unrecognized at `elab_typeclasses.c:2907`/`:3045`. Pinned by `typeclass-parametric-instance-recursive`. |
| ~~[duplicate-instance-silently-drops-a-user-definstance](../archive/duplicate-instance-silently-drops-a-user-definstance.md)~~ | low-medium | **RESOLVED 2026-09-11**: decided REJECT; an exact duplicate instance from a user file is `TUR-E0025`, with the newtype route in the message. Original row: **Filed 2026-09-09; no longer silent as of the same day.** A user `(definstance Eq [int] ...)` is dropped by the idempotent re-instance guard (first definition wins), which was written for the same stdlib file loaded twice and cannot tell that from a user redefinition. It now WARNS for any definition outside `stdlib/` (`is already defined (first definition wins): this definstance has no effect`); a stdlib reload stays silent. Pinned by `duplicate-instance-warns`. **Open:** whether a user instance should replace or be rejected -- a language decision the warning does not pre-empt. |

`user-defn-named-div-collides-with-libc` was resolved 2026-08-21 and moved to
[docs/archive](../archive/user-defn-named-div-collides-with-libc.md). Two
corrections worth carrying. First, the `tur_u_` guard the report proposed as
"the real fix" **already existed** -- `(defn strlen ...)` was already emitted
as `tur_u_strlen`; `div` was just missing from a `libc_names[]` table whose own
comment called it "grown on demand". Second, the report's "only `div`
reproduces" was wrong: its six-name control group was clean because five were
already in the table, and a 104-name sweep found **12** breakers (`div`,
`ldiv`, `lldiv`, `llabs`, `atexit`, `putchar`, `getchar`, `gets`, `chown`,
`execl`, `drand48`, `erand48`). The table is derived from the generated TU's
headers now (136 -> 713 entries) rather than grown per report. `gets` is the
case that shows why a plain header scrape is not enough -- glibc declares it
only under `_FORTIFY_SOURCE`, which `-O2` turns on, so it broke `tur run` while
`tur emit-c | cc` compiled clean. Zero fixture churn. The front-end diagnostic
the report also asked for was deliberately NOT added: these names work now, and
rejecting them would trade the bug for a restriction. Pinned by
`tests/fixtures/libc-collision-guard/`, `tests/mangle_test.c`, and
`tests/check-libc-collision-list.sh` (ctest `tur_libc_collision_list`), which
guards the bsearch sort-order precondition.

`reads-frame-cannot-name-multiple-params` was filed and resolved 2026-08-18,
and moved to
[docs/archive](../archive/reads-frame-cannot-name-multiple-params.md).
`#reads` now takes `w` or `[w g ...]` like `#writes`; the backing field went
from a single param index to a 64-bit mask. The congruence grant over a
multi-parameter frame is CONJUNCTIVE -- every named parameter must be frozen
-- which is the arm that had to be decided rather than refactored, and is
pinned by a partial-frozen negative fixture so relaxing it fails loudly.

`caret-constraint-vector-not-registered` was resolved 2026-08-17, the day
after filing, and moved to
[docs/archive](../archive/caret-constraint-vector-not-registered.md). The
`[^Class a]` defn type-param-vector spelling now registers real
TypeConstraints (uppercase `^Name` resolving to a defined class constrains
the next binder; unknown names keep the legacy HKT-param meaning). The
archived note corrects the filing's blast-radius estimate -- only 12 of the
~66 matched files were genuinely the broken two-vector shape -- and records
that this was the missing input that let the interpreter's constraint-dict
path retire `gde_reresolve_method` entirely.

`lsp-completion-internal-symbols` was resolved 2026-08-05 (a
`Binding.is_synthesized` bit filtered in the LSP collector) and moved to
[docs/archive](../archive/lsp-completion-internal-symbols.md). Its `__`-prefix
fix direction was **not** taken and the archived note says why: the prefix
means "internal" in this codebase, not "synthesized", and the stdlib writes
~46 of its own. The 200-item completion cap it also mentions is untouched and
is not tracked as an open finding -- see that note's "What this does not fix".

`definstance-constraint-type-defaults-to-int` was resolved 2026-08-05 and moved
to [docs/archive](../archive/definstance-constraint-type-defaults-to-int.md). A
`definstance` constraint type was resolved by a hardcoded `int`/`bool`/`cstr`
`memcmp` chain, so `[TC float]` and `[TC MyStruct]` both kept the parser's
`TYPE_INT` initializer -- silently ACCEPTED against `TC[int]` when that existed,
and otherwise a spurious error naming `int`, a type absent from the source, that
dropped the whole instance. Both constraint parsers now resolve through the same
name set the instance head accepts plus the type namespace, and an unresolvable
one is a hard error. Two things the report did not have: an APPLIED head
(`[(Option A)]`) binds type parameters through a `TY_APP` spine the parameter
scan did not peel, so the new strict error caught two fixtures that were
relying on the old silent default -- a reminder that **a strict error can only
be added once every legitimate resolution path is reachable** -- and the parser
looked inside the constraint form only when it was a bare symbol, so the keyword
spelling `[TC :cstr]` and the very natural `[TC nil]` (a literal; the type
spelling is `void`) fell through the same way. Pinned by four `errors/`
negatives and `tests/fixtures/definstance-constraint-user-type/`.

## Soundness limits and UB

`float-division-aborts-instead-of-ieee-inf` was resolved 2026-08-21 and moved
to [docs/archive](../archive/float-division-aborts-instead-of-ieee-inf.md). Its
open question -- was the abort *intended*? -- was settled by a fact the filing
did not have: the interpreter never aborted (`src/turi/eval.c` has always
divided floats straight through), so this was a compiled-vs-interpreted
divergence, not a language decision. `builtin_div_is_ieee()` now gates the
guard on `spec->arg_type.kind`, keeping it for the integer rows only. All three
symptoms went with it: the abort, the branch per division, and the
`-Wliteral-conversion` noise on constant divisors. Pinned by
`tests/fixtures/float-division-ieee/`; one snapshot
(`map-multiword-struct-value`, whose stdlib `Num` float instances lose the
guard) regenerated in the same commit, which was the whole blast radius --
suite 2687 passed, 0 failed.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[generator-thunk-call-site-returns-void-ptr-not-carrier](../archive/generator-thunk-call-site-returns-void-ptr-not-carrier.md)~~ | low-medium | **RESOLVED 2026-10-03** (archived): the head temp of a closure literal applied in a generic's generator body is lifted into the state struct and dispatched fat; the call site's `word_back` re-derived a pointer result after the closure-head block had already read the thunk's `int64_t`, and at A := bool/int8/uint8 the slot-0 widen wrapper called the base thunk at the spec's narrow type.  Both follow the recorded spelling now.  The generic `thunk` was not load-bearing.  Pinned by `tests/fixtures/generator-thunk-call-site-carrier`; `tests/run-fnsan.sh` 3534 passed, 0 failed |
| [float-proofs-assume-exact-reals](float-proofs-assume-exact-reals.md) | medium | **Filed 2026-09-29.** S2 decides `float` refinements over exact rationals while the runtime check evaluates them in `double`, so a proof that leans on an identity IEEE rounding breaks elides a check that would fire: `(- (+ x 0.1) 0.1)` is proved equal to `x` and is not for `x = 0.3`. A design choice, not a one-stage bug; the report has three fix directions and the user guide's Limits list states the caveat |
| ~~vec-get-byval-struct-element-returns-carrier~~ | -- | **Resolved 2026-09-05.** The ascription arm resolved a tyvar ascription of an int64 carrier through the active spec but bridged only float widths, claiming struct elements need no reinterpret; a heap container BOXES a by-value struct element, so the carrier is the box address. The same block now unboxes when `A` grounds to a boxed by-value aggregate (the bridge the concrete `(:: (vec-get v 0) P2)` spelling already took), SR4 recursive-carrier wrappers excluded. Pinned by `vec-get-byval-struct-element`. Archived to [docs/archive](../archive/vec-get-byval-struct-element-returns-carrier.md) |
| ~~[jit-ffi-interp-refuses-parametric-record-field](../archive/jit-ffi-interp-refuses-parametric-record-field.md)~~ | low | **ARCHIVED 2026-09-16**: both fix directions had landed 2026-09-10 (the report said so itself) and only the move was owed; verified at v0.48.0 -- `run-flags.sh` carries `jit-ffi-interp-parametric-record-field` and the reworded-diagnostic probe. Original row: `call-ptr` under `--interpret` refused a record with a parametric-monomorph field that the compiled path inlines by value
| ~~[defer-in-generic-hof-skipped-on-caught-panic](../archive/defer-in-generic-hof-skipped-on-caught-panic.md)~~ | medium | **RESOLVED 2026-09-11** (archived): the panic-signal early return now fires the function's open defer frames (the `[A]` was incidental -- direct vs CPS path); pinned by `tests/fixtures/defer-generic-hof-caught-panic`. Original row: a `defer` registered inside a generic (`[A]`) higher-order function is skipped on a caught `panic` unwind on the COMPILED path, while the interpreter fires it -- a silent missed cleanup and a compiled/interpreted divergence. The non-generic twin fires on both. This is why `bt-scope`'s panic caveat cannot be closed by rewriting it to a `defer`: `bt-scope` is `[A]`-generic, so the defer would be dropped on exactly the unwind it was meant to cover. Found 2026-09-05 checking that caveat; not trail-specific (the repro has no trail) |
| ~~capturing-thunk-returning-heap-field-record-garbles-int~~ | -- | **Resolved 2026-09-05.** Neither variable in the report's table was the cause: the CONTROL (`cap-int`) broke the subject. The direct path's by-args spec matchers (`find_matched_abi_spec`, `emit_call_name`'s fallback) guarded the result only for two distinct PRIMITIVE kinds, so a call returning a heap-boxed record (`HoldsLink`, riding the int64 carrier) matched the spec minted for the sibling BY-VALUE record (`HoldsInt`) -- both `TY_ADT` -- and read its box pointer back through the 16-byte aggregate. The capturing thunk only chose the emit path (the non-capturing / CPS callers were already right via the archived CPS fix). `emit_spec_result_mismatch` now also rejects a spec whose result C spelling differs from the call's when both results are decisive (not a tyvar / unknown / abstract-headed app), mirroring the CPS side's `spec_result_matches`; the `--emit-abi-trace` mirror is in lockstep. Pinned by `spec-selected-by-result-type-direct`; `region-scope-shapes` runs its `r-heap` shape again. Archived to [docs/archive](../archive/capturing-thunk-returning-heap-field-record-garbles-int.md) |

The `mir-aarch64` row was indexed 2026-08-21. It had **no row at all** since it
was filed -- the only open report in the tree that this index never listed, and
the highest-severity one. Found by sweeping `docs/reported/*.md` against the
rows here; that sweep is worth repeating whenever you touch this file, in both
directions (a row with no file, a file with no row).

The `jit-ffi-interp-*` row was filed 2026-08-21, the same day `jit-ffi`
graduated. These two are the entirety of the archived
[jit-ffi-c2mir-plan](../archive/jit-ffi-c2mir-plan.md)'s "Still open" section;
filing the second one is what keeps that section reachable now that graduation
has moved the plan into `docs/archive/`.

`reads-grant-survives-callee-global-write` was filed and resolved 2026-08-18,
and moved to
[docs/archive](../archive/reads-grant-survives-callee-global-write.md).  The
C2 `#reads` grant was publishing mutable globals into the frozen set, so a
callee could write one by name with no trace at the call site and a
refinement precondition false at the crossing was statically proven -- with
no backstop, since the runtime entry check is suppressed for `#reads`
measures.  Mutable globals are now withheld from the frozen set, restoring
the invariant `rt_collect_set_targets`' own soundness note already depended
on.

`dead-base-thunk-chain-references-undefined-ctor` was resolved 2026-08-18 and
moved to
[docs/archive](../archive/dead-base-thunk-chain-references-undefined-ctor.md).
The hand `-O0` link cliff is closed by a narrowed fix direction 1: the
never-defined base ctors of heap parametric ADTs now get **static trap
definitions** flushed into the forward-decl band (fprintf + abort naming the
ctor), so the emitted C is self-contained at any -O level; the dead chain is
still emitted but harmless, and a genuinely live base-ctor call (a compiler
defect -- previously an unconditional link error) aborts loudly at runtime
instead.  Pinned by `tests/fixtures/dead-base-ctor-trap/` (expected.c
snapshot + live output).  Deliberately NOT done: suppressing/trapping the
dead base *thunk* itself -- a shell-result thunk returning a captured carrier
value can be live-and-correct on the carrier path, and trapping it would
regress that.

`frozen-region-aliasing-via-coercing-cast` was archived when
`sealed-opaque` graduated (2026-08-17) and lives at
[docs/archive](../archive/frozen-region-aliasing-via-coercing-cast.md); the
sealing that closes the `::` alias mint is now always-on.

`emitter-thunk-type-return-mismatch` was resolved 2026-08-17 and moved to
[docs/archive](../archive/emitter-thunk-type-return-mismatch.md), with a
correction worth reading: the clang re-sweep found the class had GROWN from 2
findings to 14 -- the 2026-08-16 effect-row fat-normalization moved lambda
callbacks onto carrier-typed fat entries and reactor.c's hand-written typedefs
drifted a second time, plus five hand-packed fat boxes carried typed-convention
entries in slot 0.  All are fixed (dispatch ascriptions name real types;
hand-built boxes follow the carrier convention), the corpus sweeps ZERO under
clang `-fsanitize=function`, and run.sh now FAILs any fixture whose stderr
carries the UBSan report, backed by a clang-gated canary in
`tests/check-cc-warn-ratchet.sh`.  The full retyping of the httpd `:int`
sinks was deliberately NOT done -- the carrier ownership idiom keeps them --
so the no-lazy-`:int` rule still points at that API as a preference, but no
soundness finding remains.

`struct-return-type-mismatch-unchecked-until-cc` was resolved 2026-08-06 and
moved to
[docs/archive](../archive/struct-return-type-mismatch-unchecked-until-cc.md).
The hole was deliberate rather than missing: every tolerance in
`return_position_conflict` exists because both sides are `int64_t` in the
emitted C, and the code says so -- but a by-value record ADT lowers to a real
`tur_adt_S` aggregate, so there is no shared representation to bridge. It slots
in as one more predicate, with membership decided by asking `type_c_name` (the
function codegen uses) rather than re-enumerating which ADTs are by-value.
Three things the report did not have, all recorded there: the check must NOT be
gated on the return class the way its two neighbours are, or the instance-method
shape that started the thread stays broken; the interpreted path must be exempt,
since it boxes every value and two fixtures write that bridge deliberately via a
`#?(:tur ... :turi ...)` arm; and a `:heap` ADT-app under a scalar return was
the same defect one `-Wint-conversion` warning away from being a hard error.
Pinned by four `errors/` negatives and
`tests/fixtures/return-type-carrier-bridges-still-accepted/`.

## Build / CI / performance

| Report | Severity | One line |
| --- | --- | --- |
| ~~[nightly-arm64-timings-not-on-ci-metrics](../archive/nightly-arm64-timings-not-on-ci-metrics.md)~~ | low (observability) | **RESOLVED 2026-10-04** (archived): `nightly-arm64.yml` gained a `publish-timings` job that appends the nightly's rows to ci-metrics through `tools/ci/publish-timings.sh` under the shared `ci-metrics` concurrency group. The rows continue the unsharded macOS series and do not collide with the PR leg's `[1/4]` rows |
| ~~[run-sh-stamp-cache-ignores-the-preamble-split-mode](../archive/run-sh-stamp-cache-ignores-the-preamble-split-mode.md)~~ | medium (a green that was never earned) | **RESOLVED 2026-10-03** (archived): `stamp_key` carries a per-run `TUR_CONFIG_HASH` -- the RESOLVED preamble mode (one `TUR_SHOW_CC` probe build), `CC` and its version banner, and every `TUR_*` variable but the harness's own bookkeeping -- and the run's first line names it (`run.sh: preamble=split cc=cc config=...`).  Found on the way: `_tur_mtime` ran GNU `stat -f` (filesystem status) first, so on Linux every stamp key carried the volume's free-block count and the cache almost never hit; fixed in all four stamp-keyed harnesses, and `run-jit.sh` / `run-turi.sh` / `run-doctests.sh` now hash `stdlib/` as `run.sh` does |
| ~~[macos-sanitized-libturi-fixture-builds-hit-10s-cap](../archive/macos-sanitized-libturi-fixture-builds-hit-10s-cap.md)~~ | low-medium (intermittent CI red on `main`) | **RESOLVED 2026-10-03** (archived) by fix direction 1: `tests/run.sh` warms the sanitized `-lturi` link once, untimed, before the first fixture that imports an autolinking module; the two `expected.timeout` overrides are gone.  Original row: **Filed 2026-10-01** (CI #3052, #3081). Fixtures that import a `-lturi` module (`arc`, `httpd`, `reactor`, `turi/eval`) build with `-fsanitize=address,undefined` against the Debug `libturi.a`. On macOS, `arc-basic` and `arc-weak-upgrade` (alphabetically the first two such builds `run.sh` reaches) timed out at the 10 s build cap twice in 11 `main` runs. A first sanitized build costs ~2x a warm one even on Linux. **Worked around** with `expected.timeout` 30 on those two. Open: 41 other such fixtures on the default, and the cold cost itself. A one-off untimed warm-up in `run.sh`, like the r7rs-prelude one, would cover whichever comes first |
| ~~[r7rs-conformance-program-emits-megabytes-of-c](../archive/r7rs-conformance-program-emits-megabytes-of-c.md)~~ | medium (CI wall-clock; build cost of large r7rs programs) | **RESOLVED 2026-10-03** (archived): one shared direct->cps entry helper instead of 1,557 copies, and five linear compiler lookups indexed -- the compiled conformance round builds in 42.5 s (was 248 s).  Original row: **Filed 2026-10-01** (CI #3079). `tur_r7rs_conformance`'s compiled round builds chibi's suite as one program: 94 KB of Scheme becomes a 5.6 MB C unit, 248 s to build under the Debug `tur` (95 s emit-c, ~150 s `cc -O2`). Once it lost `RUN_SERIAL` (#977) it overran the harness's 240 s per-program cap on slow ubuntu runners: three `main` runs scored 0 passed. **Worked around** by `--timeout 480` in `tests/run-r7rs-conformance.sh`. Open: the size. 1,557 byte-identical ~670-byte direct-style CPS entry shims (~1 MB), a 4,689-line `main`, and a 3,420-line `__tur_fatbox_init` |
| ~~[jit-suite-pays-for-the-whole-prelude](../archive/jit-suite-pays-for-the-whole-prelude.md)~~ | medium (CI wall-clock; gated) | **RESOLVED 2026-09-29** (archived at filing). c2mir compiled the whole auto-loaded prelude (~377 static functions for `(println 42)`) and its heavy system headers for every JIT'd program, because unlike cc it compiles every definition it is handed; that fixed cost was most of the JIT fixture suite and pushed the 3-core macOS leg into its 1500 s ctest `TIMEOUT`. `src/compiler/jit_prune.{h,c}` drops unreached static entities (and, on a split TU, unused heavy includes) before c2mir; a TU the split declined (every r7rs and Saffron program) is pruned too. A reference the scan misses fails the fixture: the full-TU retry (TUR-W0071) now fails `run-jit.sh`. Four fixtures that fell back to cc only because of unused prelude code are reclaimed. Left: the decls region's system headers, ASan's allocator under in-process c2mir, r7rs/Saffron never getting the S2 split |
| ~~[jit-x86-64-struct-valued-statement-expression-miscompiles](../archive/jit-x86-64-struct-valued-statement-expression-miscompiles.md)~~ | medium (JIT engine, x86-64 only) | **RESOLVED 2026-09-29** (archived): rjungemann/mir#5 merged as `96c34860` and `cmake/mir.cmake` pins it. c2mir now assigns a struct statement expression's result slot after the stack variables are laid out, so it no longer overlaps the first of them. `tests/fixtures/jit-inline-c-struct-stmtexpr-slot` pins it from user inline C (708 on the old pin, 304 on the new, as gcc), and `tests/run-jit.sh` passes on the new pin. Original row: under the MIR engine on x86-64 a struct-valued `({ ... })` -- an `any` box or a by-value ADT -- in a call's argument list or as a local's initializer overwrote a sibling argument or parameter; surfaced when the Saffron dynamic call stopped falling back to cc (`saffron-prelude` 4.5 for 3.75). The emitter stopped producing the shape first (every site hoisted 2026-09-28) |
| ~~[jit-cc-fallback-reentry-inherits-monomorph-state](../archive/jit-cc-fallback-reentry-inherits-monomorph-state.md)~~ | low (latent) | **RESOLVED 2026-09-26** (archived): finding 2 was the fixture measuring the in-process engine's one-off code-generation allocations (constant, not scaling with iterations) on an unsanitized JIT, and nothing at all under ASan (mallinfo2 cannot see its allocator); `gc-heap-struct-rc` now warms its measurement window and reads `0 0` on every engine, and still catches a real per-iteration leak. Original row: **Split 2026-09-09** from the row below: the `tur jit` cc fallback re-enters `cmd_run` carrying the abandoned engine attempt's monomorph state (`over_px__lens_*` called but never emitted), reachable only when the engine fails first; plus `gc-heap-struct-rc` answering differently under `-DTUR_DEBUG_SANITIZE=OFF` in-process. Neither re-verified at the split. |
| ~~jit-suite-reports-pass-when-the-engine-is-disabled~~ | -- | **Resolved 2026-09-09** for the suite-accounting defect it was filed for: `run-jit.sh` runs one trivial program through the engine before the fixtures and fails if it falls back (the tree-wide case, in a second), and ratchets the fixtures allowed to fall back BY NAME against `tests/jit-fallback-baseline.txt` (a new fallback fails the run; a reclaimed one is reported; `TUR_JIT_FALLBACK_UPDATE=1` regenerates). Its two residual findings moved to the row above. See [../archive/jit-suite-reports-pass-when-the-engine-is-disabled.md](../archive/jit-suite-reports-pass-when-the-engine-is-disabled.md). |
| ~~[codegen-gcc14-permerrors](../archive/history/codegen-gcc14-permerrors.md)~~ | medium | **RESOLVED** (archived; duplicate removed 2026-09-11): the `-Wno-error` downgrades were already gone from `src/main.c`, every straddle is bridged, and the `run.sh` ratchet holds the corpus at zero. Original row: Latent today; breaks every `tur build` the moment CI's compiler crosses GCC 14, which promotes several emitted-C warnings to errors. Worked around, not fixed. Not Windows-specific |
| ~~[ci-two-fixtures-flake-on-hosted-runners](../archive/ci-two-fixtures-flake-on-hosted-runners.md)~~ | low each, medium as a pair | **RESOLVED 2026-09-11**: `rp7` polls instead of sleeping (2026-09-09); the rate-limit fixture no longer pins the exact `Retry-After` second and retries `connect()`. Original row: **Filed 2026-09-07.** `httpd-mw-rate-limit` (macOS JIT, `stdout mismatch`, 1 of 2718) and `rp7-reload-self-heal` (ubuntu, plus an LSan report) both failed on a **markdown-only** PR whose merge-base is a green `main`, and both passed on `gh run rerun --failed` against the identical tree. A rate limiter is timing-dependent by construction; the repl one arrived carrying spinner glyphs in its assertion text, i.e. an output-capture race, and its LSan report is likely a consequence of the failure path rather than a leak. Method note worth keeping: scanning run history *disconfirmed* the flake hypothesis and was misleading -- the same job had failed recently on `j2-load-in-process` / `cps-mixed-coloring` / `van-laarhoven-lens-*`, which were real defects on a branch that had real defects. History shows a job is fragile; only re-running the same tree shows a failure is spurious |
| ~~ci-suites-that-never-run-on-hosted-runners~~ | -- | **Resolved 2026-09-09** by its own closure condition, on both OSes rather than just Linux. All five suites read `pass` off the `ci-metrics` ledger for five consecutive `main` pushes (`b58c91e67` through `70f079975`); the push before the `ci.yml` changes, `5204f4c83`, still reads `skip` with the verbatim reasons, which is the positive control that the ledger distinguishes "did not run" from "passed". Two fix-direction predictions came out better than forecast: macOS gdb did not have to stay `partial`, and the gdb halves -- which had never executed on a hosted runner in the project's history -- passed on their first real run. See [../archive/ci-suites-that-never-run-on-hosted-runners.md](../archive/ci-suites-that-never-run-on-hosted-runners.md). |
| ~~httpd-async-limit-asserts-a-racy-split~~ | -- | **Resolved 2026-09-09.** The race was already gone -- the fixture gates each admitted handler on the busy counter reaching 2 (direction 3 in effect, from the macOS-hang fix); what remained was the literal `handler-ran=2`, now a counter bumped on handler entry. See [../archive/httpd-async-limit-asserts-a-racy-split.md](../archive/httpd-async-limit-asserts-a-racy-split.md). |

`env-doctests-are-machine-dependent` was resolved 2026-08-21 and moved to
[docs/archive](../archive/env-doctests-are-machine-dependent.md). The five
`stdlib/env.tur` examples now carry the `; doctest: <reason>` opt-out that
already existed for `tur/term`'s tty-dependent ones. The report's five FAILs
were masked on some boxes by a *segfault* in the same module -- `env/home`,
`env/path`, `env/user`, `env/shell` returned a bare nullable `: cstr`, so
`(println (env/user))` with `$USER` unset dereferenced NULL; all four now
return `(Option cstr)` via `env/get`, which had zero callers to break.
Doctests: 161 passed, 0 failed, exit 0.

`pipefail-grep-q-false-failures` was resolved 2026-08-21 and moved to
[docs/archive](../archive/pipefail-grep-q-false-failures.md); it was filed
2026-08-20 and never got a row here. All 180 pipe-into-`grep -q` sites in the
39 `tests/*.sh` that set `pipefail` are here-strings now, and
`tests/check-pipefail-grep-q.sh` (ctest target `tur_pipefail_grep_q_lint`)
fails on a new one. The lint earned its keep on its first run, catching four
sites the sweep's own regex missed because they spell the flags as two tokens
(`grep -E -q` / `grep -F -q`). Two harness defects found while verifying,
both pre-existing and fixed there: `repl-spice-load.sh` corrupted an
**absolute** `$TUR` into `$PWD/$TUR` (6 of 9 assertions failed for any caller
that exported one), and `eval-async-io.sh` lacked its siblings'
`detect_leaks=0` opt-out, so standalone it exited 1 with no output at all.

`ecs-defsystem-writes-fixture-expects-old-spices` was resolved 2026-08-18 and
moved to
[docs/archive](../archive/ecs-defsystem-writes-fixture-expects-old-spices.md).
Two layers: the fixture predated `defworld`'s `(defcomponent C)` storage
registration, and -- once past that -- `defsystem` binds `w : int` while the
generated accessors take `^borrow w : GameWorld`, so the fixture's
`(:: w GameWorld)` bridge is now TUR-E0295 and fires before the cap check.
The fixture's body became `(use-cap! Vel-write-cap)`, reaching its unchanged
`expected.diag`. Note `ecs/world.tur` itself elaborates cleanly -- this was
never a turmeric-vs-spice feature gap.

`macos-jit-leg-intermittent-45min-hang` was resolved 2026-08-18 and moved to
[docs/archive](../archive/macos-jit-leg-intermittent-45min-hang.md).  The
45-minute silent hang had already been contained (coreutils in CI so
per-fixture timeouts fire); the remaining flake -- `httpd-async-limit`
deadlocking outright -- was root-caused to the fixture stopping the async
server while a straggler client's connect could still land in a listen
backlog that `httpd-stop-async` left open (the listen fd only closed at
`httpd-async-free`, after the client joins).  Fixed in `httpd-stop-async`
(close the listener at stop), the fixture (handlers hold their slots until
both 503s are observed; clients joined before stop), and `io_kqueue.c`
(EV_DELETE used `EVFILT_READ | EVFILT_WRITE`, which collapses to
`EVFILT_READ` -- filters are values, not flags -- so WRITE knotes were never
deleted).

`macro-depth-guard-loses-race-with-asan-stack` was resolved 2026-08-18 and
moved to
[docs/archive](../archive/macro-depth-guard-loses-race-with-asan-stack.md);
it was filed 2026-08-17 and never got a row here.  Fix direction 2 landed:
the macro-expansion guard now also measures real stack headroom (per-platform
thread-stack query + reading the SP register -- a local's address is on
ASan's fake stack and useless for this) and raises the same diagnostic when
the stack is nearly gone, so the ASan-inflated Debug build reports the
runaway macro instead of aborting with a sanitizer stack-overflow.

The **emitter's** half of that same bug was resolved 2026-09-09 and moved to
[docs/archive](../archive/emit-depth-guard-loses-race-with-asan-stack.md).  It
was fixed the way the report said and not by re-tuning the constant: the three
stack-introspection helpers moved out of `elab_call.c` into a shared
`src/compiler/stack_guard.{c,h}`, and `emit_value` now raises TUR-E0712 when
EITHER the counter hits `EMIT_MAX_EXPR_DEPTH` OR a genuine nesting is under
way (depth >= 8) and `tur_stack_nearly_exhausted()` says the real stack is
nearly gone.  Both triggers were exercised on the filing host (macOS/arm64,
Debug + ASan): the default 8 MiB stack stops on headroom at depth 32 with
954 KiB left -- the eight further levels to reach 40 would have wanted ~1.8 MiB
-- while `ulimit -s 65520` still stops on the plain counter at 40.  One thing
the filed direction did not ask for and the message needed: when headroom is
what stopped the walk, an extra note says so, because otherwise the error
claims the expression exceeded 40 while the counter stood at 32.  The report
also names the four remaining counter-only walks in the refinement solver as
the places this shape could recur; none has a repro.

`incremental-elab-loses-span-file-provenance` was resolved 2026-08-13 and moved
to
[docs/archive](../archive/incremental-elab-loses-span-file-provenance.md). Its
remaining (DAP) half was **not** what the root-cause section said: no span ever
lost provenance. `diag_reset()` clears the whole SourceFile registry every eval
turn, and the incremental path reuses previously-parsed Forms rather than
re-running their `(load ...)` splices -- so the 40 loaded files are never
re-registered while the reused Forms still carry their ids, and every later
`diag_file_path()` misses. Hence `?:19` (a frame with NO path) rather than one
attributed to `<eval>`. Fixed with a save/restore of the registry around that
reset, far smaller than the report's "neither is small" estimate -- no span
remapping and no offset table, because nothing moved. The `cmd_eval_h`
workaround is removed and `tests/run-dap.sh` is now a real guard; with the
workaround in place it passed whether or not the bug existed. This does **not**
retire the two sibling `elab_lookup_*` workarounds the report groups it with --
those are name visibility across the moved stdlib/user boundary, a different
mechanism.

`tur-build-nested-src-dir-finds-no-files` was filed and resolved 2026-08-13,
and moved to
[docs/archive](../archive/tur-build-nested-src-dir-finds-no-files.md). All three
bare-directory commands (`tur test`, `tur check`, `tur build`) now walk
recursively, matching project mode. The half the filing missed: finding the
files is not enough -- the bare-directory build passed no include path, so a
recursive walk then failed with `module 'demo/lib' not found`, and `tur build
src/` is precisely what the `module not found` hint recommends. `dir` now joins
the include path as its own module root. The blast radius the report flags on
`tur test <dir>` turned out to be nil here: every `tests/cli/` case is flat.

`for-comprehension-pure-ambiguous-against-stdlib` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/for-comprehension-pure-ambiguous-against-stdlib.md),
by none of its four fix directions -- its root-cause section has the mechanism
wrong. The expected type was never missing: bare `pure` in the identical
position resolves fine, and the discriminator is `.pure` vs `pure`. `.m` means
"dispatch on the first argument", which for a return-directed method is the
*payload*, not the class type -- so the dot form asks the compiler to pick an
`Applicative` by looking at `42`. Fixed in dispatch, not in the macro:
`stdlib/macros.tur` is unchanged. Two other routes were implemented and backed
out -- emitting bare `pure` from the macro breaks the bespoke single-instance
fixtures (neither spelling works for both corpora), and relaxing the
unique-instance arrow gate in return-directed dispatch breaks
`errors/rt-return-dispatch-unascribed`, which pins that gate deliberately. Fix
direction 4 was the load-bearing one and is done.

`turi-return-directed-method-keeps-baked-instance` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/turi-return-directed-method-keeps-baked-instance.md).
Its remaining half -- the rank-2 forall shape -- is fixed: reached through a
`forall` PARAMETER there is no named generic for the elaborator to record a
substitution against, but the callee declares `x : (m int)` and the argument's
static type is `(T1 int)`, so matching the two type applications recovers
`m -> T1`. `frame_pin_hkt_tyvars_from_args` does that, only when the call
recorded no `abi_bindings`. The coverage note in that report was the important
part and applied to the fix as much as the bug: both rank-2 fixtures carry
inline-C and are PASS-skipped by the TI7 carve-out, so a fix verified against
only them would have been as invisible as the defect.
`tests/fixtures/hkt-rank2-forall-pure-two-instances` restates one with
parametric ADTs and no inline-C, so `run-turi` actually runs it. Fix direction 2
(turi dict passing) was carried to completion 2026-08-16/17 -- the interpreter
follows real dictionaries end to end and ALL THREE recovery heuristics this
family accreted are retired on sabotage evidence; see
[docs/archive/turi-dict-passing-plan.md](../archive/turi-dict-passing-plan.md)
for the full measurement record.

`lang-switch-breaks-generic-instance-resolution` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/lang-switch-breaks-generic-instance-resolution.md).
Its candidate 1 (stale vs fresh `TypeClassEnv` identity) was right but only half
the cause: the by-name retry that should have absorbed it was gated
`!concrete_is_primitive` -- and `Sym` and `cstr`, the two element types in the
report's own repros, are both primitive -- **and** the retry could not have
matched anyway, because it never spelled a primitive instance's head via
`gde_primitive_type_name` the way the precise loop above it does. Either alone
leaves the bug. The report's open blast-radius question is answered: `Eq` was
not spared by surviving the reset, it simply never reaches that path with a
primitive concrete. Three notes for whoever writes a similar test: the defect
does **not** reproduce from C via `turi_try_show_by_tag` (that is the auto-show
tier, not the broken path), `(load "stdlib/str.tur")` first **masks** it
entirely, and the harness's `check` uses `grep -qF`, which treats a multi-line
pattern as alternatives -- a before/after expectation there passes on a broken
compiler.

`jit-s2-split-disengages-on-hoisted-inline-c-include` was resolved 2026-08-13
and moved to
[docs/archive](../archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md).
Two corrections to its fix directions. Emitting the hoisted includes "above the
split marker" as direction 1 proposes is **not safe** -- `#define
_DEFAULT_SOURCE 1` sits immediately below that marker and its own comment says
it must precede every `#include`; they went below the END marker instead, which
is still ahead of every inline-C function. And moving the loop *within*
`emit_runtime_preamble` does not fix anything, because the probe calls that same
function after elaboration and so emits the includes too -- the loop had to move
out of it entirely. Direction 3 (a `TUR_JIT_TIMING`-gated reason for the
disengage) paid for itself inside one edit-compile cycle by catching that failed
first attempt. Verified: the report's program B went from 7208 preamble lines
(never engaging) to 5538, and every in-tree instance it names now engages.

`reactor-fd-callback-fn-ptr-type-mismatch` was resolved 2026-08-13 and moved to
[docs/archive](../archive/reactor-fd-callback-fn-ptr-type-mismatch.md), closing
29 of its 32 UBSan findings. Three corrections worth carrying: the reactor had
**four** mismatched sites, not one (the sibling audit the report asked for found
them); the "emitted C" instance was mostly **hand-written inline C in
`stdlib/httpd.tur`**, the same defect in a second file, not the emitter; and
`local_park_wake_cb` served both the 4-arg fd/chan and 3-arg timer conventions
from one definition on the reasoning that "the differing arity is harmless",
which is true of the ABI and false of the language. Two things block anyone
re-checking this: **GCC cannot see this class at all** (no `-fsanitize=function`),
and until this landing no clang build of the tree compiled -- `elab_memory.c` had
no trailing newline and `-Werror,-Wnewline-eof` stopped it. Also note the
affected fixtures **PASS** while emitting the UB, so the summary line is not the
signal. The residue is `emitter-thunk-type-return-mismatch` above.

`turi-toplevel-expr-subforms-elaborate-in-global-scope` was resolved
2026-08-13 and moved to
[docs/archive](../archive/turi-toplevel-expr-subforms-elaborate-in-global-scope.md).
Three corrections to the filing are worth carrying forward. The discriminator
was never the engine -- it was the **synthesized-main fold**, which runs only
when a file declares no `main`, so the *compiled path disagreed with itself*
depending on an unrelated line elsewhere in the file. The divergence was
accept/reject, not just diagnostic wording: the report's repro happens to have
mismatched `if` branch types, which is the only reason the interpreter rejected
it too. And the accepting path **miscompiles** -- the `def` elaborates as a
global but codegen emits a local, so a later reference dies in the emitted C
with `'answer_1326' undeclared`. The filed severity of low rested on "both paths
still reject"; it should have been medium. Both engines now reject, via the
report's *narrower* alternative (a statement-position bit on `Elab`), which
turned out to be the primary fix rather than the fallback.

`manifest-read-failure-degrades-to-module-not-found` was resolved 2026-08-13
and moved to
[docs/archive](../archive/manifest-read-failure-degrades-to-module-not-found.md).
`pkg_manifest_read` now distinguishes ABSENT from MALFORMED, and a malformed
manifest is recorded in a sticky verdict that survives `diag_reset()` --
re-asserted as TUR-E0624 at each compile entry point, exactly like the
`:tur-version` floor next to it. The command now fails *before* elaboration, so
the `module not found` cascade does not happen at all rather than being
annotated (the report's fix direction 3 was conditional on deferring direction
1, which was not deferred; the prototype was confirmed unreachable and removed).
Two notes for anyone re-checking this: the report's open question about where
the error state was cleared is answered in a comment 30 lines below the code it
was reading, and **the Debug build masks the bug** -- the repro exits 1 there
because LeakSanitizer catches the partial-manifest leak (also fixed), not
because the manifest error was honoured.

`mono-specs-header-comment-stale` was resolved 2026-08-13 and moved to
[docs/archive](../archive/mono-specs-header-comment-stale.md). The header
comment was rewritten to the post-graduation reality, and the report's item 4
(the general sweep) was carried out: **255 dead `docs/` citations across 88
files in `src/`** were repointed at their real locations. Three of them named
reports that were never filed *and* asserted defects that do not exist -- a
`tvar/modify` codegen no-op (the arm is dead; elab lowers the form) and a
`task-group-new` layout overflow (both layouts carry `cancel_reason`) -- so
those comments were corrected rather than backfilled with reports. Note for the
next sweep: citing `docs/archive/` up front does **not** immunise a comment (42
of the 255 already did, and rotted when the file moved on to
`docs/archive/history/`), and a single-line grep silently misses the ~6% of
citations that wrap across a `*` comment continuation.

`fixture-dirs-with-loose-tur-files-pass-without-running` was resolved
2026-08-05 and moved to
[docs/archive](../archive/fixture-dirs-with-loose-tur-files-pass-without-running.md).
A fixture dir with no `input.tur` was recorded as **PASS** while printing SKIP,
so the loss was invisible in the summary line. Two corrections in the archived
note: `sandbox/` (17 of the 30 files) **was** covered all along, by the
`tur_eval_sandbox` ctest target whose fixture list lives in a C source the
report's grep did not cover -- 13 files were genuinely uncovered, not 30; and
23 directories reached the fallback, not 4, of which **17 already carried a
`requires.dedicated-runner` marker** that the runner never reached because it
looked for the input first. The fix is that ordering plus a loud failure for
anything still undeclared. The 13 files are now real fixtures -- every one of
them discarded its result, so they asserted nothing even in principle, and two
did not compile at all once run. That turned up two separate defects, filed
above and below: a return-type mismatch unchecked whenever a struct is
involved, and a bool-to-int ascription that prints differently per path.

`emitted-c-pointer-integer-warnings-unwatched` was resolved 2026-08-06 and moved
to
[docs/archive](../archive/emitted-c-pointer-integer-warnings-unwatched.md).
`run.sh` now FAILs a fixture whose captured build stderr carries
`-Wint-conversion` / `-Wincompatible-pointer-types` (one `grep` of a file it
already writes; `TUR_SKIP_CC_WARN_CHECK=1` opts out). Two things in the archived
note are worth reading before touching it: the check must sit AHEAD of the
output comparisons, because a canary that trips it segfaults and was reported as
a plain `stdout mismatch` with the real reason never reaching the log; and the
ratchet has its own canary self-test (`tests/check-cc-warn-ratchet.sh`,
`tur_cc_warn_ratchet`), because a grep that matches nothing looks exactly like a
clean corpus -- which is how two passes of the original sweep produced a false
zero. Per-platform wording is deliberately still open; the self-test is what
will report it on a clang or Windows leg.

## Specialization / CPS (filed 2026-09-05)

Two reports from one probe. Categorizing RM2's spine residue meant putting a
`bt-scope` around a function whose result is a record rather than a scalar --
the first time anything in the tree has done that -- and both of these fell out
of it. They looked like one defect and were two; both are fixed, and fixing the
second moved its blocker to a third, filed here rather than left implied.

| Report | Severity | One line |
| --- | --- | --- |
| ~~cps-call-arm-ignores-abi-specialization~~ | -- | **Filed and resolved 2026-09-05.** A polymorphic fn instantiated at two result types with different representations, called from a CPS-lowered caller, was emitted against the WRONG specialization -- no flags needed. Two faces: `/* cps->cps */` picked a spec that was not this call's (`bt-scope` at `int` and at a 2-int record; the `int` site printed a pointer -- a silent wrong answer through a stdlib bracket), and `/* cps->direct */` fell back to the erased base when the arg pass was ambiguous, a hard C build error when the representations differ in width. Cause: `find_mono_clone_for_call` matched on argument C types alone, which identifies nothing for a generic whose type variable reaches only the RESULT. Fixed by making the call's own result type a discriminator in both matching passes, threaded from the `call_expr` each CPS IR node already retains, at all three lookup sites. A pure NARROWING -- it accepts every spec when the call's type decides nothing (an unresolved tyvar c-names to the int64 carrier exactly as `int` does, and filtering there would break the `^Show a` wrappers RC1/RC2 resolve through the same function), so it can only turn a wrong hit into the conservative base fallback or an ambiguity into a unique correct hit. No snapshot moved, which is the evidence no existing resolution changed. Pinned by `cps-spec-selected-by-result-type` and `cps-spec-erased-base-fallback-typechecks` -- one per face, because they mask each other in a combined file. Archived to [docs/archive](../archive/cps-call-arm-ignores-abi-specialization.md) |
| ~~region-bracket-lost-when-bt-scope-specializes~~ | -- | **Resolved 2026-09-05**: a region boundary now takes the `cps->direct` arm unconditionally, because that is the only arm the bracket can live on -- `cps->cps` is a tail call with nowhere to put the pop. Safe rather than a routing change: `bt-scope`'s base is colored but fell back to DIRECT style, so every shipping call site already landed there; only a resolved spec clone reached `cps->cps`. The walk's `type_extract_adt_app` gate (false for a NON-parametric ADT) was fixed alongside. **The saving is still refused**, for a newly-identified reason -- see the row below -- and an attempted field widening was REVERTED for turning a mutually-recursive result into a use-after-free (0 instead of 42). Cycle handling hardened in the same change: `seen` is a path, not a visited set. Pinned by `region-scope-adt-result`. Archived to [docs/archive](../archive/region-bracket-lost-when-bt-scope-specializes.md) |
| ~~region-walk-refuses-every-adt-result~~ | -- | **Resolved 2026-09-05** along fix direction (1): the walk consults `c->field_forms[fi]` before refusing on a NULL `full_type`, and admits a field whose declared FORM is a bare scalar primitive keyword (`:int`, `:cstr`, ...) -- which reaches nothing -- while an ADT name, `ptr`, a type variable, or any compound form stays refused. So it can only turn a REFUSE into a PASS for a provable scalar, never the reverse; the two dangerous NULL-`full_type` shapes (the self-recursive spine, whose form names the def, and a carrier-erased `:MA` field, whose form is an ADT name) are both non-scalar forms and stay refused. No name-to-def resolution and no walk into the field's type were needed, which is why the kind-based accept this report measured into a use-after-free was NOT reintroduced. `(RIP :int :int)` -- `re.tur`'s `re-find-from` result, the 312 B -- now rewinds; `region-scope-adt-result` reads `retire=1 rewind=2` with the value asserted across the pop and the mutual-recursion case (prints 42) as the negative. RM3 R5 graduation item 2's first shape; the recursive/erased/container/struct shapes stay refused for later increments. Archived to [docs/archive](../archive/region-walk-refuses-every-adt-result.md) |

## Allocation and memory-checking (filed 2026-08-22)

Reports from one thread of work: measuring the refinement solver's cost led
into how the compiler allocates, which led into what the test suite can
actually see. They are best read in that order -- each one is why the next was
found. The last two arrived later, from the same thread continuing: the
`-main` fix left a coverage hole behind it, and the `NO_MAX_SHARED` raise
turned up a defect in the instrument that would have justified it.

| Report | Severity | One line |
| --- | --- | --- |
| ~~vec-of-parametric-sum-monomorph-ice~~ | -- | **Resolved 2026-08-27 (SR2b)**: `adt_app_is_byvalue_product`'s field loop now admits a concrete-monomorph field (types.c), so the Vec registration and the binder agree. Archived to [docs/archive/vec-of-parametric-sum-monomorph-ice.md](../archive/vec-of-parametric-sum-monomorph-ice.md) |
| ~~erased-generic-field-read-overruns-subword-monomorph-box~~ | -- | **Resolved 2026-09-02**: the sub-word integer widening the layout rule already applied to multi-variant parametric monomorphs now applies to a RECORD monomorph's type-parameter-typed field too, since the parametric record's base typedef is the erased twin every generic reader uses. Pinned at `bool`, a negative `int8` and a wide `int32` through the dict-clone crossing; The `float32` residue closed the same day: the record monomorph pads a type-parameter `float32` field to the word, and a float-class poly wrapper packed into an ERASED typeclass-method `:fn` sink is bridged through its bits (`__tur_fltcarrier_*`) instead of xmm0 -- see [history/erased-fn-sink-float-wrapper-carrier-mismatch.md](../archive/history/erased-fn-sink-float-wrapper-carrier-mismatch.md). Archived to [docs/archive](../archive/erased-generic-field-read-overruns-subword-monomorph-box.md) |
| ~~codegen-carrier-straddle-in-lifted-thunk-sink~~ | -- | **Resolved 2026-09-07, the same day it was filed**, and the filing was wrong about both the site and the size. Found by `tests/regions-fuzz-src.py` (seed 1) on a shape no fixture had; nothing to do with regions -- it reproduced with `TUR_REGIONS=0`, with no region form, on `origin/main`. A `do` whose tail is a CALL returning the int64 carrier for a `:heap` ADT (with a discarded first item ahead of it, or the `do` collapses) got a result temp declared with the CONCRETE POINTER type and an unbridged assignment -- `tur_adt_TL * __t = <int64_t>`, a hard `cc` error on clang / GCC >= 14 (macOS CI red), a warning on older GCC. The lifted thunk in the repro made the shape visible, it did not produce it: marking every `"%s = %s;\n"` emitter in `emit_expr.c` and rebuilding pinned `emit_do_value` in one pass, where reading the sink lowering had produced the filing's wrong direction (`emit_fns.c`). The bridge already existed -- `bridge_control_result_int_ptr`, which the three `let` joins pair with the by-value one; the two `do` joins called only the by-value bridge, which covers carrier -> aggregate but never carrier -> pointer. Fix is the missing second call at each, on the same `(type, tail)` pair the decl used: 9 lines, **zero drift across all 148 snapshots** (the filing predicted a regen), suite 2826/0, repro pinned as `tests/fixtures/do-tail-carrier-into-heap-ptr-temp` so `run.sh`'s pointer/integer ratchet holds it on Linux too. A speculative companion fix to the CPS `letraw` binder did not fix the repro and was the sole cause of drift in two snapshots; reverted rather than guarded. Archived to [docs/archive](../archive/codegen-carrier-straddle-in-lifted-thunk-sink.md) |
| ~~[region-escape-through-unhooked-stores](../archive/region-escape-through-unhooked-stores.md)~~ | low | **RESOLVED 2026-09-28** (archived): the last item, the top-level panic jam, was reachable after all -- on both backends a region scope's call returned on a panic before its pop. The bracket now retires its generation on the panic arm, and every catch boundary retires whatever is left open above its entry depth. Pinned by `region-catch-retires-stranded-generation` on both region arms. Original row: **Filed 2026-09-06 (region-lock-hardening); NARROWED the same day.** The residue of widening the region runtime lock from "the result word" to "every word that leaves the generation". The class it was mostly about is now FIXED, and it was a **silent wrong answer**, not the documented contract this report first proposed: a typed `:heap` node handed to a hand-written inline-C body (`cell-set! [c : ptr<void> v : Link]`) let the bracket rewind under the reader, which printed garbage and exited 0 where `TUR_REGIONS=0` printed the value. Fixed at the CALLEE -- in an emitted inline-C function every parameter is already a plain C identifier, so the note goes once at body entry and covers stdlib and user inline-C alike. The filter is the engineering: "can this word BE region memory" (a `:heap` ADT or a by-value aggregate holding one; never a scalar, `cstr`, `ptr<void>`, opaque newtype or malloc-backed collection handle) rather than the result lock's "cannot prove", which emitted 12,373 notes across the fixtures. The right filter emits **zero** -- no snapshot changed. Left open at filing, neither with a repro: `extern-c` (no emitted body to note in) and a primitive storing an already-erased `:int`. **Both closed 2026-09-26**: the erased-word case by `stdlib-region-store-hooks-unswept`, and `extern-c` is unreachable -- its boundary refuses a `:heap` node and any record holding one (pinned by `errors/extern-c-refuses-region-node`). What stays open is the latent top-level panic jam recorded alongside |
| [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md) | medium | SR2b made Option/Result real sums; on the default path every `(some x)`/`(ok x)`/`(none)` mallocs a tagged carrier box nothing frees (pre-sum these were non-allocating by-value records). Interim cost until byvalue graduation; callers that care free with `(option-free (:: o :int))`. **Narrowed 3x 2026-08-30 (RM1)**: the erased residue is owned for the audited accessor consumers (freshness analysis + two drop mechanisms, 8324 -> 7364 B corpus-wide); open only for unstampable consumers, until monomorphization. **Narrowed again 2026-09-02**: `bind`/`fmap` chains over the stdlib instances are owned at static dispatch sites (instance-method masks, freshness through the continuation, per-spec re-resolution for dynamic dispatch), then comparator shim boxes and a widened result gate: 7200 -> 5643 B. Residue attributed in `docs/artifacts/leak-sweep-decomposition.md`: scaffolding, spines, dictionary sites. **Re-measured 2026-09-19**: 1630 B over the same rows, RM1's own ~320 B unchanged; one more deliberate row, a fresh box handed to a COLORED callee (elab stamps the drop-after free only for an empty effect row), pinned `known-leak` on `colored-generic-erased-carrier-param`. **Re-measured 2026-09-28, still open**: 1766 B, byte-identical before and after that day's work (two subdirectory rows the 09-19 count read as absent account for most of the difference). This report's own residue is 392 B: 176 B from fixtures' `:int` inline-C readers, 200 B from dictionary dispatch that a monomorph does not devirtualize, and 16 B from a `ptr<void>`-erased payload. Two cheap fixes were examined and declined: a DK-boundary reap for the colored-callee box (`perform` blocks the non-retain bit, and setting it would admit multi-shot double frees in callers), and `unwrap-or-carrier` as a reader (sound only over word-scalar payloads; measured zero) |
| ~~[catch-error-ascribed-result-types-handler-by-value](../archive/catch-error-ascribed-result-types-handler-by-value.md)~~ | medium | **RESOLVED 2026-09-29** (archived): an inline-C instance body now asks for the carrier-spill shim (`boxes_aggregate`), so an ascription-grounded handler boxes its by-value result; the let-init bridge no longer re-derefs an init that is already `(*(T *)...)`. `catch-error-ascribed-handler-by-value` |
| ~~value-struct-payload-sum-monomorph-box-has-no-owner~~ | -- | **Resolved 2026-09-03**: 9 of 9 -- the last two were the SITE, not the mechanism. A class-method call is built by `elab_method_call` and never ran the drop-after stamp, so the resolved instance's inferred mask now decides there (statically, or admitted per monomorph at emit for an abstract receiver); a let bound through `ok-val`/`err-val`/`unwrap` of a fresh producer is the payload's only holder and drops at scope exit; and a return-dispatched producer's cell is drained against its instance's declared result, so the erased `ok__spec__int64_t_<struct>` copy inside it is freed too. The glue route (drop at the consumer's scope exit regardless of consumer) was assessed and declined: the tag walk already IS the glue, and the remainder is a move-only discipline on Option/Result. Pinned by `sum-payload-drop-dictionary-dispatch` (leak-checked) and its retaining-instance negative. Archived to [docs/archive](../archive/value-struct-payload-sum-monomorph-box-has-no-owner.md) |
| ~~return-dispatched-sum-mint-in-constrained-instance-miscompiles~~ | -- | **Resolved 2026-09-03**: three lockstep disagreements, each fixed at the wrong site -- the carrier-producer argument disjunct now asks the re-resolved instance whether its declared result is a by-value aggregate; a base clone's header / prototype / panic-return type agree with its spilled tail (no carrier-ABI conjunct); both instance-head parsers consult the type namespace and a resolved applied head takes its type's discipline instead of a blanket CK_MOVE. Pinned by `return-dispatched-sum-mint-constrained-instance` (leak-checked). Archived to [docs/archive](../archive/return-dispatched-sum-mint-in-constrained-instance-miscompiles.md) |
| ~~rc-of-byvalue-sum-monomorph-reads-first-word~~ | -- | **Resolved 2026-09-04**, and the filing was wrong about its scope AND its fix route. "No in-tree users": `stdlib/rc.tur`'s own Functor/Foldable instances are written in terms of `rc-payload`, so `(foldl r 0 f)` over an `rc<Rat>` -- a plain by-value struct, nothing parametric -- segfaulted through the stdlib API (verified against the pre-fix compiler). And a non-parametric `:copy` sum reaches the identical shape, so the parametric monomorph in the repro was the narrow case. The named fix route was a no-op waiting to happen: `emit_abi_try_byval_twin_redirect` is a dead `return false` stub, retired by structdef-retirement DS-D. The bridge side is not it either -- the CALL SITE is already correct (`*(T *)(intptr_t)x` is right for an aggregate carrier); only the shared inline-C callee was wrong, and it cannot be specialized on `A` or ask about it. So the fact travels in the VALUE: `rc/of` sets `cb->reserved[2]` = "cb->value IS the carrier, do not dereference", and `rc-payload` branches on it. A spare reserved byte, not a new `RCK_*`, because the fact is orthogonal to the kind and zero is the scalar default. Defect (2) landed smaller than filed: `boxed_struct_payload_walk` written out as a per-site `static void __tur_rc_dropglue_N(void *)` rather than a monomorph twin of `emit_adt_byval_drop_glue` -- the control block wants a function POINTER, which is the only thing an inline walk cannot be. Archived to [docs/archive](../archive/rc-of-byvalue-sum-monomorph-reads-first-word.md) |
| ~~inline-c-option-carrier-box-leaks~~ | -- | **Resolved 2026-08-30** by fix direction 1: the compiler now owns an inline-C-returned carrier. Ownership is marked once at the call-result temp (callee body inline C + DECLARED return an Option/Result app + temp spelled int64_t) and consumed at the carrier->concrete bridge, which copies the contents out and frees the box -- so every consumer position gets it at once. Keying on the RESOLVED type instead of the declared one is a double free, not a leak: `vec-get` is also inline C and its box belongs to the vector. Leak-check now 60/0/0. Archived to [docs/archive/inline-c-option-carrier-box-leaks.md](../archive/inline-c-option-carrier-box-leaks.md) |
| ~~option-rc-payload-constructible-only-from-inline-c~~ | -- | **Resolved 2026-08-30** by fix direction 3, which proved to be the correct framing rather than the cheap one: the safe/unsafe distinction is not by-value-ness but whether the callee stores the value somewhere with an INDEPENDENT LIFETIME. A collection does; a sum constructor wraps the value in a result the caller owns. `some`/`ok`/`err` are now OWN_CARRY_BORROW rows in `own_carry_for_arg` (RETAIN would leak -- nothing releases an Option's payload at scope exit). Extraction via the generic `unwrap` stays rejected ON PURPOSE: BORROW double-drops on a second read, RETAIN leaks, and the missing piece is drop glue (RM1). Archived to [docs/archive/option-rc-payload-constructible-only-from-inline-c.md](../archive/option-rc-payload-constructible-only-from-inline-c.md) |
| ~~refine-chain-expands-the-same-dnf-four-times~~ | -- | **Resolved 2026-09-05.** A `RefineCubeCache` local threaded through `refine_sN_decide_cc` variants; the four original entry points stay as thin wrappers, so single-stage callers are byte-identical. Two departures from the filed direction, both deliberate. **LAZY, not build-once-up-front**: S0 decides several shapes syntactically BEFORE it needs cubes and those obligations build zero today, so an eager driver build would have been a regression on the cheapest path -- lazy keeps 0 at 0 and turns 4 into 1. **The `tur smt` and wasm doors got it too**, against the direction's advice: their entry points are unchanged (which is what the unit test and SX8 doors need) but each runs its own copy of the same four-stage loop. That mattered -- **the report's table measures TWO DIFFERENT DRIVERS**: its three corpus rows come from `tur smt`'s loop, its fixture row from the discharge chain, so fixing only the discharge chain left the headline benchmarks at 4, 4, 3 while appearing to close the report. All drivers: 48->13 and 4->1. Acceptance run as specified -- 125 corpus verdicts identical, 45 refine fixtures identical, `TUR_REFINE_STATS` identical. One fixture moved as predicted (`refine-json-probe-caps-attributed`, cube cap hits 4->1, because the counter counts BUILDS); cap-sweep regenerated and nothing moved but its provenance line. Archived to [docs/archive](../archive/refine-chain-expands-the-same-dnf-four-times.md) |
| ~~[solver-hot-structures-linear-scans](../archive/solver-hot-structures-linear-scans.md)~~ | low | **RESOLVED 2026-09-28** (archived): the EUF term index is an id-keyed open-addressed table that stays exact under LIFO undo, the congruence closure is a signature-table fixpoint (`O(n * arity)` per round), literal conflicts are one pass, and the S3 shared set is kept at registration. `tur_refine_corpus` byte-identical before/after, verdicts and cap telemetry; the 512-term stress unit ~510 -> ~70 ms. Original row: `euf_index` interns terms by linear scan and the congruence fixpoint is O(n^2) -- REASSESSED post-SX3: the "free fix with SX3" home is gone (SX3 trails the same arrays in place), and measurements say no fix is needed: real obligations peak at 10 of 512 terms, the one cap-pinned corpus case is a synthetic stress file deciding in 64 ms, and solver-on vs off is 21 vs 22 ms on the heaviest fixture |
| ~~examples-have-no-suite-coverage~~ | -- | **Resolved 2026-09-02**: `tests/check-examples.sh` already checks AND runs every example against two ratchets; snake left the check baseline and joined the run baseline (needs a display). A near-miss entry point (`-main` and friends) with no `main` and no top-level statements now warns `TUR-W0624` at its definition. Archived to [docs/archive](../archive/examples-have-no-suite-coverage.md) |
| ~~[workarounds-to-remove](../archive/workarounds-to-remove.md)~~ | -- | **CLOSED 2026-09-16** (archived) -- every row struck. Row 5, the last live one, swept in [turmeric-spices#74](https://github.com/turmeric-lang/turmeric-spices/pull/74): `spices/ws-server` holds the broadcast hub mutex as a `Mutex` again in both files, the six `(:: ... Mutex)` casts are gone, and a live "No Lazy `:int` Stand-Ins" violation went with them (`fixtures/broadcast/server.tur` was still carrying its mutex as `:int`). It took **two** blockers clearing, not the one the row recorded -- `global-def-store-misses-int-ptr-bridge` was found 2026-09-11 when the first fix alone still failed the macOS leg. Verified here against a freshly built v0.49.1: `tur check` clean on both files, the emitted C declares AND assigns the global the old bug dropped (`static void * hub_hymutex_2108;` ... `hub_hymutex_2108 = __ps_537;`, against the old `'hub_hymutex_1866' undeclared`), and `module-level-def-of-opaque-value` prints 42/7. The ws-server suite itself needs mbedtls, absent on this host -- a host gap, identical on tests that never touch the mutex. **A closed checklist is not a standing invitation**: the next deliberate second-best thing gets its own report, not a row appended there. Original row: checklist of places the tree is deliberately doing the second-best thing. **Rows 1 and 2 struck 2026-09-05**, leaving only row 5 (spice-repo work). Row 2 had been true for some time and nobody struck it -- exactly the failure a checklist exists to prevent. Row 1 (`StThunk`, an opaque `:ptr<void>` carrier for a `Stream` thunk) is the interesting one: BOTH its recorded blockers were wrong, and what actually kept it alive was an untyped `f : fn` parameter two functions away in `st-bind`, plus an emitter gap on `(let [lf f] ...)`. The lesson is written up in the archive: a workaround's recorded blocker is a HYPOTHESIS until someone takes the workaround out |

`c-name-accessors-share-static-buffers` was resolved 2026-09-02 and moved to
[docs/archive](../archive/c-name-accessors-share-static-buffers.md). All three
fix directions landed: `ensure_static_fatbox` returns an owned per-`EmitCtx`
string (`ctx->fatbox_names[]`, parallel to the dedup keys it already kept, so
the name lives exactly as long as the box it names), `adt_field_c_type`'s
pointer-box spelling is a one-line `intern_type_name` replacing the 16-slot
rotating pool -- a pool has a bound and fails the same way past it, just later
-- and `tests/check-static-cname-buffers.sh` (ctest
`tur_static_cname_buffer_lint`) fails any `const char *` function in
`src/compiler/` holding a function-scoped `static char buf[]`, with the four
audited-benign sites allowlisted by name.

Two things the report did not have. Its repro no longer reaches the branch
(SR1/SR2a are default now, and `rational-arith`'s Result still lowers to the
erased carrier), but a three-line `(Result Rat Oops)` over two by-value product
ADTs does -- that is now `tests/fixtures/ros-pointer-box-distinct-arms/`, and
it is a real pin rather than a demonstration, because `run.sh` FAILs a fixture
whose cc emits `-Wincompatible-pointer-types`, which is exactly the signal a
re-broken accessor produces. And the lint's first draft, whose header regex used
a greedy `.*const char \*`, bound to the `const char *shim` in
`ensure_static_fatbox`'s PARAMETER list and never recognised the function at
all: it would have shipped GREEN on a tree that still had the bug in it. Both
pre-fix bodies were reconstructed and re-run against the finished lint.

`duplicate-ctor-names-collide-in-emitted-c` was resolved 2026-09-02 and moved
to [docs/archive](../archive/duplicate-ctor-names-collide-in-emitted-c.md). The
base constructor's C FUNCTION symbol is `ctor_<Adt>_<Ctor>` now, built in one
place (`mangle_ctor_symbol`) and used by every definition site, call site and
signature-table key; the union MEMBER name stays bare, being already scoped by
the ADT's own struct. 148 snapshots regenerated in the same change; suite 2748
passed / 0 failed.

The fix direction did not anticipate that **the bare spelling is an API
surface**: hand-written inline C calls constructors by their emitted name and
`stdlib/either.tur` documents it ("Construct with `ctor_Left(v)`"), across five
stdlib files, seven fixtures, and possibly out-of-tree spices. So a constructor
name owned by exactly ONE ADT also keeps a bare-name macro alias; an ambiguous
one gets none, and inline C naming it fails at cc pointing at that constructor
rather than silently binding to whichever ADT was emitted first.

Three lifetime/resolution traps, each surfacing only as a suite failure and each
looking like an unrelated area: a curried constructor's synthesized call carries
no CtorDef (so the owner must fall back to the call's result type); ADTs are
registered BEFORE their constructors are attached, so a census read at
registration records nothing at all; and holding the AdtDef pointers instead is
a use-after-poison, because a procedural macro's nested elaboration frees the
arena. The census owns string copies taken above the elaborator teardown -- not
at the return, where `e.adt_defs` is already freed. A missed site in a change
like this is always an undefined symbol or a lifetime error, never a wrong
answer, which is what made the suite a sufficient verifier.

`cps-let-binder-bridge-lacks-position-check` was filed and resolved 2026-09-02
and moved to
[docs/archive](../archive/cps-let-binder-bridge-lacks-position-check.md). Both
fix directions landed. The check is extracted as `emit_value_is_recorded_as` --
and there were more copies than the report knew: TWO inline hand-rolled ones at
separate let-binding init sites, plus the arm sites' wrapper, plus the CPS mirror
with none. Four sites, three different answers to one question; one copy now.

The report warns against adding the missing term without a repro, because "a
change to a path with no failing case is unverifiable in the direction that
matters". The way past that was not to find a repro but to make the change
**provably inert**: instrumenting the bridge and sweeping all 2131 fixtures, only
33 reach it with a by-value init type, and in every one either the Expr-level
predicate already suppresses it or the init is recorded as `int64_t` / a pointer
/ nothing -- never the aggregate. Emitted C is byte-identical across the corpus;
suite 2752 passed / 0 failed, zero churn. It is a consistency repair that stops
the two sites drifting a third time, NOT a fix for an observed miscompile, and
the archived note says so.

The substantive finding is why no repro exists: the two conditions look
structurally exclusive. The dangerous shape needs an `if` init whose arms are
carrier producers, and when that sits in a CPS-transformed body the transform
splits the `if` before the bridge sees it -- a targeted repro reached the bridge
as two separate hits, one per branch, each recorded `int64_t`, never a merge
temp. Recorded so the search is not repeated.

`control-form-around-if-double-unboxes-carrier-arms` was resolved 2026-09-02 and
moved to
[docs/archive](../archive/control-form-around-if-double-unboxes-carrier-arms.md).
It took TWO changes, not the one it specifies. The stated fix --
`emit_arm_is_recorded_byval_agg` in `bridge_control_value_to_byvalue_temp` -- is
correct and on its own changed nothing, because the precondition the report
asserts does not hold: the merge temp's recorded C type lookup returns NOTHING.
`emit_if` declares its by-value merge temp with `emit_temp_decl` directly,
bypassing `emit_control_result_temp_decl`, which is the wrapper carrying the
`emit_localvar_record_ctype` bookkeeping -- so the temp was by-value in the
emitted C and invisible to the side table the predicate consults. Recording it is
the second half. The generalisable lesson: a position-sensitive predicate is only
as good as the recording that feeds it, and one site declaring a temp outside the
recording wrapper silently disables it.

All ten fixtures the predecessor's resolution named pass with MATCHING OUTPUT,
as its blast-radius argument predicted. Suite 2751 passed / 0 failed, zero
snapshot churn. Pinned by `tests/fixtures/control-form-around-if-carrier-arms/`
covering both the `let` and `do` wrappers and asserting field values, verified to
fail against a reverted compiler once per wrapper.

Its requested sweep of the remaining `fn_body_tail_emits_byvalue_carrier_abi`
callers is done, and found one bridging site that still asks only the Expr-level
question -- filed as the `cps-let-binder-bridge-lacks-position-check` row above.

`fat-dispatch-wide-byvalue-aggregate-argument` was resolved 2026-08-27 and
moved to
[docs/archive](../archive/fat-dispatch-wide-byvalue-aggregate-argument.md):
every fat boundary now speaks one convention (a wide by-value aggregate
crosses as an int64 box pointer), spelled in one place and consulted by the
typedef, the dispatches, the fatshims and the thunk emitters alike. Its
resolution also records why SR4 (recursive sums by value) was green but OFF by
default at the time (measured 1.4x slower / 2.2x less memory). **Flipped to
by value 2026-09-02 (RM4)** once the time cost re-measured at ~1.03x with
no arena coming; `TUR_SR4_RECURSIVE_CARRIER=1` restores the carrier.

`multi-variant-adts-always-heap-allocate` was resolved 2026-08-26 for the
NON-RECURSIVE sum population and moved to
[docs/archive](../archive/multi-variant-adts-always-heap-allocate.md). SR1
shipped on by default: such a sum flows by value as a tag+union aggregate, so
it neither mallocs nor leaks (1005 allocations / 24,112 leaked bytes -> zero on
the guarding fixture; 62.6 MB -> 1.2 MB peak RSS on a 2e6-construction loop).
Two things to carry forward. **Recursive sums** -- `Term`, `Subst`, `Stream`
and 18 others -- flow by value since 2026-09-02 (SR4/RM4), which halves their
mallocs (the payload no longer boxes); the per-node SPINE box remains and
still leaks, and RM0 recorded that no workload constructs enough of them to
justify RM2/RM3. And **the "do not start SR1 for performance" verdict
was wrong for the reason the SR plan's own section 5 warns about**: it was
priced against `logic.tur`, a workload built entirely from recursive types and
therefore structurally blind to the change being judged.

`dump-refine-json-under-reports-caps` was resolved 2026-08-26 and moved to
[docs/archive](../archive/dump-refine-json-under-reports-caps.md). Its root
cause is right in substance and wrong in mechanism, in a way worth knowing
before trusting a similar diagnosis: the fix it proposes -- move the snapshot
above the probe block in `refine_discharge_one` -- **would have changed
nothing**, because the speculative branch returns before ever reaching that
snapshot. The probe is not an earlier phase of the same discharge; it is a
different `RefineObligation`, discharged in a different call during
elaboration, so there is no window in that function to widen. Traced on the
report's own repro. Fixed by the report's own alternative reading, a second
field: `caps_hit_probe`, filled by bracketing `rt_prove_paths` in
`rt_try_prove_return`, which is the only place "on whose behalf" is defined.
One more thing the trace turned up, unfixed and minor: `g_stats.path_probes` is
only printed when `proven_by_path` is non-zero, so a path probe that proves
nothing is invisible in the `TUR_REFINE_STATS` summary too.

`mir-aarch64-fp-aggregate-abi` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/mir-aarch64-fp-aggregate-abi.md). Fixed at the root
rather than contained: MIR's aarch64 back end now implements the AAPCS64 HFA
rule, so floating-point aggregates travel in `v0..v7` and c2mir agrees with a
natively compiled callee. Pin bumped to `472fa4c6`. The interim refusals are
gone with it -- including TUR-E0711, which existed for a matter of hours -- so
**reverting the MIR pin below that commit silently reinstates the miscall
instead of diagnosing it**; that warning lives next to the pin in
`cmake/mir.cmake`.

`sanitizer-gate-not-armed-in-ci` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/sanitizer-gate-not-armed-in-ci.md), finishing the
half that landed the same day: Linux was armed, macOS was not and could not be
measured from a Linux container. The macOS leg measures **0** findings across
2703 fixtures on Apple clang 21 / arm64, matching Linux, so the fixture-suite
step now sets `TUR_SANITIZER_GATE: "1"` unconditionally. The zero was confirmed
with a positive control rather than inferred from silence -- the failure mode
this gate has already had once is reporting a clean tree because nothing was
wired up (`note_sanitizer` missing from `export -f`). Note the macOS number was
taken on a local Apple-silicon box, not the `macos-latest` runner image.

`compiled-fixtures-are-not-leak-checked` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/compiled-fixtures-are-not-leak-checked.md). Its two
"still open" follow-ups had both landed in `3c457e92` without the report being
updated: the opt-in set went from 2 fixtures to **54**, and the gate is wired to
ctest as `tur_leak_check`. It had also been held back on the grounds that one
leak is still marked known -- but that leak is
[inline-c-option-carrier-box-leaks](../archive/inline-c-option-carrier-box-leaks.md)'s
to carry (since resolved 2026-08-30), and removing the marker is row 2 of
[workarounds-to-remove](../archive/workarounds-to-remove.md).

`logic-streams-are-strict` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/logic-streams-are-strict.md): `stdlib/logic.tur`
gained immature streams and a `zzz` delay macro, so a relation with infinitely
many solutions is now expressible and `run-logic n` costs n solutions rather
than the whole search. Landing it needed a codegen fix first; the plan is in
[docs/archive/lazy-streams-plan.md](../archive/lazy-streams-plan.md).

`fat_captures_borrowed` was found being read out of uninitialized arena memory
and fixed the same day; 60 in-tree fixtures had been tripping UBSan on every
suite run without anything failing, because UBSan here prints and continues.
Paper trail:
[docs/archive/history/](../archive/history/fat-captures-borrowed-read-uninitialized.md).

`dash-main-entry-point-never-invoked` was resolved 2026-08-25 and moved to
[docs/archive/](../archive/dash-main-entry-point-never-invoked.md): both
examples' entries renamed `-main` -> `main` and the snake tutorial corrected, so
the documented entry point is now the one the compiler calls. Its residue,
`examples-have-no-suite-coverage` (nothing exercised `examples/` and nothing
diagnosed a build with no entry point), was resolved 2026-09-02 and lives in
[docs/archive/](../archive/examples-have-no-suite-coverage.md).

`self-recursive-fn-returning-call-into-fat-sink` was resolved 2026-08-27 and
moved to [docs/archive/](../archive/self-recursive-fn-returning-call-into-fat-sink.md):
the stage-2 fat-result marking is now forwarded to the recursion binding
before the body elaborates, so a self-call sees what a fresh caller sees. The
same session retyped `backtrack.tur`'s `mbind`/`fresh` continuations `^fat`,
turning the documented "must be a fat closure" convention into a compiler
guarantee (a captureless lambda used to cross thin and be executed as data).

`closure-in-defdata-field` was resolved 2026-08-26 (all three cases) and moved
to [docs/archive/](../archive/closure-in-defdata-field.md). Capturing closures
in spelled-out fn fields work at arity 0..4 in both containers (two staleness
bugs: arity-0 excluded from boxing, match-arm extraction not consulting
boxedness); thin `:fn` / >4-arity fields reject a capturing store instead of
segfaulting.

`poly-call-in-statement-position-dropped` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/poly-call-in-statement-position-dropped.md).
`emit_stmt` treated four pure WRAPPER nodes (reinterpret/cast/ascribe/poly-wrap)
as emit-nothing, deleting the wrapped call; they now delegate to the inner
expression. The pinning fixture is green and the `with-untrailed` workarounds
are reverted.

`rc-ref-conversion-and-weak-upgrade-leak` was resolved 2026-08-23 and moved to
[docs/archive/](../archive/rc-ref-conversion-and-weak-upgrade-leak.md). Its
residue -- an Option built inside inline C -- was
`inline-c-option-carrier-box-leaks`, resolved 2026-08-30 and archived.

`byvalue-adt-app-rejects-nested-monomorphs` was filed and resolved on
2026-08-22 and moved to
[docs/archive/](../archive/byvalue-adt-app-rejects-nested-monomorphs.md);
`option<list<int>>` and `result<vec<T>,cstr>` now lower by value.

`rc-of-adt-leaks-the-payload` was resolved 2026-08-22 and moved to
[docs/archive](../archive/rc-of-adt-leaks-the-payload.md): one over-narrow
condition in `emit_expr.c` (the pointer-adoption path was gated on
`needs_drop_glue`), and both the leak and the redundant allocation went with it.
Its predicted coupling to the slab allocator was then confirmed -- with the leak
fixed and the slab on, ASan reports `attempting free on address which was not
malloc()-ed`, so that blocker got worse, not better. That is what led to the
slab being **shelved** on 2026-08-25: fixing it needs a whole-program escape
pass, which removes the "no ownership analysis" advantage that was its entire
case over plain reclamation -- and reclamation measures better anyway. Decision
record in the allocation report.

Two of the remaining three were filed with claims that later measurement overturned, and both
say so in place rather than having been quietly edited: an "8x degradation with
heap size" that was an artifact of measuring cumulative passes in one process,
and a "by-value lowering is highest leverage by a wide margin" that pricing
reduced to 1.8x. Read the "Withdrawn" and "corrects this report's original
advice" notes before acting on either.

## Documentation output (filed 2026-08-26)

Two findings from building the docs pack (OD1,
[docs/guides/offline-docs-guide.md](../guides/offline-docs-guide.md)). Neither
is new -- both have been shipping on turmeric-lang.com -- but neither had
anything watching for it until the pack's link and collision passes existed.
Both repro with a plain `just docs`, and both are reported on every run.

A third finding from the same pass was *fixed* rather than filed: a bare
` ``` ` at column 0 inside a ```` ```turmeric ```` fence closes the enclosing
block, so the rest of the guide rendered as prose.
`docs/guides/thread-pool-guide.md` had been shipping five mangled code blocks
that way. `genguides.py`'s `widen_nested_fences` re-fences such blocks now, so
the class of bug is closed, not just the one instance.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[blockquoted-fenced-code-renders-as-prose](../archive/blockquoted-fenced-code-renders-as-prose.md)~~ | medium | **RESOLVED 2026-10-03** (archived): fixed by f8535a9a the same day -- `tools/genguides.py` rewrites a quoted fence as a quoted indented block (`unquote_blockquote_fences`), dedents a list-item fence (`dedent_indented_fences`, sixteen more guides had been flattened), and fails the render when a fence survives as text (`unrendered_fences`).  Re-verified against the report's repro |
`guide-cross-links-to-unrendered-docs` was resolved 2026-08-26 and moved to
[docs/archive](../archive/guide-cross-links-to-unrendered-docs.md). It was
**eight** links in five pages, not four in three: the report's list came from
the pack's link pass, which only sees pages that made it into the pack, and a
grep plus arming the gate found four more of the same shape. The "missing"
`memory-management-guide.md` was not missing -- `gc-guide.md` is that guide
under a different name, and its description matches the linking paragraph
point for point, so fix direction 1's "confirm with the author" was avoidable.
The rest went to GitHub blob URLs, which three other guides were already using
for the same kind of reference. `--strict-links` already existed in
`tools/genpack.py` with nothing invoking it; the Justfile `docs` recipe (what
CI runs) now passes it, so a new dead cross-link fails the docs build. One
detail worth carrying: a `../reported/` link rots **twice** -- once because
that directory is not rendered, and again when the report is archived and the
blob URL moves too.

`two-stdlib-modules-render-to-one-api-page` was resolved 2026-08-26 and moved
to [docs/archive](../archive/two-stdlib-modules-render-to-one-api-page.md).
Neither fix direction as written: option 1 would have been a semantic change
(`defmodule` wraps and namespaces the body, and that file is pulled in by a
`load`, not an `import`), so option 2 was taken **scoped to the fallback** --
a filename-derived pseudo-name now carries its subdirectory, while a *declared*
module name keeps its identity and URL. That confines the URL churn the report
objected to: five `stdlib/seq/*` pages move, and `stdlib/seq/core.tur`
publishing itself as `tur/core` was wrong anyway. The pass turned up a second,
worse instance in the same function: the `defmodule` scan matched inside
comments, so `stdlib/turi/eval.tur` was publishing as **`myplugin/core`** --
a name from an example in its own docstring -- with no page for `turi/eval` at
all. Both collision checks are now hard errors naming both files; `just docs`
writes 148 pages for 148 modules, was 147.

## Formatter (filed 2026-08-25)

Filed from investigating the claim that "`tur fmt`'s own output isn't
idempotent and eats the module docstring." The idempotence half is real; the
module-docstring half **does not reproduce** and the report says so. The
underlying defect is worse than either: silent comment deletion.

Verified by a two-pass `tur fmt` sweep over 223 files (`stdlib/`,
`tests/fixtures/`, `examples/`, `tutorials/` -- every `.tur.sweet` plus every
`.tur` containing a `;;;` line). Exactly two files are non-idempotent, both
from this bug; `;;;` counts were identical pre/post format across all 223,
which is the evidence against the module-docstring claim.

| Report | Severity | One line |
| --- | --- | --- |
| ~~fmt-drops-comments-in-handle-and-binding-modifier-gaps~~ | -- | **Resolved 2026-09-02**: every header/arm printer re-emits gap comments (shared `fmt_header_items`, cond-style arm loops), `^mut` takes the name slot in binding vectors, a trailing comment before a closing paren survives, and `&mut x` prints back as the sugar (its paren form re-reads doubled). Residue list 0 lost / 0 non-idempotent; seven harness cases. Archived to [docs/archive](../archive/fmt-drops-comments-in-handle-and-binding-modifier-gaps.md) |

`fmt-drops-comments-inside-bracket-vectors` was resolved 2026-08-26 and moved
to [docs/archive](../archive/fmt-drops-comments-inside-bracket-vectors.md). The
guard went into the measure (`fmt_measure_src`), not the two call sites the
report proposed, so no *enclosing* inline check can flatten a commented form
either; gap re-emission was added to every collection printer plus `fmt_call`
and `fmt_cond`. Three things the report did not have, all now in the archived
write-up: the `F_VEC`/`F_MAP`/`F_SET` inline checks omitted the
`w != UINT32_MAX` test, so uint32 wraparound would have made the new guard a
no-op at any column > 0; re-emitting a *trailing* comment on its own line
preserves the bytes but reattaches it to the following element, which is a
different claim about the code, so same-line comments now stay on their line
(this also fixes the `; third field` relocation in the repro); and a comment
before a closing bracket puts the bracket *inside* the comment unless the
printer breaks first, which turns silent loss into a syntax error. The
before/after sweep is 70 -> 8 files losing comments and 9 -> 7 non-idempotent,
with no new failures; the remaining 8 are the row above.

## Diagnostics (filed 2026-08-25)

*(empty -- the one row here is resolved)*

`fn-name-diagnostic-misleads-toward-letrec` was resolved 2026-08-26 and moved
to [docs/archive](../archive/fn-name-diagnostic-misleads-toward-letrec.md),
along its fix direction: `(fn <sym> <vec> ...)` is detected as a named-lambda
attempt and reports that, with `letrec` and named-`let` help lines carrying the
user's own name. `λ` shares the path and got it for free; a non-symbol in that
slot still gets the generic message. Two fixtures, cross-referenced so the pair
is maintained together -- the happy-path one runs *both* spellings the help
text recommends, so the suggestion cannot go stale silently. Worth knowing:
the report's own named-let snippet does not compile (its `if` branches are
`nil` and `int`); the claim it illustrates is true, the snippet is not.

## Try Turmeric / web REPL (filed 2026-08-25)

| Report | Severity | One line |
|---|---|---|
| ~~[try-share-links-never-encode](../archive/try-share-links-never-encode.md)~~ | medium | **RESOLVED 2026-10-01** (archived): `web/share-codec.js` gzips with the browser's `CompressionStream` in the same base64url format `pako.gzip` produced, and the three callers are async. Pinned by `web/tests/share-link.spec.js` -- Share then open the copied link in a fresh context (a hostile `#lang` first line stays inert), the buffer reaches the hash, a mangled link loads nothing |
| ~~[doc-lookup-poisons-the-playground-eval-session](../archive/doc-lookup-poisons-the-playground-eval-session.md)~~ | -- | **RESOLVED 2026-09-16** (archived) by [playground-session-hygiene-plan](../archive/playground-session-hygiene-plan.md) PS1-PS5: a lost elaboration session is rebuilt by replaying committed turns instead of re-elaborating everything as stdlib; the doc panel, `tur doc` and new `doc-lookup`/`doc-print` natives read `stdlib/docstrings.tur` from C, and `(doc ...)` works in every spelling under the interpreter; every def* form is redefinable by a later turn; Run rewinds to the stdlib and replays the other tabs. Two of the report's claims were corrected on contact: a lookup never spliced into the session (the failed turn was never committed), and `web/examples.js` is not the page's examples list. Also fixed on the way: after a `defmacro*`, any failed turn aborted the process ("too many source files") |

`try-docs-pane-forgets-scroll-position` was resolved 2026-08-26 and moved to
[docs/archive](../archive/try-docs-pane-forgets-scroll-position.md), along its
implementation notes, and **exercised in a browser** as its verification-status
section asks. Two things worth carrying to the next Try Turmeric change. First
a hazard the notes did not have: `rememberDocsScroll` must refuse to record
while the pane is closed, because a hidden element reports `scrollTop` 0 and a
still-queued `scroll` callback then overwrites the offset `closeDocsPane` just
banked -- scroll, hit Escape in the same frame, and the feature silently does
nothing. Second, a testing trap that nearly shipped: `showDocsPage` re-fetches,
so until that resolves the column still shows *and is still scrolled to* the
previous render, and a `waitForFunction` poll latches onto that stale value.
Two of three new tests passed against the unfixed file that way. Assert the
settled state (wait out `aria-busy`, then take one reading) -- and note that
toggling `display` preserves `scrollTop`, so a close/reopen appears to keep
your place for a moment even with no restore code at all.

## Found getting `turmeric-spices` CI green (filed 2026-08-28)

Ten findings from bringing the sibling `turmeric-spices` checkout's CI to green
([turmeric-spices#60](https://github.com/turmeric-lang/turmeric-spices/pull/60),
merged). Reported against `tur v0.40.0` / turmeric `5c9d533` on Linux
x86_64/gcc; **every repro was independently re-verified 2026-08-28** against a
freshly built `v0.40.0` on macOS arm64 / Apple clang, so all seven defects are
cross-platform and none is a stale-binary artifact.

The first three produce **no diagnostic at all**. The next three fail in `cc`,
with messages that point nowhere near the cause. The seventh is a SIGSEGV.

**Where the remaining red lives.** That PR took Linux from 20 failing spices to
0. macOS went from 32 to 11-15, and every one still red is red on `main` too --
pre-existing, not a regression. Its
[status comment](https://github.com/turmeric-lang/turmeric-spices/pull/60#issuecomment-)
sorts them into four classes. Two of them were blocked on `turmeric` and are
**fixed as of 2026-08-28** -- the compiler side is done and those jobs are worth
retrying; the remaining open row is the mbedTLS one:

| Class | Spices | Owner |
| --- | --- | --- |
| Frameworks not expressible in the manifest | `raygui`, `opengl` | **fixed** -- `docs/archive/cmake-deps-cannot-express-framework.md` |
| `ld: library 'mbedtls' not found`, cause not established | `tls`, `http`, `httpd`, `ws-client`, `ws-server`, `tourist-ws` | diagnosability item fixed -- `docs/archive/spices-ci-fetch-failure-downgraded-to-warning.md`; root cause itself still open |
| `dyld: @rpath/libz.1.dylib` -- static/shared preference | `zlib` | **fixed** -- `docs/archive/cmake-deps-link-name-not-overridable.md` |
| Genuine platform behavior differences (kqueue vs inotify, etc.) | `tourist`, `watch`, `wav`, `plot`, part of `tourist-session` | spice repo |

The mbedTLS class is six jobs presenting the *identical* misleading error. Why
it was undiagnosed is the CI row below, now fixed -- the `::warning::`
annotation carries the real `tur fetch` output instead of a generic message,
so the next occurrence should name the actual failure directly rather than
requiring a CI round-trip to find it. The root cause of the macOS mbedTLS
build failure itself is still open.

Two rows carry a **root cause that differs from how they were originally
reported** -- the narrowing is in the report, and a fix keyed on the original
framing would miss cases:

- `forward-referenced-nil-call-bound-to-auto-type` was filed as "`: nil` +
  self-recursion". Recursion is incidental: a plain forward reference with no
  recursion reproduces it, and `: void` is immune in both directions.
  **RESOLVED 2026-08-29** --
  [docs/archive/forward-referenced-nil-call-bound-to-auto-type.md](../archive/forward-referenced-nil-call-bound-to-auto-type.md).
  The narrowing held and the root cause was still somewhere else: not the
  emitter at all, but the reader parsing a bare `nil` in type position as
  `F_NIL`, which two forward-declaration pre-passes did not unwrap, so `: nil`
  forward-declared as the `TY_INT` placeholder. Worth reading before trusting
  any other report's "Fix direction" section -- both of this one's pointed at
  the wrong file.
- `nested-defn-accepted-outer-returns-zero` **RESOLVED 2026-08-29** as
  TUR-E0713 --
  [docs/archive/nested-defn-accepted-outer-returns-zero.md](../archive/nested-defn-accepted-outer-returns-zero.md).
  Two of the three stacked failures it diagnosed are not failures: a definition
  in expression position is a shipped feature (Phase B3 nested defn, with four
  fixtures relying on it), so its recommended fix would have deleted it. Only
  TAIL position is the defect. Its failure (3), the `return 0;` codegen
  backstop, is still unaudited. Chasing why it was silent turned up
  `nil-tail-not-checked-against-declared-return`, filed below.
- `module-level-def-with-linear-init-emits-no-global` **RESOLVED 2026-08-29** --
  [docs/archive/module-level-def-with-linear-init-emits-no-global.md](../archive/module-level-def-with-linear-init-emits-no-global.md).
  Its title is a misnomer and its central question does not arise: linearity is
  never consulted on the failing path. The control varied two axes at once
  (`:linear` opaque vs `vec-new`) and credited the difference to linearity; a
  plain non-linear `defopaque` global fails identically. Root cause was
  `def_is_opaque_type_decl` matching any `def` whose value merely HAS an opaque
  type, rather than the type declaration itself. A one-conjunct fix
  (`init == NULL`). Unblocks row 5 of `workarounds-to-remove`.
- `emit-value-dispatch-unbounded-recursion` **RESOLVED 2026-08-29** as
  TUR-E0712 --
  [docs/archive/emit-value-dispatch-unbounded-recursion.md](../archive/emit-value-dispatch-unbounded-recursion.md).
  Fix direction (1) (the depth guard) only; (2), shrinking the frame or moving
  to a worklist, is still open. The bound is 40: it must clear the worst
  sanitized crash threshold (47) and clear the deepest nesting that actually
  occurs in-tree (20, measured across 3014 files). Those two only just fit,
  which is the argument for (2).
- `defmodule-bare-toplevel-forms-silently-dropped` **RESOLVED 2026-08-29** as
  TUR-E0711 --
  [docs/archive/defmodule-bare-toplevel-forms-silently-dropped.md](../archive/defmodule-bare-toplevel-forms-silently-dropped.md).
  The drop was not a fall-through arm to turn into a diagnostic: the emitter
  never reads the module body list at all, so the check is new code, and it
  tests the ELABORATED expression rather than the head symbol (a macro
  expanding to a `defn` has an arbitrary head). Two non-definition forms are
  legal here and would have been broken by a bare `else`: a bare inline-C
  block, and module-level `defer`. Its second finding -- a suite registering
  zero tests exits 0 -- is a harness change and stays open.
- `hoisted-includes-wrapped-in-has-include` **RESOLVED 2026-08-29** --
  [docs/archive/hoisted-includes-wrapped-in-has-include.md](../archive/hoisted-includes-wrapped-in-has-include.md).
  Its symptom analysis was right and **both** its fix directions would have
  broken the build: the `__has_include` wrap is load-bearing and deliberately
  used -- `stdlib/fs.tur`, `term.tur` and `image.tur` each write a per-platform
  header bare, outside its `#ifdef`, to be hoisted and then skipped. 12 of the
  18 hoisted headers in-tree are platform-conditional. Fixed by making the
  tolerance opt-in (`/* tur:optional */`) and the default name the header.
- `control-form-around-if-double-unboxes-carrier-arms` was filed as "`let`
  around `if`". `do` does it too, and it is specifically the **residue of the
  2026-08-21 fix** to `byvalue-product-tail-var-double-unboxed-nonparametric` --
  that fix guarded both `emit_if` arms and not the do/let companion 900 lines
  earlier. Root cause pinned to `emit_expr.c:2106`.

`nil-tail-not-checked-against-declared-return` was resolved 2026-09-02 and
moved to
[docs/archive](../archive/nil-tail-not-checked-against-declared-return.md).
`RET_CONFLICT_NIL_BODY` joins the return-position dispatcher, so
`(defn f [] : int nil)` is a TUR-E0709. Its root-cause guess was right on the
mechanism and off on the shape: there was no `TY_NIL` *exemption* -- the
dispatcher is a list of predicates each targeted at one confusable PAIR
(float-vs-int register class, cstr-vs-int, bool-vs-integer) and nil simply had
no predicate, which is why one added predicate closes it with no re-plumbing.
Its warning about `return_kind` starting at TY_NIL was load-bearing and correct.

The scope line came from a measurement: the check fires on a nil LITERAL tail,
not any nil-TYPED tail, because checking every nil-typed tail also rejects
`(defn main [] : int (println ...))` -- **25 fixtures**, and the idiomatic entry
point where the 0 a nil tail produces is the wanted exit code. Every shape the
report argues from lands on the literal. `body_tail_is_nil_literal` peels
EX_DO/EX_LET/EX_LETREC, without which the multi-form and single-form cases
disagreed -- the inconsistency the report says does not exist.

Both diagnostics stay: a definition written literally in tail position still
gets TUR-E0713's paren message (it fires first), and the general check closes
the residual the report identified but could not fix -- a definition arriving via
MACRO EXPANSION has collapsed to EX_NIL_LIT and slipped past E0713's exact-head
match. The typeclass instance-method caller passes `check_nil_body = false`
deliberately, as a separate blast radius. Suite 2751 passed / 0 failed, zero
churn.

| Report | Severity | One line |
| --- | --- | --- |
| ~~spices-ci-fetch-failure-downgraded-to-warning~~ | -- | **Resolved 2026-09-04, item (1)**: turmeric-spices commit `e24a41a8` tees `tur fetch`'s output and folds it into the `::warning::` annotation, dropping the "often optional" editorializing. Archived to [docs/archive](../archive/spices-ci-fetch-failure-downgraded-to-warning.md). The durable fix (an exit code from `tur fetch` distinguishing optional vs required failures) is split out to [tur-fetch-exit-code-optional-vs-required](../archive/tur-fetch-exit-code-optional-vs-required.md) |
| ~~tur-fetch-exit-code-optional-vs-required~~ | -- | **Resolved 2026-09-09** via direction 1: an `:optional` dep that cannot be fetched is skipped (lock row dropped, run continues) and `tur fetch` exits 0 / 1 (only optional failed, lock written) / 2 (a required dep or step failed). Guides updated; `tests/run-spice-fetch.sh` pins all three. The spices-repo CI step (direction 2) is that repo's change. See [../archive/tur-fetch-exit-code-optional-vs-required.md](../archive/tur-fetch-exit-code-optional-vs-required.md). |

## Found getting `turmeric-spices` CI green (filed 2026-09-10)

One finding from the second pass at the sibling checkout's CI
([turmeric-spices#66](https://github.com/turmeric-lang/turmeric-spices/pull/66)),
which took that repo's matrix from 13 failing jobs to 92/92. Twelve of the
thirteen were macOS-only, and every other one was a defect in the spices
themselves -- this is the only `turmeric` bug in the set.

Reported against `tur v0.46.0` on macOS 27 / arm64 / AppleClang 21, which is the
compiler `macos-latest` now ships. That strictness is why this surfaced at all:
clang 21 makes `-Wint-conversion` an error by default where gcc still warns, so
the emitted C had been wrong for as long as the store site has existed and no
CI leg had ever said so.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[global-def-store-misses-int-ptr-bridge](../archive/global-def-store-misses-int-ptr-bridge.md)~~ | medium | **RESOLVED 2026-09-11** (archived): `emit_store_int_ptr_bridge` now applies the `let` binder's bridge at all four store sites; pinned by `tests/fixtures/global-def-int-ptr-bridge`. Original row: A module-level `def` whose declared type and initializer straddle the int64/pointer carrier duality emits the store with **no bridging cast** -- `hub_hymutex_1969 = __ps_526;`, global `int64_t`, temp `void *`. The READ side bridges correctly, and `let` bridges the identical straddle (`emit_expr.c:3178-3186`), so it is the store alone. Four emitters share it, in both directions: the whole-program and separate-compilation `EX_DEF` initializers, the `^thread-local` init fn, and plain `set!`. Invisible on gcc and older clang, fatal on clang 21 and GCC 14. Took out the macOS leg of the `ws-server` spice, worked around there by ascribing `:ptr<void>` instead of `:int` -- which is the better-typed spelling regardless, so the workaround is not a debt |
| ~~[typeclass-method-resolution-ignores-the-class](../archive/typeclass-method-resolution-ignores-the-class.md)~~ | **RESOLVED 2026-09-11, archived.** Symptom B and the two diagnostics landed earlier; symptom A -- order-dependent instance registration -- is fixed too. Not by reordering: instance bodies call ordinary defns while defn bodies need registered instances, a CYCLE no phase order satisfies (a two-sweep Pass 2 breaks stdlib at map.tur). Broken by TIME instead -- a defn that cannot resolve a class method is elaborated speculatively and retried once its unit is fully processed, in both elaborate_program and elab_defmodule |
| ~~[default-method-spliced-at-carrier-type](../archive/default-method-spliced-at-carrier-type.md)~~ | high | **RESOLVED 2026-09-16** (archived): a spliced default's `: a` annotations are the class signature's, already recorded, so `elab_definstance` now lets the spliced copy inherit the class signature under the instance substitution exactly like a bare-parameter method; `__inst_C_pick_float` is `double (double, double)` and `(g 2.5 7.1)` answers 2.5. Pinned by `typeclass-default-method-class-var-result` (float/cstr/int rows plus the int-only control). Original row: **Silent wrong answer.** A typeclass DEFAULT method whose declared types mention the class variable is spliced into each instance with its `: a` annotations intact and elaborated literally, so `C [float]`'s copy emits as `int64_t __inst_C_pick_float(int64_t, int64_t)` and converts its own arguments. Dispatch is correct; the SIGNATURE is wrong. Workaround was a constrained generic defn, which is what stdlib's `max`/`min` use and still do
| ~~[definstance-not-dispatchable-across-modules](../archive/definstance-not-dispatchable-across-modules.md)~~ | **RESOLVED 2026-09-11, archived.** An instance method is never `is_exported`, so all three linkage sites emitted it `static` and the header loop skipped it. All four sites (the CPS entry too -- its own comment warned about the contradiction) now consult `emit_inst_method_wants_external`, gated on the instance's ORIGIN FILE rather than `is_from_stdlib`: a `(load ...)`-ed file is spliced into every module that loads it, and the naive guard produced `duplicate symbol '___inst_Monoid_mempty_Any'` across 4 TUs |
| ~~[float32-block-temp-widens-to-double](../archive/float32-block-temp-widens-to-double.md)~~ | low | **RESOLVED 2026-09-16** (archived): fixed at elab, not in the emitter -- the tail literal of a `do`/`let`/`if` body is retyped to the declared `float32`/`float64` width (the sibling of the int-literal widening `elab_defn` already did), so the block temp is `float` and there is one rounding. Pinned by `float32-block-tail-literal` with `7.1`. The run.sh float-destination exclusion is left in place (comment updated): narrowing it is a corpus sweep of its own. Original row: A float literal closing a MULTI-expression `: float32` body gets a `double` temp and narrows on return; a single-expression body is fine
| ~~[byvalue-struct-param-as-if-arm-derefs](../archive/byvalue-struct-param-as-if-arm-derefs.md)~~ | medium | **RESOLVED 2026-09-16** (archived): not the `if` unifier -- the merge temp was right and the ARM bridge was wrong. `emit_arm_is_byval_agg_var` admitted only a by-value SUM parameter (a parameter is never in the localvar table); it now admits a by-value PRODUCT parameter too. Pinned by `byvalue-struct-param-if-arm` (both arm orders, call and ctor, plus the fold that already worked). Original row: A by-value struct PARAMETER used directly as an `if` arm emits `*(tur_adt_S *)(intptr_t)(x)` -- dereferencing the struct. `tur check` is clean, cc rejects it
| ~~[set-add-elem-hash-disagrees-with-set-member](../archive/set-add-elem-hash-disagrees-with-set-member.md)~~ | medium | **RESOLVED 2026-09-16** (archived), and it was two EMITTER defects, neither in set.tur: (1) `(set-add-elem__ (set-new) x)` recorded `A -> <tyvar>` (the receiver pins nothing) and the site-correction pass skipped bare-tyvar params, so the call ran through the carrier base whose `(.hash x)` is baked to `Hash[int]` -- a Sym hashed by its address; a bare-tyvar argument now pins a binding elab left abstract. (2) The clone the second call minted named its `(Set A)` result `int64_t` in the prototype and `tur_adt_Set__sym *` in the definition (a cc error on any plain generic returning a heap container, no typeclass needed); the result is grounded through the call's bindings when all of them are ground. Pinned by `set-add-elem-typed-member` (membership, Sym/cstr/int) and `generic-heap-result-spec-fwd-decl`. Original row: `set-add-elem__` computes a different hash than `(hash x)`, so elements it adds are counted by `set-count` but invisible to `set-member?`
| ~~[signal-compose-hand-rolled-vec-readers](../archive/signal-compose-hand-rolled-vec-readers.md)~~ | -- | **RESOLVED 2026-09-16** (archived). Fixed in [turmeric-spices#74](https://github.com/turmeric-lang/turmeric-spices/pull/74): both helpers deleted, `compose.tur` now has zero inline-C blocks, `spices/signal` is 6/6 green (verified here against a freshly built v0.49.1, not taken from the merge). The premise held; two things it did not predict did not. The hand-rolled copies had **already drifted** -- local `size_t len/cap` where stdlib declares `int64_t`, same width on a 64-bit host, which is why nobody noticed. And the Vec must be read through a `(Vec ptr<void>)` ascription: spelling the parameter as the type callers actually build with `vec-of` compiles clean and then **SIGBUSes** before printing, because the elements are fat closure boxes. **The interpreter coverage this report was FOR is still not collectible**, for a reason it did not know about -- `tur --interpret` takes no `-I` and runs no spice discovery, so no multi-module spice test can be interpreted at all; filed as [interpret-takes-no-include-path-or-spice-discovery](../archive/interpret-takes-no-include-path-or-spice-discovery.md) (since resolved). Original row: `spices/signal/src/signal/compose.tur` hand-writes `__vec-get-i` / `__vec-len-i` as inline C that reinterprets the Vec layout (`{data,len,cap}`) by hand, justified by a comment claiming project mode auto-loads only `macros.tur`. **Stale:** `stdlib_autoload.c`'s list is shared by single-file and project mode and carries `vec.tur`; `spices/plot` and `spices/linalg` both call stdlib `vec-get` in project mode today. Costs interpreter coverage -- run-turi.sh PASS-skips any fixture whose program has a user inline-C block -- and puts a private copy of the Vec layout in a spice. Delete both, call `vec-get`/`vec-len` |
| ~~[direct-call-does-not-inherit-caller-tyvar-binding](../archive/direct-call-does-not-inherit-caller-tyvar-binding.md)~~ | medium | **RESOLVED 2026-09-12** on both back ends. A constrained generic whose tyvar reaches NO parameter inherited the caller's binding when PASSED as a function value but not when CALLED -- and the two back ends picked DIFFERENT wrong instances (Gmax vs Gsum), which showed the answer was arbitrary. Root cause was one guard shape in three places: `if (n_abi_bindings > 0)` treats "the call pins nothing" as "nothing to do", when that is exactly when the caller's dictionary is needed -- the relay probe's entry, `emit_abi_register_call`'s zero-bindings gate (the hard one to find: the call is scanned with the correct enclosing spec and takes none of that function's `return;` sites, exiting through this gate instead), and `frame_record_abi`'s guard in the interpreter. Plus `spec_match_bindings` needing the `(!abi_changes && instance_changes)` arm or the clones dedup. Keyed on the constraint CLASS throughout. Pinned by `constrained-generic-instance-inheritance` (10 rows, both back ends, alpha-renamed siblings). |
| ~~[turi-nested-class-method-call-picks-first-instance](../archive/turi-nested-class-method-call-picks-first-instance.md)~~ | high | **RESOLVED 2026-09-16** (archived): the compiled fix ported after all, as a rule -- the driver's three dispatch gates never fire for a receiver that is itself a class-method call (its elaborated type is the representative's), so a fourth mirrors `emit_reresolve_disp_type`'s nested branch: when the inner method returns the class variable, walk inward to the dispatch tyvar and read its frame dictionary. Float was right by accident (turi runs the wrong instance's `(if (< x y) ...)` dynamically). All four listed fixtures run under `--interpret` with their `requires.compiled` markers removed. Original row: `tur --interpret` resolves a NESTED class-method call in a constrained generic to the first-declared instance -- a crash on cstr, a SILENT WRONG ANSWER over newtypes (`law-associative?` certifying a non-associative instance)

## Windows port

Originally filed 2026-07-31 during the Windows-support sweep on `main`. The
Windows CI leg that watches them was build-only until 2026-09-04 and now runs
the **full fixture suite** (`windows` job in `.github/workflows/ci.yml`), so
these are fixture-watched. All three originals are resolved and archived:
`windows-posix-inline-c-gaps` and
`windows-pipe-reactor-fixtures-do-not-build` (both superseded by main's
`requires.posix-apis` skip marker), and `windows-subprocess-and-shared-lib-gaps`
(2026-09-26). So are `windows-httpd-async-limit-hangs-on-ci` and
`jit-windows-support-spike` (both 2026-09-26). Open here: the Windows JIT
corpus is not in CI, and a c2mir conversion bug the spike's last fix exposed.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[windows-subprocess-and-shared-lib-gaps](../archive/windows-subprocess-and-shared-lib-gaps.md)~~ | -- | **RESOLVED 2026-09-26** (archived). The last open item, section 3, is fixed: `tur repl --engine jit` built its module-name shadow directory with `symlink()`, an `ENOSYS` stub on Windows, so every load there silently fell back to the subprocess build. `repl_jit_build` now hard-links, then copies, on Windows; verified on a MinGW cross build under Wine (`repl-spice-jit.sh` 1/3 -> 4/0), and the `windows-jit` CI job now runs that script. Everything else had already landed: `install` 34/34 on Windows, `fetch` clones/locks/integrity-checks, `new` and REPL spice loading verified end to end, `build --shared` emits a `.dll`, every shell-string site converted |
| ~~[windows-httpd-async-limit-hangs-on-ci](../archive/windows-httpd-async-limit-hangs-on-ci.md)~~ | -- | **RESOLVED 2026-09-26** (archived). The "fixture half" was the SERVER: the async 503 path did `send` + `close()` over an unread request, which sends RST, and a Windows client that had not yet read the 503 lost it to the reset (`WSAECONNRESET`, 0 bytes) -- so `busy` never reached 2. Every 503 connection was reset even on passing runs; two cores only made the client late enough to lose the bytes (12 of 20 hung). The 503 path now closes through the new `tur_reactor_linger_close` (FIN, drain until the client closes, 2 s cap), the fixture counts a client that drops out (`lost=N` in the diff, not a hang), `tur_reactor_linger_unit` pins it (and fails 3/3 against a plain close), and the `requires.win-concurrent-loopback` skip is gone. Original row: Hung on GitHub's Windows runners while passing locally; the reactor half (source slots never reused) was fixed 2026-09-04 |
| ~~[jit-windows-support-spike](../archive/jit-windows-support-spike.md)~~ | -- | **RESOLVED 2026-09-26** (archived). Its last defect, the whole-preamble fallback, was four: c2mir's win64 `va_start` was the MSVC `__va_start` (and, once pointed at `__builtin_va_start`, handed MIR the va_list's value instead of its address); `TUR_SETJMP` took MinGW's SEH `setjmp` under c2mir; winpthreads/`clock_gettime`/`nanosleep`/`sprintf` are not in tur.exe's export table; three prelude declarations were missing. All fixed, and variadic inline-C definitions now JIT on the split path too. Making that path link let every Scheme program reach the JIT on Windows (they never engage S2), which exposed escape-only `call/cc` under c2mir (fixed: a host `tur_win_stack_base` + the `tur_sjlj` pair) and a c2mir conversion bug (filed below). Section 4, the corpus in CI, moved to its own row |
| [windows-jit-corpus-not-in-ci](windows-jit-corpus-not-in-ci.md) | low | Filed 2026-09-26. The `windows-jit` job smoke-tests; it does not run `run-jit.sh`. Running it locally shows why that matters: 47 fixtures (httpd, image, tmpfile, file-port, rwlock, ...) fall back to cc on Windows -- c2mir cannot parse MinGW's `<sys/types.h>`/`<sys/stat.h>` family, which JIT_PRELUDE_WIN does not guard-stuff -- and the fallback ratchet, whose baseline is Linux-generated, fails on every one. Needs a Windows baseline (or the headers stuffed) before the corpus can gate; sharded like `windows-split`, a third of the corpus runs in ~5.5 min here |
| ~~[c2mir-llp64-long-long-vs-unsigned-int](../archive/c2mir-llp64-long-long-vs-unsigned-int.md)~~ | medium | **RESOLVED 2026-09-28** (archived): rjungemann/mir#4 merged as `79cb2905` and `cmake/mir.cmake` pins it. c2mir's `arithmetic_conversion` now follows C11 6.3.1.8, so `long long OP unsigned int` is `long long` on win64 too. Linux `^r7rs` JIT fixtures re-run on the new pin: 97 passed, 0 failed. Original row: on LLP64, `(int64_t)5u - 7u` was 4294967294 under `tur jit`, and `r7rs-bignums` was the visible victim (worked around in-tree; the cast stays) |
| ~~jit-win-prelude-shadows-user-fn~~ | -- | **Resolved at filing; archived 2026-09-10** (it sat here with a fixed-at-filing row). A program defining a libm name (`log2`, `sqrt`, `floor`, ...) was called through JIT_PRELUDE_WIN's `double` prototype: the int argument goes in xmm0 while the callee reads rcx, so the program ran to completion and printed a pointer. Fixed by dropping a prelude declaration the program itself defines; `jit_prelude_win_shadowed` (`src/jit_engine.c:652`) and `tests/fixtures/jit-win-prelude-name-shadow` pin it. Its "Still open" half was [jit-win-prelude-and-libc-names-drift](../archive/jit-win-prelude-and-libc-names-drift.md), resolved 2026-09-17. See [../archive/jit-win-prelude-shadows-user-fn.md](../archive/jit-win-prelude-shadows-user-fn.md). |
| ~~[jit-win-prelude-and-libc-names-drift](../archive/jit-win-prelude-and-libc-names-drift.md)~~ | low (latent) | **RESOLVED 2026-09-17** (archived): taken the way the report preferred -- derive and assert, rather than widen `libc_names` -- as property D of `tests/check-libc-collision-list.sh`, already the static guard for that table. Pure source read, so it runs on every platform, which was the point. Deviation stated in the resolution: this repo has no build-time script hooks (every check of this kind is a ctest target), so "fails the build" is "fails `tur_libc_collision_list`". The extractor is deliberately STRICTER than `jit_engine.c`'s own `jit_prelude_decl_name`, which sees only single-line declarations -- the multi-line ones it skips (`qsort`, `pthread_create`, ...) have no run-time shadow protection and so matter most. 108 prelude declarations, 83 already covered; the 25 that are not (20 `math.h`, 5 Win32/UCRT) sit in a `PRELUDE_ONLY` ratchet checked in BOTH directions, so a stale or spent entry fails too. All four drift shapes exercised by hand. Original row: Nothing keeps `src/jit_win_prelude.h` and `mangle.c`'s `libc_names` in step, so a name added to the prelude re-opens the shadowing hole **silently** -- a wrong answer, not a build error |

`windows-install-binary-placement` sat here and is **resolved** -- it now lives at
[docs/archive/windows-install-binary-placement.md](../archive/windows-install-binary-placement.md).
`tur install` works end to end on Windows (`run-install.sh` there: 34 passed, 0
failed). Chasing it turned up two defects that were not install-specific at all and
have their own archived records: `rename()` does not replace an existing file on
Windows, so every temp-file-then-rename atomic update in the tree was write-once
([windows-rename-does-not-replace](../archive/windows-rename-does-not-replace.md)),
and `state.tur` was written with unescaped backslashes, so the registry could be
written and never read back
([windows-state-tur-unescaped-paths](../archive/windows-state-tur-unescaped-paths.md)).

## Godot embedding

Filed 2026-08 while bringing the `turmeric-godot` GDExtension up on Windows.

`godot-baked-in-prelude-fails-to-eval` was resolved and moved to
[docs/archive](../archive/godot-baked-in-prelude-fails-to-eval.md) on
2026-09-07, when the fix finally shipped in
[turmeric-godot#1](https://github.com/turmeric-lang/turmeric-godot/pull/1). It had
been fixed since 2026-08-04 but sat on an unmerged branch, so it was
deliberately held open rather than archived against a fix nobody had.

| Report | Severity | One line |
| --- | --- | --- |
| [godot-aot-staged-build-lacks-godot-natives](godot-aot-staged-build-lacks-godot-natives.md) | high (AOT) | The staged transient project has no `godot-*` natives, so the AOT path cannot compile a script that touches the engine. Interpreter path unaffected |
| [godot-packed-array-push-is-a-no-op](godot-packed-array-push-is-a-no-op.md) | medium | All nine `godot-packed-*-push` natives in `turmeric-godot` `push_back` onto a COPY of the arena `Variant` and drop it, so every `Packed*Array` a script builds stays size 0 -- silently, with no diagnostic. `godot-array-push` / `godot-dict-set` are unaffected: Godot's `Array` and `Dictionary` are reference types, so mutating a copy mutates the shared body. Defeats the whole T3.D `Packed*Array` surface (vertex buffers, tilemap cells, byte blobs) |
| [jit-godot-embedding-spike](jit-godot-embedding-spike.md) | research | Whether the JIT can replace the AOT stage-and-subprocess cache in the GDExtension |
| [godot-shim-threading-under-workerthreadpool](godot-shim-threading-under-workerthreadpool.md) | medium (latent) | `cb_call` runs every interpreted method through `turi_call` on the script's single, unlocked `TuriEnv`, which is not thread-safe, so a node on a `WorkerThreadPool` sub-thread process group or the physics thread can corrupt it. The MIR half of J7 is already solved in-tree. Split out of the archived godot-binding-refresh plan's J7 |

## Platform-independent, found on a platform sweep

| Report | Severity | One line |
| --- | --- | --- |
| ~~unify-release-archive-layout~~ | -- | **Resolved 2026-09-09.** All four archives ship the prefix layout; `tvm` and Trowel accept either shape and landed first, and the `<exe_dir>` probe stays so older archives still compile. Turned up a live defect on the way: `tvm install` of a flat release produced a toolchain that could not compile at all, because normalizing only the binary stranded `libturt_runtime.a`. See [../archive/unify-release-archive-layout.md](../archive/unify-release-archive-layout.md). |
| ~~release-archive-cannot-compile~~ | -- | **Resolved 2026-09-06, all platforms** (archived 2026-09-09; it sat here with a RESOLVED row). `locate_runtime_lib` probes `<exe_dir>` and the archives ship `libturt_runtime.a`; the release workflow compiles a program out of the artifact it just built, on every leg. The layout split it left behind was `unify-release-archive-layout`, resolved 2026-09-09. See [../archive/release-archive-cannot-compile.md](../archive/release-archive-cannot-compile.md). |
| ~~jit-c2mir-implicit-decl-truncates-pointers~~ | -- | **Resolved at filing; archived 2026-09-10** (it sat here with a fixed-at-filing row). c2mir had no prototype for `strtok`, `strpbrk` or `memchr`, so their returned pointers came back cut to 32 bits -- non-NULL and unusable. Fixed by emitting `extern` prototypes under `#if defined(_WIN32) && !defined(__GNUC__)`; re-verified present in `emit_module.c` before the move. The list of three is a lower bound, and that residue is recorded in the archived file rather than kept open -- it needs a Windows JIT to answer. See [../archive/jit-c2mir-implicit-decl-truncates-pointers.md](../archive/jit-c2mir-implicit-decl-truncates-pointers.md). |
| ~~lsp-dap-windows-gaps~~ | -- | **Resolved** (archived 2026-09-09; it sat here with a RESOLVED row). stdio transport, spice walk-up, file-URI spelling and debuggee-output capture all fixed; on Windows the LSP harness is 70/0 and the DAP harness 68/68. Its "What remains" section says "nothing in this report". See [../archive/lsp-dap-windows-gaps.md](../archive/lsp-dap-windows-gaps.md). |
| ~~src-cmakelists-add-test-never-registers~~ | -- | **Resolved 2026-09-09** via direction 1 plus the direction-3 guard: `enable_testing()` moved above `add_subdirectory(src)`, `tur_trail` registers (and PASSES on its first ever run), and `tests/check-ctest-registration.sh` (ctest `tur_ctest_registration_lint`) fails if an `add_test` under `src/` is missing from `ctest -N`. See [../archive/src-cmakelists-add-test-never-registers.md](../archive/src-cmakelists-add-test-never-registers.md). |
| ~~pkg-hash-shells-out-to-sha256sum~~ | -- | **Resolved 2026-09-06** (archived 2026-09-09; it sat here with a RESOLVED row). The lockfile hash is an in-process SHA-256 over a sorted, `.git`-free walk; `tests/unit/pkg_hash.c` and `tests/run-spice-fetch.sh` cover it. See [../archive/pkg-hash-shells-out-to-sha256sum.md](../archive/pkg-hash-shells-out-to-sha256sum.md). |
| ~~windows-text-mode-read-rejects-own-files~~ | -- | **Resolved 2026-09-06** (archived 2026-09-09; it sat here with a RESOLVED row). Text-mode `fopen("r")` + `ftell` size + strict `fread` compare rejected every CRLF file; the readers open binary. See [../archive/windows-text-mode-read-rejects-own-files.md](../archive/windows-text-mode-read-rejects-own-files.md). |
| ~~windows-spice-fetch-shell-quoting~~ | -- | **Resolved at filing** (archived 2026-09-10; it sat here with a row although the report's own header said "Fixed in the same change"). `tur fetch` could not fetch anything: POSIX `'...'` quoting interpolated into a string cmd.exe runs, which passes `'` through literally (`could not create leading directories of ''./spices/demo''`). Fixed in `pkg.c` and `install.c` -- the latter also needed a real port of `rm -rf` (cmd.exe has no `rm`) and `cd /d` (a bare `cd` does not change drive). Re-verified on main before archiving rather than taking the report's word for it: `cmd_arg` is used at 11 sites in pkg.c, install.c carries `pkg_cmd_arg` / `TUR_CD` / `TUR_DEVNULL` / `inst_is_link` across 12, and every surviving `'...'` in either file is message text or the comment recording what `inst_rm_rf` used to be. Archived to [docs/archive](../archive/windows-spice-fetch-shell-quoting.md) |

The row above was found while adding a Windows regression test and is not a
Windows defect at all.  `term-set-cooked-restores-zeroed-state` was the
previous occupant and was
resolved 2026-08-05 (fix direction 2 -- one inline-C body owning the saved
state -- plus a pty-backed round-trip fixture); it now lives at
[docs/archive/term-set-cooked-restores-zeroed-state.md](../archive/term-set-cooked-restores-zeroed-state.md).
The heading stays because the *category* is worth keeping in view: a defect
found while sweeping one platform is not thereby a defect of that platform,
and that one had been in the POSIX path from the start.

## The `any` surface (filed 2026-09-07)

Three findings from surveying `any` while writing
[docs/archive/saffron-lang-plan.md](../archive/saffron-lang-plan.md). None is
a miscompile; all three are in the same area -- what happens when a value's
type is not statically pinned -- and all three would be prerequisites for any
dynamic-dispatch layer over `any`. Each has a one-file repro against `v0.44.2`
(`2da89e84`).

| Report | Severity | One line |
| --- | --- | --- |
| ~~local-fn-value-into-rank2-slot-gets-a-by-name-wrapper~~ | -- | **Resolved 2026-09-09** via fix direction 1, in three places rather than the one the report named -- its "rank-2 path in elab_call.c" was the plain-defn forall site, and `fmap` goes through the typeclass-method dispatch site, whose local pass-through arm was gated on `closure_fn_binding && !is_global` (a capturing-closure VALUE only): a parameter and a let-bound lambda have no closure binding and fell to the by-name wrapper. Now `!is_global`, in the args loop and its receiver-position twin; the plain-defn site got the same arm, with a diagnostic for a CONSTRAINED forall (its carrier leads with dict slots only a wrapper can absorb). Then the part the report did not predict: a PARAMETER segfaulted in the pass-through, because its binding type is an unboxed TY_FN identical to a let-bound lambda's, so the bare-fnptr shim ran fat-box memory as code -- a param's representation is decided by fat normalization, not the boxed bit, so the emitter gate now excludes it, and a `(let [g f])` alias is recognised through the emitted-local C-type registry. Fixture covers all four local shapes plus both controls, both back ends. The by-value STRUCT control fails for a pre-existing, independent reason, filed below. Archived to [docs/archive](../archive/local-fn-value-into-rank2-slot-gets-a-by-name-wrapper.md) |
| ~~fmap-over-byvalue-struct-element-passes-the-struct-as-int64~~ | -- | **Resolved 2026-09-09** via fix direction 1, and the diagnosis was half right: the spec's cast already spelled the one convention every thunk shares for a wide by-value ADT (argument as an int64 box pointer, result by value); two things disagreed with it. The generated WRAPPER's result: `make_poly_wrapper_ex` carried a by-value result only for a parametric monomorph (TY_APP), so a plain `defstruct` result stayed the int64 carrier and the fat-return path malloc-boxed it -- now the same non-heap by-value product test its own PARAM arm uses. And the spec's ARGUMENT: the typed-carrier dispatch spelled the wide slot `int64_t` (B4 slice 2) but never boxed the value, because that path was written for a binder that was ALREADY the carrier; it now spills the aggregate and passes its address like `fat_dispatch_box_arg`, with both B4 match-binder sites recording their `int64_t` C type in the emitted-local registry so the raw-carrier binder passes through untouched (`hkt-cata-wide-byvalue-carrier` regressed on the first cut until that record went in). Fixture: global defn, lambda, forwarded parameter, `none`, and `(Result Pt cstr)`, both back ends. Archived to [docs/archive](../archive/fmap-over-byvalue-struct-element-passes-the-struct-as-int64.md) |
| ~~poly-fn-with-any-parameter-is-called-with-the-int64-carrier~~ | -- | **Resolved 2026-09-09.** The call side was already right; the `__poly_` wrapper retyped only float-class parameters away from the int64 carrier, and an `any` is the same register-class problem one size up (a 16-byte `tur_tagged_t`). One added conjunct. `fmap` over `(Option any)` with an `any`-taking closure prints 42; pinned by `poly-fn-any-parameter` on both back ends. [Archived](../archive/poly-fn-with-any-parameter-is-called-with-the-int64-carrier.md) |
| ~~inferred-return-defaults-inconsistently~~ | -- | **Resolved 2026-09-07**, and the filed diagnosis was wrong: `elab_defn` DOES infer the body's type -- that block just runs after the return-position conflict check, so the check compared the body against the un-inferred `TY_NIL` default. Not a missing inference, an ordering bug. The fix skips the conflict check when the return is unannotated (`!return_annotated && return_kind == TY_NIL`), because an unannotated return has nothing to conflict with; both conjuncts earn their place, one separating unannotated from a written `: nil`, the other keeping the `goto done_return_annotation` paths checked. `(defn pi [] 7.1)` now emits `static double pi()`. Every annotated mismatch still errors -- verified across a seven-case matrix, since the risk was dropping a check rather than fixing one. Archived to [docs/archive](../archive/inferred-return-defaults-inconsistently.md) |
| ~~type-of-on-boxed-closure-diverges~~ | -- | **Resolved 2026-09-07** via the narrow fix the report recommended: one row in the preamble's name switch, so a `TY_FN` tag answers "fn" rather than falling through to "unknown". Pinned by `any-type-of-boxed-fn`, which both suites run -- the assertion is parity, not the string, since the divergence survived precisely because nothing compared the back ends on a function payload. Archived to [docs/archive](../archive/type-of-on-boxed-closure-diverges.md) |
| ~~any-fn-tag-does-not-discriminate-signatures~~ | -- | **Resolved 2026-09-07** via fix direction 2, so direction 1 (reject a fn target) was never needed and the `(cast x (-> int int))` capability survives intact. A fn payload interns a per-SIGNATURE id keyed on the fn type's rendered spelling (`"(fn [int] : int)"` -- arity, each param's TypeKind, result kind, C-ABI bit), riding P1's cross-TU hash with no further work. Direction 3 landed as real parity, not a documented divergence: `type_name`'s `TY_FN` case became a shared `tur_fn_type_key`, so the interpreter reconstructs the identical key from a `TuriClosure`'s `FnDef` -- two renderers would have drifted, and drift here IS the defect. Swept up a second one: the interpreter's cast switch had no `TY_FN` arm, so a fn target fell into `default: ok = true` and `(cast 7 (-> int int))` handed back an int typed as a function. Residual coarseness (fn arg types are TypeKinds, so `(-> Pt int)` == `(-> Qt int)`) is identical on both back ends. Archived to [docs/archive](../archive/any-fn-tag-does-not-discriminate-signatures.md) |
| ~~any-type-guide-examples-do-not-compile~~ | -- | **Resolved 2026-09-07** via the minimum fix direction plus the fixture the report asked for (`docs-any-guide-examples`, run by both suites, so these cannot rot again silently). Scope was wider than filed: compiling *every* example rather than the two named turned up four more of the same species -- `deftype` used to name a union and an intersection (it is the recursive binder, so a use site fails to unify; `defalias` is the documented tool), a fabricated `str` helper, a `defclass` with no `definstance`, and a stale `type-of` -> `"adt"` claim this guide's own text contradicts. One example could NOT be made to compile because the compiler is wrong: the gradual-typing section's `(debug-print x)` on a union-typed `x`, filed below. Archived to [docs/archive](../archive/any-type-guide-examples-do-not-compile.md) |

Two more were added 2026-09-07 by follow-up research into the same plan's S0
and D8. Both are **existing machinery that is wrong**, not gaps, and the first
is the most serious thing this survey turned up:

| Report | Severity | One line |
| --- | --- | --- |
| ~~any-type-ids-are-per-tu~~ | -- | **Resolved 2026-09-07.** The id is now a hash of the type's identity key (FNV-1a over `type_name`, forced clear of the `TypeKind` range) instead of this TU's intern index, so no coordination between translation units is needed and separate compilation, `--shared` and the CMake path all agree. `__tur_any_name_ext` became a registry: each TU publishes `{id, name, boxed}` rows into a global chunk list at static-init and lookups walk the union, replacing the single function pointer every TU overwrote. Failure mode 4 falls out for free -- the `boxed` flag rides the same row as the name, so a drop reads the flag the MINTING TU published. Pinned by `tests/run-any-type-id-multi-module.sh` (ctest `tur_any_type_id_multi_module`), verified to fail without the fix with all four behaviours visible. Archived to [docs/archive](../archive/any-type-ids-are-per-tu.md) |
| ~~forall-dict-byvalue-receiver-emits-uncompilable-c~~ | -- | **Resolved 2026-09-07** via fix direction 1, the guard: the dispatch site now rejects a class whose method takes a by-value aggregate when reached through a rank-2 `forall`, instead of emitting a cast cc will not accept. Direction 2 is now measured rather than guessed -- the caller ALREADY heap-boxes the aggregate into the carrier, so all that is missing is a per-instance deref wrapper in the dict SLOT; that is a dictionary-ABI change and stays with D8. The interpreter runs the guarded program correctly, so this is a compiled-back-end restriction, not a language one. Archived to [docs/archive](../archive/forall-dict-byvalue-receiver-emits-uncompilable-c.md) |
| ~~forall-dict-float-result-truncated~~ | -- | **Resolved 2026-09-07** via fix direction 1, and the report had leaned the wrong way: direction 2 (a real `double` return) would have meant a different carrier type per result type, i.e. a change to the shared `tur_poly_fn_t` ABI, so bit-reinterpreting was the SMALLER fix. Both ends were converting numerically; both now use `tur_sc_bits_f64` / `tur_sc_f64_from_bits` (producer in `emit_fns.c`'s clone return, consumer in `emit_expr.c`'s poly-call result, a new float arm beside the pointer-class arm that had always been there). The helpers already existed, with a comment saying an intptr_t cast would truncate -- the dict-clone path just never applied it. Archived to [docs/archive](../archive/forall-dict-float-result-truncated.md) |
| ~~any-narrowing-broken-for-parametric-receivers~~ | -- | **Resolved 2026-09-07.** All three fix directions landed as one change, because `is?` and `cast` now share a single target resolver -- they must agree, since `is?` guards a narrowing `cast` has to accept. An applied target (`(Option float)`) resolves through the shared annotation parser, so it interns the same `TY_APP` the widen site did; a bare constructor is a hard error naming the arity (forward-compatible with relaxing it to head-matching later); and a same-name mismatch now panics `holds a different instantiation of Option`. Interpreter parity restored by head-matching an applied target, with the residual divergence (interp cannot discriminate instantiations) documented in two compiled-only fixtures. Archived to [docs/archive](../archive/any-narrowing-broken-for-parametric-receivers.md) |
| ~~generic-fn-in-any-return-position-emits-uncompilable-c~~ | -- | **Resolved 2026-09-07**, and the filed diagnosis was wrong: the widen site did not fail to request the monomorph. Instrumenting both positions showed the call nodes are IDENTICAL -- same `(Option float)` result, same `A := float` binding -- and only emit diverged. `emit_abi_scan_expr`, the walk that seeds the monomorphization worklist, had no case for `EX_UNION_INJECT`, so a call under a widen was never scanned. Four cases added (the widen plus the three box readers). Scope was wider than filed: argument position and user-defined generics too, not just the stdlib return case. Archived to [docs/archive](../archive/generic-fn-in-any-return-position-emits-uncompilable-c.md) |
| ~~any-coercion-not-driven-by-expected-type~~ | -- | **Resolved 2026-09-07.** Fix direction 3's sweep found FOUR positions, not the two filed, each with a different cause: `let` (binding takes the initializer's type), `def` (hand-builds its EX_ASCRIBE and misses elab_ascribe's coercion -- a cc error, not a silent one), the `if` join (nothing pushed a declared `: any` return as the body's expected type), and letrec/named-`let` (`fwd_decl_scan_params` commits a forward param kind only from an allow-list that omitted `any`, so an `: any` accumulator was forward-declared `int`). The last was found by sweeping -- `cstr` and `float` accumulators worked and `any` did not. Archived to [docs/archive](../archive/any-coercion-not-driven-by-expected-type.md) |
| ~~interp-native-ctor-loses-adt-name~~ | -- | **Resolved 2026-09-07** via fix direction 1, after running the sweep the report asked for -- which showed the severity was understated. `is?` compares exactly the names `type-of` reports, so this was a **false negative** (`(is? x (Option float))` = 1 compiled, 0 interpreted), not a cosmetic string, and a type-case took the wrong arm silently. Scope turned out bounded: every native ADT construction goes through `turi_make_struct`, 11 call sites (`Some`/`None`, `Ok`/`Err`, `Left`/`Right`, plus one FFI record that is not a ctor). There is no ADT registry on the env, so the fix recovers the `CtorDef` from the binding `EX_DEFDATA` registers for each constructor, matching on the `adt_ctor_native` function pointer -- which is what makes it safe against a same-named defn -- and via `turi_env_find_binding`, since a `turi_env_get` miss would malloc an error string per construction. Attaching at construction rather than at lookup also fixes field-access-by-name and the `is_heap` copy rule, which read `ctor` too. The residual instantiation-vs-head-match disagreement was checked and is the documented one, identical for ctor-built values. Archived to [docs/archive](../archive/interp-native-ctor-loses-adt-name.md) |
| ~~interp-collection-handles-report-as-int~~ | -- | **Resolved 2026-09-07** via fix direction 2, which turned out to mean the interpreter's widen must BOX -- `EX_UNION_INJECT` was the identity there, so there was nothing to tag. It now boxes on exactly the condition the compiled widen mints an id on (a named type), which is what makes the two agree rather than merely both being non-silly; a struct, a primitive and a closure are deliberately not boxed (the last would have broken the fn-signature path). Both directions closed -- `type-of` "Vec", `is? (Vec int)` 1, `is? int` 0 -- and the handle stays usable through `cast`. The box never escapes because `cast` is the only route to a payload, INCLUDING an `is?` guard, which elaborates to `(let [x (cast x T)] ...)`, so one unwrap covers every consumer. Note the first attempt was dead code: the live widen site is `eval_unary_post`'s transparent-shim `default`, not the `eval_expr_impl` switch -- a probe printing nothing at all found that in one step. Archived to [docs/archive](../archive/interp-collection-handles-report-as-int.md) |
| ~~[interp-inline-c-opaque-segv-in-any-reflection](../archive/interp-inline-c-opaque-segv-in-any-reflection.md)~~ | medium (was **high**/crash) | **RESOLVED 2026-09-11**: the re-tag site reads the opaque def off the binding's full fn type and leaves an opaque result an immediate; `type-of` answers `Route` on both back ends. Original row: **CRASH FIXED 2026-09-08.** `type-of` on an `any` holding an inline-C `defopaque` value segfaulted the interpreter: the inline-C result path re-tags a TURI_INT as TURI_STRUCT when the declared return is TY_ADT, and an opaque IS a TY_ADT, so payload 7 became a `TuriStruct *` of 0x7. Fixed by hardening the two readers that dereferenced it (alignment + zero page -- facts, not heuristics: neither can reject a real TuriStruct*). Direction 1, the "real fix", is BLOCKED and measured: `FnDef.return_type` carries no AdtDef -- NULL for an opaque AND for a genuine defdata -- because it exists for the lifetime pass and stores `type_from_kind(return_kind)`. Residue is a divergence, not a crash: interpreted answers `adt` where compiled answers `Route`; and an opaque over a LARGE int is still bit-indistinguishable from a pointer. Needed its own runner (`tur_interp_inline_c_opaque`) -- run-turi.sh PASS-skips every inline-C program, which is why nothing caught it |
| ~~any-drop-inlining-warns-free-nonheap~~ | -- | **Resolved 2026-09-09** via direction 2 (direction 1 cannot see a payload that is a call result): `__tur_any_drop` is `noinline, noclone` -- `noclone` because gcc's IPA-CP otherwise minted a `.constprop` clone and warned from inside it. `run.sh`'s ratchet now FAILs on `-Wfree-nonheap-object`; fixture `any-opaque-immediate-drop-no-warning`. See [../archive/any-drop-inlining-warns-free-nonheap.md](../archive/any-drop-inlining-warns-free-nonheap.md). |
| ~~union-to-any-widen-emits-uncompilable-c~~ | -- | **Resolved 2026-09-07** via fix direction 1: `EX_UNION_INJECT` grew a fourth arm (beside the float bit-pattern and the by-value aggregate) that switches on the member index and re-tags with THAT member's `any` id, so the payload word carries across untouched and the `any` reports "int"/"cstr"/"Pt" rather than "union". All three positions go through it. **The fix introduced a use-after-free, found and closed here**: making the widen emittable let a union payload reach `let_binding_any_freeable`, which treats a non-frame-boxed inject as a box the scope allocated -- false for a union, where the widen ALIASES a box the union owns, so the `any` local's drop freed it and the next `match` printed garbage (visible without ASan). `any_expr_is_owned_temp` had the identical hole and escaped only because the frame-box rule sets a flag it reads as "not owned" -- coincidence, not reason, so both now say it directly. Unblocked the guide's gradual-typing example. Archived to [docs/archive](../archive/union-to-any-widen-emits-uncompilable-c.md) |
| ~~partial-application-widened-to-any-is-a-ptr~~ | -- | **Resolved 2026-09-07** via fix direction 1: `elab_partial_apply` typed both the EX_CLOSURE and its EX_LET as `TYPE_PTR_VOID`; they are a real `(fn [remaining...] : result)` with the `boxed` bit now, so `type-of` on a partial application answers "fn" on BOTH back ends and an `is?` target can name it at all. Blast radius was one fixture, not the sprawl the report feared -- `struct-curry-ctor`, because `type_fn` carries only TypeKinds and the curried constructor's nominal result was lost; copying `result_full_type`/`arg_full_types` from the thunk type (which already computes both) fixed it. **Surfaced a pre-existing segfault**: a CAPTURING lambda cast back out of an `any` ran a fat `{thunk,env}` box as code, because `cast` emits its call from the target type and `(-> int int)` is bare. A boxed fn now interns a distinct box id, turning that into a cast panic -- which also corrects an overclaim in the fn-tag row above (`cast` to a fn worked only for NON-capturing functions). Residue filed below. Archived to [docs/archive](../archive/partial-application-widened-to-any-is-a-ptr.md) |
| ~~any-cannot-recover-a-capturing-closure~~ | -- | **Resolved 2026-09-07** via the filed direction (fatten every fn payload at the widen), in three small changes rather than the large one expected -- the dispatch machinery already existed. `elab_coerce_to_any` shims a bare fn to fat (a `c-fn` is exempt: a real C pointer with no environment), `any_narrow_target` marks a fn target fat to match, and `elab_call_head_expr` marks a head taken from an `EX_ANY_CAST` as boxed -- the SAME signal the poly-carrier and cata-carrier heads beside it already use for a runtime-chosen thunk. A closure, a partial application, a lambda and a named `defn` now all round-trip and come back callable, on both back ends. The ownership question resolved better than feared: a file-scope fn's `{thunk, orig_fn}` box is a LINK-TIME CONSTANT, so `ensure_static_fatbox` hoists it and the widen allocates nothing -- safe to share precisely because nothing frees an `any` fn payload (`boxed = 0`). Pinned by `any-fn-widen-no-alloc` under the leak harness. Archived to [docs/archive](../archive/any-cannot-recover-a-capturing-closure.md) |
| ~~[any-fn-widen-through-local-binding-leaks](../archive/any-fn-widen-through-local-binding-leaks.md)~~ | low-medium | **RESOLVED 2026-09-11** (archived): an immutable local alias of a global fn (`widen_fn_alias`) hoists the same static box; `any-fn-widen-no-alloc` covers it. Original row: A fn widened to `any` through a LOCAL binding (`(let [f (fn ...)] (peek f))`) leaks its 24-byte fat shim box per widen -- unbounded in a loop. The two common shapes (a `defn` named as a value, a lambda widened directly) are leak-clean because their box is a link-time constant the emitter hoists to a static; a local binding is not one, and the guard cannot see through it to the lifted global. The frame-box rule would cover it in argument position but needs a non-retain mask bit that is 0 for an ordinary `any` parameter. Residue of the capturing-closure row above |
| ~~lang-unknown-base-diagnostic-names-nothing~~ | -- | **Resolved 2026-09-09** via fix directions 1 and 3: `detect_lang_dialect` threads the base token out through the `out_bad` pair the unknown-LAYER path already had and returns `READER_UNKNOWN`, and every report site (`main.c`, the module loader, the top-level elaborator, the interpreter) prints `unknown #lang base 'saffron/bogus' -- see \`tur lang-layers\` for the valid bases` as TUR-E0331 beside the TUR-E0330 layer wording. "not yet implemented" is kept for the case it describes. All three fixtures that asserted the old substring were unknown bases, as predicted, and now assert the new one; a layer typo still gets the layer message. Near-miss suggestions (direction 2) not done. Archived to [docs/archive](../archive/lang-unknown-base-diagnostic-names-nothing.md) |
| ~~saffron-any-return-defeats-the-frame-box-rule~~ | -- | **Resolved 2026-09-07, and the report's own root cause was half wrong.** There were TWO blockers in series and it named only the second. The first: `expr_subtree_has_inline_c` had no arm for the three dynamic nodes, so every Saffron body hit its conservative `default` ("may hide inline-C") and `elab_infer_nonretain_masks` skipped the function entirely -- before the result gate was consulted at all. A probe printed `get-x body=106 inlinec=1`; reading the control flow had produced a plausible wrong answer for the second time this stage. The `any` readers had needed exactly this fix before, with a comment saying so. The second blocker was the filed one, and the fix is not to widen the result whitelist but to stop treating it as the whole answer: an `any` result now RUNS the escape walk unconfined, which draws the distinction a kind test cannot -- `(defn f [x] (+ x 1))` and `(defn get-x [p] (.x p))` qualify, `(defn dyn [x] x)` does not. `box_uses_confined` gained arms for the same three nodes (dyn-op and dyn-field are non-aliasing readers; a dyn CALL keeps the strict answer, like an `fn_expr` callee). Better outcome than the filed directions expected: the box is not freed, it is never ALLOCATED -- the widen emits a caller-frame copy. RC-managed `any` boxes, which the plan proposed, were not needed. leak-check 87/0. Archived to [docs/archive](../archive/saffron-any-return-defeats-the-frame-box-rule.md) |
| ~~[byvalue-recursive-adt-boxes-are-never-freed](../archive/byvalue-recursive-adt-boxes-are-never-freed.md)~~ | low-medium | **RESOLVED 2026-09-28** (archived): a consuming callee now discharges an OWNED parameter -- each arm of its one `match` frees the box a moved-out binder came from and the sub-spine of an unused one (`byval-recursive-adt-consumed-by-callee`, 344 B / 13 allocs -> clean). "Owned" is a whole-program provenance fact, and needing it exposed a use-after-free in the 2026-09-07 scope-exit drop: a copy out of a `^borrow`, a global or a `vec-get` shares its spine and was freed under its owner. Both consumers now refuse such copies (they leak: `byvalue-recursive-shared-copies-leak`). Original row: **NARROWED 2026-09-19:** a local lent to a callee proven non-retaining is now freed by its own scope (`byval-recursive-adt-lent-to-callee`), and `:copy` is a documented regions contract (gc-guide). Open: a callee that consumes the value and returns part of the spine. Original row: **PARTIALLY FIXED 2026-09-07.** A self-recursive by-value `defdata` allocated one box per link and freed none (measured in plain Turmeric, no `any` anywhere: 3 cells / 3 allocations, 5 / 5, linear). A DIRECT self-reference now points the field's `drop_inner_def` at its own def -- making the existing by-value drop glue recursive, which is exactly a spine walk -- and a non-escaping local frees that spine at scope exit via `drop_recspine_<T>(&xs)`, the twin of the boxed-fn-field drop beside it. **`:copy` is the soundness line and it was measured**: drop glue makes a type move-only, and the shared-tail program is already TUR-E0201 there, while under `:copy` it compiles and the emitted C shows both boxes carrying the same tail pointer -- so `:copy` keeps the leak, and `with-region` (verified to reclaim the whole spine) is the answer. TWO RESIDUES left: a local handed to a callee is moved, and parameter-side discharge is blocked by pattern-match binders ALIASING the parent spine (a callee-side free would double-free, not leak); and `:copy`, which is most of the stdlib's recursive types (`Term`/`Subst`/`Stream`, `Regex`). Flag flip alone: 2861/0, no snapshot moved |
| [byvalue-recursive-shared-copies-leak](byvalue-recursive-shared-copies-leak.md) | low | **NARROWED 2026-09-28:** every use-after-free shape is closed. An rc-field struct copied out of a shared view (`^borrow`, global, container element, binder of one) was a live double decrement in four shapes; it is now cloned where it becomes an owner, and a struct with a borrowed `ref` field returned by value is TUR-E0108. The source of a copy lent to a `^borrow` callee is freed (result-alias bit + alias tracking), a fresh spine lent to a non-retaining callee is freed after the call (`(llen (build 3))` leaked every box), and a recursive local's top cell releases all its owning fields. The leak gate now builds with `TUR_RUNTIME=source` so runtime-side UAFs are instrumented. Open: a consuming callee reached from a borrow discharges nothing for any caller, and a container never frees its elements' spines. Original row: Filed 2026-09-28: a by-value recursive value COPIED out of a `^borrow` parameter, a global or a container element shares its spine with its source; it used to be freed under the owner (ASan use-after-free) and is now refused by the ownership provenance, so it leaks -- as does a source lent to a `^borrow` callee, a container's element spines, and every caller of a consuming callee one of whose callers passes a borrowed copy. Same hole, not covered: an rc-field struct copied out of a `^borrow` double-decrements today. Fix direction: forbid moving out of a borrow / global / container for drop-glue types |
| ~~[any-widen-stored-in-an-adt-field-has-no-owner](../archive/any-widen-stored-in-an-adt-field-has-no-owner.md)~~ | medium | **RESOLVED 2026-09-28** (archived): a compiled single-unit `#lang saffron` program now allocates from the r7rs-gc collector (`TUR_SAFFRON_GC=0` / `--no-saffron-gc` opt out), which reclaims the boxes no static owner reaches -- measured, since LSan cannot see that heap: a rebuild-map-fold loop peaks at 6 MB vs 84 MB and growing, and fits 256 MiB where it died before; all 95 Saffron fixtures pass collecting on every allocation. The leak gate builds Saffron with the collector off, so `saffron-higher-order` stays `known-leak` against the new `saffron-static-ownership-residue`. Original row: **Re-measured 2026-09-19:** still 680/17; the sibling's ADT-parameter inference cannot admit `lmap` because `h` (an `any` alias of the parameter) is handed to an opaque dynamic call. Refcounting the `any` box remains the direction. Original row: **Narrowed twice; the slug is now misleading and the heading is the accurate one.** The ADT-FIELD case is fixed (an `:any` field is owning, `__tur_any_drop` in the glue), and it was never the root: attributing `saffron-higher-order`'s 21 leaks to their sites gives 9 direct roots in ARGUMENT and RETURN position and 12 field/spine boxes that leak only because their holder does. `EX_UNION_INJECT` then turned out to have no arm in `expr_subtree_has_inline_c` or `box_uses_confined`, so the inline-C gate skipped the WHOLE function for any body whose result is a widen -- most Saffron functions, since an unannotated return is `any`. Both arms added (5 of 2311 fixtures improve). What REMAINS is one shape: a recursive walker passing a pattern-match binder into its own recursive call, where a caller-side free risks use-after-free and a callee-side free double-frees. Prerequisite for S6 |
| [saffron-static-ownership-residue](saffron-static-ownership-residue.md) | low | **Re-examined 2026-09-28, stays open:** the collector cannot be switched on for `--shared` -- it finds roots in the executable's data segment and registered thread stacks, and a host that loads the library keeps returned handles where no scan reaches, so it would free live objects; that needs an explicit root API at the FFI boundary. The shared-copies ownership work does not reach what remains (a box whose holder sits behind an opaque dynamic call). Filed 2026-09-28, by design: what the static drops cannot own in a `#lang saffron` program built WITHOUT the collector (`TUR_SAFFRON_GC=0`, `--shared`) -- `any` widens around a walker that hands binders to an opaque `(f h)`, a capturing closure returned as `any`, a capturing closure passed to a dynamic call from CPS code. A default build reclaims all three; the leak gate measures this residue (`saffron-higher-order` known-leak) so the static drops stay honest. **Narrowed 2026-09-28** (same day): a returned closure a `let` owns, and a lambda passed to a non-retaining `any` parameter, are dropped statically; `tailcall-dyn-leak` runs clean with the collector off and lost its `known-leak` |
| ~~[global-mut-cell-of-byvalue-adt-or-fn-emits-bad-c](../archive/global-mut-cell-of-byvalue-adt-or-fn-emits-bad-c.md)~~ | low-medium | **RESOLVED 2026-09-19** (archived): `set!` derefs a pass-by-pointer binder into a by-value target; a thin function-valued global is declared as a C function pointer, cast at init and `set!`, and forward-declared unconditionally. Pinned by `set-global-from-recursive-match-binder` and `global-fn-value-def`. Residue filed as the row below |
| ~~[fn-cell-set-with-capturing-closure-segfaults](../archive/fn-cell-set-with-capturing-closure-segfaults.md)~~ | medium | **RESOLVED 2026-09-19** (archived): a `^mut` fn cell is fat from its init, a boxed global dispatches fat, and a fat value into a cell that stayed thin is a static diagnostic. Alongside: a local's spine drop now refuses when a match binder escapes into a closure/store (was a use-after-free). Pinned by `fn-cell-capturing-closure` and `closure-retains-match-binder-of-dropped-local` |
| ~~[let-alias-of-fn-param-call-undeclared](../archive/let-alias-of-fn-param-call-undeclared.md)~~ | low-medium | **RESOLVED 2026-09-19** (archived): `emit_call_name`'s early exits spelled a local callee by its raw symbol; they now use the declared name. Re-pointing a `^mut` cell that aliases a poly-fn parameter is a static diagnostic. Pinned by `let-alias-of-fn-param-call` and `errors/set-poly-fn-param-alias-cell` |
| ~~[same-method-name-in-two-classes-dispatches-by-declaration-order](../archive/same-method-name-in-two-classes-dispatches-by-declaration-order.md)~~ | high | **RESOLVED 2026-09-15** (archived) via fix direction (1), the diagnostic: a cheap pre-scan over the CLASS registry asks whether two non-stdlib classes declare the name at all, and only then does the instance search keep going past its first exact match -- so the early exit and everything that resolves today are untouched, and the error fires only when both classes have an instance matching the receiver. `TUR-E0020` already existed and was missing from `diag_code_to_string`, so every ambiguous-dispatch error printed a bare `error []`; that row is added, which fixes the two pre-existing call sites too. Fix direction (2), the `defclass`-time warning, deliberately not taken. Pinned by `errors/same-method-name-in-two-classes`. Original row: A **silent wrong answer** with no diagnostic anywhere: two classes declaring the same METHOD name compile clean (`tur check` exits 0 with no output) and dispatch to whichever instance registered last, so swapping two unrelated `definstance` forms flips the answer. Both `typeclass_env_find_method` and `elab_user_method_instance_matches` take the first name match over registries whose head is the most recent entry, with no ambiguity check. Reachable whenever two libraries pick an obvious name (`encode`, `decode`, `size`, `render`); found deciding whether the msgpack spice could reuse json's `encode` -- it cannot, and distinct method names are the only guard. Fix: keep scanning after the first hit and diagnose a second non-stdlib class declaring the name |
| ~~vec-of-any-repr-decision-ice~~ | -- | **Resolved 2026-09-07.** The report's own next step (`--emit-abi-trace`) named the two deciders, and they turned out to be two DIFFERENT questions answered as one. Does `(Vec any)` NAME a monomorph? It failed every arm of `adt_app_type_arg_is_concrete` because the concrete-layout table rejects TY_ANY -- deliberately, and for an unrelated reason (a 16-byte by-value FIELD is an ABI change) -- so the binder fell to the carrier while repr_of said heap pointer. Admitted in the predicate that asks exactly that, leaving the layout table alone. How is an `any` ELEMENT stored? A slot is one word and a tur_tagged_t is two, so erasing it drops the tag; `repr_of` now answers BOXED_AGG at CONTAINER_ELEM, the same answer a by-value aggregate has, which brings store/read/element-free with it (`type_is_boxed_container_elem` IS that call -- vec-free releases the boxes, LSan clean). A third site was NOT predicted: the call-argument carrier crossing is gated on `rarg.kind == TY_ADT`, so a raw `tur_tagged_t` went into `vec_push_ex`'s `int64_t` formal. Fix direction 2 (the `vec-of` TUR-E0201) was one word: the type witness is never read, only its type, so `^borrow` -- which is what the layout table's own TY_ANY note said was blocking anyone from testing this. Residue filed below. Archived to [docs/archive](../archive/vec-of-any-repr-decision-ice.md) |
| ~~vec-any-interp-keeps-one-element-tag~~ | -- | **Resolved 2026-09-07** via fix direction 1: a byte per element rather than one tag per vec. Under `--interpret` every element of a heterogeneous Vec had reported the type of the LAST one pushed -- a wrong answer, no diagnostic -- because `native_vec_push` recorded one tag per vector ("the homogeneous element tag", its own words) and `vec-get` re-tagged every element with it. Right for as long as the type system enforced homogeneity; `(Vec any)` is the first element type for which it is not, and `any` is exactly the type whose content IS the per-value tag. The side table stays keyed on the vec header and process-lifetime; only the payload widens, and an unrecorded index still reads TURI_INT so vecs built by other natives are untouched. Five call sites took an index. The fixture now runs on BOTH back ends and they agree; it carried requires.compiled for exactly one commit, to record the divergence rather than hide it. Map and Set are NOT answered here -- they share no such mechanism, but whether their reads preserve a per-entry tag is unsettled and belongs before the `#map{}`/`#set{}` work. Archived to [docs/archive](../archive/vec-any-interp-keeps-one-element-tag.md) |
| ~~saffron-unannotated-param-container-cast-panics~~ | -- | **Resolved 2026-09-08 for containers.** `(defn pick [v] (vec-get v 0))` panicked `cast: any holds a different instantiation of Vec` compiled and answered correctly interpreted. `vec-get`'s `(Vec A)` is a `TY_APP` whose ARGUMENT is a tyvar, so the seam's exemption -- which tests the KIND -- never fired and it checked against an instantiation nothing had chosen. Direction 2 alone had been attempted and reverted because it turned the panic into a WRONG ANSWER (the element's box address); the missing half is that the call's tyvar bindings are collected from the argument BEFORE the seam replaces it, so `A` stayed open, the result collapsed to the int64 carrier, and the return widened the box POINTER. Grounding the open args to `any` PLUS re-collecting after the substitution fixes both. Fixtures assert VALUES on both back ends (int/float/cstr/bool/nested), not the absence of a panic -- the reverted attempt would have passed a crash-only fixture. A determined arg is left alone, pinned separately. Archived to [docs/archive](../archive/saffron-unannotated-param-container-cast-panics.md) |
| ~~match-arm-binder-in-any-monomorph-typed-as-carrier~~ | -- | **Resolved 2026-09-08, and it was worse than filed.** A match arm binder in a parametric monomorph took its C type from the ctor's DECLARED field type (a tyvar -> the int64 carrier) while the typedef emitter had already substituted and laid the slot out at the real type -- specialization half-applied. The filed `any` case was the LOUD half (aggregate-to-integer is a C error). Probing the fix with a FRACTIONAL FLOAT surfaced the silent half: `(Box float)` lays the slot out as `double` and the same line is a legal LOSSY conversion, so `(ub (MkBox 3.25))` compiled and returned **3** (7.9 -> 7, -2.75 -> -2) -- a wrong answer, no diagnostic, no Saffron involved. An integer-valued float would have hidden it entirely. Fixed by typing the binder from `adt_field_type_for_app` against the monomorph, scoped to scalar/cstr/`any` substitutions (aggregates keep the deref and box branches). The switch path already carried the complementary base-carrier leg. Archived to [docs/archive](../archive/match-arm-binder-in-any-monomorph-typed-as-carrier.md) |
| ~~saffron-match-any-scrutinee-on-parametric-adt-emits-bad-c~~ | -- | **Resolved 2026-09-09** via direction 1 (a parametric ctor call BUILDS at `any` in a Saffron file, as an `(:: arg any)` on the FORM -- the same answer S6 gives container literals) plus the guard relaxation it makes sound. The report's open `CK_MOVE` question was not a design question: `CK_UNIQUE` is 0 and `CK_MOVE` aliases it, so the zeroed `any` type argument was MOVE-typed and the arm binder inherited it -- `type_from_kind`, which is how parameters build theirs, is the whole fix. The report also treated `defgadt` and parametric `defdata` as one bug; they are two. An INDEXED GADT cannot take the widen (erasing an index discards what a GADT carries) and now gets direction 3's diagnostic instead of a C error about aggregates; an UNINDEXED one declines silently, because `errors/saffron-gadt-skolem-escape` asserts it still reaches the skolem check. Pinned by `saffron-match-parametric-adt` and `errors/saffron-match-indexed-gadt`. [Archived](../archive/saffron-match-any-scrutinee-on-parametric-adt-emits-bad-c.md) |
| ~~cps-coloring-walk-has-no-arm-for-union-inject~~ | -- | **Resolved 2026-09-09.** All three shapes print 42 on both back ends. The route the report called blocked was being attempted one layer too low: hoisting the control op into a let needs Expr-from-CVar synthesis in the CPS IR, and none in the ELABORATOR, where the temp is an ordinary Binding and a control op in a let INIT is already lowerable. The widen has to end up INSIDE the let -- wrapping it changes nothing, which is why a hand test appeared to disprove the route. Two of the three shapes turned out to be blocked by defects with no CPS content at all, found by running each repro's control: a concrete return annotation over a dynamic body never narrowed, and a dyn call's concrete argument was passed raw into a tur_tagged_t slot -- both pinned separately by `saffron-dynamic-body-concrete-return`. The first briefly turned a loud abort into a printed pointer while the hoist was in and the narrowing was not, exactly the trap the report warned about. The three fixtures its partial-fix note claimed existed do not exist, which is why that round's failure moved silently. Pinned by `saffron-callcc-under-any-nodes`. [Archived](../archive/cps-coloring-walk-has-no-arm-for-union-inject.md) |
| ~~cli-usage-error-paths-exit-zero~~ | -- | **Resolved 2026-09-09** via direction 1: `usage_error(usage_X)` prints the same text and returns 2 on every error path (16 subcommands, `tur run` included -- its unknown-flag arm lived in `justrun.c`), `--help` arms keep 0; `tur expand --help` (exited 2) and `tur smt --help` (exited 3) fixed in passing, `emit-c`/`emit-h` gained an explicit `--help` arm. `tests/run-flags.sh` asserts the PAIR per subcommand. See [../archive/cli-usage-error-paths-exit-zero.md](../archive/cli-usage-error-paths-exit-zero.md). |
| ~~gendocs-misparses-the-spaced-annotation-form~~ | -- | **Resolved 2026-09-09** via direction 2: a standalone `:` in `_parse_params` attaches the next token as the type (spelled fused), one helper reads the return position in both spellings and skips `#fx{...}`, and the tokenizer keeps balanced `()`/`[]`/`<>` groups whole -- compound types were broken in EITHER spelling. Stdlib re-measured: 2002 defs, 0 `:` params, 0 `:` returns (was 984 / 1064). `tests/tools/check_gendocs_parse.py`. See [../archive/gendocs-misparses-the-spaced-annotation-form.md](../archive/gendocs-misparses-the-spaced-annotation-form.md). |
| ~~cli-mallocs-module-base-dir-through-the-borrowed-path~~ | -- | **Resolved 2026-09-08.** `src/main.c` had two sites that malloc a directory slice and assign `env->module_base_dir` directly, leaving `module_base_dir_owned` false -- so `turi_env_free` correctly declined to free what it was told it did not own. The field's contract allows direct assignment for a BORROWED pointer; these handed it a fresh allocation. Both now call `turi_env_set_module_base_dir` (which strdups and owns) with a temporary they free immediately. A clean `--interpret` run is leak-free with detection ON, where it previously reported the path allocation on every run -- noise at the top of the channel CLAUDE.md documents for chasing real interpreter leaks. The sibling `*args*` cons cells still report and are explicitly not this bug. Import resolution rides on this field, so the five import-bearing ctest targets were run alongside both suites. Archived to [docs/archive](../archive/cli-mallocs-module-base-dir-through-the-borrowed-path.md) |
| ~~set-of-element-type-is-not-checked~~ | -- | **Resolved 2026-09-09** in the corrected order: direction 2 then 1. `Hash [any]` (typeclass-hash.tur) and `MapKey [any]` (map.tur) delegate by runtime tag through `is?`-narrowed arms -- an `any` holding "two" hashes and keys as a bare "two", 7.1 as a bare 7.1, anything else by its type name -- pure Turmeric, so both back ends agree without a native; the runtime HAMT's per-entry comparator is what makes mixing them sound. Then `set-add1` became a call to the constrained typed `set-add-elem__ [^Hash A ^MapKey A] [s : (Set A) x : A]`, the `vec-push!` shape, so `(set-of 1 "two")` is a TUR-E0001 at the cstr. Two things surfaced on the way: `definstance` over `any` is accepted (first in the tree), and the `set-add`/`set-remove`/`set-member?` macros spliced the hash expression INSIDE the let that had already aliased -- moved -- the same `any` element (use-after-move, TUR-E0201), fixed by binding the hash first. `#set{...}` in a Saffron file now takes the `[...]` widen, reversing the plan's "no change" decision with byte-identical behaviour. Fixtures: `set-of-any-elements`, `errors/set-of-heterogeneous`, `saffron-set-literal` (rewritten header). Archived to [docs/archive](../archive/set-of-element-type-is-not-checked.md) |
| ~~typeclass-dispatch-on-any-receiver-emits-uncompilable-c~~ | -- | **Resolved 2026-09-07.** `@TypeName` now implies the checked unbox -- the witness names the instance, which is exactly what the unbox needs -- so following the compiler's own hint works, and a wrong witness panics rather than reinterpreting. The single-instance half had a worse cause than filed: an `any` receiver took the KIND_ARROW match arm ("head is not primitive"), so it was an EXACT match with zero fallbacks and the ambiguity guard never applied at any instance count. The new check keys on the SELECTED instance, so it holds however dispatch got there, and a genuine `definstance C [any]` still resolves. The diagnostic now names all three working routes. Archived to [docs/archive](../archive/typeclass-dispatch-on-any-receiver-emits-uncompilable-c.md) |
| ~~adt-ctor-underscore-mangles-twice~~ | -- | **Resolved 2026-09-10.** A parametric ADT constructor with `_` in its name (`Wrap_q`) was declared in the by-value monomorph typedef through a local non-alnum fold that kept the `_` raw, while the ctor body and match arm addressed the member through the injective mangler (`Wrap_unq`): uncompilable C, no diagnostic. Both folds in `types.c` now call `mangle_adt_name`. Found by the first run of `tests/saffron-fuzz-src.py`. Archived to [docs/archive](../archive/adt-ctor-underscore-mangles-twice.md) |

The third row generalises past the docs: the builtin operator table is keyed on
concrete argument kinds and has no `TY_ANY` row, so `+`, `-`, `=`, `<` and
`println` all reject an `any` argument with TUR-E0006. That is not filed as a
defect -- `any` is documented as a storage and reflection type -- but it is the
single largest gap between what ships and what a dynamic dialect needs, and it
is scoped as D4/G3-G9 in the Saffron plan. (All landed: S3-S6, 2026-09-07/08.)

## Found writing the Saffron tour (filed 2026-09-14)

Five findings from drafting `docs/guides/introducing-saffron.md` and compiling
every example in it. Nothing here needed a fuzzer: each is the first shape a
newcomer's own file takes -- print a value, match an ADT, dispatch a method,
raise an effect -- which is why the existing Saffron fixtures, written from the
operation list rather than from a program, do not cover any of them. Two are
build breakers and two are silent wrong answers.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[saffron-perform-argument-skips-the-any-seam](../archive/saffron-perform-argument-skips-the-any-seam.md)~~ | -- | **RESOLVED 2026-09-14** (archived). `elab_perform` now runs the seam in BOTH directions, and the second was not in the filing: an `any` argument into a concrete parameter takes the checked unbox (a mismatch panics `cast: any holds int, not cstr`), and a CONCRETE argument into an `any` parameter takes the widen every other `any` slot gets. The second half only surfaced while fixing the `defeffect` default below -- the two are one fix. Pinned by `saffron-perform-any-argument-seam` and `saffron-perform-any-argument-mismatch`. It did NOT fix the hole underneath, now filed as `perform-does-not-typecheck-its-arguments` |
| ~~[saffron-effect-row-lost-through-unannotated-call](../archive/saffron-effect-row-lost-through-unannotated-call.md)~~ | -- | **RESOLVED 2026-09-14** (archived). Two gaps, and the filing was wrong about both: the `any` return was never involved, and the control that says so had always worked. (1) The row walks in effect_check.c and the coloring walks in cps.c lacked arms for the Saffron dynamic nodes AND for the `any` WIDEN -- the widen is the subtle half, since an unannotated defn wraps its whole body in one. (2) The CPS IR delegated the node to the direct emitter because its probe asks about control OPERATORS, not calls to effectful FUNCTIONS; fixed by an elaboration hoist, gated on a new `unit_has_user_effect` so the fixture corpus showed zero churn, and declining for lazy `and`/`or` (short-circuit) and NIL-typed operands (`void` local). Pinned by `saffron-effect-through-dyn-operand`, now compiled. One residue split out: `effect-row-lost-through-a-constructor-argument`, which is NOT Saffron-specific |
| ~~[saffron-cps-vec-element-carrier-mismatch](../archive/saffron-cps-vec-element-carrier-mismatch.md)~~ | -- | **RESOLVED 2026-09-14** (archived). Two fixes, one at each end of a vector: the CPS arg path now heap-boxes a `tur_tagged_t` into a carrier slot and passes the address (the bridge its own comment said belonged elsewhere, and that nothing downstream performed), and the CT_LETCALL arm now dereferences the carrier word into a `tur_tagged_t` binder -- the CPS twin of emit_expr.c's `temp_is_tagged` bridge. The filing's note that `vec-len` and a user fold were fine is what said the trigger is the element WORD, so both fixes key on the C spelling rather than on the callee. Pinned by `saffron-cps-vec-element-and-perform` |
| ~~[saffron-dynamic-dispatch-on-payload-adt-emits-bad-c](../archive/saffron-dynamic-dispatch-on-payload-adt-emits-bad-c.md)~~ | -- | **RESOLVED 2026-09-14** (archived). One missing arm in `emit_instance_dyn_table`: `dict_slot_param_is_carrier` correctly declines a parameter passed as `const T *`, and the conversion chain then fell through to the scalar `(T)__r` GCC rejects. The real trigger was narrower than filed -- not "carries a payload" but "large enough to be passed by pointer", since a single-int-payload ADT rides the int64 carrier and worked. Pinned by `saffron-dyn-dispatch-payload-adt` (all three ADT shapes plus a primitive through one dispatch site) |
| ~~[saffron-defeffect-params-default-to-int](../archive/saffron-defeffect-params-default-to-int.md)~~ | -- | **RESOLVED 2026-09-14** (archived). `defeffect` now calls `saffron_default_param_kind`, the same helper `defn` and `fn` use, rather than a hardcoded `TY_INT`; the helper lost its `static` instead of the one-line policy being copied into a second file, which is the drift that produced the bug. Defaulting rather than rejecting is right because an effect parameter is an ordinary value slot, unlike the two exclusions the helper already documented. Pinned by `saffron-defeffect-param-defaults-to-any` |

**Status 2026-09-14.** All five resolved. The fifth took two rounds -- its row
half landed with the others, its codegen half needed an elaboration hoist -- and
split off one residue that turned out not to be Saffron's at all.

Two findings came OUT of the fixing, both wider than the five they came from and
both open:

| Report | Severity | One line |
| --- | --- | --- |
| ~~[perform-does-not-typecheck-its-arguments](../archive/perform-does-not-typecheck-its-arguments.md)~~ | **medium** (was high) | **RESOLVED 2026-09-16** (archived): the aggregate and pointer-shaped half done the way the primitive half was -- one call verdict per shape, measured, made into a table (`perform_arg_shape_mismatch`); `Point <- Other` (which printed Other's field as `.x`), `ptr<void> <- "hi"`, `cstr <- nil`, `(Option int) <- (some "x")` and the rest are TUR-E0001 now, and the `ptr<void>` question was an allowlist of three kinds, not an `arg_ok` factoring. Six `errors/perform-arg-*` fixtures plus `perform-arg-aggregate-and-pointer-shapes`. Original row: **NARROWED TWICE 2026-09-15.** (a) ARITY was unchecked on BOTH sides and each had a segfaulting direction -- too few `perform` args left the declaration-sized slot array's tail uninitialised, a handler clause binding too many read past its end; all four shapes are now TUR-E0002. The reason arity was deferred the first time was a conflation: `defeffect` "parameter defaulting" defaults a parameter's TYPE, never makes an argument optional. (b) PRIMITIVE argument types are now checked, and **the filing's premise that this needs the ~300-line `arg_ok` factored out first is wrong for primitives** -- measured, a call is exact TypeKind equality with two alias pairs (int/int64, float/float64) and NO implicit widening; those 300 lines are tyvars, HKT carriers, aggregates, borrows and fn values. (c) The reason the first pass could only check LITERALS was that an un-annotated lambda param defaults to `int` and genuinely carries a `cstr` -- deliberately, since the bidirectional inference that would refine it is gated off for primitive expected types. Running the check in a later pass does not help (probed: still `int` at effect_check). Fixed by recording the missing information, `Binding.type_is_carrier_default`, so a declared `int` is distinguishable from a carrier one; the check now reaches variables, call results and field reads. (d) The CARRIER EXEMPTION is **dropped**, so `perform` now agrees with an ordinary call exactly: a call always rejected a carrier param (`expected cstr, got int`) while `perform` accepted it, which made the effect ABI the one place an un-annotated parameter escaped its declared type. Migration cost was one annotation per site and nothing else -- all 7 affected fixtures were fixed by writing `(fn [s : cstr] ...)` and every one produces byte-identical output, which is the evidence no behaviour depended on the laxness. `type_is_carrier_default` now drives the DIAGNOSTIC instead of suppressing it (a note naming the annotation to add, since the argument is not what wants changing). Note `errors/effect-fn-type-mismatch`: its un-annotated param made the new TUR-E0001 mask the TUR-E0009 it asserts -- a new check can hide an existing one, not only add to it. STILL OPEN: aggregates, `ptr<void>` params, and the `arg_ok` factoring for the non-primitive cases. Original row: `(perform (Log 42))` against `Log [msg : cstr]` compiles clean in plain TYPED Turmeric and segfaults |
| ~~[effect-row-lost-through-a-constructor-argument](../archive/effect-row-lost-through-a-constructor-argument.md)~~ | high | **RESOLVED 2026-09-15** (archived). Both of the filing's leads were wrong and ruling them out located it: the effect ROW was always correct (`--dump-effects` printed `mk : #{Ask}`), and the ctor leaf exemption in `cps_collect_calls` does descend into the argument list, as the filing suspected it might. The bug was one pass over -- `--dump-cps-coloring` printed `mk uncolored` beside the correct row, **and that disagreement between the two dumps is the whole diagnosis**. Neither walk in cps.c had an arm for `EX_MAKE_STRUCT` or `EX_GET_FIELD`, and `default:` meant "no children", so every node kind added since has been a silent hole -- the THIRD filing of that one shape. Both walks now fall back to a shared child enumeration instead of a per-kind switch. Coloring the caller then exposed a second layer (a ctor call has no `fn_binding`, so the CPS translation rejected the non-atomic argument as an indirect call); the existing elaboration hoist now covers the ctor call and the field read, producing literally the `(let [n (g)] ...)` spelling the filing named as the shape to preserve. 9 of 2977 fixture snapshots moved, regenerated in the same change. Pinned by `effect-row-through-constructor-arg`. Two adjacent CPS-backend limits it merely reaches are split out as `colored-call-inside-match-evicts-the-cps-backend`. Original row: `(Box (g))` where `g` performs loses the row, the handler is called unreachable, and the program aborts -- in plain typed Turmeric, every signature annotated. Was the last shape still failing after the Saffron effect-row fix, and is the half of it that was never Saffron's. The report names the constructor-as-leaf exemption in `cps_collect_calls` as the first place to look and warns that the exemption is right for the callee and wrong for its arguments |
 The guide routes around each open one and says so where a reader
would otherwise walk into it, and the two resolved notes were taken back out of
it when they landed.


## Found fixing the constructor-argument effect-row report (filed 2026-09-15)

One finding came out of that fix, and it is a finding precisely because the fix
made it visible: the coloring walks no longer drop the call edge that reaches
these two shapes, so what used to be a silent `tur: unhandled effect` abort at
run time is now a compile-time eviction diagnostic. Neither shape was
introduced by the fix -- the `let` control spelling hits both identically,
which is the measurement that separates them from it.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[colored-call-inside-match-evicts-the-cps-backend](../archive/colored-call-inside-match-evicts-the-cps-backend.md)~~ | medium | **RESOLVED 2026-09-16** (archived): both were gates in front of existing machinery -- `match_dk_ok` now admits a by-value record or flat sum scrutinee (the CPS match emitter already had the by-value path; a single-ctor record has no tag word and emits its one arm unconditionally), and `call_args_ok` admits a by-value aggregate into a cps->direct callee whose declared parameter IS that aggregate (the reject's carrier premise holds only for a generic base). A defstruct record handed to a CALL evicted the same way, so the "defstruct is fine" control was about the field-read consumer. Pinned by `cps-match-byvalue-scrutinee-colored-arm` and `cps-handle-byvalue-adt-result-into-call`. Original row: Two CPS-backend shapes a colored function cannot take, both reported as `this effect operation has no lowering here` pointing at the `perform` in a callee several functions away rather than at the form that actually evicted. (1) A `match` ANYWHERE in a colored function evicts (`unsupported form: EX_MATCH`) -- the repro's scrutinee is a literal, so no constructor argument is involved; distinct from the archived `cps-backend-effect-under-match`, which was the SEED walk and is fixed. (2) A `handle` whose result is a by-value **defdata ADT** evicts (`BODY-STRUCT-CORE`) -- and the useful control is that a **defstruct record** in the same position works, so the discriminator is ADT-vs-record, not "by-value aggregate". Both want work in `cps_ir.c`, neither is a follow-on to a coloring fix |
| ~~[cps-edge-walk-misses-nodes-and-colored-frames-leak](../archive/cps-edge-walk-misses-nodes-and-colored-frames-leak.md)~~ | medium | **RESOLVED 2026-09-16** (archived), in the report's order: the leak was an ORPHANED drop, not a missing one -- the delegated argument's sum box was queued in the direct emitter's `sum_pending` and no CPS statement ever drained it -- so the CPS emitter now carries a per-function deferred-drop table (captured at `emit_letraw`, fired after the letcall / letprim / cps->direct tail that consumes the atom); then `cps_collect_calls` took the shared enumerator; the cps->cps tail arm binds the answer and frees before returning (unreachable today: the shape needs a colored callee over an unpinned tyvar, which sig-rejects). `typed/result-basic` colors 23 and is leak-clean under run-leak-check.sh. Original row: Two findings that only mean anything together. (1) `cps_collect_calls` -- the walk that builds the call-graph EDGES coloring propagates along -- has arms for ~30 of ~119 node kinds and `default:` means "no children", so a call under any later kind records no edge; **EX_MATCH has no arm**, so a call in a match arm has never contributed one. Its sibling `cps_directly_uses_control` now falls back to a shared enumeration and is safe there (a missing arm is only ever a false negative). (2) Giving the edge walk the same enumerator is three lines and **does not work**: it sets `has_indirect` for an unresolved callee, so descending somewhere new colors any function that merely calls a callback there -- measured on `tests/fixtures/typed/result-basic`, 10 colored -> 23 (result-map, option-map, option-eq?, six typeclass instances), because every stdlib HOF calls its callback inside a `match`. And a newly colored frame LEAKS: the CPS path does not emit the `tur_region_free` the direct path does, so that fixture went from clean to 16 leaked bytes in `ctor_Result_Ok`. `run.sh` compiles fixtures unsanitized and cannot see it; `tests/run-leak-check.sh` is what caught it. Fix ORDER matters: close the CPS ownership gap FIRST, then widen, and gate on the leak harness. Written, measured and deliberately reverted while fixing `effect-row-lost-through-a-constructor-argument`, which took just the two safe arms |

### Feature-surface gaps from the same audit (filed 2026-09-16)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[multi-party-sessions-have-no-timed-receive](../archive/multi-party-sessions-have-no-timed-receive.md)~~ | low-medium | **RESOLVED 2026-09-28** (archived): `(timeout (-> A B T) [ok ...] [expired ...])` in a `defprotocol`, projected with every role but the receiver required uniform across the two branches (`TUR-E0220`, checked at declaration), `recv-timeout-from` on the receiving role, and a router slot `skip` so the message a receiver gave up on is dropped instead of blocking its sender or landing on the next receive. Pinned by `session-mp-timeout` and its `--interpret` twin. Original row: Binary sessions have `recv-timeout`; multi-party role endpoints have no equivalent, so a role blocked in `recv-from` has no bounded wait and no recovery from a stalled peer. Asymmetry, not a wrong answer -- and invisible, since the guide's Timeouts section does not say it stops at binary. The type machinery is not the blocker (`Timeout` has a dual rule, projection already lowers `choice`); what is missing is global-type syntax for a timed interaction plus its projection/mergeability rule, which is where the design risk sits. Hurts most where it cannot be worked around: a compiled multi-party program hangs, where `--interpret` would at least detect the stall |

## stdlib `:int` stand-ins (filed 2026-09-16)

The declared-signature counterpart to the session row above. Where
`session-payloads-are-int64-only` is an erasure hidden in a runtime slot, this is
the same erasure written into the stdlib's public types -- so it fails loudly
instead of silently, and is an expressiveness hole rather than a wrong answer.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[fmap-over-underdetermined-constructor-is-a-defless-shell](../archive/fmap-over-underdetermined-constructor-is-a-defless-shell.md)~~ | low (was also a silent wrong answer) | **RESOLVED 2026-09-29** (archived): a constructor that fixes some parameters and leaves others open is typed as the application, with each open slot a marked type variable (`tyvar_.open_slot`) that stays the carrier: `(Ok 7.1)` is `(Result float B)`, as `(ok 7.1)` already was. `(ok-val (fmap (Ok 41) inc))` prints 42. The bare ADT was also a silent wrong answer this row did not know about: `(ok-val (Ok 7.1))` printed the float's bits, and two float reads on the erased layout converted instead of reinterpreting. Pinned by `tests/fixtures/ctor-open-param-keeps-payload-type`. Original row: Filed 2026-09-28 (stdlib audit, S3). `(ok-val (fmap (Ok 41) inc))` is TUR-E0001 "expected (Result A B), got (? ?)": a constructor application with an undetermined parameter is typed as the bare ADT, so the dispatch has no TY_APP chain to ground `fmap`'s result against. Blocks the generic `either.tur`. Annotating the receiver works around it |
| ~~[word-result-fn-into-nil-slot](../archive/word-result-fn-into-nil-slot.md)~~ | low-medium | **RESOLVED 2026-10-03** (archived): a written `: nil` / `: void` makes a defn void (the body runs for effect), and `fn_type_structurally_compatible` then refuses a word-returning function in a `(fn [...] nil)` slot.  Suite 3530/0; all 776 turmeric-spices files check identically, `tourist-ws` route_test included |
| [stdlib-int-stand-in-audit](stdlib-int-stand-in-audit.md) | design defect; S1 subset high | The stdlib twin of the 2026-06-14 spices audit, which scoped itself to `../turmeric-spices` and never swept `stdlib/`. Same S1-S4 rubric. **38 callback parameters** typed `^fat f : int` (24, of which 17 in `httpd.tur`) or `ptr<void>` (14): measured, `^fat` enforces callable-ness but **nothing about the shape** -- a 3-arg `cstr` closure, a nullary `float` thunk and a 4-arg `int` closure all pass `tur check` into the same `^fat handler : int` sink. **19 container/cell payloads** declared `:int` across `chan`/`atomic`/`future`/`ref`/`fiber`/`backtrack-dfs`, which cannot carry a `float` at all. Plus an ADT-erased-in-its-own-API class (`either.tur` takes `e : int` throughout; `json/bool [v : int]`). **Updated 2026-09-28:** no `^fat f : int` is left in `stdlib/` (free, parsec, httpd's `compose-middleware-of`, and `future-map`/`future-then` now spell their fn types), and a variadic callee's fixed fn-typed parameter is shape-checked at last (it was never checked -- `errors/variadic-fixed-fn-param-shape-checked`). The remaining `ptr<void>` callbacks are raw C code pointers, a different design item. A generic `either.tur` was blocked on `fmap` over a receiver with an open slot grounding to `(? ?)`. **2026-09-29:** that is resolved, and the generic `either.tur` now waits on `generic-call-result-leaks-callee-tyvar-names` (below). **2026-10-02:** S3's `either.tur` landed -- `(Either L R)` throughout, `str->int-checked` declares `(Either int int)`; pinned by `either-generic-payloads` and `errors/either-from-right-default-type`. Left: `dfs-set` (`BtCell` parameterisation breaks every caller spelling bare `BtCell`), the `future.tur` redesign, `fiber-yield` (no handle to carry a type), and the C-code-pointer `ptr<void>` callbacks |
| ~~[generic-call-result-leaks-callee-tyvar-names](../archive/generic-call-result-leaks-callee-tyvar-names.md)~~ | medium | **RESOLVED 2026-10-01** (archived): a generic call's unbound result variable kept the CALLEE's name -- in a body over `[A B E]`, `(ok x)` was `(Result B B)` -- and the emitter re-captured it by rehydrating `ok`'s `B := E` from the enclosing spec's `B`.  `result-map` with a type-changing function reached cc.  Unbound result variables now bind from the expected type, the rehydration leaves spec-variable values to the composition, and a leftover collision is renamed apart (`ok.B`).  Unblocks the generic `either.tur` (stdlib audit S3).  Pinned by `tests/fixtures/generic-call-result-binds-from-expected` |

Two measured results worth keeping, because both correct the obvious reading.
First, a `:int` payload parameter is loud for a `float` (`TUR-E0001`) but
**unsound for aggregates**: `(chan-send ch (Pt 42))` passes `tur check` with exit
0 and dies in cc with `passing 'tur_adt_Pt' ... to parameter of incompatible type
'int64_t'`. That is a plain soundness hole, independently fixable, and fixing it
protects every site in the report at once -- it is the report's floor. Second,
`^fat` catching non-callables means the spices audit's S1 wording ("even a
flagrantly wrong handler signature compiles silently") is half right: the
signature is unchecked, but a non-function is rejected.

The report also records what is **not** a defect, so the next sweep does not
"fix" it: the phantom-typed carrier opaques (`List [A] :int`, `Backtrack [A]`,
`Kleisli [A B]`, `Zipper [A]`, `NonEmpty [A]`, `SizedBuf [n]`, `Goal [A]`,
`Parser [A]`) are the representation the typed path deliberately chose --
`list-typed.tur:20` says "phantom element type A" and cites
end-to-end-monomorphization-plan Phase 1.1. And the per-file `: int` counts
(`httpd.tur` 50, `schema.tur` 42, `range.tur` 38) are a scale signal, not a
defect count: most are genuine lengths, ports and indices.


## Found on CI while landing the constructor-argument fix (filed 2026-09-15)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[mcp-server-exits-mid-session-on-windows](../archive/mcp-server-exits-mid-session-on-windows.md)~~ | -- | **RESOLVED 2026-09-26** (archived). Not the transport: the CPS emitter's classification cache (`ensure_S`, and `fd_for_binding`'s table) was keyed on the program's and EmitCtx's ADDRESSES, and every MCP tool call compiles the file again in one process; once the allocator reused both addresses, stale CTerms with freed Bindings were emitted and the server died with an access violation (silent on Windows). Found three CI occurrences (09-10/14/18); a replay of the harness's call sequence killed 18 of 20 servers at call 25-40, 0 after the fix. `emit_cps_ir_forget` clears the caches around every top-level emission; `tur.exe` now prints the exception code and `tur.exe+offset` when it crashes on Windows; the harness waits for the exit status and gains a 200-call `test_mcp_repeated_analysis`. Original row: the MCP server closed stdout between `hover` and `definition` |

## Found building the msgpack spice (filed 2026-09-14)

Four defects found in one session implementing
[msgpack-spice-plan](../archive/msgpack-spice-plan.md) as
`turmeric-spices/spices/msgpack`. Three are build breakers with a known
workaround in that spice; the fourth is a silent wrong answer in the reader.

**All four are RESOLVED (2026-09-14) and archived.** Each is pinned by a
fixture; suite green. One filed root cause did not survive the fix and is
corrected in its archived file: `struct-instance-makes-generic-ok-val` is not a
producer/consumer split in `emit_module.c`'s ABI pass (that pass gets it right)
but a by-args spec match in the CARRIER base body, in `emit_expr.c`. The other
three were as filed -- the `unwrap` one is tyvar NAME capture, which its own
closing hint predicted.

The compiler side of each report is done. **Their `spices/msgpack` workarounds
have NOT been removed**: that repo is a separate checkout and was not present
in the session that fixed these. Each archived report keeps its "what to remove
when this is fixed" section, and those removals are still outstanding -- see the
follow-up row below.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[struct-instance-makes-generic-ok-val-see-a-byvalue-result](../archive/struct-instance-makes-generic-ok-val-see-a-byvalue-result.md)~~ | -- | **RESOLVED 2026-09-14** (archived), and it corrects the filed root cause. Not a producer/consumer specialization split in `emit_module.c` -- that pass interns exactly the right `ok_val__spec__...` and the spec body calls it. The broken body is the CARRIER base, where `find_matched_abi_spec`'s structural by-args match adopted the by-value instance clone: a return-dispatched method's by-value and carrier clones have identical arguments and differ only in the return, and `emit_spec_result_mismatch` treated the call's bare-`TY_TYVAR` result as "cannot tell". A tyvar result is not cannot-tell -- it is a value in a generic body, where everything rides the int64 carrier -- so it is now decisive against a by-value-aggregate spec result. A pure narrowing. Pinned by `tests/fixtures/struct-instance-byvalue-result-consumer`. Original row: Adding a value-struct instance to a return-type-dispatched class flips that class's method onto the by-value `Result` representation program-wide, but an `ok-val` inside a constrained generic stays at the carrier, so cc rejects the program. Nonlocal: the struct and the generic need not share a file, a module, or a spice -- deriving a codec for one more struct breaks a list decoder that never mentions it. Three controls narrow it: deleting the struct instance fixes it, a SCALAR second instance is fine, and how the first instance's body is written is irrelevant (the first guess, and wrong). json has the identical shape and compiles only because its primitive instances happen to be carrier-shaped inline C |
| ~~[generic-unwrap-specializes-by-the-enclosing-type-argument](../archive/generic-unwrap-specializes-by-the-enclosing-type-argument.md)~~ | -- | **RESOLVED 2026-09-14** (archived) by fix direction 1, and the report's closing hint was right: it is tyvar NAME capture. stdlib's `unwrap` quantifies over `A` and so did the enclosing generic, so `emit_abi_find_type_binding`'s `strcmp` resolved the callee's `A` out of the caller's `{A -> float}` (renaming the enclosing param to `B` is a complete workaround, confirmed as a control). `emit_abi_register_call` now unifies the callee's parameters against the concrete call-site argument types and lets those win -- under four measured guards: not a typeclass method; the parameter pins its tyvar inside a type application; that application's spine head is a concrete constructor, not a higher-kinded tyvar (`emit_abi_unify_collect` walks positionally and has no hole logic, so `m a` against `(Result int cstr)` transposes); and the argument's type is concrete AS WRITTEN rather than after spec resolution. Dropping any one of them regresses a fixture. The capture has a SECOND site on the result -- a bare-tyvar result is re-resolved through the active spec's bindings, by name again -- and correcting only the arguments left a `double` landing in an `int64` slot: it compiles and prints the right answer for a small integer, caught only by the suite's `-Wfloat-conversion` ratchet, which is not in the default cc flags. Pinned by `tests/fixtures/generic-unwrap-unrelated-option`. Original row: Inside a type-parameterized `defn`, `unwrap` on an `(Option int)` -- a type unrelated to the type parameter -- is specialized by the ENCLOSING type argument, so at `A = float` the emitted call is `unwrap__spec__double_tur_adt_Option__float` against an `Option__int`. Hides completely while the generic is only instantiated at `int`, where right and wrong coincide; the second instantiation breaks untouched code. `emit_abi_instantiate_type` consults the caller's binding set where `emit_abi_unify_collect` should first bind from the concrete argument |
| ~~[pinned-instance-dispatch-loses-an-opaque-return-type](../archive/pinned-instance-dispatch-loses-an-opaque-return-type.md)~~ | -- | **RESOLVED 2026-09-14** (archived) by fix direction 1. The `@TypeName` witness path rebuilt the result as `type_from_kind(result_kind)` -- from the TypeKind ALONE, which is lossless only for a primitive, and that is exactly why the `cstr` control passed. A `defopaque` is a `TY_ADT` whose identity lives in `as.adt_.def` and `type_from_kind` has nowhere to put it, so the result came back def-less and rendered `<adt>`. The path now prefers the declared `result_full_type` (substituting the instance's type args for the class's type params) and falls back to the kind only when the result is still abstract. Pinned by `tests/fixtures/pinned-dispatch-opaque-return`, with the `cstr` control alongside. Original row: A `@T`-pinned method call whose declared return type is a `defopaque` reports its result as `<adt>`, so every call site needs a redundant `(:: ... TheOpaque)`. The same program with a `cstr` return is fine, which is why json never hit it -- a binary codec cannot return `cstr`. Compile-time only, but the diagnostic names a type the user never wrote |
| ~~[int-literal-overflow-wraps-silently](../archive/int-literal-overflow-wraps-silently.md)~~ | -- | **RESOLVED 2026-09-14** (archived), all three symptoms, root cause as filed. `read_number` accumulates the MAGNITUDE into a `uint64_t` (defined whatever the input, killing the `:320` UB and the silent wrap together), range-checks it once the type suffix is known against the bound for the sign in hand, and applies the sign as `~mag + 1` in unsigned arithmetic rather than `-ival` (the `:407` UB). `0x`/`0b` literals stay exempt so `0xFFFFFFFFFFFFFFFF` still means -1, and a sized suffix keeps its own better message. `atom_int_typed` spells INT64_MIN as `(-INT64_C(9223372036854775807) - 1)`, removing the clang warning. A fourth site came along: `1f64` on an integer lexeme now recovers via `strtod` instead of the accumulator. Pinned by `tests/fixtures/int-literal-int64-bounds` plus two `errors/` fixtures bracketing the asymmetric boundary. Original row: `9223372036854775808` compiles clean and prints `-9223372036854775808`; `99999999999999999999` prints `7766279631452241919`. No diagnostic, though the fixed-width suffix paths ten lines below already emit exactly the right one. Same code makes the legal `-9223372036854775808` two UBSan findings (`reader.c:320` accumulate, `:407` negate) and emits `INT64_C(-9223372036854775808)`, which clang warns on. The sign is applied after accumulation, so INT64_MIN's magnitude is one past INT64_MAX throughout |
| ~~[msgpack-spice-workarounds-to-remove](../archive/msgpack-spice-workarounds-to-remove.md)~~ | -- | **RESOLVED 2026-09-15** (archived). Swept in [turmeric-spices#71](https://github.com/turmeric-lang/turmeric-spices/pull/71). Three rows swept as filed. The fourth did not, and finding out why is the value in this row: restoring the `DecodeMp` forwards turned up a SECOND defect the `struct-instance-makes-generic-ok-val` fix does not cover -- now archived as [instance-method-call-inside-an-instance-body-takes-the-enclosing-result-type](../archive/instance-method-call-inside-an-instance-body-takes-the-enclosing-result-type.md). The filed verification (`container-round-trip.tur`) was necessary but NOT sufficient: it passes 20/20 with the forwards restored and the second defect present; `derive-round-trip.tur` and `decode-checked.tur`, the two suites exercising a DERIVED struct instance, are what fail. Original row: dead scaffolding in `turmeric-spices/spices/msgpack` -- four carrier-shaped inline-C `DecodeMp` instances, two `__mp-arr-*` helpers keeping `unwrap` out of a generic body, eight `(:: ... Buf)` wrappers on pinned `encode-mp @Cons` calls |
| ~~[instance-method-call-inside-an-instance-body-takes-the-enclosing-result-type](../archive/instance-method-call-inside-an-instance-body-takes-the-enclosing-result-type.md)~~ | -- | **RESOLVED 2026-09-15** (archived), filed and fixed in the same change. `emit_abi_register_call`'s by-name binding rehydration matched the CALLEE's class type parameter against the enclosing spec's binding for the same name -- but a class's type parameter carries one name across every instance, and the callee's instance already pins it (`__inst_D_dec_int`'s `a` IS `int`). Inside `D [Pt]`'s body that minted `__inst_D_dec_int__spec__tur_adt_Result__Pt__cstr_...` over a body returning `Result int cstr`. The tyvar-name capture of `generic-unwrap-specializes-by-the-enclosing-type-argument` one layer up -- a class's type parameter rather than a `defn`'s. Latent while every instance is carrier-shaped; one by-value instance result exposes it. The rehydration now takes the pin from the callee's own `owner_instance->type_args`. Pinned by `tests/fixtures/instance-method-forwards-inside-instance-body`, whose primitive instances FORWARD to a typed function -- an inline `(ok ...)` body specializes to the same wrong result type and stays self-consistent, which is why the sibling `struct-instance-byvalue-result-consumer` fixture does not catch it |


## Found auditing session types under the interpreter (filed 2026-09-16)

Three defects found while answering "can session types be expanded on turi?"
(the answer, and the measurements behind it, are in
[turi-session-expansion-plan](../archive/turi-session-expansion-plan.md)). The
headline of that audit is that the parity matrix row for Sessions is **stale**:
15 programs covering every session fixture shape run correctly under
`tur interpret`, and the carve the row names was deleted when Slice A landed.
These three are what is genuinely left.

They interact, so read them together: the third one is why the second one's
fixture passes vacuously, and it also produced two false leads about the
interpreter's deadlock detection -- that detection is **sound**, do not go
hunting for a false-positive deadlock in the channel runtime.

**Two more were added 2026-09-16** from a follow-on pass over the session
*feature* surface (rather than its interpreter parity): the payload-type row and
the multi-party-timeout row at the bottom of this section. The payload one is the
most consequential finding of either pass.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[compiled-async-fiber-deadlocks-on-a-session-op](../archive/compiled-async-fiber-deadlocks-on-a-session-op.md)~~ | medium | **RESOLVED 2026-09-26** (archived): an `async` body that captures a Session / Role endpoint runs on its own OS thread and `await` joins it (`tur_async_thread_via`); TUR-W0043 now fires only for a body that makes both endpoints itself. Pinned by `session-async-peer-on-thread`. Original row: `(async (fn [] (recv ch)))` **hangs the compiled binary** while the identical program runs under `--interpret`: compiled `async` runs its body synchronously on the spawner's stack and the session runtime blocks that thread on a condvar. Since 2026-09-17 it is no longer silent -- `TUR-W0043` warns at the `async` site and points at `session-spawn` / `session-join` (`stdlib/session.tur`, landed 2026-09-16), which is the working spelling on both backends. What remains is the real fix: making a session op an `await`-shaped suspension point in a compiled async body, scoped in the report as a plan-sized change |

## Found building the nng spice (filed 2026-09-16)

Two defects found writing `turmeric-spices/spices/nng`, a Tier-3 wrapper over
nanomsg-next-generation. Both are loud (the C compiler rejects the emitted
translation unit) and both have workarounds the spice ships with a pointer
back here, so neither blocked it.

They are unrelated to each other, but they share a shape worth naming: each is a
per-binding or per-field emission site that asks a payload's C type a question
without first asking whether that payload is a carrier (the first) or inhabited
(the second). Both are seams the end-to-end monomorphization plan removes.

**Both RESOLVED 2026-09-16 and archived**, in the same change that filed them.
Each fix turned out to be a SHARED-HELPER problem rather than a one-site patch:
the first decision already had two byte-identical copies and needed a third
site, and the second had four sites across three files -- including the CPS
mirror, which is the one that actually emitted the failing line. Both spice-side follow-ups are settled
(turmeric-spices#75), and differently: `spices/nng`'s `Ack` opaque is gone, and
its pub/sub retry KEEPS its delegation -- inlining a twelve-caller helper to
re-prove a defect a compiler fixture already pins would be worse code. "Undo the
workaround" is the obvious reading of a fixed report and it is right about half
the time.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[tail-recursive-let-drops-carrier-bridge](../archive/tail-recursive-let-drops-carrier-bridge.md)~~ | -- | **RESOLVED 2026-09-16** (archived), root cause as filed, fixed by the report's structural direction rather than its fix direction 1 -- the decision is now `emit_let_init_carrier_bridge_type` and the TCO back-edge is kept. Original row: A `let` that binds a carrier-returning producer (an inline-C body declared `: (Result T E)`) inside a SELF-TAIL-RECURSIVE body is emitted as `struct x = <int64_t>;` -- `emit_tail`'s inline `EX_LET` arm (`emit_fns.c:718`) assigns `emit_value`'s result straight into a by-value-typed local instead of routing it through `emit_carrier_bridge`, which the non-TCO path (`emit_let_value`) does. Both ingredients are required: delegating the receive to a non-recursive helper fixes it, and so does taking the recursive call out of tail position. The arm's own comment already notes it duplicates `emit_let_value`'s per-binding bookkeeping (for `any` drops) -- the bridge is the piece that was not duplicated. Rejects a retry loop over a fallible operation, which is the obvious spelling of a poll |
| ~~[result-nil-ok-payload-emits-void-field](../archive/result-nil-ok-payload-emits-void-field.md)~~ | -- | **RESOLVED 2026-09-16** (archived). Fixed by giving the payload the int64 slot the erased twin already reads, NOT by the filed "treat it as a zero-field constructor" -- that would change ctor arity and so every call site and match arm. Four sites, not the three filed: the fourth is the SR2a/SR2b binder override, which put `void` back after normalisation. Original row: `(Result nil E)` and `(Option nil)` type-check and then emit `struct { void _0; } Ok;`, `ctor_Result_Ok__nil__int(void _0)`, and `void _un_N = (void)__scrut->as.Ok._0;` -- three `cc` errors naming only generated identifiers. `nil` is the right type for "worked, carries nothing", which is the return of every setter and connector in a C-wrapping spice; without it each one reaches for `(Result int int)` plus an "ok carries 0" convention (the `:int` stand-in CLAUDE.md forbids) or a per-spice `Ack` opaque, which is what nng ships. The nullary-constructor path already exists; the fix is routing `nil` fields into it at all three sites |

## Found executing playground-session-hygiene-plan (filed 2026-09-16)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[refine-call-sites-re-resolved-every-session-turn](../archive/refine-call-sites-re-resolved-every-session-turn.md)~~ | medium | **RESOLVED 2026-09-17** (archived) by fix direction 1: `turn_start_n_refine_call_sites` joins the PS4 watermarks and the pass starts there, so a turn resolves its own crossings and not every earlier turn's. The waste was not just the walk -- `refine_collect_obligation` does not deduplicate, so each re-resolution minted a fresh undischarged obligation that was discharged again, and the RT7 memo reuses only the VERDICT by design, so the diagnostics re-ran too (TUR-W0372 repeated per turn for a `#reads` crossing or under `--strict-refine`; an E0371 fails its own turn, which discards the session). 60 crossing turns collected 1830 obligations (= 60x61/2), now 60; a *trivial* turn at depth 440 cost 27 ms, now 0.12 ms. One correction to the filing: its repro (300 plain `defn` turns) collects **zero** obligations -- the pass early-`continue`s before the `rt_collect_*` work its profile named -- so an ordinary session never paid this; it needs a refinement crossing. Pinned by `tur_wasm_glue_session_unit`, which counts obligations rather than timing. The three sibling deferred passes (`wf_resolve_write_frames`, `rf_resolve_read_frames`, `wf_lint_image_globals`) have the same session-cumulative shape and two are quadratic per turn; unfixed, and unpaid unless a session uses `#writes` / `#reads` / image-cache. Original row: Under an `ElabSession`, `refine_resolve_call_sites` walks every refinement crossing the session has ever collected, on every turn -- the array is session state and nothing clears it. 49% of a 300-turn session replay's samples were under it |
| ~~[web-examples-js-is-unused-and-stale](../archive/web-examples-js-is-unused-and-stale.md)~~ | low | **RESOLVED 2026-09-19** (archived): `web/examples.js` deleted; the page's `EXAMPLES` in `main.js` is the one list. Original row: nothing imported `web/examples.js` and 7 of its 11 examples failed on their first run |

## Found fixing the session protocol check (filed 2026-09-18)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[pap-captured-arg-skips-session-role-protocol-check](../archive/pap-captured-arg-skips-session-role-protocol-check.md)~~ | medium | **RESOLVED 2026-09-26** (archived): the partial-application gate now admits `TY_SESSION`/`TY_ROLE` and is renamed `slot_needs_full_type_check`; pinned by `errors/session-protocol-mismatch-at-pap`, `errors/role-protocol-mismatch-at-pap` and the positive `session-pap-protocol-match` (a captured endpoint partway through a `Rec`). Original row: The saturated arg check now compares a session/role endpoint's protocol (`elab_call.c:6487`), but the PARTIAL-APPLICATION path (`elab_call.c:4862`) still gates strictness on a STRUCT/ADT-only `slot_is_nominal`, so a captured `TY_SESSION`/`TY_ROLE` arg is never compared to its slot -- **under-saturating a call bypasses the check a saturated call performs**. Two controls pin it to that path: the same mismatch is TUR-E0001 when saturated, and a matching protocol compiles when partially applied. Fix is the same two-place widening, plus renaming `slot_is_nominal` (a session protocol is structural, not nominal) |

## Found cleaning up the rationale guide's session example (filed 2026-09-17)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[session-type-eq-ignores-the-protocol](../archive/session-type-eq-ignores-the-protocol.md)~~ | -- | **RESOLVED 2026-09-18** (archived). The filed root cause was only HALF of it: `type_eq` did lack a `TY_SESSION` case (now `sess_proto_eq` in `types.c`), but adding that alone changed nothing -- the operative gate is the saturated positional arg check at `elab_call.c:6487`, which consulted `type_eq` only for `TY_STRUCT`/`TY_ADT` and matched everything else on `TypeKind` alone, and every endpoint has kind `TY_SESSION`. Both had to change. Equirecursion, not termination, was the difficulty: back-refs are sentinels so the graph is a finite DAG, but `Rec` is unfolded at use sites, so folded and unfolded spellings must still compare equal -- the risk was a false NEGATIVE rejecting working code. The fix caught two genuinely mis-wired fixtures (`session-rec`, `session-echo-rpc` had their endpoints swapped and compiled anyway). Pinned by `errors/session-protocol-mismatch-at-call` and `errors/session-endpoints-swapped`. Original row: `type_eq` fell through to `return 1` so any `Session[P]` compared equal to any `Session[Q]`, and a protocol mismatch across a call boundary was accepted with no diagnostic |

## Found investigating two PWA reports from a phone (filed 2026-09-17)

| Report | Severity | One line |
|---|---|---|
| ~~[pwa-overlays-ignore-ios-safe-area](../archive/pwa-overlays-ignore-ios-safe-area.md)~~ | -- | **RESOLVED 2026-09-18** (archived), confirmed in the installed app on the reporter's phone. `position: fixed` resolves against the viewport, so every overlay escaped the safe-area padding `#app` reserved for the black-translucent status bar: the docs pane's 44px topbar sat inside a 59px inset with **every** control that leaves the pane in it -- close, Contents, search -- so a reader who opened a doc had no way back. The companion "black area at bottom" is the same inset at the other end. **It took two commits**, and the first one never ran: dfe9edacb gated the safe-area tokens on `standalone, fullscreen` but the pinned shell consuming them on `standalone` alone, and iOS reports `fullscreen` for a black-translucent home-screen app -- so on the only platform with a notch, the shell block never matched, `#app` kept `100dvh`, and it landed short by top+bottom inset (59 + 34 = 93 CSS px). The band matches that SUM, not either inset alone, which is also why 37d44e55d's earlier "drop the bottom inset" reading (34px against a 59px band) did not hold. ce5a6ad3a collapsed both gates to one `html.pwa` class set before first paint. It shipped green twice because the test read rule TEXT for `display-mode: standalone` -- which a dead rule satisfies -- rather than measuring the rendered shell, as it now does |

| Report | Severity | One line |
|---|---|---|
| ~~[pwa-installed-build-cannot-be-updated](../archive/pwa-installed-build-cannot-be-updated.md)~~ | -- | **RESOLVED 2026-09-18** (archived), in two halves. The recovery lever for clients already stuck shipped in [#903](https://github.com/turmeric-lang/turmeric/pull/903): `sw-kill.js` had never been run, its `activate` did async work with no `event.waitUntil()` (the browser could kill it before one cache was deleted), and at `/sw.js` it would have put a PWA in an infinite reload loop -- now tested against a real worker and real caches, shipped by `TUR_SW_KILL=1`, runbook in guides. The update lifecycle itself followed in [#904](https://github.com/turmeric-lang/turmeric/pull/904): `reg.update()` on every foreground (`visibilitychange` + persisted `pageshow`, since a resumed standalone app fires no navigation and `register()` on `load` therefore never ran on iOS), a guarded `controllerchange` re-navigation, Force update and `applySwUpdate` navigating to a cache-busting URL instead of `location.reload()` (WebKit answers a reload from its own HTTP/page cache, which neither the unregister nor the Cache Storage wipe touches -- the whole of why it was a placebo on iOS), and a build stamp read from the live worker's cache key rather than a compiled-in constant. `pwa-update.spec.js` is red against the pre-change files and green after; its first run had passed 2 of 4 for the wrong reason, service workers being off on loopback without `?sw=1` |

## Found executing the sweet-exp fn/handle paren pass (filed 2026-09-17)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[sweet-dollar-inside-brackets-is-a-silent-symbol](../archive/sweet-dollar-inside-brackets-is-a-silent-symbol.md)~~ | -- | **RESOLVED 2026-09-17** (archived): fixed by the report's preferred direction as **TUR-E0332**, but emitted from the READER rather than the preprocessor's `$` branch -- every other decline path rewrites to something (`$` at EOL or before a comment wraps an empty rest, `$x` interns as `$x`), so a bare `$` from a `READER_SWEET` file means exactly one thing, which makes it a two-line guard in `read_symbol_or_minus` with the caret mapped back through the xform map for free. The `bd == 0` guard was never the bug and stays. Pinned by `tests/fixtures/errors/sweet-dollar-inside-brackets`; plain `.tur` files keep a legal `$` identifier. The guide audit it carried was split out and then executed (9 `$` conversions in the three tutorials, the rest deliberately skipped) -- `docs/archive/history/sweet-dollar-guide-audit-plan.md`. Original row: A `$` rest-of-line marker inside `(...)`, `[...]` or `{...}` was neither rewritten nor diagnosed -- it survived into the AST as a bare `$` symbol, changing the form's shape with no error |

## Found archiving the signal / ws-server spice reports (filed 2026-09-17)

Both reports archived that day were already fixed in
[turmeric-spices#74](https://github.com/turmeric-lang/turmeric-spices/pull/74)
(merged 2026-09-16) and were sitting unarchived -- the failure mode
`workarounds-to-remove` itself existed to prevent, on its own last row. One new
finding came out of verifying the first one's stated payoff.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[interpret-takes-no-include-path-or-spice-discovery](../archive/interpret-takes-no-include-path-or-spice-discovery.md)~~ | low | **RESOLVED 2026-09-26** (archived): `tur interpret` / `tur debug` parse `-I` before the file, `tur eval --file` and `tur run --engine=interp` thread it, `TuriEnv` carries the include list to both `elaborate_program_session` calls, and `cmd_eval_h` runs the same spice walk-up `tur check` does (honouring `--no-auto-spice`). Pinned by `interpret-spice-imports`. Original row: `tur --interpret` is reachable only for **single-file** programs: the arm at `src/main.c:12123` does no flag parsing at all (`argv[2]` is the path, `argv+3` is `*args*`), so `-I src` is taken as the *filename* and surfaces as `load: cannot open '-I'`; and no spice walk-up runs, so `tur --interpret tests/signal/test_compose.tur` from a directory with a `build.tur` fails `module 'signal/core' not found` where `tur run` and `tur check` on the same file both pass. Nothing is missing in the elaborator -- `elab_load_module` already prefers `-I` dirs (`elab_module.c:337`); the two `elaborate_program` calls in `src/turi/eval.c` (13474, 13809) pass `include_dirs=NULL` hard-coded and there is no channel to them. `src/main.c:4797` asserts the opposite in a comment ("the tree-walker discovers the enclosing spice itself") and that comment is the recorded reason `-I` is dropped on the `tur run --engine=interp` path, which fails identically. Invisible to `tests/run-turi.sh`, which drives single-file fixtures. Found because it is what blocks the interpreter coverage `signal-compose-hand-rolled-vec-readers` was filed to buy |

## Environmental hazards (filed 2026-09-18)

Not defects in `tur`, but they present as total `tur` failures, so they are
indexed here to be findable from the symptom. Filed on request after a sweep
found them documented in prose with no report backing them -- so there was
nowhere to record whether they are still live.

| Report | Severity | One line |
| --- | --- | --- |
| [macos-asan-runtime-deadlocks-at-startup](macos-asan-runtime-deadlocks-at-startup.md) | medium when live | On a mismatched toolchain/OS pairing the Debug build spins forever in `InitializeShadowMemory` before `main()`, so every invocation hangs, `tur --version` included. Reproduces from a bare `int main(void){}` compiled `-fsanitize=address`. **Latent as of filing**: does not reproduce on macOS 27.0 / Apple clang 21.0.0, where `build/tur` is genuinely ASan-linked and runs, and both installed runtimes (Apple 21, brew 22.1.4) pass the bare repro. **Mechanism corrected on investigation** -- the macOS runtime is a *dylib loaded from an absolute rpath at startup*, not code linked into the binary, so (a) updating the CLT fixes binaries already built and no rebuild is needed, (b) a major CLT bump makes the pinned `clang/21` path vanish and old binaries fail to LOAD rather than hang, and (c) a foreign runtime is rejected by a guard symbol (`___asan_version_mismatch_check_apple_clang_2100`) rather than silently used, so the deadlocking pairing must be a runtime the binary accepts. Also notes that the recommended Homebrew-LLVM remedy is the same route CLAUDE.md:245 warns fails every fixture on `___asan_version_mismatch_check_v8`. Provenance is one commit message (#660); `InitializeShadowMemory` appears in no CI log or prior report. Deliberately not auto-disabled -- #660 keyed an auto-off on Darwin >= 27 then reverted it, and this host being a working Darwin 27 shows Darwin version was the wrong variable (compare CLT version to OS version instead). Backstopped by the `perl -e 'alarm 10'` smoke check at `ci.yml:167`. **Re-verified still latent 2026-09-28** on the same pairing, and CLAUDE.md's copy of it synced to the corrected mechanism the same day: it had still described the runtime as linked in, listed no CLT-update remedy, gave no way to tell whether the hazard is live before rebuilding, and did not connect its Homebrew-LLVM advice to the fixture-link trap in the very next section |

## Found executing the stdlib int-stand-in audit (filed 2026-09-18)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[run-sh-stamp-cache-ignores-the-stdlib](../archive/run-sh-stamp-cache-ignores-the-stdlib.md)~~ | medium | **RESOLVED 2026-09-25** (archived): `stamp_key` carries a once-per-run hash of every file under `stdlib/`. Original row: `stamp_key` is `hash(input) + hash(expected.c) + mtime(tur)` -- the **stdlib is not in it**, and stdlib/*.tur is data the compiler reads rather than something linked into the binary. So a stdlib-only edit invalidates no stamp, every fixture PASS-skips from cache, and the summary reports `3047 passed, 0 failed` for a run that recompiled nothing. Observed on PR #909: local green, CI red with 148 codegen mismatches from one `json/bool [v : int]` -> `[v : bool]` change. Only springs on stdlib-ONLY edits -- a compiler rebuild bumps `TUR_MTIME` and invalidates everything -- i.e. exactly the shape of a stdlib audit or signature pass. Workaround `TUR_FORCE=1`; fix is to fold a one-shot hash of `stdlib/*.tur` into the key |

## Found executing the stdlib int-stand-in audit, S1/S2 (filed 2026-09-18)

Both are products of that work rather than part of it. Neither is a regression
from it -- the second reproduces on `vec-push!`, parametric since long before.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[plain-fn-typed-params-are-kind-matched](../archive/plain-fn-typed-params-are-kind-matched.md)~~ | medium | **RESOLVED 2026-09-25** (archived): the structural check now also runs for a plain fn param routed onto the typed poly carrier (recorded `TY_PTR_VOID`), so arity and float-vs-word mismatches are TUR-E0001 as for `^fat`; a same-carrier `(fn [cstr] cstr)` stays accepted on both. Original row: A parameter declared `(fn [int] int)` accepts `(fn [a : cstr] : cstr a)` with exit 0, arity mismatches included: call arguments compare `TY_FN` to `TY_FN` by KIND. S1's `fn_type_structurally_compatible` closed this for `^fat` parameters only, because it is reached through the LT2 block, which reads the expected type from `arg_full_types` -- and `types.h:681` says that array is "NULL for monomorphic args". A `^fat` param records one; a plain `(fn [int] int)` param does not, so there is nothing to compare and the check returns early. `fn_type_subtype`'s own comment claims "arity mismatch caught elsewhere"; it is caught nowhere. The predicate needs no change (it already compares carrier class and skips tyvar/unknown/any, which took S1 from 59 regressions to 0) -- the work is populating `arg_full_types` for the monomorphic case, which is load-bearing for the rank-2 paths |
| ~~[parametric-stdlib-diagnostics-print-tyvar-internals](../archive/parametric-stdlib-diagnostics-print-tyvar-internals.md)~~ | low | **RESOLVED 2026-09-26** (archived): the mismatch diagnostic substitutes the call's bindings into a type-variable parameter (`expected int, got float`), and `type_name_buf` prints applications in source spelling (`(Chan A)`, `(Map K V)`). Pinned by `errors/vec-push-bound-payload-mismatch`. Original row: A payload mismatch on a parametric container reads `expected tyvar, got float` and a handle mismatch reads `expected (type-app Chan tyvar 'A')` -- neither is a spelling a user can write, and the first lands on the most likely first mistake with the new API (two payload types in one container, where `A` is already bound and the message should say `expected int`). Reproduces on `vec-push!` today, so NOT an S2 regression; S2 widened its reach to chan/ref/atomic. `(type-app F X)` is `type_name_buf`'s general fallback (`types.c:3295`), whose sibling branch already prints the partial-application form as `(F _ X)`. Fixing the printer touches every snapshot that spells `type-app`, so it wants its own change with the regen in it |

## Found investigating the remaining S2 sites (filed 2026-09-19)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[generic-closure-capture-of-float-truncates](../archive/generic-closure-capture-of-float-truncates.md)~~ | high | **RESOLVED 2026-09-25** (archived): an unannotated lambda keeps its tyvar result's name, the per-spec clone covers by-value aggregates, and a spec's fill of a shared int64 slot bridges the bits; pinned by `tests/fixtures/generic-closure-capture-register-class`. The callback shape is filed as `generic-closure-float-passed-to-fn-typed-callback`. Original row: `((capture 7.25))` returns **7**, silently, exit 0, where `(defn capture [A] [v : A] : (fn [] A) (fn [] v))`. The closure-env struct is SHARED between the carrier base and every monomorphized spec and its payload field is `int64_t`, so the float spec's `__t287->v = v` assigns a `double` into it -- a numeric conversion, not a bit-reinterpret. Boundary measured: a generic pass-through (`ident`) is correct, `vec-push!`/`vec-get` are correct, only a **capture into a closure** breaks. The compiler already emits the right union idiom one line away (`bt_hycell_hynew(((union { double s; int64_t d; }){.s = 0.0}).d)`) and the CPS backend has it factored as `slot_store`/`slot_load`; the direct backend's env fill uses neither. **Second, independent defect at the same site**: the spec also drops the `TUR_REGION_NOTE_WORDS` the base emits, for every spec and not just float ones -- a missed hook on the set CLAUDE.md names explicitly. Blocks S2's `dfs-set`: parameterising it turns a loud `TUR-E0001` into `3.45846e-323` |
| ~~[generic-closure-float-passed-to-fn-typed-callback](../archive/generic-closure-float-passed-to-fn-typed-callback.md)~~ | medium-high | **RESOLVED 2026-09-26** (archived): a dispatch through a `(fn [..] R)` binding with a concrete scalar `R` counts as typed, and the per-spec inner-closure clone fires when a parameter's fn type takes or returns a float-bound tyvar, so the clone dispatches `k` with `double` in xmm0. Pinned by `generic-closure-float-fn-callback`. Original row: Filed 2026-09-25, pre-existing. `(fn [k : (fn [A] bool)] (k v))` at A = float hands `k` garbage (`4.7e-310`): the shared thunk dispatches `k` through the int64 carrier ABI into the caller's typed `double` fatshim. The per-spec clone that fixed the returned-value shape is vetoed by `closure_return_dispatches_untyped`. `--interpret` is right |

## Found sweeping the region store hooks (filed 2026-09-19)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[stdlib-region-store-hooks-unswept](../archive/stdlib-region-store-hooks-unswept.md)~~ | medium | **RESOLVED 2026-09-26** (archived) at the coercion, as recommended: a typed node passed implicitly into an inline-C `:int` parameter is wrapped in the erasing `(:: x :int)`. Two holes under the premise, also fixed: the direct-call emitter stripped an argument's ascriptions (so an explicit `(f (:: node :int))` was never noted), and the erasure predicate excluded `ptr<void>` / `any` targets. Zero snapshot changes. Pinned by `region-escape-via-erased-argument`, which asserts `TUR_REGION_STATS`. Original row: **29 stdlib stores of an erased caller word carry no region note, verified in EMITTED C** (a source grep cannot see the notes the emitter adds itself). Includes `httpd-new-pool` / `router-add`, whose handlers S1 gave real function types -- **typing the parameter produced no automatic note**. The study narrows it twice. First, an erasing ascription is ITSELF a hooked site, so a value reaching a store through `(:: x :int)` is already noted and the store hook is redundant on that path -- measured: removing `bt-set!`'s note leaves `region-escape-via-store` at `rewinds=1 retires=11`, i.e. **that fixture's case 6 does not test its own hook**, while removing `vec-push!`'s moves it to 2/10. Second, the gap the hook really covers is the IMPLICIT erasure (a typed node passed straight into an `:int` parameter, accepted by elab_call.c's `TY_ADT -> TY_INT` hatch with a plain cast) -- demonstrated on promise-fulfill: `rewinds=0` hooked, `rewinds=1` unhooked. So the fix is that ONE coercion site, not 29 manual notes. And `TUR_REGION_STATS=1` is the only instrument that sees any of it: stdout assertions pass with the hook removed |

## Found executing the single-body-control-forms report (filed 2026-09-21)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[stdlib-capability-vtables-uncompilable](../archive/stdlib-capability-vtables-uncompilable.md)~~ | medium | **RESOLVED 2026-09-26** (archived): structs hoisted to a file-scope c-block, `*-type` defns deleted, typed handles (`FileSystem`/`Logger`/`Random` opaques, the `time` module's `Time`) and `c-fn` callbacks, `io.tur` takes `FileSystem` from this module, `with-capability` takes a binding vector. Pinned by `capability-module-roundtrip`. Original row: `(load "stdlib/capability.tur")` + a bare `main` is **36 C errors and a failed `cc`** -- the module cannot be compiled at all. Primary defect is **compiler-independent**: all four capability structs (`FileSystem`/`Logger`/`Random`/`Time`) are defined inside their own `*-type` defn body, so the tag has C block scope (C11 6.2.1) and every other inline-C body's `typedef struct X X;` declares a fresh **incomplete** tag -- 28 of the 36 errors, and it would fail on Linux gcc too, so "AppleClang is stricter" is the wrong read. The other 8 are strictness-gated (`-Wreturn-mismatch` on eight `void` defns that `return` a value; `-Wint-conversion` on four `*-free` bodies handing an `int64_t`-carried handle to `free()`). Same class as [log-capability-vtable-uncompilable](../archive/history/log-capability-vtable-uncompilable.md), whose fix hoisted `log.tur` and `stdlib/test/capability.tur` into file-scope c-blocks and **left this sibling behind**. Latent because nothing in-tree loads it: `tests/fixtures/capability-stdlib-roundtrip` loads `stdlib/test/capability.tur`, a different file. Drive-by: `with-capability`'s docstring example uses a binding vector its `[binding cap_expr & body]` signature does not accept |

## Found executing proper-tail-calls T6 (filed 2026-09-23)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[saffron-catch-unwind-around-dyn-call-fn-crashes](../archive/saffron-catch-unwind-around-dyn-call-fn-crashes.md)~~ | high | **RESOLVED 2026-09-26** (archived): the interpreter was never affected (`tur run --interpret` compiles). The CPS direct entry wrapper read a boxed return through the NULL `__r` a panicking body leaves; it now reads `__r ? load : (T){0}`. The side note (a panic-only Saffron body emitted `void` against callers typed `any`) is fixed too. Pinned by `saffron-catch-unwind-cps-panic`. Original row: Pre-existing on `main`. A Saffron function that makes a dynamic call and panics, called under `catch-unwind` with an `any` thunk, SIGSEGVs on the compiled path AND under `--interpret` -- even when the dynamic call never runs. Removing the dynamic call from the body, or not passing a function value, makes both engines print the caught panic |
| ~~[cps-capturing-closure-env-leaks-through-dyn-call](../archive/cps-capturing-closure-env-leaks-through-dyn-call.md)~~ | low | **RESOLVED 2026-09-28** (archived): two ways the same day. The collector a compiled Saffron program now allocates from reclaims it (1.4 MB heap over 3M closures), and statically: `any` parameters join `nonretain_param_mask` (a dynamic call of a parameter is an invocation, not an escape), a capturing lambda widened to `any` at such a slot is hoisted into a `__borrowc` let, and a CPS caller registers its env for the DK entry-boundary reap -- in the main body, and only when `__kont` is not the trampoline root, since a T6 bounce would run the lambda after the reap. Pinned by `saffron-lambda-arg-env-freed`; `tailcall-dyn-leak` passes a capturing lambda again. Original row: Pre-existing on `main`. A capturing lambda built in a CPS-lowered function and passed to a dynamic call leaks its 32-byte env per call under LeakSanitizer; the CPS backend's env reap covers only leaf-admitted non-escaping closures |

## Found landing r7rs-lang-plan R2 (filed 2026-09-23)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-compiled-dynamic-shapes](../archive/r7rs-compiled-dynamic-shapes.md)~~ | medium | **RESOLVED 2026-09-24 (r7rs-lang-plan R6), archived.** The letrec placeholder in a dynamic file is `any` and the lambda init is pinned to match; every Scheme lambda returns `any`; a fn type's box id spells its rest slot, the emitter registers each boxed variadic with its fixed count and the dynamic call (and the T6 trampoline) pack the surplus arguments into the `(Cons any)` chain, the interpreter packing at its dynamic call likewise; the H8 adaptor declines a variadic. Section 3 was misread -- it is `list-length`'s untyped walker on a `(Cons any)` chain, now [list-length-on-cons-any-segfaults](../archive/list-length-on-cons-any-segfaults.md). Pinned by `tests/fixtures/saffron-letrec-any-closure`, `saffron-variadic-dynamic-call`, `r7rs-control`, and the four r7rs fixtures that lost `requires.interp-only`. Original row: Three dynamic-closure shapes the COMPILED back end refuses and `--interpret` answers correctly, none Scheme-specific: a letrec-bound closure over `any` that calls itself (cc: "aggregate value used where an integer was expected" -- the shape every Scheme named `let`/`do`/`letrec` lowers to), a dynamic call the fat-closure apply helpers refuse (a `nil`-returning callee, anything past four arguments, and -- on BOTH back ends, found at R5 -- a variadic callee through `apply`, since the outbound `any` adaptor is synthesized with the fixed parameter count), and `type-of` on an `any` rest parameter passed through an identity (SIGSEGV). The plan stages R7RS interpreter-first and lifts the compiled path in R6; until then `tests/fixtures/r7rs-core-forms-interp` and `r7rs-named-let-sum` carry `requires.interp-only` |
| ~~[list-length-on-cons-any-segfaults](../archive/list-length-on-cons-any-segfaults.md)~~ | low-medium | **RESOLVED 2026-09-26** (archived): worse than filed -- every Saffron list is a `(Cons any)`, so plain `(length xs)` and `(list-length (list 1 2 3))` segfaulted and `list-head` answered the tag. A carrier helper call (`list-length`/`length`/`list-head`/`car`/`list-tail`/`cdr`) on a `(Cons A)` whose head is wider than a word (`any`, union, inline aggregate) is now elaborated as its typed twin (`tlength`/`thead`/`ttail`); `tlength` is tail-recursive. Pinned by `list-helpers-wide-head-element` and `list-length-byvalue-aggregate-element`. Original row: Filed 2026-09-24 out of the row above. `stdlib/list.tur`'s `list-length` is inline C over the untyped `{ int64_t head; int64_t tail; }` cell, and a `(Cons any)` monomorph's cell has a 16-byte tagged head, so the walker reads the tail out of the head and dereferences it: `(list-length (:: xs (Cons any)))` on a `& xs : any` rest is a compiled SIGSEGV where `--interpret` answers. A Turmeric-level walk over the same chain is right on both back ends; the R7RS prelude uses its own |
| ~~[toplevel-def-initializers-run-before-toplevel-expressions](../archive/toplevel-def-initializers-run-before-toplevel-expressions.md)~~ | medium | **RESOLVED 2026-09-25** (archived): every top-level form runs in source order compiled, as under `--interpret`. With no user `main` a `def` initializer is a statement of the synthesized `int main()` at its position (and the synthesized-main fold steps aside for a `def` after a statement whose initializer is a call); a `#lang r7rs` program with imports assigns a define after the first expression in its body (`def ^mut` + `set!`). Fixtures `toplevel-def-init-order`, `r7rs-toplevel-order` |
| ~~[r7rs-procedure-body-forward-reference](../archive/r7rs-procedure-body-forward-reference.md)~~ | medium | **RESOLVED 2026-09-25** (archived): the lowering marks a forward-referenced variable define `^mut : any` and the elaborator's Pass 1 pre-declares every top-level `(def ^mut name : any ...)` ahead of the bodies, `elab_def` filling the binding in; both back ends, programs and library bodies; `tests/fixtures/r7rs-forward-reference`. Original row: filed 2026-09-25. `(define (f) y)` before `(define y 1)` is "unbound symbol 'y'" on both back ends; R7RS allows it. A `defn` is pre-declared, a plain variable is not; fix is to pre-declare every top-level defined variable of a Scheme program as an `any` global |
| ~~[dynamic-returned-closure-env-is-never-freed](../archive/dynamic-returned-closure-env-is-never-freed.md)~~ | low-medium | **RESOLVED 2026-09-28** (archived): two ways the same day. The collector reclaims it in both dialects it names (r7rs since 09-25, Saffron since 09-28), and statically: a producer whose tail widens a capturing lambda is `returns_fresh_any_closure`, and its binding's scope drop releases the env through the drop-glue header (`__tur_any_closure_drop`, skipped once `tur_dk_pinned` says a re-entrant continuation exists). The escape walk gained `EX_DYN_CALL`, `EX_DYN_OP` and `EX_UNION_INJECT` arms, and `(set! b ...)` is an escape. A self-applied closure is admitted through a per-parameter mask, a CPS body reaps at the entry boundary, and a tail-position `let`'s value return now fires its `any` drops. Pinned by `saffron-returned-closure-env-freed` and `saffron-returned-closure-set-not-dropped`; `tailcall-dyn-leak` lost its `known-leak`. Original row: Filed 2026-09-24 (r7rs-lang-plan R9). In a dynamic file a capturing closure returned as `any` is never freed (its `let` gets no scope-end drop); the typed twin is freed |
| ~~[compiled-closure-copies-a-captured-mut](../archive/compiled-closure-copies-a-captured-mut.md)~~ | medium | **RESOLVED 2026-09-26** (archived): shared capture in every dialect. A `^mut` a lambda in the rest of its `let` mentions moves into a shared `:heap` cell (`TurMutCell`); its name becomes an alias read as `(.v <cell>)` and written as the field, so every closure env holds the one cell (`elab_let_mut_to_cell`, elab_forms.c). Scalars, cstr, sym and `any` only; macro-introduced lambdas unseen. Also fixed: a write-only closure (`(fn [] (set! n 5))`) was a cc error, and a nil-bodied Saffron lambda panicked through a dynamic call. The cell is never freed: [mut-cell-is-never-freed](../archive/mut-cell-is-never-freed.md) (since resolved). Pinned by `closure-captured-mut-shared` and `saffron-closure-captured-mut-shared`. Original row: Filed 2026-09-24 (r7rs-lang-plan R10, found by chibi's suite). A compiled closure copies a captured `^mut` word at creation; the interpreter shares the frame, so a `set!` after capture, or from another closure, is seen by `--interpret` and not by `tur run` -- two answers, every dialect. `#lang r7rs` is immune since R10 (its lowering boxes an assigned-and-captured variable, `ac_walk`); typed Turmeric and Saffron are not |
| ~~[untyped-forward-callee-result-retagged-as-pointer](../archive/untyped-forward-callee-result-retagged-as-pointer.md)~~ | low-medium | **RESOLVED 2026-09-26** (archived): the module path's forward declarations give an unannotated dynamic-file return `any`, as the entry path's do. Broader than filed: any Scheme or Saffron library whose procedure called one defined further down failed at `cc` when imported, from either kind of importer; pinned by `run-r7rs-import.sh` and `run-saffron-import.sh`. Original row: Filed 2026-09-24 (r7rs-lang-plan T1). A call to an untyped defn defined LATER in the file re-tags the callee's `any` result as a pointer (`TUR_TAG(3, ...)` on a `tur_tagged_t`), a C compile error, when a Turmeric module is the entry and imports a Scheme library; a Scheme entry compiles the same prelude. Worked around by declaring `: any` on the prelude procedures; the forward-declaration return type needs to agree with the emitted one |
| ~~[r7rs-repl-toplevel-expression-value-not-widened](../archive/r7rs-repl-toplevel-expression-value-not-widened.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): the lowering widens a prompt's top-level expression through an `any` identity; pinned by `tests/fixtures/r7rs-repl-echo-widened`. Original row: Filed 2026-09-24 (r7rs-lang-plan T4). `tur repl --lang r7rs` echoes `(let ((v (vector 1 2))) v)` as a number: a top-level expression's value is its elaborated type's representation (a bare `Vec` here), never widened to `any`, so the prelude's `write` prints the pointer. Programs are unaffected; T4's `eval` sidesteps it by routing every evaluated expression through an `any` identity |
| ~~[r7rs-internal-define-forward-set](../archive/r7rs-internal-define-forward-set.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): a forward-referenced internal value define is hoisted as a mutable cell around the body (letrec*); pinned by `tests/fixtures/r7rs-internal-define-forward-set`. Original row: Filed 2026-09-24 (r7rs-lang-plan T5). A body's internal defines are bound in order, not as `letrec*`, so `(define (a) (set! b 1)) (define b 0)` is "set!: 'b' is not bound" on both back ends; a reference to a later definition is fine, a `set!` is not |
| ~~[r7rs-toplevel-define-named-like-a-turmeric-form](../archive/r7rs-toplevel-define-named-like-a-turmeric-form.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): a defined or `only`/`rename`-imported global named like a Turmeric form is renamed through the clash table wherever it occurs, libraries and importers in step; pinned by `tests/fixtures/r7rs-toplevel-form-names` and `run-r7rs-import.sh`. Original row: Filed 2026-09-24 (r7rs-lang-plan T5). A top-level `(define gen ...)` is elaborated as Turmeric's `gen` form at `(gen)`; R10 renamed local binders named like Turmeric forms, not global definition names |
| ~~[o0-build-cannot-link-contract-handler](../archive/o0-build-cannot-link-contract-handler.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): `runtime/contract_handler.c` joined `TURT_RUNTIME_SOURCES`. Original row: Filed 2026-09-24 (r7rs-lang-plan T5). At `-O0` every program fails to link: `tur_set_contract_handler`/`tur_get_contract_handler` live in `runtime/contract_handler.c`, which is in `tur_core` but not in `libturt_runtime.a`; `-O2` drops the unused static callers, so the default never sees it |
| ~~[r7rs-toplevel-reentry-reruns-forms](../archive/r7rs-toplevel-reentry-reruns-forms.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): each top-level statement runs under its own prompt (`r7rs-toplevel__`), so a continuation is the rest of the FORM; re-entering it from a later form finishes the form and continues after the invoking one, as chibi and Racket do. Both back ends; fixture `r7rs-toplevel-reentry` |
| [r7rs-callcc-memory-never-freed](r7rs-callcc-memory-never-freed.md) | medium (interpreter only since 2026-09-25) | **2026-10-05:** the interpreter's pin now also switches off frame reclamation, so one stored continuation takes a list loop from 47 MB to 214 MB; direction: a per-frame capture epoch. **Narrowed 2026-09-28** to a continuation that may outlive its `call/cc`, under `--interpret`: an escape-only `call/cc` (every `k` heads a call; lambdas only as args of `for-each`/`map`/..., named lets whose name only heads calls) is lowered to the one-shot escape on both back ends (`callcc_escape_only`, fixture `r7rs-callcc-escape-only`); the compiled side's per-call growth was the DK reap list, which nested CPS entries now drop under the collector (`__dk_reap_drop_to`; a million guards and escapes fit in 256 MiB, 397 MB before, `run-r7rs-gc.sh` `reclaim-escapes`); the interpreter's pin measured at nothing; the interpreter's images, still never freed (no collector to say a continuation is dead), are kept as deltas against a per-range keyframe and abandoned drive stacks are freed before a re-entry jump (a 16,000-step generator: 1131 MB to 284 MB, 69 to 16 KB a step; fixture `r7rs-callcc-reentry-many`). Original: Filed 2026-09-24 (r7rs-lang-plan T5 behavior change). **Compiled half resolved 2026-09-25**: the r7rs-gc collector graduated and reclaims the images (docs/archive/r7rs-gc-plan.md); the interpreter is unchanged and is what this row is about now. Every `call/cc` mallocs a copy of the C stack that is never freed, and the first one pins DK frames (compiled) and the driver's temporaries (interpreter) for the rest of the run: 100,000 escapes grow a compiled program to 366 MB; 10,000 take the Debug interpreter to 1.3 GB. The `--enable=r7rs-gc` experiment reclaims images compiled (10 MB); the interpreter is unchanged |
| ~~[dk-reap-list-shared-across-threads](../archive/dk-reap-list-shared-across-threads.md)~~ | medium | **RESOLVED 2026-09-28** (archived): wider than filed -- the trampoline's landing `g_dk_driver`, resume chain/value and meta-stack were process globals too, so any compiled program running effects on two threads crashed (`longjmp causes uninitialized stack frame`, 15/15 runs; a Scheme `call/cc` loop on five threads segfaulted in `__dk_reap_push`, 20/20). All eleven DK globals are `TUR_THREAD_LOCAL` (host accessors in `tur_tls.c` under the JIT), a `FiberBlock` carries its own reap list and entry depth across switches, and the `tur_gc_threaded` gate is gone. Under `tur jit` the dynamic tail-call trampoline's four variables were shared too (c2mir has no thread-locals); host slots now. Fixtures `threads-effects-tail-resume`, `r7rs-threads-cps-entries` |
| ~~[r7rs-dynamic-environment-shared-across-threads](../archive/r7rs-dynamic-environment-shared-across-threads.md)~~ | medium-high | **RESOLVED 2026-09-29** (archived): the handler, wind and parameter-binding stacks and a re-entry's delivered value live in one runtime thread-local, `tur_r7rs_dyn` (emit_dk_runtime.c; `TUR_TLS_FRESH` under clang, a host slot under `tur jit`, a collector root), which `tur_fiber_block_resume` swaps per fiber; `parameterize` binds in it instead of writing the cell. Not `^thread-local`: that declines the prelude split and is the thread's, not the fiber's. An inline-C body may now declare `: any`. Fixtures `r7rs-threads-dynamic-env`, `r7rs-threads-fiber-dynamic-env` (5 of 5 wrong before, all right after). Original row: Filed 2026-09-28. The prelude's `r7rs-handlers__`, `r7rs-winders__` and `r7rs-cont-delivered__` are process globals, and `parameterize` writes the parameter's shared cell: a `guard`/`raise` loop on five threads fails every run (another thread's handler, its escape invoked here, or none at all). Fix: `^thread-local` for the three, a per-thread binding for parameters |

## The documented R7RS differences, as reports (filed 2026-09-25)

r7rs-lang-plan 9.3 and the guide's "Where it differs" list the ways
`#lang r7rs` knowingly departs from R7RS-small that no chibi test reaches.
Each is now a report with a repro measured on both back ends, so it can be
picked up, or archived, like any other finding. Two of the guide's bullets
live elsewhere in this index: compiled top-level order was
`toplevel-def-initializers-run-before-toplevel-expressions` above (resolved
2026-09-25), the loop-through-a-procedure-variable bullet was
`cps-self-tail-call-relies-on-sibling-call` (resolved 2026-09-28), and its
successor, the tail call after a `guard`, is
`mutual-tail-call-through-guard-grows-the-stack`.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-apply-more-than-four-arguments](../archive/r7rs-apply-more-than-four-arguments.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): the fat-shim family, the `any` widen, the dynamic call's slots and the trampoline all carry eight (`TUR_FAT_SHIM_MAX_ARITY`), and so does the prelude's `apply`; pinned by `tests/fixtures/r7rs-apply-many-args`. Original row: `(apply f '(1 2 3 4 5))` and `call-with-values` past four values panic on both back ends (`r7rs-apply-list__` has arms for 0-4); a dynamic call `(g 1 2 3 4 5)` is a compile-time refusal on the compiled back end (`emit_dyn_call`, `TUR_APPLY4_T`) and works interpreted. A direct call is fine. Fix is the R6 rest-chain packing for the surplus, in a Scheme-gated path |
| ~~[r7rs-char-ready-always-true](../archive/r7rs-char-ready-always-true.md)~~ | low | **RESOLVED 2026-09-25** (archived): `r7rs-io-ready?__` answers from the buffer, eof, or a zero-timeout `poll()` on the descriptor (Windows still `#t`). Original row: `char-ready?` / `u8-ready?` resolve the port and answer `#t`, so a poll on the console or a pipe says ready and the read blocks. String and bytevector ports are genuinely always ready; a file port needs a zero-timeout `poll()` behind an inline-C helper |
| ~~[r7rs-import-except-refused](../archive/r7rs-import-except-refused.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): import sets nest in any order and `except` makes a name the program's own; pinned by `tests/fixtures/r7rs-import-sets` and `run-r7rs-import.sh`. Original row: `(except <lib> name...)` and any nested import set are refused because the lowering maps one modifier onto Turmeric's `:refer`/`:as`. The Scheme side knows every export list, so the set can be folded (except removes, only keeps, prefix/rename re-spell) into one `:refer` before the Turmeric import is written |
| ~~[r7rs-include-refused](../archive/r7rs-include-refused.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): the lowering reads the file with the Scheme reader, relative to the includer, and splices it at top level, in expression position and as a library declaration; pinned by `tests/fixtures/r7rs-include`. Original row: `include` / `include-ci` at top level and `(include ...)` in a `define-library` are refused: the lowering cannot read a second file under the Scheme reader without its own `#lang` line. Fix is a read-as-Scheme entry point that splices the forms and registers the path for diagnostics |
| ~~[r7rs-command-line-first-element-is-tur](../archive/r7rs-command-line-first-element-is-tur.md)~~ | low | **RESOLVED 2026-09-25** (archived): a pre-declared `*argv0*` global (`:cstr`) set by every emitted `main` and by the interpreter; `command-line` conses it. Pinned by `tests/fixtures/argv0-global`. Original row: `(command-line)` conses the literal `"tur"` onto `*args*`; the emitted `main` and the interpreter both drop `argv[0]`, so nothing records it. Fix is a runtime `argv[0]` global set by both, exposed to the stdlib without reading `g_tur_args` raw |
| ~~[r7rs-unicode-case-mapping-gaps](../archive/r7rs-unicode-case-mapping-gaps.md)~~ | low | **RESOLVED 2026-09-25** (archived): the tables are generated from the UCD files, fetched from ICU at a pinned release tag (Unicode 16.0.0), with Final_Sigma, UCD simple mappings and the Alphabetic property; pinned by `tests/fixtures/r7rs-unicode-case`. Original row: Four gaps from deriving the `(scheme char)` tables through Python's `unicodedata`: no Final_Sigma in `string-downcase` ("ΟΔΥΣΣΕΥΣ" downcases to a non-final sigma), a simple case mapping taken from the full one so `(char-upcase #\x1F80)` is itself, `char-alphabetic?` without Other_Alphabetic (`#\x0345` is `#f`), and a Unicode version that follows the generating Python (14.0.0). Fix is generating from the UCD files |
| ~~[r7rs-map-for-each-at-most-four-sequences](../archive/r7rs-map-for-each-at-most-four-sequences.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): the walkers carry arms 5-8, so the cap is the shim arity (eight) on both back ends; five- and eight-sequence cases in `tests/fixtures/r7rs-base-library`, and the million-element `(map + a b)` still runs at `-O2`. Original row: filed 2026-09-25 (found revisiting the guide's "Where it differs"). `map` and `for-each` take at most four sequences on both back ends -- R7RS 6.10 has no limit -- and `vector-map`, `vector-for-each`, `string-map` and `string-for-each` share the walker and the cap. `r7rs-mapn-go__` spells `f`'s arguments out inline, one arm per count, deliberately: T8 found that calling through a helper nested a C frame per element and overflowed a million-element `(map + a b)` even at `-O2`. The cheap fix is arms 5-8, since the shim arity is eight now, not the four the stale comment names |
| ~~[r7rs-define-record-type-not-an-internal-definition](../archive/r7rs-define-record-type-not-an-internal-definition.md)~~ | low-medium | **RESOLVED 2026-09-26** (archived): a body's `define-record-type` is lifted to the top level under fresh names (`R7rsRec_<name>__N`, `<proc>__vN`) that the body's scope maps its names to; pinned by `tests/fixtures/r7rs-internal-record-type` and `run-r7rs-import.sh`. Original row: Filed 2026-09-25 (found revisiting the guide's "Where it differs"). `define-record-type` is accepted only at the top level or in a `define-library` body; R7RS 5.5 makes it a definition, so it may open any body. `lower_record_type` emits a `defstruct` plus top-level defns into the declaration stream, and expression position has nowhere to put them. Fix is lifting them to the top level under a per-occurrence gensym (the struct name is `R7rsRec_<record name>` today, so two bodies naming one record type would also collide) |
| [r7rs-library-file-shape-and-export-rename](r7rs-library-file-shape-and-export-rename.md) | low-medium | **Export rename resolved 2026-09-26**: a definition exported under `(rename internal public)` is spelled `public` in the module (clash table), or aliased (forwarding defn / `any`) when it is imported or exported twice; pinned by `run-r7rs-import.sh`. The "module not found" error for a Scheme import now says the library's name is its path. The two file-shape restrictions stay open, **held by decision 2026-09-26** (one library per file for now; r7rs-lang-plan Section 8, question 7). Original row: Filed 2026-09-25 (found revisiting the guide's "Where it differs"). Three `define-library` restrictions R7RS does not impose, all from lowering a library to one `defmodule` per file: only one library per file (`sl->has_library`), the library's name IS its file path (`library_module` slash-joins the parts and module resolution looks for that `.tur`), and `(export (rename internal public))` is refused because `:exports` is bare names. The export rename is the cheapest and the one a port hits most |

## Found executing r7rs-lang-plan T8, the memory audit (filed 2026-09-25)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[void-self-tail-call-not-lowered](../archive/void-self-tail-call-not-lowered.md)~~ | medium | **RESOLVED 2026-09-25** (archived): a `: nil` body walks the tail spine and ends each leaf in a bare `return;`; pinned by `tests/fixtures/void-self-tail-call-loop`. Original row: Every dialect. A self tail call in a `: nil` function is not a loop (`tco_spine_ok` excludes `TY_NIL`, emit_fns.c:5990), so it grows the C stack unless gcc makes the sibling call -- it does at `-O2`, not at `-O1`, and every ASan build is `-O1`. A 10^7-step `: nil` countdown segfaults at gcc `-O1`; the `: int` twin is a loop. The R7RS prelude works around it with value-returning `-lp__` loops |
| ~~[cps-self-tail-call-relies-on-sibling-call](../archive/cps-self-tail-call-relies-on-sibling-call.md)~~ | medium | **RESOLVED 2026-09-28** (archived). Self tail calls were fixed 2026-09-26: a backedge, `goto __tur_cps_self`. Mutual tail calls are fixed now in two ways. A cycle of CPS functions is fused into one `__cps_tcg_N` whose cross calls are jumps, and T5's direct groups take colored functions the CPS backend declines. The report's `ping`/`pong` prints `done` at `-O0` and `-O1`. Pinned by `tests/fixtures/r7rs-cps-loops-unoptimized` and `r7rs-cps-mutual-tail-unoptimized`, both built at `-O0`. Still a C call: a member reached through a closure or an internal `define`, and groups over 8 members or 32 slots. The `guard` shape is filed as `mutual-tail-call-through-guard-grows-the-stack`. Original row: A CPS (`__cps`) function's self tail call is emitted as `return f__cps(...)` (emit_cps_ir.c:6920), never a backedge, and gcc 13 has no `musttail`: a million-element Scheme `for-each`, `map`, `member` or `delay-force` stream segfaults at gcc `-O1`, passes at `-O2` and interpreted |
| ~~[mutual-tail-call-through-guard-grows-the-stack](../archive/mutual-tail-call-through-guard-grows-the-stack.md)~~ | low-medium | **RESOLVED 2026-09-29** (archived): `g1` was CPS-colored but EVICTED at emit time. Its base case `'g-done` is an `any` widen of a quoted symbol, which `operand_uses_control` did not know. `EX_SYM_LIT` is a delegated literal now, and an indirect call may take widened-literal arguments (`(list 1 2)`). So the pair is a cps->cps cycle and the existing group fusion takes it. A million deep at `-O0`, pinned in `tests/fixtures/r7rs-cps-mutual-tail-unoptimized`. Original row: Filed 2026-09-28 (cps-self-tail-call-relies-on-sibling-call). `(define (g2 f n) (guard (e (#t 'caught)) (f n)) (g1 f (- n 1)))`, with `g1` tail-calling back into `g2`, overflows an 8 MiB stack. A count of 30,000 crashes at `-O0` and 70,000 at `-O2`. `guard` makes `g2` CPS while `g1` stays direct, so neither group fusion takes the pair. `g2__cps` calls `g1` as `cps->direct` and boxes the result for `dk_run`, and `g1` enters `g2` through its prompt-and-setjmp wrapper: three C frames per round trip, at every `-O` level. Fix: color the direct partner of a CPS tail cycle so the cps->cps fusion takes it |
| ~~[r7rs-prelude-value-returning-loop-workaround](../archive/r7rs-prelude-value-returning-loop-workaround.md)~~ | low | **RESOLVED 2026-09-25** (archived): the 28 `-lp__` loops T8 added to stdlib/r7rs are folded back into their `: nil` originals and the wrappers are gone; a million-element `string-fill!`, `write`, `read` and `read-line` pass at `-O1`, where the loops now lower to `goto __tur_tailcall` |
| ~~[r7rs-heap-data-never-reclaimed](../archive/r7rs-heap-data-never-reclaimed.md)~~ | medium | **RESOLVED 2026-09-25** (archived): the r7rs-gc collector graduated -- the allocator of every compiled single-unit `#lang r7rs` program on Linux and macOS, `TUR_R7RS_GC=0` / `--no-r7rs-gc` to build without it (docs/archive/r7rs-gc-plan.md). Original row: by design today: a Scheme program's pairs, vectors, strings, records, promises and procedures are `:heap` boxes, which the memory model never frees (gc-guide), so memory only grows -- a loop building a dead four-element list peaks at 429 MB for 10^6 iterations. The named exemption for the `r7rs-*` fixtures' leaks; the fix is RC plus the cycle collector, or a tracing collector, for the Scheme heap. A conservative collector was in as the `--enable=r7rs-gc` experiment (docs/archive/r7rs-gc-plan.md; 429 MB -> 10 MB; since 2026-09-25's second pass it also sees values kept in Turmeric maps and `rc<T>` cells, refuses a thread start, and has its macOS roots) and this stays open until it graduates |
| ~~[r7rs-caught-raise-leaks-runtime-records](../archive/r7rs-caught-raise-leaks-runtime-records.md)~~ | low-medium | **RESOLVED 2026-09-25** (archived): the collector graduated and the records are garbage. Original row: compiled: each `raise` a `guard` catches leaks about 1 KB -- DK frames abandoned by the escape's longjmp, the `call/ec` escape record, the `raise-continuable` record, a packed rest chain (1,016,072 bytes for 1,000 raises). Under `--enable=r7rs-gc` the records are collected (200,000 caught raises: 266 MB -> 30 MB); open until the collector graduates |
| ~~[r7rs-remaining-scratch-leaks](../archive/r7rs-remaining-scratch-leaks.md)~~ | low | **RESOLVED 2026-09-25** (archived): the collector graduated and the scratch is garbage. Original row: the prelude's per-call scratch T8 did not fix: ratio and complex literal part spellings, a literal string decoded to code points for a string operation, a few char spellings and error messages. Under `--enable=r7rs-gc` each is collected like any other garbage; open until the collector graduates |
| ~~[r7rs-programs-compile-slowly](../archive/r7rs-programs-compile-slowly.md)~~ | low-medium | **RESOLVED 2026-09-28** (archived): the prelude is compiled once and linked. `tur build` writes a `#lang r7rs` program as two C units -- a library unit (runtime preamble + auto-loaded stdlib, external linkage, cached by content hash under `<tmpdir>/tur-build/prelude/`) and the program unit, which declares what it uses. Stdlib binding ids have their own range so the library text is the same across programs; any split the emitter cannot divide, or a failed `cc`/link, builds one unit as before (`TUR_PRELUDE_SPLIT=0` forces it). Release `tur`, one-line program: 3.0 s -> 0.95 s with the library cached (the first build, which compiles it, 8.5 s; follow-up r7rs-prelude-library-cold-compile). Also fixed on the way: CPS fresh ids and temp names collided with binding ids and emitter temps. Check `tests/check-r7rs-prelude-split.sh`. Original row: Filed 2026-09-25 (PR 923 CI). Every `#lang r7rs` program builds the whole prelude: 6-8 s locally, over the 10 s fixture budget on CI runners, almost all of it `cc -O2` over ~29,000 emitted lines. The Scheme fixtures carry `expected.timeout` 60 as a stopgap; the fix is a precompiled prelude, an object cache, or emitting only reachable definitions. **Halved 2026-09-25**: 71% of the `cc` time was GCC's `-Wmisleading-indentation` (quadratic on the lowering's brace-less `if` chains); the driver now appends `-Wno-misleading-indentation` to every `cc` over emitted C, 6.4 s -> 3.1 s for a one-line program; the rest (emit 0.9 s, codegen 1.4 s) stays open. **Re-measured 2026-09-27**: Release `tur` 3.55 s (emit 0.29 s, `cc -O2` 3.2 s); no gcc pass dominates, `-O1` would break CPS tail calls, parallel LTO only helps wall time, and the prelude's C differs between programs (16.7k lines), so a hash cache of it would miss. The fix is the prelude as a separately compiled unit with a stable interface, linked prebuilt |
| ~~[r7rs-prelude-library-cold-compile](../archive/r7rs-prelude-library-cold-compile.md)~~ | low | **RESOLVED 2026-09-29** (archived): a cold cache compiles the library unit in one piece per CPU (up to 8; `TUR_PRELUDE_JOBS`), at `-O2`, and `cc -r` joins them into the cached object: 9.1 s to 4.6 s on four cores, 6.3 s on two, Release `tur`. `emit_split_pieces` keeps every piece's declarations and static helpers, gives each a contiguous run of the external functions, a `static` twin of each small one (reached by a call-only macro, so inlining survives and addresses still name the one definition), and defines state once (statics renamed `tur_sp_`). Run time within noise. `check-r7rs-prelude-split.sh` fails on state defined in two pieces. Any failure compiles whole. Original row: Filed 2026-09-28 (r7rs-programs-compile-slowly). The prelude split makes every build after the first about 1 s, but the first one on an empty cache takes 8.5 s for a one-line program (3.0 s as one unit): the library unit exports every stdlib definition, so cc compiles all ~1,780 functions instead of dropping the ~1,100 a program never reaches. Once per `tur` version, `cc` and flags, per `<tmpdir>`. Fix: compile the library in parallel pieces, warm the cache at build/install time somewhere that survives a reboot. **Measured 2026-09-28:** `-O1 -foptimize-sibling-calls` halves the library compile (7.8 s to 3.8 s) and beats every other middle setting, but costs 9-12% run time on prelude-heavy code; left at `-O2` as the maintainer's trade to make, or measure a narrower optimization level for it |
| [r7rs-prelude-split-wrong-symbols-on-windows](r7rs-prelude-split-wrong-symbols-on-windows.md) | low | **Fix landed 2026-09-28, split still off on Windows pending one Windows run:** a split build has no weak data (the library unit defines its keyword records, the program unit declares them or keeps its own `static`), and separately compiled modules use `selectany` on PE; the diagnosis is confirmed with MinGW-w64 ld (a weak data record read at an offset resolves to the wrong bytes even in one object). Original: Filed 2026-09-28 (rjungemann/turmeric#952 CI). On Windows the prelude split links but quoted symbols print as other strings (`|uired field|` for `caught`): the keyword records both units share are `__attribute__((weak)) const` (SYM2, `sym_codegen_emit`), and PE/COFF resolves them to the wrong bytes (suspected). The split is off on Windows unless `TUR_PRELUDE_SPLIT=1`. Fix: strong library records plus `extern` in the program unit, the way fat boxes are carried |
| ~~[r7rs-prelude-split-gc-seam-on-macos](../archive/r7rs-prelude-split-gc-seam-on-macos.md)~~ | low | **RESOLVED 2026-09-28** (archived), confirmed on a Mac: the first of the two guesses, constructor order. Mach-O has no cross-object initializer priority -- ld64 runs them in link order -- so the collector's `constructor(101)` in the library unit ran after the program unit's `__tur_static_init_ctor`, whose bands had already grown the symbol table (`sym_tab_grow`) and built an empty HAMT (`tur_hamt_new`) with libc. The weak reference was fine and the collector's state was not forked; both were measured out. `tur_gc_install_rt_allocator()` is now factored out of the constructor and also called as the FIRST statement of `__tur_static_init`, which is ahead of every band on every platform. The split is **on by default on macOS**, and `tests/check-r7rs-prelude-split.sh` runs there -- its Mach-O arm needed fixing too, because plain `nm` cannot tell writable data from read-only on Mach-O (everything non-text is `s`), so it read a read-only static as forked state; it asks `nm -m` for the section now. The ELF arm has the same blind spot one section over -- `.data.rel.ro` is read-only after load but `nm` classes it `d` -- which a first version of the fix walked into: its `static const` allocator table read as state defined in both units on ubuntu-latest. The table is a plain local now, filled field by field, since `tur_rt_set_allocator` copies it anyway. Original row: Filed 2026-09-28 (rjungemann/turmeric#952 CI). Under the prelude split on macOS, `r7rs-gc-seam` prints `(16249)` for a list kept only in a Turmeric map: the collector frees it |
| ~~[r7rs-reentrant-callcc-not-on-windows](../archive/r7rs-reentrant-callcc-not-on-windows.md)~~ | -- | **RESOLVED 2026-09-26** (archived): `r7k_stack_base` reads the TEB (`gs:[0x08]`) and the capture/restore jumps with `__builtin_setjmp`/`__builtin_longjmp`, which unwind nothing, in the prelude and its interpreter twin. Verified under Wine on a MinGW cross build (a four-re-entry generator, the six fixtures at `-O2` and `-O0`, and every r7rs fixture compiled and interpreted); the six `requires.posix-apis` markers are gone, so Windows CI confirms it for real. c2mir-compiled code (`tur jit`) stays escape-only, unreachable today. Original row: The T5 copying continuations need the thread's stack base, which the prelude finds only on glibc and macOS; on Windows `call/cc` is the escape |

## Found executing the open R7RS reports (filed 2026-09-26)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[cps-pap-inline-ignores-wrapper-arguments](../archive/cps-pap-inline-ignores-wrapper-arguments.md)~~ | high | **RESOLVED 2026-09-26** (archived), the day it was found: `pap_extract` now requires the wrapper body to pass exactly the captures, then the parameters. Pinned by `tests/fixtures/cps-pap-inline-ignores-wrapper-arguments` and `r7rs-closure-one-capture-call`. Original row: every dialect. Inside a CPS function a closure called in place or through a `let`, whose body is one saturated call, was taken for a partial application on arity alone and rewritten to `(TARGET captures... args...)`: `((fn [x] (sub x p)) 10)` ran as `(sub p 10)`, `((fn [] (neg (+ p 1))))` as `(neg p)`, silently; under `#lang r7rs` `((lambda () (list p)))` handed `list` a bare value for its rest chain and `cc` refused it |

## Found in the Try Turmeric language picker (filed 2026-09-27)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-sweet-base-dialect-missing](../archive/r7rs-sweet-base-dialect-missing.md)~~ | low | **RESOLVED 2026-09-29** (archived): `READER_R7RS_SWEET`, a second Scheme row in `LANG_BASES[]`: the sweet-exp preprocessor, taught Scheme's lexemes (`sweet_lexeme_end`: `#\(`, `|sym|`, `#;`), then the Scheme reader with neoteric on, reading `f{n - 1}` as SRFI-105's `(f (- n 1))`. Libraries, `tur fmt` (checked, layout kept), the REPL and the playground picker take it; `r7rs/neoteric` is the unknown base the error fixture pins now. Fixed on the way: `#\(` and a `;` in a string after `$` in `turmeric/sweet`, and `#;` last in a list in every dialect. Fixtures `r7rs-sweet`, `sweet-lexemes-not-structure`, `r7rs-datum-comment-anywhere`. Original row: Filed 2026-09-27. `#lang r7rs/sweet` is TUR-E0331: Scheme is the one language with no sweet-exp base, though SRFI-110 is a Scheme SRFI. Deliberate (r7rs-lang-plan Section 8 Q5; `LANG_R7RS`'s `reader_axis_free = false`, lang_dialects.c:55), filed so it can be picked up. The real work is pointing the sweet-exp pass at the Scheme reader rather than the Turmeric one; `LANG_BASES[]` and `reader_axis_free`'s two-valued answer are the easy half |

## Found executing the open R7RS reports, second pass (filed 2026-09-28)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-repl-forgets-macros-and-set](../archive/r7rs-repl-forgets-macros-and-set.md)~~ | medium | **RESOLVED 2026-09-28** (archived), the day it was found, except `prefix` over a user library: the interpreter names a session's earlier turns to the Scheme lowering (`scheme_lower_set_session_prior`), which replays their `import` sets and re-registers their `define-syntax` macros; a REPL/`eval` turn's top-level variable is always the mutable `any` cell; a turn importing a library keeps its expressions and definitions at top level. Fixtures `r7rs-repl-macros-and-set-persist`, `r7rs-eval-macros-and-set-persist`. Original: at `tur repl --lang r7rs` (and through `eval`), a macro one turn defined was "unknown function or operator" in the next, `set!` of an earlier turn's variable was "'n' is immutable", `(import (srfi 1))` then `fold` was unknown, and a turn importing a user library ran none of its expressions: incremental elaboration lowers each turn alone |

## CI instrumentation (filed 2026-09-28)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[macos-jit-hang-loses-both-diagnostics](../archive/macos-jit-hang-loses-both-diagnostics.md)~~ | medium | **RESOLVED 2026-09-28** (archived) for the instrumentation defect it was filed for; the stall itself continues as the row below. All three fix directions landed: `perl -e 'alarm 2100'` heads the ctest pipeline so a hang FAILS the step (a `timeout-minutes` kill cancels the job and leaves the `if: always()` upload `pending` -- which is precisely how the log was lost, and the upload step's comment claiming otherwise is corrected); the leg's three ctest targets carry `TIMEOUT` 1500 / 600 / 900 so a per-test kill names the culprit; and `tests/run-flags.sh` has a per-invocation timeout for the first time, via a generated `$TUR` wrapper that bounds all ~100 call sites and the ones added later. Original row: Filed 2026-09-28 (rjungemann/turmeric#953). The `JIT engine (macos-latest)` 45-minute hang recurred, and the 2026-08-02 instrumentation built to diagnose it produced NOTHING: a `timeout-minutes` kill leaves the `if: always()` upload `pending` and the job's log blob 404s, so the artifact and the streamed console are both gone. The `coreutils`/`gtimeout` containment is still in place and is not the cause |
| ~~[r7rs-gc-torture-fiber-thread-cases-crash](../archive/r7rs-gc-torture-fiber-thread-cases-crash.md)~~ | medium (collector memory safety; fails the Auxiliary legs) | **RESOLVED 2026-09-30** (archived): glibc's x86-64 `setjmp` stores `rbp` pointer-mangled and every spill the collector read was a `setjmp`, so main's new `FiberBlock`, held in `rbp` across the contended scheduler lock inside `spawn`, was invisible to a collection while main was parked there -- freed with its stack, then enqueued dangling; every crash shape was that reuse. `TUR_GC_FP` records the frame pointer plainly at each spill and the scan marks it; 0 of 200 torture runs fail (was 6 of 40 to 23 of 80), `fiber-dynamic-env` 0 of 40. Original row: Filed 2026-09-29. Under `TUR_GC_TORTURE=31` the compiled fiber/thread fixtures crash intermittently, and which one changes run to run: `r7rs-threads-fiber-migration` 10 of 60 runs on `main`, and `threads-fiber-dynenv` / `r7rs-threads-fiber-dynamic-env` in `main`'s own CI (exit 139, and 134 `stack smashing detected`); on macOS the same pair hung to the ctest timeout. Came with #966/#969. Not the call/cc TLS rewind (rate unchanged with that fix). Look at the collector's scan of a parked fiber's stack and `r7dyn`, and at `r7k_stack_base` for a capture on a fiber stack |
| ~~[jit-r7rs-callcc-reads-unknown-tag-after-per-thread-dynenv](../archive/jit-r7rs-callcc-reads-unknown-tag-after-per-thread-dynenv.md)~~ | -- | **RESOLVED 2026-09-29** (archived): not JIT-specific. On glibc a non-main thread's TCB and static TLS sit at the top of its stack mapping, which is the top `pthread_getattr_np` reports, so a call/cc image measured to it rewound the thread's thread-locals on every re-entry -- visible once `82867413` moved the delivered value into `tur_r7rs_dyn`. `tur jit` runs the program on such a thread; a compiled program that re-enters on a worker failed the same way. `r7k_stack_base` now stops below the thread's TCB and every module's TLS block (`pthread_self()`, `dl_iterate_phdr`). New fixture `r7rs-threads-reentry-on-worker`. A bisect built without `libturt_runtime.a` sees every commit pass (the JIT then falls back to cc). The r7rs-gc torture crashes in the fiber cases are a separate bug |
| ~~[jit-fork-child-inherits-asan-allocator-lock](../archive/jit-fork-child-inherits-asan-allocator-lock.md)~~ | low (CI flake, JIT legs) | **RESOLVED 2026-10-04** (archived): closure condition met -- since #1050 the Linux JIT leg is 25 of 26 green, the last 17 consecutive, against 18% red before; no `(fork-failures 1)` since. The one red run failed the same fixture with a different symptom, filed as the row below.  Original row: Filed 2026-09-29. `r7rs-threads-lifecycle`'s fork check prints `(fork-failures 1)` about 1 run in 3 under the Debug (ASan) `tur jit`: gdb shows the child blocked on ASan's size-class allocator mutex, inherited held from the burner thread mid-`malloc` at the fork. glibc's `malloc` is fork-safe and this libsanitizer's allocator is not, so only the sanitized in-process JIT (CI's configuration) sees it. Same shape as the archived `g_gen_lock` fix, but the lock is ASan's. Fix: narrow the fixture's fork check under a sanitized JIT host, or a runtime that locks its allocator at fork. **2026-10-03:** direction 1 landed (`TUR_JIT_HOST_ASAN` in the JIT prelude; `life-forks` stands down only under it), measured locally 3/8 -> 0/8 on a Linux Debug + `TUR_JIT` build. **Open until** ~15 consecutive green Linux JIT legs |
| [jit-r7rs-threads-lifecycle-exited-1-once](jit-r7rs-threads-lifecycle-exited-1-once.md) | low (one CI occurrence) | **Filed 2026-10-04.** `r7rs-threads-lifecycle` `exited 1 (expected 0)` once on `JIT engine (ubuntu-latest)` (run 37149022036), after the fork skip landed -- not the fork symptom, which exits 0. Likely a sanitizer report in the JIT'd program; unreproduced in 40 local runs. Next step: read that run's `jit-ctest-log-ubuntu-latest` artifact |
| [jit-linux-aborting-fixtures-time-out](jit-linux-aborting-fixtures-time-out.md) | low (CI noise; `continue-on-error` leg) | **Filed 2026-10-07.** Fixtures that end in `abort()` (panics, failed contracts and casts) time out at 15 s on `JIT engine (ubuntu-latest)` in 2 of 3 `main` runs checked (5 and 11 of them), in clusters that abort together, while each takes ~0.5 s locally. Unverified hypothesis: the runner's piped core-dump handler serializes cores of the sanitized `tur`. Next step: print `core_pattern` on the runner, then `ASAN_OPTIONS=handle_abort=1` in `run-jit.sh` |
| ~~[r7rs-eval-host-procedure-name-on-the-stack](../archive/r7rs-eval-host-procedure-name-on-the-stack.md)~~ | medium | **RESOLVED 2026-10-07** (archived, filed the same day): `host_value` registered a native under a `char name[64]` stack buffer and the global binding kept the pointer, so a later hash probe read a dead frame -- `FAIL r7rs-eval -- stdout mismatch` (an ASan stack-buffer-overflow) on every Linux JIT run checked. The name now lives in the env's `sym_arena` |
| [macos-jit-leg-stall-unexplained](macos-jit-leg-stall-unexplained.md) | medium | Filed 2026-09-28, split out of the row above when its instrumentation half was fixed. **2026-09-29:** the first instrumented occurrence (#970, `tur_jit_fixture_tests ***Timeout 1500`) was throughput, not a stall -- a steady ~1.9 fixtures/s cut off in the last few fixtures and the `errors/` pass; its cause is fixed in [jit-suite-pays-for-the-whole-prelude](../archive/jit-suite-pays-for-the-whole-prelude.md), and the earlier 45-minute occurrences stay unexplained. `JIT engine (macos-latest)` has stalled for the whole 45-minute job budget four times; the leg is the one JIT leg that is not `continue-on-error`, so it gates. **Deliberately thin: there is no evidence to reason from** -- the 2026-09-28 occurrence destroyed its own, and the two earlier root causes (`httpd-async-limit`'s listen-fd deadlock, the missing `timeout(1)`) were genuinely fixed and are not live leads. The next occurrence will hand you a `jit-ctest-log-macos-latest` artifact, the hung test's name, and a per-case bound inside `run-flags.sh`. Look first at an untimed phase AROUND a fixture (`_run_timed` wraps the `tur ... jit` invocations, not harness setup or an untimed compile/link), then at a `jit-ffi-*` case. Runner size is context, not cause: 3-core `macos-latest` explains 19 min, not 48. **2026-10-03:** a third instrumented occurrence, and the first with NO `***Timeout` -- the suite finished in 1055 s (below its 1125 s p90) and only `r7rs-tail-calls` died, at its own 60 s `expected.timeout`. The 2026-10-01 fix raised the per-TEST bound (1500 -> 2400) and left the per-FIXTURE one alone, so the failure migrated down into it; that fixture is ~40M trampolined tail calls at -O0 and 60 s was almost exactly its slow-draw time, so it is now 120 (#1044). Measured rates: macOS 2/289 = 1% (both isolated), Linux 55/302 = 18% and absorbed by `continue-on-error` -- the Linux half decomposed into two already-reported fixtures the same day and is archived as [jit-linux-leg-failures-absorbed](../archive/jit-linux-leg-failures-absorbed.md) |
| ~~[flags-parametric-record-field-compares-asan-warning](../archive/flags-parametric-record-field-compares-asan-warning.md)~~ | low (harness; JIT legs) | **RESOLVED 2026-09-29** (archived, same day): the check keeps stderr in its own file and compares stdout only, refusal grep on both streams; `flags summary: 130 passed, 0 failed` on a sanitized JIT build. Original row: Filed 2026-09-29. `jit-ffi-interp-parametric-record-field` in `tests/run-flags.sh` fails on a CORRECT answer under the sanitized JIT build: it captures `2>&1` and compares the whole text against `42\n1`, and stderr carries ASan's makecontext/swapcontext warning (the program's `(unsafe ...)` is a handle, evaluated through `eval_handle`'s ucontext). Red on every `JIT engine (ubuntu-latest)` run (`flags summary: 129 passed, 1 failed`), invisible because that leg is `continue-on-error`. Fix: compare stdout only, grep the refusal diagnostic on both streams |

## Found landing r7rs-lang-plan R3 (filed 2026-09-23)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[saffron-open-generic-result-not-grounded](../archive/saffron-open-generic-result-not-grounded.md)~~ | -- | **RESOLVED 2026-09-26** (archived), and not by the fix the title names: `(map-new)` was already `(Map any any)`. Four defects: the `(& K)` key check had no borrow seam (repro 1); the any-arg seam bound a container's type variables from a scalar sibling (repro 2, and the same panic for `vec-push!` / `unwrap-or` on an `any`); `#map{}` literals kept typed keys, so a literal behind an `any` failed every seam; and `[]` stayed an open `(Vec A)` because its `(vec-new)` carries the stdlib's span. Every Saffron map is now `(Map any any)`, keys included -- an annotated `(Map Sym any)` parameter no longer takes a literal, as `(Vec int)` never took `[1 2 3]`. Pinned by `saffron-open-generic-grounded` and `errors/saffron-typed-map-rejects-wrong-key`. |
| ~~[borrowed-aggregate-key-skips-the-key-check](../archive/borrowed-aggregate-key-skips-the-key-check.md)~~ | -- | **RESOLVED 2026-09-26** (archived). A borrow now records its target's full type (`ref_borrow.target_full`) when the kind does not name it, so `type_eq`, the `(& K)` binding arm and a concrete `(& Pt)` parameter all compare targets: a String or struct key into a `(Map int int)` is refused (`expected &int, got &String`), as is a borrowed `Qt` into a `(& Pt)` -- a second face found on the way. In a Saffron file the refusal lets the borrow seam box a struct key, and `(deref k)` of a `(& K)` is the named tyvar, so the callee reads the box at K = `any`. Pinned by `errors/borrowed-aggregate-key-refused`, `errors/borrowed-aggregate-param-refused`, `borrowed-aggregate-target-typed` and `saffron-borrowed-struct-key-widened`. |
| ~~[saffron-applied-class-var-result-takes-one-instances-type](../archive/saffron-applied-class-var-result-takes-one-instances-type.md)~~ | -- | **RESOLVED 2026-09-26** (archived). The root was a layer below dispatch: `definstance` substituted the class variable into a result that IS it but not into one that contains it, so `Wrap [Pt]`'s impl kept returning an open `(Option a)` that lowered to the carrier and every dispatched result carried one unresolved tag. Now the variable is substituted through the application, the instance body sees the result as its expected type, and the dispatch goes through a per-instance `any` witness; the dict slot spells a carrier-ABI result as the impl does. Found on the way and fixed: a Saffron data literal under a pinning expectation (`(defn f [x : float] : (Vec float) [x x])`) built a `(Vec any)` typed `(Vec float)` -- garbage floats, compiled. Pinned by `saffron-dyn-applied-class-var-result`, `typeclass-applied-class-var-result`, `saffron-literal-under-typed-return`, `errors/instance-applied-result-mismatch` and `errors/saffron-dyn-return-directed-applied-result` (the report's own repro leaves the receiver unannotated, which makes the method return-directed; that is now an error with a hint). |

## Found landing the r7rs-gc threads plan (filed 2026-09-25)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[jit-fork-child-hangs-with-threads](../archive/jit-fork-child-hangs-with-threads.md)~~ | low | **RESOLVED 2026-09-25** (archived): the child was waiting on the JIT's lazy-generation lock, `g_gen_lock`. `pthread_atfork` now takes that lock around `fork` (src/jit_engine.c), and `r7rs-threads-lifecycle` runs its fork check under the JIT again |
| ~~[keyword-seed-collides-with-symbol-seeder](../archive/keyword-seed-collides-with-symbol-seeder.md)~~ | medium | **RESOLVED 2026-09-25** (archived): the seeder is now `__tur_symtab_seed`. Fixture `r7rs-keyword-seed` |
| ~~[tur-ret-disagrees-with-inline-c-signature](../archive/tur-ret-disagrees-with-inline-c-signature.md)~~ | low | **RESOLVED 2026-09-25** (archived): when the signature emitter writes the carrier `int64_t` for an inline-C body, `__TUR_RET__` now follows it (src/compiler/emit_fns.c). Fixture `inline-c-tur-ret-heap-result` covers Map and Vec |
| ~~[jit-threaded-program-hangs-under-load](../archive/jit-threaded-program-hangs-under-load.md)~~ | medium | **RESOLVED 2026-09-26** (archived): the cause was MIR's lazy generation rewriting a function's call thunk in place while another thread ran through it. `tur jit` now generates every remaining function at a program's first `pthread_create`, while it is still single-threaded (src/jit_engine.c). Five thread-locals the JIT shared between threads now have host slots too (src/runtime/tur_tls.c). Under load: 0 bad runs of `r7rs-threads-pause`, where lazy generation had 6 in 150 |

## Found auditing type-confusion-detection-plan (filed 2026-09-26)

The plan's nightly fuzzer had found the first row four times and filed it zero
times: its issue step used a `fuzz` label that did not exist. See the plan's
section 5b (`docs/archive/type-confusion-detection-plan.md`).

| Report | Severity | One line |
| --- | --- | --- |
| ~~[constrained-generic-float-result-into-generic-value-converts](../archive/constrained-generic-float-result-into-generic-value-converts.md)~~ | high | **RESOLVED 2026-09-26** (archived): a carrier-returning spec now bit-casts a float tail when the class-method call in tail position is re-targeted to a float instance, and it reads the tyvar-declared test from `result_full_type` too (src/compiler/emit_fns.c). Fixture `constrained-generic-float-result-into-generic`. Original: a constrained generic's float result passed into another generic printed `-nan` for `-4.25` |
| ~~[let-bound-class-method-result-in-constrained-generic-truncates](../archive/let-bound-class-method-result-in-constrained-generic-truncates.md)~~ | high | **RESOLVED 2026-09-26** (archived): a class-method call on an abstract-tyvar receiver is typed with the receiver's `A` when the class declares receiver and result as its variable, not with the representative instance's `int` (src/compiler/elab_typeclasses.c). Fixture `let-bound-class-method-result-keeps-its-tyvar`. Original: `(let [y (n x x)] y)` in a constrained generic returned `-4` for `-4.25` |
| ~~[let-bound-generic-call-result-in-generic-truncates](../archive/let-bound-generic-call-result-in-generic-truncates.md)~~ | high | **RESOLVED 2026-09-26** (archived): a `let` binding over a generic call instantiated to the enclosing signature's own type variable is typed with that variable, through a reinterpret the emitter lowers per clone. It bridges from the carrier only when the hoist temp is recorded as one (src/compiler/elab_forms.c, src/compiler/emit_expr.c). Fixture `let-bound-generic-call-result-keeps-its-tyvar`. Original: `(let [y (gid x)] y)` in a generic returned `9` for `9.75`, stdlib `vec-get`/`unwrap-or` were hit the same way, and a by-value aggregate did not compile |

## Found planning SRFI support for `#lang r7rs` (filed 2026-09-26)

The first three came from the probes behind `docs/archive/r7rs-srfi-plan.md`
(its Section 2.3); chibi's R7RS suite reaches none of them. The syntax-leaks
report came out of resolving the first: R7RS imports Turmeric libraries, but
Turmeric's own surface should not leak into Scheme source. The rare-hang note
came from the wide test run after that work. The last two came out of S0's
splice measurement: chibi's SRFI 1, spliced into a program, defines `any`,
which the stdlib's type annotations read. The program half of that is fixed
(`r7rs-program-shadows-names`); the library half is the report.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-leading-colon-identifiers](../archive/r7rs-leading-colon-identifiers.md)~~ | medium | **RESOLVED 2026-09-26** (archived): a leading `:` is an identifier in user Scheme source (`scheme_user_source`, src/compiler/reader.c); seam code writes the symbol `'k`, the keyword's runtime value, and a stray `:k` gets a help line saying so. Fixtures `r7rs-colon-identifiers`, `errors/r7rs-colon-is-not-a-keyword`. Original: in a `#lang r7rs` source file, `':x` reads as the symbol `x`, and `':::` or a parameter named `:x` is an error. The runtime `read` gets all three right. Blocks SRFI 42 (`:list`, `:range`, ...) and the `:::` custom ellipsis; a colon inside an identifier (`char-set:letter`) is fine |
| ~~[r7rs-gc-threads-lifecycle-rare-hang](../archive/r7rs-gc-threads-lifecycle-rare-hang.md)~~ | low | **RESOLVED 2026-09-27** (archived): a self-deadlock on the collector's `world` lock. The fixture's registry count took `world` through the release-point wrapper; contended, the wrapper parked, took it, and unparked into `world` again. `tur_gc_mutex_lock` now takes the collector's own locks unparked (src/runtime/r7gc.c). The fixture's detach phase runs twenty rounds, which hang every time without the fix; one round hit it about once in 600 runs. Original row: Unconfirmed. One `run-r7rs-gc.sh` run timed out `threads-lifecycle` under `TUR_GC_TORTURE=31`
| ~~[r7rs-turmeric-syntax-leaks](../archive/r7rs-turmeric-syntax-leaks.md)~~ | medium | **RESOLVED 2026-09-27** (archived): in user Scheme source the Turmeric `#` literals, inline C and `@` are read errors naming the Scheme spelling or the `(turmeric ...)` seam; `true`/`false`/`nil`/`^tailcall` are identifiers; the auto-loaded stdlib is visible only through `(import (turmeric stdlib/<file>))` (any import set, REPL turns included) and a Turmeric built-in not at all; an unknown name is an error on both back ends. Original row: Turmeric's surface is live in user `#lang r7rs` source. Keywords, brackets (now parentheses) and Turmeric special forms (now refused; `#lang turmeric` at the REPL) are resolved. Still open: `#map{}`/`#set{}`/`#rat{}`/`#cx{}`/`#?()`, inline C, `^tailcall`, `@`, `true`/`false`/`nil` as literals, and every auto-loaded stdlib name (`println`, `vec-new`, `box`, ...) with no import; `--interpret` also runtime-dispatches a name the compiled back end refuses. Ten measured items, the fixtures that depend on them, and fix directions (namespace first) |
| ~~[r7rs-define-library-cannot-export-syntax](../archive/r7rs-define-library-cannot-export-syntax.md)~~ | medium | **RESOLVED 2026-09-26** (archived): the library leaves exported macros out of its module exports and exports the helpers their templates name under a hidden spelling; each importer reads the library's source, renames and registers the macros under the import set's names (src/compiler/scheme_lower.c `lib_*`). `tests/run-r7rs-import.sh` `library-exports-macros`, `library-macros-under-import-sets`, `turmeric-imports-macro-library`. Original: exporting a `define-syntax` name from a `define-library` is "exported symbol ... is not defined in this module" (src/compiler/elab_module.c:1580): the macro is consumed by the Scheme expander and never reaches the module interface. The same macro in a `load`ed file works |
| ~~[r7rs-library-defines-standard-or-stdlib-name](../archive/r7rs-library-defines-standard-or-stdlib-name.md)~~ | medium | **RESOLVED 2026-09-28** (archived): one context-free rule (`lib_needs_respelling`): a library respells a standard or stdlib name it defines, or a rename's public name, to `<name>--user`, and every importer learns which from the library's source (`lib_respelled_exports`) under any import set. `run-r7rs-import.sh` `library-defines-standard-and-stdlib-names`, `library-standard-names-under-import-sets`. Original row: A `define-library` body defining `square` (a standard name) or `None`/`list-length` (auto-loaded stdlib names) is "already defined by an auto-loaded stdlib module" compiled; interpreted it is unreachable or silently replaces the stdlib's. Programs are fixed (they shadow or respell, both back ends, REPL turns included), and type names (`any`, `int`) are fixed everywhere. Needs the library to respell and the importer to learn each export's spelling (extend `lib_syntax_of`'s source read) |
| ~~[r7rs-repl-echoes-multiple-values-opaquely](../archive/r7rs-repl-echoes-multiple-values-opaquely.md)~~ | low | **RESOLVED 2026-09-28** (archived): the R7RS prompt echoes each value on its own `=>` line and nothing for `(values)`; a definition echoes nothing either (it printed `=> 3` / `=> #<procedure>`). Hook fixture `r7rs-repl-echoes-multiple-values`. Original row: `tur repl --lang r7rs` echoes `(values 1 2)`, `(values)` and `(exact-integer-sqrt 17)` as `#<R7rsValues>`: the echo writes the prelude's carrier struct. Fix in the echo branch of src/turi/repl.c: one `=> ` line per value, none for zero |
| ~~[r7rs-cond-expand-ratios-feature-drift](../archive/r7rs-cond-expand-ratios-feature-drift.md)~~ | low | **RESOLVED 2026-09-26** (archived): `feature_holds` reads one array, `R7RS_FEATURES`, and fixture `r7rs-features-agree` asks `cond-expand` (through `eval`) about every identifier `(features)` returns. Original: `(features)` lists `ratios` (stdlib/r7rs/prelude.tur:2424), but `cond-expand` does not hold it (`feature_holds`, src/compiler/scheme_lower.c:3970): two hand-kept copies of one list. r7rs-srfi-plan S1 generates both from one table |

## Found landing r7rs-srfi-plan S2 (filed 2026-09-27)

The first two came from SRFI 2's `and-let*`, whose whole point is guarding
a call that would fail. Neither blocks S2: chibi's SRFI 2 suite passes in
full, because its guarded calls are to `+`, which takes `any`. The third
blocks SRFI 17. The fourth is from S2's pull request's macOS CI, in a
collector test the stage did not touch. The fifth turned up in S3, while
checking that pull request's fix under clang.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-dead-mistyped-call-refused-at-compile-time](../archive/r7rs-dead-mistyped-call-refused-at-compile-time.md)~~ | medium | **RESOLVED 2026-09-28** (archived): in a Scheme file a statically mistyped argument is widened and takes the checked cast, so the call compiles and raises only if it runs; a variadic procedure's fixed parameters too (they compiled unchecked and crashed), and too many arguments to a known procedure raises "f: too many arguments". The interpreter's cast to `Sym` now checks. Fixture `r7rs-dead-mistyped-call`. Original row: `(let ((x #f)) (if x (car x) 0))` and a never-called `(define (g) (car 5))` are TUR-E0001 "expected R7rsPair, got bool/int" on both back ends: the typed prelude's argument check refuses a concrete mismatch that R7RS makes an error only when it runs. Fix: in a dynamic file, widen and let the checked cast fail at run time |
| ~~[r7rs-type-errors-are-uncatchable-panics](../archive/r7rs-type-errors-are-uncatchable-panics.md)~~ | medium | **RESOLVED 2026-09-27** (archived): a Scheme cast that fails, the dynamic operator under the prelude's numeric code, and the prelude's "it is an error" helpers all raise an error object through `r7rs-type-error__` ("car: not a pair", the value its irritant), compiled through a hook the prelude installs and interpreted by name. The hot helpers raise through inline C so nothing new compiles to CPS. SRFI 1 and 41 floors raised (160, 184). Fixture `r7rs-type-errors-raise`. Original row: **Narrowed in S6:** `vector-ref`/`vector-set!` index errors now raise. `(car 5)` through a variable panics ("cast: any holds int, not R7rsPair") on both back ends; `guard` cannot catch it, where chibi and Racket raise an error object. Fix: in `#lang r7rs`, a failed boundary cast raises an error object |
| ~~[r7rs-prelude-procedures-lose-identity](../archive/r7rs-prelude-procedures-lose-identity.md)~~ | medium | **RESOLVED 2026-09-27** (archived): one adaptor per function. The lifted adaptor lambda is cached on the wrapped function's binding (`any_adaptor`) and reused at every boxing, so its static fat box is the procedure's identity, in a library and the program alike (one TU); a local alias boxes through the global's, and the interpreter no longer re-homes the shared adaptor per reference. SRFI 17 unblocked. Fixture `r7rs-procedure-identity`, `run-r7rs-import.sh` `library-shares-procedure-identity`. Original row: `(eqv? car car)` is `#f` on both back ends: boxing a typed prelude procedure as `any` wraps it in a fresh adaptor at every reference (`saffron_dyn_fn_adaptor`, elab_call.c:825), so each reference is a new procedure. The program's own procedures keep their identity. Blocks SRFI 17, whose `setter` table is keyed on `car`, `vector-ref`, ... Fix: one adaptor per function, made once per compile |
| ~~[r7rs-gc-threads-lifecycle-macos-timeout](../archive/r7rs-gc-threads-lifecycle-macos-timeout.md)~~ | low-medium | **RESOLVED 2026-09-27** (archived): a deadlock. The harness's new stack dump caught it on PR #949. macOS's `pthread_create` holds libpthread's thread-list lock, and the collector's `pthread_kill` takes it. A thread stopped mid-create held the lock the collector then waited on. `tur_gc_pthread_create` and `tur_gc_pthread_detach` now park around the libc call. Original row: `threads-lifecycle` timed out (>300 s) under `TUR_GC_TORTURE=31` on macOS CI (PR #948) |
| ~~[r7rs-raise-musttail-fails-under-clang-x86-64](../archive/r7rs-raise-musttail-fails-under-clang-x86-64.md)~~ | medium | **RESOLVED 2026-09-27** (archived): not an inlined setjmp. clang 18's dead argument elimination narrowed `r7rs_hyraise`'s `tur_tagged_t` return to its tag word under a `musttail` call whose callee still returns both. Every function that makes a `TUR_MUSTTAIL` call is now address-pinned in a `used` table (`emit_musttail_pins`), under clang only. Fixture `r7rs-raise-under-musttail` (`requires.musttail`). Original row: Found landing S3: every `#lang r7rs` program that reaches `raise` fails to build with clang on x86-64 ("failed to perform tail call elimination on a call site marked musttail" in `r7rs_hyraise`). Only with the call/cc runtime `tur build` pastes, and only at `-O1` and above: most likely a `__builtin_setjmp` wrapper inlined into the `musttail` caller. CI misses it (gcc on Linux, arm64 on macOS) |
| ~~[r7rs-too-few-arguments-returns-a-procedure](../archive/r7rs-too-few-arguments-returns-a-procedure.md)~~ | medium | **RESOLVED 2026-09-27** (archived): in user Scheme source an under-saturated call to a known procedure runs its arguments and raises "f: too few arguments (expects at least 1, got 0)"; a dynamic call with the wrong count raises "wrong number of arguments (N given)" (compiled `__tur_dyn_call_arity`, interpreted arity checks), and calling a non-procedure raises "not a procedure". SRFI 41 passes in full (floor 187). Fixture `r7rs-too-few-arguments`. Original row: Found landing S7: `(f)` of `(define (f a . rest) a)` returns a procedure (Turmeric's partial application) instead of raising, so `guard` sees nothing; SRFI 41's `(test-error (stream-zip))` passes through. Fix: under-saturated calls in `#lang r7rs` raise |
| ~~[letrec-mutual-recursion-between-capturing-closures](../archive/letrec-mutual-recursion-between-capturing-closures.md)~~ | medium | **RESOLVED 2026-09-28** (archived): `elab_letrec` predicts which `fn` members capture before elaborating any, so a call to a LATER capturing sibling is captured; the emitter ties the knot (the earlier env's slot is zeroed, then patched once the later member is bound). Pinned by `letrec-mutual-capturing-closures` and `r7rs-internal-defines-mutual-capture`. Original row: Found landing S6: two `letrec` members (or `#lang r7rs` internal defines) that capture a local and call each other fail in cc ("'od' undeclared"). S6 made a call to an EARLIER capturing sibling capture it; a LATER one has no init yet to say it captures. Fix: allocate the group's envs, then fill them |

## Found landing r7rs-srfi-plan S2's SRFI 17 (filed 2026-09-28)

Not caused by that change -- it reproduces with its compiler diff reverted,
and CI is green at the same commit. It is here because SRFI 17's work is what
ran the fixture that shows it.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-reentrant-callcc-wrong-with-eval](../archive/r7rs-reentrant-callcc-wrong-with-eval.md)~~ | medium | **RESOLVED 2026-10-01** (archived): fixed by `7c90e00b8`, which landed ~10 hours after the `bf31e725c` this was filed against and was never in it -- the re-entry path's thread-local stores were made through a stale address after `setjmp`'s second return. The report's closing hypothesis (the Mac's OS-ahead ASan runtime, not a code bug) is **wrong**: the repro still fails at `bf31e725c` under today's CLT, and at `main` the suspected configuration -- ASan-instrumented `libturi.a`, `libclang_rt.asan_osx_dynamic.dylib` loaded in the emitted binary -- prints the right answer. `eval` mattered only because linking the embedded interpreter moved where the stale store landed: value corruption at `bf31e725c`, SEGV in the capture at `7c90e00b8^`. `docs-r7rs-guide-examples` passes |

## Found fixing the captured-`^mut` copy (filed 2026-09-26)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[mut-cell-is-never-freed](../archive/mut-cell-is-never-freed.md)~~ | low | **RESOLVED 2026-09-28** (archived): the `let` frees the cell at scope exit when every closure capturing it is a let-bound, non-escaping closure (a lambda passed to a non-retaining parameter is one, via the `__borrowc` hoist) whose own body does not let the cell out; anything else keeps it. Pinned by `mut-cell-freed-at-scope-exit` under `run-leak-check.sh`. Original row: The shared heap cell a lambda-captured `^mut` now lives in (`TurMutCell`) is allocated like any `:heap` value and never freed: 8 bytes (16 for `any`) per evaluation of the `let`, one per iteration in a loop. Fix: free it at the `let`'s scope end when every capturing closure's env is freed there (the emitter already decides that per closure), or make it an `rc` cell once `set!` through an `rc` of a parametric struct is allowed |

## Found driving the open-reports PR's Windows suite (filed 2026-09-26)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[static-instance-spec-calls-any-lambda-as-concrete-result](../archive/static-instance-spec-calls-any-lambda-as-concrete-result.md)~~ | high | **RESOLVED 2026-09-29** (archived): the `any` bridge (`elab_fn_any_bridge`) marshals a function argument whose signature differs from the slot's only in where `any` appears; at a method dispatch its target is the instance body's own view of the parameter (Saffron: bare tyvar params read as `any`). `saffron-static-instance-spec-any-lambda` |

## Found archiving proper-tail-calls-plan (filed 2026-09-28)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[tail-grammar-skips-and-or-and-carrier-lets](../archive/tail-grammar-skips-and-or-and-carrier-lets.md)~~ | low-medium | **RESOLVED 2026-10-03** (archived): `emit_tail`'s inline `let` arm shares `emit_let_value`'s declaration ladder (`emit_let_binding_decl`) and puts its scope-exit releases on the drop channel the backedge and every `return` fire, so `tco_let_simple`'s carrier-ABI bail is gone -- a `Vec`, a list or a parametric heap ADT bound in a loop's `let` keeps the backedge (`tailcall-carrier-let-deep`, -O0, 1,000,000 steps).  A binding with a release stays refused only when it is used as more than a number while the backedge carries a non-number (`TC_LET_DROP`).  Also fixed: a by-value recursive spine bound in a tail `let` leaked every iteration (`tailcall-carrier-let-releases`, leak-checked).  The `and`/`or` half was fixed 2026-09-29 |

## Found archiving saffron-lang-plan (filed 2026-09-28)

S9's two remaining dynamic-dispatch limits, split out so the plan could move to
`docs/archive/saffron-lang-plan.md`. Both are compiled-only clean panics at the
witness's checked cast; `--interpret` answers.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[musttail-indirect-aggregate-arg-dangles-on-aarch64](../archive/musttail-indirect-aggregate-arg-dangles-on-aarch64.md)~~ | high on arm64 | **RESOLVED 2026-10-01** (archived): a `musttail` call forwarding a by-value aggregate over 16 bytes (`__dynwit_Comb_comb_Two`'s 32-byte `tur_adt_Two__any`) passed the callee a pointer into its own released frame on AAPCS64; x86-64's byval area outlives the jump, so only macOS failed (`saffron-class-fn-extra`, #1007).  `tail_call_musttail_ok` now admits only pointer, scalar and `tur_tagged_t` parameters |
| ~~[saffron-dyn-witness-fn-arity-defaults-unary](../archive/saffron-dyn-witness-fn-arity-defaults-unary.md)~~ | low -> **was high** | **RESOLVED 2026-09-30** (archived): worse than filed. The unary case silently printed the element's TYPE TAG (`4` for `2.5`) compiled. In a dynamic dialect an arity-less `fn` class parameter is now an `any` called dynamically. Also: the M7 gate mints the spec for a dynamic-call body; static dispatch widens `any` args; a `: a` result is narrowed with a checked unbox. An unresolved-tyvar widen now traps instead of tagging `TY_TYVAR`. Pinned by `tests/fixtures/saffron-class-fn-extra` and the Saffron fuzzer's new `hofm_*` shapes. Original row: a `g : fn` extra was cast unary by the dispatch witness, so a binary lambda panicked |
| ~~[saffron-dyn-parametric-extra-read-as-class-var](../archive/saffron-dyn-parametric-extra-read-as-class-var.md)~~ | low | **RESOLVED 2026-10-03** (archived) by fix direction 2: a bare EXTRA parameter of an instance on a parametric head is typed `any` in a dynamic dialect.  Original row: **Narrowed 2026-09-29**: a SPELLED `n : int` extra on a parametric-head instance is an `int` again (`saffron-dyn-spelled-int-extra`). Open: a BARE `n`, which the instance's own Prereq-4 rewrite retypes to the head (`n : Vec`) exactly as `Eq [Vec]`'s `y` -- the fix starts at that rewrite, not the witness |

## Found reducing the Saffron `: any`-result panic (filed 2026-09-28)

The "`: any` result" aside in saffron-dyn-witness-fn-arity-defaults-unary,
reduced. It is not Saffron-specific and not about fn parameters, and reducing
it turned up two neighbours. All three are compiled-only; `--interpret`
answers.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[erased-instance-body-tags-a-type-variable-widened-to-any](../archive/erased-instance-body-tags-a-type-variable-widened-to-any.md)~~ | medium-high | **RESOLVED 2026-09-29** (archived): an HKT `: any` result now attaches the M7 element bindings, and an `any` widen of a bound tyvar in an instance body is an `instance_changes` trigger, so the spec (where the widen tags correctly) is minted even when the ABI does not change. `instance-any-result-widens-bound-tyvar`, `saffron-instance-any-result-widens-element` |
| ~~[fn-param-call-prototype-spelled-from-the-call-not-the-fn](../archive/fn-param-call-prototype-spelled-from-the-call-not-the-fn.md)~~ | high | **RESOLVED 2026-09-29** (archived): both halves were elaboration, not the emitter. Result half: the non-HKT dispatch now binds the METHOD's own tyvars (`b := float`) so the instance spec resolves them; argument half: `elab_poly_call` widens a concrete argument where a typed carrier's parameter is `any`. `instance-fn-param-call-result-tyvar`, `instance-fn-param-call-any-args` |
| ~~[concrete-result-fn-passed-where-an-any-result-fn-is-expected](../archive/concrete-result-fn-passed-where-an-any-result-fn-is-expected.md)~~ | medium-high | **RESOLVED 2026-09-29** (archived): the `any` bridge marshals a function whose signature differs from a ground parameter only in `any` slots; for the generic spelling, a call through a LOCAL fn value no longer binds the enclosing definition's rigid tyvar from the expected return, and the `any`-widen spec trigger now covers plain generics (`B := int` changes no ABI). `fn-arg-any-result-bridge` |

## Found investigating the AOT-compiled REPL plan (filed 2026-09-29)

Found while checking the 2026-06-28 draft of
[aot-compiled-repl-plan](../upcoming/aot-compiled-repl-plan.md) against
`main`. The first two were resolved the next day; the first was that plan's
phase C0. The third was filed 2026-09-30 and resolved the same day.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[turi-inline-c-bool-return-tagged-as-int](../archive/turi-inline-c-bool-return-tagged-as-int.md)~~ | medium | **RESOLVED 2026-09-30** (archived): the consumer of `try_exec_simple_inline_c` re-tags a `TURI_INT` result as a bool when the declared result is `bool` (`fn->return_type`, falling back to the binding's full result type), so `println`, `type-of` and a `match` on `true`/`false` agree with the compiled binary under `--interpret` and at the REPL. Pinned by `tests/fixtures/inline-c-bool-return-prints`, allowlisted in `run-turi.sh`. Original row: a user inline-C body declared `: bool` came back as a bare `TURI_INT`, printing `1`/`0` and aborting a `match` |
| ~~[repl-continuation-counter-misreads-reader-syntax](../archive/repl-continuation-counter-misreads-reader-syntax.md)~~ | medium | **RESOLVED 2026-09-30** (archived): the REPL decides whether a multi-line form is finished with `reader_open_depth` (`src/compiler/reader.c`). It lexes the whole accumulated input the way the reader does, so ```` ```c ```` bodies, strings across lines, nested `#\| \|#` comments and `#\(` literals no longer hang the `..` prompt or split a form. A blank line inside a string or fence is kept as content, and end of input mid-form prints `(cancelled)`. Pinned by `tests/turi/repl-multiline-input.sh`. Original row: `paren_balance` counted brackets inside all of those, so a C `for (...;...;...)` swallowed piped input with exit 0 |
| ~~[duplicate-defn-in-one-file-reaches-the-c-compiler](../archive/duplicate-defn-in-one-file-reaches-the-c-compiler.md)~~ | low | **RESOLVED 2026-09-30** (archived): `elab_defn` records which form claimed a Pass-1 forward declaration (`Binding.defn_claim`). A second `defn` of the name from another form in the same file is now `defn: 'f' is already defined`, with a note at the first. Redefinition on a later REPL turn still works. Pinned by `tests/fixtures/errors/defn-redefine-same-file{,-result-type,-arity}`. Original row: `check` passed, `--interpret` ran the later body, and `build`/`jit` failed with `redefinition of 'f'` |
## Found landing reflected-measures-plan RF4 (filed 2026-09-30)

Unrelated to `^reflect` (reproduces with no experiment on); found writing the
RF4 fixture, whose measure originally returned a refined ADT.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[refined-adt-return-type-miscompiles](../archive/refined-adt-return-type-miscompiles.md)~~ | medium | **RESOLVED 2026-09-30** (archived): the `: (type-expr)` return path in `elab_defn` peeled the refinement but never captured the base ADT's def into `return_adt_def`, which the `: Lst` keyword and defalias paths both do. The signature therefore fell back to the `int64_t` carrier and the caller read the call as a constructor. It captures it now. Pinned by `tests/fixtures/refine-adt-return{,-no-contracts}`. Original row: any `defn` whose RESULT is `#refine{ r : <ADT> | ... }` failed to build |

## Found building crdt-spice-plan C4 and C5 (filed 2026-09-29)

Found implementing delta-state CRDTs and an RGA sequence in
`turmeric-spices/spices/crdt` (turmeric-spices#76). All three are worked
around in the spice, with the workaround and its reason recorded at each
site; none blocked the work. The first two are the reason C4's `DeltaCRDT`
carries a witness parameter and ships no generic helpers.

| Report | Severity | One line |
| --- | --- | --- |
| [associated-type-unusable-nullary-and-generic](associated-type-unusable-nullary-and-generic.md) | medium | **Narrowed 2026-10-03: half 2 is fixed** -- `(Inner A)` at a type variable is an unreduced projection that each call reduces through the instance (`assoc-type-projection-at-tyvar`, `assoc-type-projection-across-modules`); open: half 1, a nullary method returning only the associated type.  Two halves, both hard errors at `check`. (1) A nullary class method whose only mention of the class variable is through an associated type (`(empty [] : Inner)`) is declared and instanced fine but is unreachable: calling it is `unknown function or operator`, and an ascribed call is `ascribed type does not match the result shape`. Dispatch reads the first parameter, so there is nothing to resolve from; a fundep'd two-parameter class fails the same way because dispatch reads only the FIRST class variable. (2) `(Inner A)` projected at a type VARIABLE inside a generic constrained by that very class is `no instance binding for associated type` -- at a concrete type it reduces as documented -- so no generic function can take or return the projection. A third shape also blocks: a constrained generic in a module holding no instance of its own reports "this program declares no instance at all" even when instances exist elsewhere in the program. Workaround for (1) is a witness parameter the method does not read  **Narrowed 2026-10-03:** the third blocker (a constrained generic in a module holding no instance -- the spice layout -- reported TUR-E0015 "declares no instance at all") is fixed: an imported module's defn that fails only for want of an instance is parked and retried once the importer registers one (`tests/fixtures/typeclass-generic-in-instance-less-module`).  Half 1 and the projection at a type variable stay open |
| ~~[vec-push-byvalue-struct-param-emits-unbridged-pointer](../archive/vec-push-byvalue-struct-param-emits-unbridged-pointer.md)~~ | medium | **RESOLVED 2026-09-30** (archived): both escaping carrier-bridge arms in `emit_call` now dereference a pass-by-pointer struct PARAMETER before heap-promoting it, and skip the later pass-by-ptr deref. That covers the non-parametric seam-4 arm and the `expr_emits_byvalue_carrier_abi` arm, which a parametric `(Tri int)` parameter reaches. Pinned by `tests/fixtures/vec-push-byvalue-struct-param`, with the two-word control. Original row: `(defn push! [v : (Vec T) x : T] (vec-push! v x))` did not compile for any struct wider than two words |
| ~~[phantom-parametric-heap-let-binding-repr-ice](../archive/phantom-parametric-heap-let-binding-repr-ice.md)~~ | medium | **RESOLVED 2026-09-30** (archived): the trigger was a phantom parameter plus one concrete `:int` field, which is SC7's transparent-int-newtype shape. That collapse ignored `:heap`, while `repr_of` answered heap-ptr. `:heap` now opts out, which also keeps its reference semantics. Found alongside it: a genuine (non-`:heap`) transparent newtype let binding was freed as a pointer at scope exit, because its identity constructor counted as a fresh box. That is fixed too. Pinned by `tests/fixtures/phantom-heap-let-binding` and `transparent-int-newtype-let-no-free`. Original row: `(let [a (holder-put ...)] ...)` over a phantom-parametric `:heap` value ICE'd with `repr-shadow binding let-bind ... want=heap-ptr got=scalar-bits` |

## Found executing the security audit's WP2 (filed 2026-09-29)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[buf-puts-breaks-the-incidental-nul-invariant](../archive/buf-puts-breaks-the-incidental-nul-invariant.md)~~ | medium | **RESOLVED 2026-10-01** (archived): the preferred direction landed -- `buf_putc`/`buf_write` reserve the extra byte and store the NUL uncounted, so every Buf append keeps `data[len] == '\0'` and `buf.h` documents it; `buf_truncate` shortens without breaking it. `buf_put_quoted` is a plain `buf_puts` again, the exact swap that was the repro: `tests/spice-c-sources-tests.sh` passes 10/10 with it and fails on the old `buf.c` with the original `strlen` overread |

## Found executing security-audit-plan WP5 (filed 2026-09-30)

Found while probing the region store hooks for the security audit's runtime
memory-safety package; unrelated to the audit's own findings, which are
tracked in `docs/upcoming/security-audit-plan.md`.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[stm-inside-closure-captured-tvar-undeclared](../archive/stm-inside-closure-captured-tvar-undeclared.md)~~ | medium | **RESOLVED 2026-10-01** (archived): `collect_free_vars` dropped every capture under a node kind it had no arm for (`stm`/`atomically`/`tvar/*`, `select`, dynamic `binding`, `with-handler`), so the lambda lifted captureless; its `default:` now falls back to `cps_visit_children`. Same change: its fixed 256-entry stacks (a compiler heap overflow on a 300-form lambda body) grow, `(with-handler (handler ...) body)` elaborates to the `handle` it is, and the interpreter zeroes a handler value's `HandleExpr`. Pinned by `closure-captures-under-every-form` and `with-handler-literal-lowers-as-handle` |

## Found resolving the wasm-spices plan's open questions (filed 2026-09-30)

Found while answering the browser-integration open questions in
`docs/upcoming/hold/wasm-spices-plan.md` against a real Emscripten toolchain
(emcc 5.0.5-git, CMake 4.3.3). Latent behind raylib specifically, whose own
cmake floor sits exactly at CMake 4's cutoff.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[wasm-arm-suppresses-cmake-policy-min](../archive/wasm-arm-suppresses-cmake-policy-min.md)~~ | medium | **RESOLVED 2026-09-30** (archived 2026-10-01): a5edd6df dropped the `!wasm` conjunct, so `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` reaches `emcmake` too on CMake >= 4. The report was left behind when the fix landed |

## Found wiring the /ci spec into CI (filed 2026-10-01)

| Report | Severity | One line |
| --- | --- | --- |
| [docs-offline-cold-pane-never-boots](docs-offline-cold-pane-never-boots.md) | medium | `docs-offline.spec.js:145` (`docs browse offline on a cold pane`) fails on every run: with the origin stopped, `page.reload()` never reaches `window.turmericApp` and times out at 30s. The describe block is `mode: 'serial'`, so the two tests after it **did not run** -- three of the four offline-docs assertions are unexercised, not one. Verified on `main`'s own tip (run 36901587136, `8bb60d0`: 1 failed, 2 did not run, 128 passed) as well as on #1009, so it is not the PR's. Invisible because `Run broader smoke suite (desktop, non-blocking)` is `continue-on-error`: the step renders with a green check and only a `::warning` says otherwise -- the arrangement [try-turmeric-browser-suites-green-while-failing](../archive/try-turmeric-browser-suites-green-while-failing.md) describes, whose JUnit row does put an honest `status: fail` on `/ci`'s `web_desktop`. **Mechanism NOT established** -- filed from two CI runs; reproducing needs a production build (the spec skips without `dist/sw.js`). Leads in the report, starting with whether `sw.js`'s precache still covers the app shell after the Vite output moved to `dist/client/`. Either way the reporting wants fixing: a test that has failed every run for an unknown length of time makes the suite's failure count meaningless |

## Found adding a platform matrix to the turmeric-spices README (filed 2026-10-01)

Found as four red `turmeric-spices` CI jobs on a README-only PR, which made the
compiler the only candidate. Bisected to the day: the spices CI re-pins turmeric
`main` per run, and the last green run predates PR #1007.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[forward-call-to-aggregate-result-types-as-carrier](../archive/forward-call-to-aggregate-result-types-as-carrier.md)~~ | high | **RESOLVED 2026-10-02** (archived): the trigger was the caller sitting in a `defmodule` body, not the imported payload -- the module's pass-1 forward decl kept every non-scalar return as the `TY_INT` placeholder (the top-level pre-pass already resolved them), so `(Result int cstr)`, a same-module struct and a bare `: Box` failed the same way.  It now commits a bare registered ADT or a closed application, and a decl naming a type the module defines further down is retried once that type registers.  Pinned by `tests/fixtures/forward-call-aggregate-result-in-module` |
| ~~[forward-call-to-generic-callee-typed-as-placeholder](../archive/forward-call-to-generic-callee-typed-as-placeholder.md)~~ | medium | **RESOLVED 2026-10-02** (archived): a caller above a GENERIC callee, or above a defn with a function-typed parameter, got a forward decl too lossy to call through -- TUR-E0709, "expected int, got float", invalid C for a lambda argument, and a SIGSEGV for a lambda handed to a later generic HOF.  Pass 2 now elaborates such a caller after its callee (fix direction 2); a `def`/`definstance` that names a waiting defn elaborates it first, and stdlib forms take no part.  Pinned by `tests/fixtures/forward-call-generic-callee` and `-in-module` |
| ~~[mutually-recursive-generics-see-placeholder-result](../archive/mutually-recursive-generics-see-placeholder-result.md)~~ | low-medium | **RESOLVED 2026-10-02** (archived): a cycle of generics with an `(Option A)` result saw each other's forward decl and the call typed as the `int` placeholder ("then=(Option A) else=int").  A stuck cycle's members are now primed speculatively first -- `elab_defn`'s early signature update survives a rolled-back body -- so each sees the others' full signatures.  Pinned by the `ping-opt`/`pong-opt` cycle in `forward-call-generic-callee` and `-in-module` |

## Found executing the open loop-invariant reports (filed 2026-10-03)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[refine-float-field-selector-declared-int](../archive/refine-float-field-selector-declared-int.md)~~ | high (soundness) | **RESOLVED 2026-10-03** (archived, found by the new `shape_reflect` and fixed in one change): a record field selector was declared Int whatever the field's type, so a `float` field's constructor axiom or a written `(= (.a v) 0.5)` was a contradiction that proved every goal and elided its check.  `rt_resolve_fn` now gives `.f` its field's sort (Real where Int and Real fields share a name) |
| ~~[early-return-bypasses-return-refinement](../archive/early-return-bypasses-return-refinement.md)~~ | high (soundness) | **RESOLVED 2026-10-03** (archived, found and fixed in one change): an early `return` left a function with a refined return or `:post` unchecked -- statically "proved" from the last body form alone (check elided), and at runtime the C `return` jumped past the whole-body wrap.  Now the obligation is reported unknown when the body can `return`, and `elab_return` wraps each returned value in the enclosing function's checks (`Elab.ret_contract`; lambdas and default methods clear it).  `?` was not audited |

## Found cutting the v0.60.0 release (filed 2026-10-03)

Both found when `v0.60.0`'s `linux-aarch64` release leg failed and `Create
Release` was correctly skipped, leaving the tag with no release behind it. The
first is why that happened; the second is the mechanism underneath it, and bites
user inline C on its own.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[jit-xopen-source-guard-inert-on-glibc](../archive/jit-xopen-source-guard-inert-on-glibc.md)~~ | high (cost a release) | **RESOLVED 2026-10-03** (archived): the fork (turmeric-lang/mir#6, vendored at its merge commit `3c0d8c84`) declares the `__uint128_t` stand-in on Linux aarch64 and makes c2mir honour glibc's `aligned` attributes, without which `ucontext_t` would have parsed 16 bytes short of gcc's layout. Verified under qemu-user with the runner's arm64 glibc: a cross-built `tur jit` runs hello-world and the fiber corpus natively. The release workflow's aarch64 exception is deleted; the next release leg is the real-hardware check |
| [c2mir-rejects-uint128](c2mir-rejects-uint128.md) | medium | **Narrowed 2026-10-03:** the aarch64 system-header half is fixed (see jit-xopen-source-guard-inert-on-glibc). Open: user inline C doing ARITHMETIC on `__uint128_t` still falls back to cc on every target -- undeclared on x86-64, a layout-only struct on aarch64; MIR has no 128-bit integer type, so it needs lowering to 64-bit pairs |
| ~~[constrained-generic-monomorph-passbyptr-arg](../archive/constrained-generic-monomorph-passbyptr-arg.md)~~ | high | **RESOLVED 2026-10-03** (archived): a re-resolved instance call in a monomorph spills EVERY pass-by-ptr argument, not just the receiver (`emit_reresolved_param_is_by_ptr`); a spec clone no longer registers its by-value params as pbp; and a `let`-bound method result's merge temp takes the re-resolved instance's type, not the representative's.  `constrained-generic-passbyptr-args`.  Original row: A constrained generic over a pass-by-ptr aggregate does not compile. The monomorphized caller takes its own params by value and then passes one argument with `&` and the other without -- `__inst_Join_join_Reg(&__t177, q)` against `(const tur_adt_Reg *, const tur_adt_Reg *)` -- so the address-taking follows a per-argument spill rather than the callee convention. The specialized instance body also derefs a parameter it declared by value. Sibling of the dict-wrapper defect fixed in `tests/fixtures/dictwrap-passbyptr-param-convention`, which takes the direct-call route precisely to avoid this one; the fix direction is to give the monomorph path the same `type_struct_pass_by_ptr` predicate the impl, the slot typedef and the wrapper now share |

## Found investigating the three Sentry reports (filed 2026-10-03)

The nightly `Fuzz`/`TSan` jobs report findings to Sentry and nowhere else (see
the security guide's "Where a fuzz or TSan finding goes"). Three issues stood in
the `turmeric-ci` project; all three are accounted for:

- the `type` harness finding at seed 20261003 was already triaged by PR #1040 as
  [generator-thunk-call-site-returns-void-ptr-not-carrier](../archive/generator-thunk-call-site-returns-void-ptr-not-carrier.md)
  (confirmed still live with fnsan **armed** -- a run whose banner says
  `fnsan: UNAVAILABLE` proves nothing about it; since resolved and archived,
  rjungemann/turmeric#1049);
- one is a wiring smoke test, not a finding: `kind=connectivity-probe`,
  `target=wp7-setup-check`, fired 22 s after the `SENTRY_DSN` secret was set,
  and its payload names `json_decode_array` in `src/compiler/json.c` -- neither
  the function nor the file exists in the tree;
- the third is the row below.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[parse-shell-array-spins-on-zero-progress-value](../archive/parse-shell-array-spins-on-zero-progress-value.md)~~ | medium | **RESOLVED 2026-10-03** (archived, found and fixed in one change): `parse_shell_array` advanced its cursor only by what `parse_value` consumed and never checked that it consumed anything, so a `#`, `\n` or `\r` where a value is expected stalled `p` while each turn appended an empty string and doubled the array -- seen both as `out-of-memory (malloc(2147483648))` (2^28 entries, so ~268M turns: unbounded, not slow) and, in CI with less headroom, as `libFuzzer: timeout after 15 seconds`. A DoS on untrusted Justfile input, not memory-unsafe. **The identical defect had already been found by this same fuzzer in the dependency-argument loop and fixed there** -- the guard was never applied to this sibling, because the first fix was written as a fix to one loop rather than to the `parse_value`-returns-nothing contract. Now guarded, with the `realloc`-failure leak beside it fixed and CI's reproducer pinned as `tests/fuzz/seeds/fuzz_justfile/shell-array-comment-hang.bin`. Found 2026-10-01, Sentry `TURMERIC-CI-2`; the triggering line in isolation does **not** reproduce (the logical-line scanner's bracket-depth handling decides whether a `#`/newline lands inside the span), which is why the seed is the unreduced artifact |

## Found probing SICP code for the SRFI 216 plan (filed 2026-10-03)

Found running book-style SICP code and an SRFI 18 thread-start prototype
against `#lang r7rs`, for
[r7rs-srfi-18-216-sicp-plan](../archive/r7rs-srfi-18-216-sicp-plan.md).
Chapters 1-3.5 already run on both back ends. These are what a student will
hit next; the SICP guide's "Known rough edges" section points at each.

| Report | Severity | One line |
| --- | --- | --- |
| ~~[r7rs-saved-standard-procedure-follows-redefinition](../archive/r7rs-saved-standard-procedure-follows-redefinition.md)~~ | high (blocks SICP 4.1) | **RESOLVED 2026-10-04** (archived): a program that defines a standard procedure's name gets one variable that starts out holding the standard procedure, and its defines become `set!`s (`redefinitions_to_set`), so a value saved earlier keeps the standard one on both back ends. Pinned by `tests/fixtures/r7rs-program-redefinition` |
| ~~[r7rs-program-redefinition-refused](../archive/r7rs-program-redefinition-refused.md)~~ | high (every SICP chapter) | **RESOLVED 2026-10-04** (archived; found transcribing SICP 2.1 for sicp-corpus): a second top-level `define` of a program's own name was refused (`defn: ... already defined`); R7RS 5.3.1 makes it a `set!`, and now it is. Pinned by `tests/fixtures/r7rs-program-redefinition` |
| ~~[r7rs-internal-procedure-value-not-eq](../archive/r7rs-internal-procedure-value-not-eq.md)~~ | high (SICP 3.3.5) | **RESOLVED 2026-10-04** (archived; found transcribing SICP 3.3 for sicp-corpus): compiled, each reference to an internal procedure used as a value made a new closure, so `(eq? me me)` was `#f` and the constraint system failed; such a procedure is now bound once. Pinned by `tests/fixtures/r7rs-internal-procedure-identity` |
| ~~[r7rs-redefining-eval-with-scheme-eval-fails-to-compile](../archive/r7rs-redefining-eval-with-scheme-eval-fails-to-compile.md)~~ | medium | **RESOLVED 2026-10-04** (archived): on-demand imports are recorded before the clash and redefinition passes, so the program's `eval` is respelled and starts out as the standard one. Pinned by `tests/fixtures/r7rs-redefine-eval-with-scheme-eval` |
| ~~[r7rs-apply-variadic-over-eight-arguments](../archive/r7rs-apply-variadic-over-eight-arguments.md)~~ | medium | **RESOLVED 2026-10-04** (archived): past eight elements a variadic callee gets its fixed arguments spread and the rest of the list as its rest chain, on both back ends. Pinned by `tests/fixtures/r7rs-apply-long` |
| ~~[r7rs-deep-recursion-segfaults-silently](../archive/r7rs-deep-recursion-segfaults-silently.md)~~ | medium | **RESOLVED 2026-10-04** (archived): a compiled r7rs program's main runs on a 1 GiB-stack thread (a million-deep recursion prints its answer; twenty million on Linux), and an overflow past that prints `stack overflow: recursion too deep`.  Original row: Non-tail recursion 1,000,000 deep: compiled program exits 139 with no output (100,000 is fine; the interpreter does 1,000,000). No `sigaltstack` handler, so no "stack overflow" message |
| ~~[r7rs-program-file-named-with-leading-digit-fails-to-compile](../archive/r7rs-program-file-named-with-leading-digit-fails-to-compile.md)~~ | medium | **RESOLVED 2026-10-04** (archived): a program module named after a file whose stem starts with a digit is prefixed `r7rs-program-`. Pinned by `tests/fixtures/1-r7rs-program-leading-digit` |
| ~~[turmeric-module-cannot-call-a-scheme-procedure-value](../archive/turmeric-module-cannot-call-a-scheme-procedure-value.md)~~ | medium (expressiveness) | **RESOLVED 2026-10-05** (archived): a call through an `any` value is a run-time-checked dynamic call in every dialect, so a Turmeric module calls a Scheme procedure it takes as `[f : any]`. Pinned by `tests/fixtures/r7rs-turmeric-calls-scheme-procedure` and `tests/fixtures/any-value-call` |

## Found writing the debugging guides (filed 2026-10-04)

Both found running the commands `docs/guides/debugging-guide.md` documents,
against `./build/tur` v0.62.0 on macOS/arm64.

| Report | Severity | One line |
| --- | --- | --- |
| [panic-location-names-the-runtime-not-the-call-site](panic-location-names-the-runtime-not-the-call-site.md) | medium (UX) | **Narrowed 2026-10-08:** a failed injected contract (`:pre`, `:post`, a refinement, a loop invariant) panics at its predicate's own `.tur` line on both engines (`panic at boom.tur:2: Precondition failed in safe-div: (not= b 0)`), through `tur-contract-check-at`, and so do `assert!` / `require!` / `ensure!` at the macro use the program wrote. **Narrowed 2026-10-04:** a `(panic ...)` names its own `.tur` file and line on both back ends (`panic at boom.tur:3: ...`). **Narrowed 2026-10-07:** a failed contract names its function, the predicate's location and source (`Precondition failed in safe-div at boom.tur:2: (not= b 0)`). Still open: `--panic-trace` prints raw frame pointers, and a runtime-raised panic's `panic at` names the runtime.  Original: Every compiled panic says `panic at <tmp>/x_tur.c:1127` -- `__FILE__`/`__LINE__` expanded inside `tur_panic` itself (`emit_module.c:14423`), so it names the runtime, never the caller, even under `--debug`. The interpreter prints an empty `panic at`; a failed `:pre` says only `Precondition failed`; `--panic-trace` prints defer-frame pointers, not a stack. The debugging guide carries the workaround (lldb / `tur debug`) |
| ~~[panicking-if-branch-leaks-cc-uninitialized-warning](../archive/panicking-if-branch-leaks-cc-uninitialized-warning.md)~~ | low | **RESOLVED 2026-10-04** (archived): the diverging branch gives the merge temp its dead zero; `panic-in-value-if-branch-no-cc-warning` pins it.  Original row: An `if` with `panic` in one branch leaves the result temp unset on that path, so `cc` prints `-Wsometimes-uninitialized` on every `tur run`/`build`. `emit_fns.c:6564` already fixes the whole-body version; the branch-level case is uncovered |

## Found reviewing interpreter rot risk after the JIT default (filed 2026-10-04)

| Report | Severity | One line |
| --- | --- | --- |
| [turi-ios-readiness-unverified](turi-ios-readiness-unverified.md) | medium (platform readiness) | iOS can only run the tree-walking interpreter (no JIT, no runtime dylibs), and since `TUR_JIT` defaulted ON no CI leg builds turi without the JIT. Six areas to check: `TUR_JIT` defaults ON for an iOS configure, `ucontext` fibers, spice loading via `system()`+`dlopen`, FFI limited to the shape table, inline-C bodies (only C1/MIR runs them), `process/spawn` forks. Apple SDK claims unverified. The JIT default is fixed for iOS; the `interp-nojit` CI job (per PR) and `nightly-ios.yml` (libturi cross-compile) landed 2026-10-04 to answer the rest |

## Found executing stdlib-os-surface P0-P2 (filed 2026-10-04)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[ok-nil-unconstructible-in-turmeric](../archive/ok-nil-unconstructible-in-turmeric.md)~~ | low | **RESOLVED 2026-10-07** (archived): `(Ok nil)` (any constructor with a `nil` payload) passes the 0 word, keeping the argument's side effects, and a forward call to a `(Result nil E)` function no longer reads `nil` as an unknown name (TUR-E0012 at 0:0). Pinned by fixture `ok-nil-constructible` |
| ~~[inline-c-builders-cannot-nest-option-in-result](../archive/inline-c-builders-cannot-nest-option-in-result.md)~~ | low-medium | **RESOLVED 2026-10-07** (archived): composed builders (`tur_ok_int(tur_some_ptr(s))`) now read back correctly -- an inline-C box whose payload nests a sum is converted field by field and its inner boxes freed, on the match/let and argument paths. Pinned by leak-checked fixture `inline-c-result-nested-sum` |

## Module system (filed 2026-10-05)

Found while building the Trowel plugin system, which evaluates Turmeric source
in-process via `turi_eval`. Slash-qualified module member calls (`Foo/bar`)
work inside the defining module and inside modules that import `Foo`, but are
not resolved by the elaborator at the top level (outside any `defmodule`).

| Report | Severity | One line |
| --- | --- | --- |
| ~~[qualified-module-calls-unresolved-at-toplevel](../archive/qualified-module-calls-unresolved-at-toplevel.md)~~ | medium | **RESOLVED 2026-10-07** (archived): a top-level `(Foo/bar)` resolves against the module's members (longest module prefix first); a private member is a `not exported` error instead of a runtime dispatch that found it. Pinned by fixtures `module-qualified-call-at-toplevel` and `errors/module-qualified-private-at-toplevel` |

## Found executing the open reports, CPS coloring (filed 2026-10-07)

| Report | Severity | One line |
| --- | --- | --- |
| ~~[cps-coloring-resolves-a-parameter-to-a-same-named-global](../archive/cps-coloring-resolves-a-parameter-to-a-same-named-global.md)~~ | medium | **RESOLVED 2026-10-07** (archived, filed the same day): `cps_find_node`'s C-symbol fallback matched a parameter or local by its bare name, so `(f x)` through a parameter `f` was an edge to a global `f`. A pure global left the caller uncolored (an effectful lambda passed to it was refused), and it was most of why the r7rs library object varied: stdlib `__cons-fmap`'s callback is `f`. 117 fixtures hit it, 40 changed codegen |
| ~~[cps-effectful-fnval-escapes-through-unthreaded-param](../archive/cps-effectful-fnval-escapes-through-unthreaded-param.md)~~ | high | **RESOLVED 2026-10-07** (archived, filed the same day): a fn-value parameter given an effectful lambda in one call and a pure fn in another was never threaded, so the program compiled and aborted with `unhandled effect`. The `fn_cps` dispatch is no longer delegated (its pure fallback now calls `.fn` at its real result type), and an effectful value that a call would not thread -- an unthreaded parameter, or a fat closure with no `fn_cps` entry -- is withdrawn to the E2 taint (a compile-time refusal, never a run-time abort) |
| [cps-effectful-callback-through-multi-arg-or-untyped-param](cps-effectful-callback-through-multi-arg-or-untyped-param.md) | medium | **Narrowed 2026-10-08:** a callback of up to 8 arguments (word integers, `cstr`, `ptr<void>`), a capturing closure, and a `bool` / unit result now thread through the `fn_cps` slot (one shape predicate, `cps_ir_fncps_sig_ok`, for the fill and the analysis; a capturing closure gets an env-box registry dispatcher). Still refused: an untyped `^fat` parameter, and a `float` / narrow-integer / `bool` argument or a `float` result |
| [cps-effectful-closure-returned-through-empty-row-aborts](cps-effectful-closure-returned-through-empty-row-aborts.md) | medium | An effectful closure returned by a call at an empty-row fn type, `(app1 (adder 3) 1)`, compiles and aborts with `unhandled effect` (pre-existing): the threading analysis follows only a named fn, a lambda literal or a let temp of one into a parameter, so the returned lambda is never registered and its fat value carries no `fn_cps` slot |
| ~~[cps-let-alias-of-effectful-fn-param-refused](../archive/cps-let-alias-of-effectful-fn-param-refused.md)~~ | low | **RESOLVED 2026-10-08** (archived): an immutable `let` alias of a fn parameter whose every use is a saturated call is inlined by the CPS translator (the zero-capture case of `PapInline`), and the threading classifier counts a call through it as a call through the parameter -- one predicate, `cps_ir_let_fnparam_alias`, for both. Pinned by `tests/fixtures/cps-let-alias-of-effectful-fn-param` |

## Filing conventions

- One defect per file. If you find yourself writing a second report against a
  fixture that already has one, check whether you are describing the same root
  cause from a different symptom -- that is how the three interpreter reports
  above became one.
- When absorbing a report into another, **archive** the absorbed file with a
  header saying the defect is still open and pointing at the successor, and
  carry forward anything it independently established. Correct its wrong turns
  explicitly so they are not re-derived; see the two archived turi reports for
  the shape.
- A report that is fully resolved moves to `docs/archive/`, never stays here
  with a RESOLVED status line.

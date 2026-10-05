# SRFI 18, SRFI 216 and a SICP corpus for `#lang r7rs`

Status: in progress -- T0 done 2026-10-03; T0b, T0a, T1, T2, T3 and T5's fixtures done 2026-10-04; every 2.1a report fixed 2026-10-04/05. sicp-corpus through 4.3 done 2026-10-05 (its two proper-tail-call reports fixed). Open: the rest of T4 (4.4 query system, 5.2, 5.5). Extends docs/archive/r7rs-srfi-plan.md
(the `SRFI_LIBS[]` table, one `stdlib/srfi/<N>.scm` per SRFI, one fixture per
SRFI); no new mechanism and no new `--enable` (D8 of that plan applies).

## 0. The ask

People will walk through SICP in the R7RS dialect. Support SRFI 216 ("SICP
Prerequisites (Portable)"), take SRFI 18 (threads) as its prerequisite, and
find ways to keep SICP's code working -- ideally a corpus of book snippets run
as tests.

## 1. What SRFI 216 asks for

Library `(srfi 216)`, exports:

| Name | Kind | Notes |
|---|---|---|
| `true`, `false` | constants | `#t`, `#f` |
| `nil` | constant | `'()` |
| `the-empty-stream` | constant | `'()` in the reference |
| `stream-null?` | procedure | `null?` |
| `cons-stream` | syntax | `(cons a (delay b))` |
| `runtime` | procedure | integer microseconds |
| `random` | procedure | exact in -> exact out; inexact in -> inexact out |
| `parallel-execute` | procedure | runs thunks concurrently, waits for all |
| `test-and-set!` | procedure | atomic on a one-element cell |

The reference implementation (MIT, github.com/scheme-requests-for-implementation/srfi-216)
imports `(srfi 18)` for the last two and `(srfi 27)` for `random`. It ships
`srfi-216-tests.scm`, written against `(srfi 78)` -- which we have -- so it
ports to `tests/r7rs/srfi/216/` under the conformance runner as-is.

Two reference-implementation bugs not to copy:

- `runtime` multiplies by `jiffies-per-second` where it must divide.
- `parallel-execute` buffers every thread's output into one string port and
  prints it after the join, so SICP's interleaving examples show no
  interleaving. Write to the real port.

## 2. Measured (2026-10-03, origin/main b87ca3553, Debug build)

### 2.1 Book-style SICP code already runs, on both back ends

A probe of ~200 lines of chapter 1-3 code (sqrt by Newton, count-change,
fast-expt with a bignum result, timed-prime-test, higher-order `sum`,
`fixed-point`, `deriv`, rationals, eight-queens with a user `filter`,
symbolic differentiation, a `make-table`-backed `put`/`get`, message-passing
accounts, `set-cdr!` queues, and 3.5 streams with a user `cons-stream`
macro, variadic `stream-map` via `apply`, the `fibs` and sieve streams) gave
the book's answers compiled (`tur run`) and byte-identical output under
`tur --interpret`. Top-level redefinition of `abs`, `sqrt`, `filter` and
`square` was accepted.

So the dialect is not the obstacle for chapters 1-3; the prerequisites are.
Today a reader has to type `(define nil '())` and friends before the first
exercise. Tail calls hold up: a 10,000,000-step self loop and 1,000,001-deep
`even?`/`odd?` mutual recursion run on both back ends.

### 2.1a Hazards found on the way (filed 2026-10-03)

Each has a report with a repro. The first blocks SICP 4.1 as printed.

| Report | Effect on a SICP reader |
|---|---|
| ~~[r7rs-saved-standard-procedure-follows-redefinition](../archive/r7rs-saved-standard-procedure-follows-redefinition.md)~~ | **Fixed 2026-10-04.** `(define apply-in-underlying-scheme apply)` then `(define (apply ...))`: compiled hung, interpreted `unbound variable: apply--user` |
| ~~[r7rs-redefining-eval-with-scheme-eval-fails-to-compile](../archive/r7rs-redefining-eval-with-scheme-eval-fails-to-compile.md)~~ | **Fixed 2026-10-04.** the evaluator's `eval` plus `(import (scheme eval))`: C compile error |
| ~~[r7rs-apply-variadic-over-eight-arguments](../archive/r7rs-apply-variadic-over-eight-arguments.md)~~ | **Fixed 2026-10-04.** `(apply + long-list)` panics |
| ~~[r7rs-deep-recursion-segfaults-silently](../archive/r7rs-deep-recursion-segfaults-silently.md)~~ | **Fixed 2026-10-04.** a 1e6-deep linear recursive process exits 139 with no output, compiled |
| ~~[turmeric-module-cannot-call-a-scheme-procedure-value](../archive/turmeric-module-cannot-call-a-scheme-procedure-value.md)~~ | **Fixed 2026-10-05.** not reader-facing; shapes D1 below |

The student-facing guide, [sicp-guide](../guides/sicp-guide.md), documents
the first four with workarounds (rename the evaluator's `eval`/`apply` to
`mc-eval`/`mc-apply`; use `accumulate`; use `--interpret`) and carries a
stopgap SICP prelude until `(srfi 216)` lands.

### 2.2 Threads: what exists

- **Compiled:** a Scheme closure runs on a real pthread. Probe: a Turmeric
  module starts a pthread whose entry calls a *named* Scheme export
  (`run-entry id`), which looks the thunk up in a Scheme-side table and calls
  it. Two thunks, a 100k-element allocation loop and a `set!` on a shared
  global, joined; correct 3/3 under `TUR_GC_TORTURE=50`. The table also keeps
  the thunk reachable for the collector, which a `calloc`'d block would not
  (the guide: libc memory is not scanned).
- **A Turmeric-syntax module cannot call a Scheme procedure value.** Both
  `(f)` on an `any` and on an untyped parameter are refused ("'f' is not a
  function or continuation"); only Scheme-semantics code (the prelude, a
  Scheme library) can. Hence the call-back-by-name shape above.
- **Interpreter:** turi has a cooperative fiber scheduler, futures, `async`,
  and mutex/chan/task-group natives, but runs no interpreted code on a second
  OS thread (its one `pthread_create` is the C `run-ring` benchmark). An
  inline-C body is refused outright. The precedent to follow is
  `stdlib/session.tur`'s `session-spawn`/`session-join`: a pthread compiled,
  and under turi a native override that calls `eval_spawn_fiber`
  (`src/turi/eval.c`, "turi-session-expansion S2"). One fixture source runs on
  both back ends.

SRFI 18 explicitly permits green threads, so fibers under turi are conforming.

## 3. Design

### D1 -- SRFI 18 is a `library` row; the thread start is the relay shape

`stdlib/srfi/18.scm` holds the Scheme half: thread records (thunk, name,
specific, end-result, end-exception, state), the registry that roots them,
and `thread-entry__`, the named export a new thread calls. A small Turmeric
module (`stdlib/r7rs/thread.tur`) holds the C half: create/join/yield/sleep
over pthreads, and mutexes and condition variables over pthread objects. The
Scheme side calls the Turmeric side through the `(turmeric ...)` seam; the
Turmeric side calls back only `thread-entry__`.

- **Interpreter:** native overrides for the Turmeric half, as session.tur
  has: start = `eval_spawn_fiber`, join = await, mutex lock on a held mutex
  and condvar wait = yield to the scheduler until runnable.
- **Uncaught exception in a thread:** stored; `thread-join!` raises
  `uncaught-exception` wrapping it, per SRFI 18.
- **Dynamic environment:** already per-thread (r7rs-threads-dynamic-env); a
  new thread starts with no handlers, which is what SRFI 18 asks.
- **Out of scope first cut:** `thread-terminate!` (refuse with a reason; a
  pthread cannot be killed safely), absolute-time timeouts beyond
  `thread-join!`/`mutex-lock!`/`thread-sleep!` (time objects are the SRFI's own
  record over `current-time`).
- **Registry access is locked.** The probe's alist was only safe because
  every `register!` happened on the main thread.

**As built (T1-T2, 2026-10-04): simpler than the above.** No registry, no
callback by name, no pthread mutex per SRFI mutex:

- `stdlib/r7rs/thread.tur` is not a Turmeric module but a prelude-shaped
  `#lang r7rs` file, like `(scheme time)`'s. Such a file can call a Scheme
  procedure value, so a new thread's first frame (`r7rs-thread-entry__`)
  calls its thunk directly. The SRFI table splices it in ahead of
  `18.scm` (`SRFI_HELPERS` in scheme_lower.c), and 18.scm calls its
  `r7rs-thread-...__` names, as other SRFI files call prelude internals.
- **Rooting without a registry.** The thunk is handed from the creator's
  stack to the child's: the creator waits (parked) until the child has
  copied it, so it is on a scanned stack at every instant. Threads are
  detached; a join waits on the thread record, not on `pthread_join`.
- **One monitor.** Every SRFI 18 mutex, condition variable and join is
  Scheme state in a record, changed only under one pthread mutex, and every
  wait is a timed or untimed wait on its one condition variable,
  re-checked on wake. A condition variable keeps a queue of waiter tokens,
  so `signal` wakes exactly one and `broadcast` all. Every blocking call is
  one r7gc.c already routes through a release point.
- **current-thread** is a parameter: the parameter bindings are per thread
  (and per fiber), and each new thread binds it first thing.
- **Abandonment is decided lazily**: a locked mutex whose owner has ended
  reads as abandoned (`mutex-state`, `mutex-lock!`).
- **Interpreter (T2):** natives for the inline-C layer of thread.tur (turi
  overrides only an inline-C defn by name): start is `eval_spawn_fiber`,
  the monitor is a no-op (fibers switch only at a wait), a wait/sleep/yield
  is `turi_sched_yield` (fiber.c), and every switch saves, clears and
  restores turi's process-global dynamic-environment slots, so each green
  thread has its own handlers and parameter bindings. A wait with nothing
  able to run anywhere reports a deadlock instead of spinning.

### D2 -- SRFI 216 is a `library` row over 18 and 27

`stdlib/srfi/216.scm`, written fresh (not the reference, see Section 1).
`random` is over `(srfi 27)`. **Measured at T0:** the pruner did not drop
27's top-level state, so every `(srfi 216)` import cost what importing
`(srfi 27)` does, whether or not the program calls `random`: +142 KB of
emitted C over a bare program (1.51 MB to 1.65 MB, +9%). **Fixed in T0b**
(Section 4): +1.8 KB unused, +40 KB with `random` called.
Binding `random` to `stdlib/random.tur` instead, as an earlier draft
suggested, is not a drop-in: its float functions return `:int` (a scaled
integer, `rand-float`, `random-next-float!`), and it has no bignum ranges,
which SICP's Fermat test (1.2.6) reaches with a large `n`. `test-and-set!` runs under SRFI 18's monitor since T3; until
then nothing else could run between its test and its set.

**`parallel-execute` before SRFI 18 (decided at T0).** Not an error: it
runs its thunks one after another, in argument order. That is a schedule a
concurrent run may produce, so every result is one SICP 3.4 allows, and the
section's code (serializers, mutexes over `test-and-set!`) runs. The cost is
that no interleaving ever shows; sicp-guide.md says so in its 3.4 entry. T3
replaces it with real threads. **Replaced at T3 (2026-10-04).**

**Conflicts (D5 of the SRFI plan):** `stream-null?` and `the-empty-stream`
also exist in `(srfi 41)` with different representations; importing both is
an error naming `(prefix (srfi 41) s:)`. `random` does not collide with
anything in `(scheme base)`.

**A program that defines an imported name is refused (corrected at T0).**
An earlier draft said a reader's own `(define nil '())` should shadow the
import. The SRFI machinery already decides otherwise for every SRFI
(`srfi_bind`, R7RS 5.2): the program is refused, and the message names
`(except (srfi 216) nil)` as the fix. T0 keeps that rule rather than make
216 a special case. It costs SICP little: the book defines none of 216's
names itself (its `stream-car`/`stream-cdr` are not in 216). The guide says
what to do.

### D3 -- a SICP corpus as a test suite

Source and license:

- The MIT Press code files (`mitpress.mit.edu/sites/default/files/sicp/code/`)
  refuse scripted fetches (403) and carry no stated license.
- sarabander's HTML edition (github.com/sarabander/sicp) is **CC BY-SA 4.0**.
  Adapted snippets carry ShareAlike.
- Racket's `#lang sicp` (sicp-lang/sicp) is LGPL-3.0; its exports are a
  useful checklist (`inc`, `dec`, `identity`, `amb` beyond 216; see D4) but
  not a test source.

**Decided 2026-10-03: the corpus lives in its own repo**, proposed as
`turmeric-lang/sicp-corpus`, licensed CC BY-SA 4.0 to match its source, so
no ShareAlike content enters this tree. It differs from the SMT-LIB
precedent (`turmeric-lang/smt-lib-benchmarks`, whose labelled subset
`tests/corpus/import-smtlib.py` copies in): nothing is imported here.
Instead the corpus repo carries its own runner and CI, which builds
Turmeric `main` and runs every program on both back ends, as
turmeric-spices' CI does (and with the same caveat: two runs hours apart
use different compilers, so a red run names the Turmeric sha it built).

Layout there: `<chapter>/<section>.scm`, each a plain `.scm` program that
imports `(srfi 216)` (and the D4 extras where the section needs them),
transcribes one section's code in the book's order, and prints the book's
stated results, with an `expected` file next to it. A program blocked on an
open Turmeric report carries an xfail marker naming the report, like
`expected.xfail` here, so it turns red the day the fix lands without it.
Exercises are excluded (user work, and solutions are not ours to ship).

Priorities, by what a reader hits and what stresses the implementation:

1. 1.1-1.3, 2.1-2.3, 3.1-3.3, 3.5 (cheap; the probe in 2.1 is most of it).
2. 4.1 metacircular evaluator: redefines `apply` and `eval` after saving
   `apply-in-underlying-scheme`. Fails today on both back ends (2.1a); the
   corpus test goes in with the fix, as printed in the book, not renamed.
3. 4.3 `amb` (call/cc-heavy), 4.4 query system (streams + tables), 5.2 the
   register-machine simulator, 5.5 the compiler: the large ones, and the best
   regression tests for call/cc, deep non-tail recursion and GC.
4. 3.4 concurrency: `parallel-execute`/`make-serializer` over 216. Output is
   nondeterministic; assert invariants (final balance in the allowed set),
   not exact output.

Not covered: 2.2.4's picture language (SRFI 216 defers it to SRFI 203).

### D4 -- the `#lang sicp` extras ship as a built-in library

**Decided 2026-10-03: ship them.** `inc`, `dec`, `identity` and `amb`, as
Racket's `#lang sicp` provides them, plus `amb-reset!`, which is ours:
Racket's `amb` has no way to drop the last search's choice points, and
exhausting a second search otherwise resumes the first. Course materials
written against Racket assume the four, which is the argument for shipping
them.

- **Available today** as a paste-in block in sicp-guide.md's "Extras",
  measured on both back ends: SICP 4.3.2's multiple-dwelling puzzle gives
  the book's answer, collecting all solutions by forced failure works, and
  exhaustion is `error: amb tree exhausted`, catchable by `guard`.
- **Not in `(srfi 216)`**, which is a finished spec; adding names to it would
  break programs that define them.
- **Name: `(sicp extras)`** (decided 2026-10-03). Only `(scheme ...)` and
  `(srfi N)` are table-driven built-ins today (`SCHEME_LIBS[]`,
  `SRFI_LIBS[]` in src/compiler/scheme_lower.c), so this needs a third
  head or a generalization of the SRFI table to a named-library row. The
  file is `stdlib/sicp/extras.scm`, spliced in inline mode like an SRFI
  (`amb` is `define-syntax`, which inline mode already handles). A
  `(turmeric sicp)` name was considered and rejected: the `(turmeric ...)`
  head means a Turmeric-language module, and `amb` is Scheme syntax.
- `amb`'s state is a top-level variable, so it is per program, not per
  thread; under T1 threads, two threads searching at once share it. Document
  that rather than fix it (Racket's is the same).

## 4. Stages

- **T0 -- SRFI 216 before threads. DONE 2026-10-03.** Row, file,
  `(features)` entry and guide row, all ten exports (`parallel-execute`
  sequential, see D2). The reference suite, rewritten into chibi's `test`
  vocabulary, under `tests/r7rs/srfi/216` (21 of 21 on both back ends,
  floor 21); `tests/fixtures/r7rs-srfi-216` runs book-style 1.2.6, 2.2.1, 3.4
  and 3.5 code; `errors/r7rs-srfi-216-41-conflict` pins the refusal. The
  guide's paste-in prelude became `(import (srfi 216))`.
- **T0b -- `(srfi 216)` stops paying for all of SRFI 27. DONE 2026-10-04.**
  Direction 1 below, in two parts. The pruner (src/passes/srfi_prune.c,
  `alloc_only`) now drops a `def` whose initializer is a call that only
  allocates: literals, lambdas, constructors, local `set!`, and direct calls
  to non-recursive `stdlib/srfi/` procedures of the same shape or to the
  prelude's copying constructors (`PURE_PRELUDE`). And SRFI 27 keeps its
  sources' state in a one-slot vector, so `random-integer`/`random-real`
  draw from the default state directly rather than through the record's
  closures (which reach `pseudo-randomize!` and the rest). Measured: an
  unused `(srfi 216)` import went from +142 KB of C to +1.8 KB (a record
  typedef and `(scheme time)`'s three procedures), and a program calling
  `random` from +147 KB to +40 KB. `tests/check-r7rs-srfi-prune.sh` pins
  both. SRFI 27's own suite: 36 of 36 on both back ends. Original text:
  Today every
  import adds 142 KB of emitted C (D2), whether or not the program calls
  `random`. **Done when:** a program that imports `(srfi 216)` and never
  calls `random` emits the same C, within noise, as one without the import,
  and a program that calls `random` adds only what `random-integer` and
  `random-real` reach. Pin both with a size check, as
  `tests/check-r7rs-srfi-prune.sh` does for the other SRFIs. Directions, in
  order of preference:
  1. Teach the SRFI pruner (src/passes/srfi_prune.c) to drop SRFI 27's
     top-level state (`default-random-source`) when nothing
     reached keeps it. This helps every `(srfi 27)` importer,
     not only 216. Start from what r7rs-srfi-plan S7 measured: 27 keeps
     2,076 lines "the pruner cannot drop".
  2. Failing that, a small generator of 216's own for `random` (fixnum and
     float ranges in inline C, with a bignum range built from fixnum draws
     in Scheme), so 216 no longer imports 27. Mind the inline-C costs
     r7rs-srfi-plan D3 lists: an interpreter twin, and the region-note and
     GC-rooting audits.
- **SRFI 18 (T1-T3) is part of this plan and required, not optional.**
  SICP 3.4 is about interleavings, and the sequential `parallel-execute`
  (D2) can never show one. The plan is not done until T3 has replaced it
  and the guide's "one after another" caveat is gone. SRFI 18 also stands
  on its own as an R7RS library people expect.
- **T1 -- SRFI 18, compiled. DONE 2026-10-04.** D1's two halves, as built
  (D1's last part). Fixture `r7rs-srfi-18`: threads, values, uncaught
  exceptions, join timeouts, owned / not-owned / abandoned / timed-out
  mutexes, a bounded buffer and a broadcast on condition variables, time
  objects, the refusals, and eight threads on one mutex allocating between
  turns (r7rs-threads-stress's shape through the SRFI API). It runs in
  `tests/run-r7rs-gc.sh` under a collection every 31st allocation
  (`threads-srfi18`); 30/30 plain runs and a `TUR_GC_TORTURE=1` run
  matched. An unused `(srfi 18)` import costs only its eight record
  types' declarations: thread.tur is pruned like an SRFI file, and
  `make-parameter` with no converter counts as allocation-only.
- **T2 -- SRFI 18 under turi. DONE 2026-10-04.** Native overrides on
  fibers (D1's last part); `requires.compiled` dropped, the fixture's
  output is identical on both back ends.
- **T3 -- 216's thread procedures over 18. DONE 2026-10-04.**
  `parallel-execute` starts a thread per thunk and joins them all;
  `test-and-set!` runs under SRFI 18's monitor and yields when the cell was
  already set (SICP's mutex spins on it, which would never let the holder
  run under green threads). Measured: 300 runs of 3.4.1's unserialized
  `(* x x)` / `(+ x 1)` pair gave 101 (226), 11 (73) and 100 (1). The
  `r7rs-srfi-216` fixture now checks 40 runs of each, unserialized and
  serialized, stay inside the book's sets. The guide's "one after
  another" caveat is gone.
- **T0a -- `(sicp extras)`** (D4): the library-head change and the file;
  a fixture with the guide's `amb` cases on both back ends. Independent of
  T0, and the guide's extras block becomes the import. **DONE 2026-10-04.**
  The SRFI table took a named row rather than a third table:
  `(sicp extras)` maps to `SICP_EXTRAS_NUM` (src/compiler/scheme_lower.c),
  `lib_label` names it in every message, it is never an `srfi-N` feature,
  and its definitions are spelled `sicpx--<name>`. `amb` sets its choice
  point through an exported-to-nobody `amb-set-fail!`, since a macro's
  expansion cannot `set!` a library's variable. Fixture
  `r7rs-sicp-extras` (both back ends): 4.3.2's multiple-dwelling puzzle,
  every Pythagorean triple to 15 by forced failure with exhaustion caught
  by `guard`, and a fresh search after `amb-reset!`.
- **T4 -- SICP corpus** in `turmeric-lang/sicp-corpus` (D3), in the
  priority order of D3: the repo, its runner and CI first, then sections.
  Each section that fails gets a report under this repo's `docs/reported/`
  and an xfail marker there.
  **Priority 1 and 4.1 written 2026-10-04** (sicp-corpus PR #1): 1.1-1.3,
  2.1-2.3 (no picture language), 3.1, 3.3, 3.4, 3.5, and 4.1.1-4.1.4 as
  printed (the evaluator's own `eval` and `apply`, its driver loop fed the
  book's session); `(corpus prelude)` gave way to `(import (srfi 216))`.
  Writing them found three defects, fixed here the same day:
  [r7rs-program-redefinition-refused](../archive/r7rs-program-redefinition-refused.md)
  (every section that refines a procedure),
  [r7rs-internal-procedure-value-not-eq](../archive/r7rs-internal-procedure-value-not-eq.md)
  (3.3.5's constraints), and the 2.1a `apply` report. Until they reach
  `main` the affected sections are xfail there. Open: 4.1.5-4.1.7, 4.2,
  4.3 (with `(sicp extras)`), 4.4, 5.2, 5.5.
- **T5 -- guides.** `docs/guides/sicp-guide.md` exists (landed with this
  plan) and is written for a student with casual Scheme, not for Turmeric
  users. Each stage keeps it true: T0 replaces its pasted prelude with
  `(import (srfi 216))`; T3 removes "3.4 will not run"; each 2.1a fix
  deletes its "Rough edges" entry (each report carries a Guide upkeep
  note). Pin its examples on both back ends in a fixture, as
  `tests/fixtures/docs-r7rs-guide-examples` does for r7rs-guide.md; today
  they were checked by hand (extracted verbatim and run, 2026-10-03).
  r7rs-guide.md gets SRFI table rows for 18 and 216 and a pointer to the SICP
  guide. **Mostly done 2026-10-04:** the guide's examples and stated
  results are pinned on both back ends in `docs-sicp-guide-examples` (and
  `docs-sicp-guide-no-import` for the no-import claim); r7rs-guide.md has
  the 18 and 216 rows, a `(sicp extras)` paragraph and SRFI 18 in its
  Threads paragraph, all pointing at the SICP guide. What stays open is the
  standing upkeep: each 2.1a fix deletes its "Rough edges" entry.

## 5. Open questions

1. ~~Is CC BY-SA test content acceptable in the repo?~~ **Decided
   2026-10-03:** a separate repo (D3).
2. ~~Should the `#lang sicp` extras ship?~~ **Decided 2026-10-03:** yes,
   as a built-in library (D4), named `(sicp extras)`.
3. ~~Does a program with no `(import ...)` get the standard procedures?~~
   **Measured 2026-10-03:** yes, on both back ends
   (`(define (square x) (* x x)) (display (square 5))` prints 25); the
   guide says so.
4. ~~The corpus repo~~ **Created 2026-10-03:**
   [turmeric-lang/sicp-corpus](https://github.com/turmeric-lang/sicp-corpus),
   public, CC BY-SA 4.0, with `run.sh`, CI, `(corpus prelude)` standing in
   for SRFI 216, and section 1.1. Adding 1.1 found
   [r7rs-program-file-named-with-leading-digit-fails-to-compile](../archive/r7rs-program-file-named-with-leading-digit-fails-to-compile.md) (fixed 2026-10-04),
   which is why its files are named `sec-<section>.scm`.
5. ~~Name for the extras library~~ **Decided 2026-10-03:** `(sicp extras)`.

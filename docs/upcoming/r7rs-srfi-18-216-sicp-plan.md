# SRFI 18, SRFI 216 and a SICP corpus for `#lang r7rs`

Status: proposed (2026-10-03). Extends docs/archive/r7rs-srfi-plan.md
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
| [r7rs-saved-standard-procedure-follows-redefinition](../reported/r7rs-saved-standard-procedure-follows-redefinition.md) | `(define apply-in-underlying-scheme apply)` then `(define (apply ...))`: compiled hangs, interpreted `unbound variable: apply--user` |
| [r7rs-redefining-eval-with-scheme-eval-fails-to-compile](../reported/r7rs-redefining-eval-with-scheme-eval-fails-to-compile.md) | the evaluator's `eval` plus `(import (scheme eval))`: C compile error |
| [r7rs-apply-variadic-over-eight-arguments](../reported/r7rs-apply-variadic-over-eight-arguments.md) | `(apply + long-list)` panics |
| [r7rs-deep-recursion-segfaults-silently](../reported/r7rs-deep-recursion-segfaults-silently.md) | a 1e6-deep linear recursive process exits 139 with no output, compiled |
| [turmeric-module-cannot-call-a-scheme-procedure-value](../reported/turmeric-module-cannot-call-a-scheme-procedure-value.md) | not reader-facing; shapes D1 below |

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

### D2 -- SRFI 216 is a `library` row over 18 and 27

`stdlib/srfi/216.scm`, written fresh (not the reference, see Section 1).
`random` over `(srfi 27)`: check whether the pruner drops enough of 27's
2,076 lines when only `random-integer`/`random-real` are reached; if not,
bind to `stdlib/random.tur` through the seam instead (216 asks for no
saveable state). `test-and-set!` locks one private mutex.

**Conflicts (D5 of the SRFI plan):** `stream-null?` and `the-empty-stream`
also exist in `(srfi 41)` with different representations; importing both is
an error naming `(prefix (srfi 41) s:)`. `random` does not collide with
anything in `(scheme base)`.

**User redefinition must still win.** Readers paste the book's own
`(define nil '())` and `(define (stream-car s) ...)` after importing 216;
pin that a program's definition shadows the import (R7RS calls redefining an
imported name an error, but the existing `r7rs-program-shadows-names`
behavior allows it -- keep it consistent).

### D3 -- a SICP corpus as a test suite

Source and license:

- The MIT Press code files (`mitpress.mit.edu/sites/default/files/sicp/code/`)
  refuse scripted fetches (403) and carry no stated license.
- sarabander's HTML edition (github.com/sarabander/sicp) is **CC BY-SA 4.0**.
  Adapted snippets would carry ShareAlike; keep them in their own directory
  with a `COPYING` note, as `tests/r7rs/CHIBI-COPYING` does, and decide
  (Section 5, question 1) whether that is acceptable in the repo.
- Racket's `#lang sicp` (sicp-lang/sicp) is LGPL-3.0; its exports are a
  useful checklist (`inc`, `dec`, `identity`, `amb` beyond 216) but not a test
  source.

Layout: `tests/r7rs/sicp/<section>.scm`, each a `#lang r7rs` program that
imports `(srfi 216)`, transcribes one section's code in the book's order,
and prints the book's stated results; an `expected` file next to it. Run by
a ctest target on both back ends (compiled, `--interpret`), like the
conformance runner. Exercises are excluded (user work, and solutions are not
ours to ship).

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

## 4. Stages

- **T0 -- SRFI 216 minus threads.** Row + file with every export except
  `parallel-execute`/`test-and-set!` (those raise "needs (srfi 18), stage
  T1"); fixture; the reference test suite's non-thread checks. Unblocks
  readers for chapters 1-3.5 immediately.
- **T1 -- SRFI 18, compiled.** D1's two halves; fixture
  `r7rs-srfi-18` (`requires.compiled` until T2); the stress cases from the
  existing r7rs-threads fixtures re-expressed through the SRFI API.
- **T2 -- SRFI 18 under turi.** Native overrides on fibers; drop the
  `requires.compiled`.
- **T3 -- 216's thread procedures** over 18; the reference suite in full.
- **T4 -- SICP corpus**, in the priority order of D3; each section that
  fails gets a report under `docs/reported/`.
- **T5 -- guides.** `docs/guides/sicp-guide.md` exists (landed with this
  plan) and is written for a student with casual Scheme, not for Turmeric
  users. Each stage keeps it true: T0 replaces its pasted prelude with
  `(import (srfi 216))`; T3 removes "3.4 will not run"; each 2.1a fix
  deletes its "Rough edges" entry (each report carries a Guide upkeep
  note). Pin its examples on both back ends in a fixture, as
  `tests/fixtures/docs-r7rs-guide-examples` does for r7rs-guide.md; today
  they were checked by hand (extracted verbatim and run, 2026-10-03).
  r7rs-guide.md gets SRFI table rows for 18 and 216 and a pointer to the SICP
  guide.

## 5. Open questions

1. Is CC BY-SA test content acceptable in the repo, or should the corpus be
   our own transcription limited to short excerpts, or live in a separate
   repo (as the SMT-LIB benchmarks do)?
2. Should `inc`/`dec`/`identity`/`amb` (Racket's `#lang sicp` extras) ship
   anywhere? They are not in 216; a `(turmeric sicp)` convenience library is
   one option.
3. Does a program with no `(import ...)` at all get `(scheme base)`
   implicitly? A reader pasting book code will not write imports; the guide
   should say what happens.

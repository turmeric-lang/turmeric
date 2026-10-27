# Interpreter delimited-control gaps: multishot / escaping / nested-handler resume

> **RESOLVED 2026-06-14** (turi-interpreter-delimited-control-plan). The
> tree-walking interpreter now runs *capturable* handles on the driver
> work-stack (a heap-owned `TuriWsCont` continuation captured between the
> `perform` and the matching `DK_PROMPT`), with the ucontext-fiber path kept as
> the fallback for black-boxed performs (`while`/`try`/`async`/native-HOF
> bodies). This fixes escaping `k` (heap-owned, survives the handle return) and
> nested resume (the captured prompt slice re-installs the enclosing handlers).
> All five fixtures now pass under `--interpret` and their `requires.tur-only`
> markers are removed; verified against `tests/run-turi.sh` (no regressions).
>
> Multishot: true multishot on both paths -- the interpreter clones the captured
> slice per resume; the compiled path snapshots/restores the suspended fiber
> stack per resume (a degeneracy that was fixed in the same effort). See
> [turi-effect-multishot-degenerate-resume.md](turi-effect-multishot-degenerate-resume.md).
>
> Deferred: capturing a continuation *through a native HOF callback* still
> errors cleanly (no crash); lifted later by
> [turi-cek-stackless-reentry-plan.md](../upcoming/v1/turi-cek-stackless-reentry-plan.md) (SR).

**One-line summary:** Under `tur --interpret`, the effect-handler/continuation
machinery (`src/turi/eval.c` fiber path, `:691+`) is one-shot and single-frame:
resuming a continuation more than once (`^multishot`), resuming one that has
escaped its `handle` block, or resuming *through* nested handlers all fail --
variously a `SIGABRT`, a heap-use-after-free, or an `unhandled effect` error.
The compiled path implements all three correctly.

**Severity:** Mixed, all real. Multishot resume and the escaping-continuation
case are **crashes** (SIGABRT / ASan heap-use-after-free) -- not silent
miscompiles, but hard failures. The nested case is a clean wrong-error
(`unhandled effect: B`). None is a fixture bug; each is a genuine
interpreter-side hole in delimited control.

## Affected fixtures (all carved `requires.tur-only`, reason `interp-continuation`)

### 1. Multishot resume (`^multishot k` resumed >1x) -- SIGABRT

`fh-multishot-value`, `multishot-copy-capture`, `multishot-handler`. Minimal
(`multishot-handler`):

```turmeric
(defeffect Ask [] :int)
(defn main [] : int
  (let [r (handle (perform (Ask))
            (Ask [] ^multishot k)
              (+ (resume k 10) (resume k 20)))]    ; resumes k twice
    (println r))                                   ; expected 20
  0)
```

Observed: `==N==WARNING: ASan is ignoring requested __asan_handle_no_return ...`
then `SIGABRT` (rc=134), empty stdout. Expected `20` (compiled).

Root cause: a continuation is backed by a single ucontext fiber that is consumed
on the first `resume`; the second `resume` re-enters a finished/freed fiber. A
correct multishot needs to **clone** the captured continuation (its fiber stack
+ saved state) per resume. This is the same machinery the
[turi-multishot-continuation-snapshot-miscompile.md](turi-multishot-continuation-snapshot-miscompile.md)
report needs (`tur_continuation_snapshot`).

### 2. Escaping continuation resumed after `handle` returns -- heap-use-after-free

`effect-capture-k`:

```turmeric
(defeffect Ask [] :int)
(defn compute [] : int (let [x (perform (Ask))] (* x 2)))
(let [^mut k-store 0]
  (let [first-result (handle (compute) (Ask [] k) (do (set! k-store k) 0))]
    (println first-result)                  ; 0
    (let [second-result (resume k-store 5)] ; resume AFTER handle returned
      (println second-result))))            ; expected 10
```

Observed: ASan `heap-use-after-free` (rc != 0). Expected `0` then `10`.

Root cause: `k` is stored and resumed *after* its `handle` block has returned, so
the continuation's captured fiber stack / `EFFECT_CONT` state has already been
torn down. Supporting this needs heap-owned continuation state whose lifetime is
the continuation value's, not the `handle` frame's.

### 3. Resume through nested handlers -- `unhandled effect: B`

`effect-handler-capture-nested`:

```turmeric
(defn run [x y z : int] : int
  (handle (handle (handle (+ (perform (A)) (+ (perform (B)) (perform (C))))
                    (C [] k) (resume k x))
            (B [] k) (resume k y))
    (A [] k) (resume k z)))                 ; (run 1 20 300) expected 321
```

Observed: `eval: unhandled effect: B`. Expected `321`.

Root cause: when the innermost handler resumes `k`, the continuation does not
re-establish the *outer* handler frames (B, A), so the next `perform (B)` finds
no handler. Correct delimited control must capture the continuation up to the
matching prompt while keeping the enclosing handlers installed on resume.

## Proposed fix direction

These are one feature, not three bugs: a continuation representation that (a) is
heap-owned and clonable (fixes 1), (b) outlives its `handle` frame (fixes 2), and
(c) preserves the enclosing handler stack across resume (fixes 3). This overlaps
the explicit-stack evaluator in
[docs/upcoming/v1/turi-eval-trampoline-plan.md](../upcoming/v1/turi-eval-trampoline-plan.md):
once evaluation is reified on a heap work-stack, a continuation is a heap value
that can be copied and re-entered, which is exactly what all three need. Until
then the fixtures are carved (they exercise compiled multishot/nested control and
still pass on the compiled path).

## Validation

- After a fix: each fixture prints its expected output under `--interpret`;
  remove the `requires.tur-only` markers and add to the `run-turi.sh` allowlist.
- `tests/run.sh` already exercises all five on the compiled path (still green).

Tracked for the flip in
[docs/archive/history/turi-interpret-flip-residual-plan.md](../archive/history/turi-interpret-flip-residual-plan.md)
(Bucket R4).

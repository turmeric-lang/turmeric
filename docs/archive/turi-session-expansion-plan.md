# Expanding session types under the interpreter (turi) -- Plan

> **Status:** Executed 2026-09-16 -- S0, S1, S2, S3, S3.5 (payload row) and S6
> landed in one change; S4 (tagged `session_op` on `InlineC`) and S5 (reclaim
> channels on close) were left as the hygiene items the plan says they are;
> the multi-party timed receive (S3.5's second finding) stays open in
> `docs/reported/`. Outcome: 57 of 58 session fixtures run under
> `run-turi.sh` (was 29 of 54), one source per protocol, no `ptr<void>` spawn
> anywhere, float payloads exact, and the interpreter's deadlock detection
> documented and pinned.
> **Last Updated:** 2026-09-16
> **Type:** Interpreter / runtime / test coverage
> **Builds on:** [turi-session-types-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-session-types-plan.md)
> (Slices A + A.5 + B + C + D, all landed and archived)

---

## The short answer

**Yes -- but the interesting work is not where the parity matrix points.**

[turi-parity-guide.md](../guides/turi-parity-guide.md) carries this row:

| Feature group | tur | turi | Notes |
| --- | --- | --- | --- |
| Sessions (`make-session`, `close`) | OK | partial | session builtins lower to inline-C the tree-walker cannot run (`session-close` carved) |

**That row is stale in every particular.** The carve it names was deleted when
Slice A landed; `tests/fixtures/session-close/` has carried no `requires.tur-only`
marker since. The session builtins do not fall through to the inline-C wall --
`eval_session_intercept` (`src/turi/eval.c`) routes all fifteen emitted
`tur_session_*` / `tur_router_*` templates to a cooperative fiber runtime.

To find out what "partial" actually costs today, 15 programs were written
covering every distinct shape in the session fixture suite and run under
`./build/tur interpret` at v0.48.0. **All 15 produced the compiled fixture's
expected output.** Recursive (`Rec`) protocols, `offer`/`choose`, projection
(`project G R`), `Session`/`project`/`Role` struct fields, delegation, effects +
sessions, STM coexistence, 2- and 3-role multi-party, the `stdlib/session.tur`
protocol templates (`rpc-call`, `echo-server-loop`), the REPL, `tur run --engine
interp`, and session-endpoint-over-session delegation (which has no fixture at
all). One divergence surfaced, in `recv-timeout` on the fiber arm only.

So the honest matrix row is `OK` with one named exception, and the expansion
work is four different things:

1. one genuine semantic divergence (S1),
2. a **compiled-side** defect that makes turi the more capable backend and forces
   every fixture to be written twice (S2),
3. a large test-coverage hole that is purely mechanical to close (S3),
4. a capability the compiled path structurally cannot offer (S6).

---

## What was measured

`ASAN_OPTIONS=detect_leaks=0 ./build/tur interpret <prog>`, v0.48.0, 2026-09-16.
Each program is the compiled fixture with its `pthread` peer re-spelled as
`async`/`await`; nothing else changed.

| Shape | Maps to fixture | Result |
| --- | --- | --- |
| Recursive `Rec` echo + `choose-left`/`choose-right` | `session-rec`, `session-choose-*` | `1 2 3` -- correct |
| Projection `(project G R)`, two roles | `session-project-basic` | `42` -- correct |
| Projection through a choice | `session-project-choice` | `99` -- correct |
| `(Session P)` as a defstruct field | `defstruct-field-session` | `42` -- correct |
| `(project G R)` as a defstruct field | `defstruct-field-session-project` | `42` -- correct |
| `(Role G R)` as a defstruct field | `defstruct-field-session-role` | `42` -- correct |
| Delegation (ownership transfer to a callee) | `session-delegation` | `42` -- correct |
| Sessions + algebraic effects + handler | `session-effects` | 3 lines -- correct |
| Session alongside an STM-shaped read | `session-stm` | `42` -- correct |
| Multi-party 2-role calc | `session-mp-calc` | `84` -- correct |
| Multi-party + effects | `session-mp-effects` | 3 lines -- correct |
| Multi-party delegation | `session-mp-delegated` | `99` -- correct |
| `stdlib/session.tur` `rpc-call` | (no fixture) | `42` -- correct |
| `stdlib/session.tur` `echo-server-loop` | (no fixture) | `7` -- correct |
| Session endpoint sent **over** a session | (no fixture) | `42` -- correct |

Also confirmed working: sessions at the `tur repl` prompt, and `tur run --engine
interp`. Both matter -- the parity guide's "keep every entry point in sync"
section exists because capabilities have shipped into one entry point and not the
others, and sessions have not.

Not measured: the WASM REPL. `eval.c` compiles into `libturi_wasm.a` so the
intercept is present, but the fiber scheduler's `swapcontext` under Emscripten
was not exercised. One browser probe settles it; until then the matrix should not
claim it either way.

### Where the coverage actually is

```
$ TUR_FORCE=1 TURI_FILTER='session' bash tests/run-turi.sh
turi fixture summary: 29 passed, 0 failed, 25 skipped of 54 discovered
  (of which 25 inline-c carve-outs -- TI7, never run under turi)
```

All 25 skips are the fixture's own hand-rolled `spawn`/`join` inline-C, not any
session op. Nine of the 25 are covered by a `-turi` twin. The other **16 run
correctly under the interpreter today and are tested by nobody**:

```
defstruct-field-session          session-effects           session-mp-effects
defstruct-field-session-project  session-choose-left       session-project-basic
defstruct-field-session-role     session-choose-right      session-project-choice
session-delegation               session-echo-rpc          session-rec
session-delegated-rpc            session-mp-calc           session-stm
                                 session-mp-delegated
```

---

## Phases

Each is independent; S0 and S3 need no compiler change at all.

### S0 -- Correct the parity matrix row (docs only)

Replace the stale row with what is true, and name the one real exception:

| Sessions (binary + multi-party) | OK | OK | full surface runs on a cooperative fiber rendezvous; `recv-timeout` inside a fiber ignores its deadline (S1); a peer written with `async` runs under turi but **deadlocks compiled** (S2) |

Add the same two caveats to
[session-types-guide.md](../guides/session-types-guide.md), which today shows a
`spawn` it never defines and says nothing about either backend's limits. Per the
["no archeology in guides"](https://github.com/turmeric-lang/turmeric/blob/main/CLAUDE.md)
convention, each caveat is one deletable paragraph linking its report by GitHub URL.

Cost: under an hour. Do this first regardless of what else happens -- the current
row actively misleads anyone asking the question this plan asks.

### S1 -- Fiber-context `recv-timeout` deadline

The one genuine session divergence. A 50ms `recv-timeout` inside an `async`
fiber returns the **Left (success)** branch on a value that arrives 800ms later;
the compiled `pthread_cond_timedwait` returns **Right (timeout)**. Main-context
`recv-timeout` is correct, which is exactly why no fixture catches it.

Filed as
[turi-fiber-recv-timeout-ignores-its-deadline](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-fiber-recv-timeout-ignores-its-deadline.md),
with the mechanism, the in-tree comment that predicted it, and the fix direction:
arm a `turi_timer_add` future alongside the channel's `recv_waiter` so either the
deposit or the deadline resumes the fiber. The timer wheel already exists -- it is
what `sleep-async` uses. The new wiring is a channel wait with two wake sources,
where today there is one `recv_waiter` slot.

**Do this before S3.** It is the only phase where a new fixture can currently go
red.

### S2 -- One peer-spawn that works on both paths

The blocker for everything downstream. `(async (fn [] (server ch)))` -- the
obvious way to write a peer -- **hangs the compiled binary forever with no
diagnostic**, and runs correctly under turi. Filed as
[compiled-async-fiber-deadlocks-on-a-session-op](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/compiled-async-fiber-deadlocks-on-a-session-op.md).

That is why 20+ fixtures and the guide's examples all carry:

```turmeric
(defn spawn [f : ptr<void>] : ptr<void>
  ```c
  pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t));
  pthread_create(tid, NULL, tur_session_thread_wrapper, f);
  return (void *)tid;
  ```)
```

-- untyped `ptr<void>` in and out, exactly the stand-in
[CLAUDE.md](../../CLAUDE.md) forbids, copy-pasted into the user-facing guide.

Three fix directions, in the report. Recommended here: **direction 2, a typed
`session-spawn` / `session-join` in `stdlib/session.tur`** -- `(fn [] nil)` in, a
`defopaque SessionPeer` out -- backed by `pthread_create` compiled and by the
fiber scheduler under turi via a native override (`wk_register_*_natives`, which
is precisely the escape hatch the parity guide documents for stdlib inline-C).
It costs one stdlib module plus one native registration, deletes 20+ inline-C
blocks, fixes the guide's examples, and makes **one fixture source run on both
suites**. It does not fix the hang for a user who writes `async` directly, which
is why the report also asks for direction 3 (diagnose the shape) regardless.

### S3 -- Close the 16-fixture coverage hole

Purely mechanical once S2 lands: each of the 16 fixtures above gets its
`spawn`/`join` replaced by the S2 pair, the `-turi` twin is deleted, and one
source runs under both `run.sh` and `run-turi.sh`. Net effect: 54 session
fixtures become ~44, and interpreter coverage goes from 29/54 to ~44/44.

Without S2 it is still worth doing the cheap half -- add 16 `-turi` variants
using `async`/`await`. That is strictly more coverage than today and costs
nothing but files. It is the worse shape long-term (two files per protocol,
drifting), so prefer it only if S2 slips.

Two fixtures also want fixing rather than duplicating:

- `session-timeout-expired-turi` currently passes **vacuously** -- its peer uses
  `(await (sleep-async 200))`, which per
  [awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body.md)
  never reaches its `send` at all. It prints `timeout` because nothing is ever
  deposited, so it would pass with `recv-timeout` stubbed to always time out.
  Drop the `await` and it becomes a real test.
- `stdlib/session.tur`'s templates (`rpc-call`, `echo-server-loop`,
  `echo-client-call`, `pubsub-recv-loop`) have **no fixture on either path**.
  Two of the four were verified by hand here; `echo-client-call` does not compose
  with `choose-right` at all (its inferred recv-pair return decays, the KB-029
  note in that file), which is a pre-existing surface gap worth its own look.

### S3.5 -- Feature-surface gaps (separate pass, same audit)

A follow-on pass looked at the session *feature* surface rather than its
interpreter parity, and found two things that are not turi-specific at all. They
are sequenced here because S3's new fixtures are where they would be pinned.

- **Payloads are int64-only** --
  [session-payloads-are-int64-only](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/session-payloads-are-int64-only.md),
  **severity high and the most consequential finding of either pass.** A `float`
  payload is silently truncated on the compiled path (`7.25` -> `7`, no
  diagnostic); `cstr` and delegated endpoints fail to build on macOS; by-value
  structs fail everywhere. All four type-check and all four are correct under
  `--interpret`. It is why every fixture and every guide example sends `int`, and
  it means **delegation -- a headline session-type feature -- does not build on
  macOS**. Fix the float row first (it is the only silent one); the floor is a
  diagnostic instead of a wrong number.
- **No multi-party timed receive** --
  [multi-party-sessions-have-no-timed-receive](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/multi-party-sessions-have-no-timed-receive.md).
  Binary has `recv-timeout`, multi-party has nothing. Sequence it after S1: a
  fiber-context timed receive is broken for binary sessions today, and
  multi-party would inherit the same hole.

Two things this pass checked and found **working**, recorded so they are not
re-investigated: multi-party recursion (`loop`/`continue`, guarded by a `choice`)
projects and runs correctly end-to-end, and multi-party `choice` projects
correctly. The `session-project-loop` fixture only type-checks them -- its `main`
prints `"ok"` and never runs the protocol -- so S3 should give that one a real
body.

### S4 -- Harden the intercept (optional)

`eval_session_intercept` dispatches by `memcmp` against the **emitted C text**:
`SESS_PFX("({ tur_session_send(")`, plus `session_int_after` to scrape the role
index and branch tag back out of the template string. It is complete and it works,
but it re-parses information the elaborator had in hand and threw away. Editing a
template's spacing in `elab_sessions.c` or `elab_forms.c` silently un-routes the
op, and the failure lands as an `inline-C not supported` error far from the edit.

The fix is a small tagged field on `InlineC` (`session_op` enum + the already-known
role/tag operands) set where the template is built and read by the intercept, so
both elaborators and the interpreter agree by construction. Not urgent -- the
`-turi` fixtures do catch a drift -- but it is the difference between a loud
failure and a puzzling one, and it gets cheaper the sooner it happens.

### S5 -- Reclaim channels on close (small)

`TuriChan` / `TuriRouter` / `TuriRole` are bump-allocated from the env value pool
and freed only at env teardown. `session_close` already decrements `refcount` to
zero and then does nothing with it. Measured cost: 20,000 sequential sessions add
~9 MB over the async-only baseline (98 MB vs 89 MB vs 54 MB with no async at all)
-- so ~460 bytes per session, bounded by nothing. Irrelevant for a fixture,
relevant for a long-running `tur repl` or an embedded `turi_env` serving requests.
The refcount that would drive a free path is already maintained.

### S6 -- The capability worth having: turi as a session deadlock oracle

This is the one that is genuinely new rather than catch-up.

Because the interpreter's rendezvous is cooperative and single-threaded, it can
**see** that no participant can make progress. The compiled pthread runtime
structurally cannot -- it has no global view, so it hangs. Measured:

```
;; main recvs; nothing ever sends
$ ./build/tur interpret t-dl1.tur
tur: eval: session recv deadlocked (no sender)          exit 1

;; two fibers each blocked on the other's channel
$ ./build/tur interpret t-dl2.tur
tur: eval: session recv deadlocked (no sender)          exit 1
```

Both detected in milliseconds, both clean errors with a nonzero exit. The
compiled equivalents run until killed.

Session types already give static protocol conformance -- the checker rejects a
`send` where the protocol says `recv`. What they do **not** give is deadlock
freedom across independently-typed channels, which is exactly the residue
`session_park_or_spin` is positioned to catch. `tur --interpret` is therefore a
protocol-level deadlock check you can run before shipping, on the real program,
with no model to write.

That is a documentable feature and it is nearly free -- the detection already
exists and fires correctly. What it needs:

1. **Say so.** A section in `session-types-guide.md`: run your protocol under
   `--interpret` first; a `deadlocked` error is a real bug in your protocol, not
   an interpreter limitation.
2. **Better messages.** Today: `session recv deadlocked (no sender)`. It could
   name the channel's debug protocol tag (`TUR_DBGPROTO`, already threaded
   through `tur_session_new` and currently discarded by the intercept) and which
   participants are parked.
3. **Fixtures** pinning both the no-peer and the mutual-block cases, so the
   detection cannot silently regress into a hang.
4. **Soundness note.** No false positives were found. Two apparent ones during
   this investigation both traced to the `await (sleep-async ...)` defect
   (S3/report 3) -- the peer genuinely never sent. A correct program whose peer
   sleeps before sending is handled fine: `turi_sched_step` returns true while
   any timer is pending, so the main context waits it out. Verified both with the
   receiver in the main context and with both peers as fibers. Anyone revisiting
   this should not go hunting for a false-positive deadlock; there isn't one.

Item 2's prerequisite is worth calling out: `TUR_DBGPROTO("...")` is already in
the emitted `tur_session_new` template and the intercept currently ignores it.
Reading it costs one `session_int_after`-shaped scan (or comes free with S4).

---

## Recommended stopping point

**S0 + S1** is the minimum that makes the documentation true and removes the one
wrong answer. Half a day, no new surface.

**S0 + S1 + S2 + S3** is the version worth doing: it deletes the `ptr<void>`
spawn stand-in from the stdlib, the fixtures, and the guide; halves the session
fixture count; and takes interpreter coverage from 29/54 to ~44/44. S2 is the
only phase with real design in it.

**S3.5's payload row outranks all of it on severity.** It is the only finding in
either pass that is a silent wrong answer on the default path, and it is
independent of every other phase -- do not let the turi sequencing above delay
it.

**S6** is the one to do if the goal is to *expand* session types rather than
finish porting them -- it is a capability the compiled backend cannot have, it
already works, and it is mostly a writing job.

S4 and S5 are hygiene. Do them when something else is already open in
`eval.c`; neither justifies its own change.

---

## Validation / definition of done

- Per phase: its fixtures pass under `bash tests/run-turi.sh` (12-minute timeout,
  per [CLAUDE.md](../../CLAUDE.md)).
- `bash tests/run.sh` unchanged for S0/S1/S4/S5/S6; S2/S3 move fixture sources,
  so expect fixture churn and regenerate snapshots in the same change.
- `python3 tools/check_turi_parity.py` stays at `0 gaps` (it is at
  `118/119 handled, 1 carved out, 0 gaps` today -- sessions have not contributed
  a carve since Slice A).
- S1: the fiber-context `recv-timeout` fixture prints `timeout`, not the value.
- S2: one fixture source passes under both `run.sh` and `run-turi.sh` with no
  `requires.*` marker.
- S6: the two deadlock fixtures fail the suite if detection regresses to a hang
  (a fixture's 10s run timeout makes a regression a FAIL, not a stall).

---

## See also

- [turi-session-types-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-session-types-plan.md)
  -- Slices A-D, the runtime this builds on (archived, complete).
- [turi-parity-guide.md](../guides/turi-parity-guide.md) -- the matrix S0 corrects.
- [session-types-guide.md](../guides/session-types-guide.md) -- the user guide
  S0/S2/S6 amend.
- `src/turi/eval.c` -- `TuriChan`, `session_send_on`/`session_recv_on`,
  `session_recv_timeout`, `session_park_or_spin`, `eval_session_intercept`,
  `TuriRouter`/`TuriRole`.
- `src/compiler/elab_sessions.c`, `src/compiler/elab_forms.c` -- the two places
  the intercepted templates are built.
- The three reports this plan cites:
  [turi-fiber-recv-timeout-ignores-its-deadline](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-fiber-recv-timeout-ignores-its-deadline.md),
  [compiled-async-fiber-deadlocks-on-a-session-op](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/compiled-async-fiber-deadlocks-on-a-session-op.md),
  [awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body.md).

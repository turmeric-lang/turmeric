# Workarounds to remove once their blockers land

**Status: CLOSED 2026-09-16 -- every row is struck.** Row 5, the last live one,
was swept in
[turmeric-spices#74](https://github.com/turmeric-lang/turmeric-spices/pull/74):
`spices/ws-server` holds the broadcast hub mutex as a `Mutex` again, in
`fixtures/broadcast/server.tur` and `tests/broadcast_test.tur` both, and the six
`(:: ... Mutex)` casts are gone. That also retired a live "No Lazy `:int`
Stand-Ins" violation -- `server.tur` was still carrying its mutex as `:int`.

Verified here against a freshly built `tur` v0.49.1 rather than taken from the
merge commit:

- `tur check` is clean on both files with the direct spelling.
- The emitted C declares AND assigns the global the old bug dropped:
  `static void * hub_hymutex_2108;` ... `hub_hymutex_2108 = __ps_537;`. The
  symptom was `'hub_hymutex_1866' undeclared`, so this is the proof the row
  asked for.
- `tests/fixtures/module-level-def-of-opaque-value`, the fixture row 5 named,
  builds and prints `42` / `7`.

The ws-server *suite* was not run here: all six of its tests need mbedtls
headers, which this host does not have (`fatal error: 'mbedtls/ssl.h' file not
found` at the cc step, identically on every test including ones that never touch
the mutex). That is a host gap, not a regression -- the PR reports
`6 tests, 6 passed, 0 failed` with broadcast_test among them.

**A closed checklist is not a standing invitation.** This file recorded the
workarounds live in the tree as of 2026-08; the next deliberate second-best
thing gets its own report under `docs/reported/`, not a row appended here.

---

**Severity:** none on its own. This is a checklist, not a defect -- but each row
is a place the tree is deliberately doing the second-best thing, and second-best
things outlive their reasons unless someone writes down what the reason was.

**Originally:** OPEN by construction, to be closed when the last row was
struck. It is.

Each row: what is in the tree, why, what to do when the blocker clears, and how
to prove the workaround is no longer needed.

## 1. `StThunk` exists only because a `:fn` field crashes -- REMOVED 2026-09-05

**Struck.** `stdlib/logic.tur` declares the field with its real type now --
`(StInc (fn [] Stream))` -- and the four `(:: ... :StThunk)` wrappers are gone.

The row's recorded blocker was wrong, and so was the one before it. Neither the
original crash (fixed and archived) nor the 2026-09-02 probe's reading ("every
construction site fails TUR-E0295") was what kept it alive: the change fails at
exactly ONE site, `st-bind`'s `(:: (f v) :Stream)`, because `f : fn` is an
untyped fn parameter two functions away. Typing it, and `mbind` with it, removes
the ascription and the error. A compiler fix was then needed for the `(let [lf f]
...)` alias (a fn-typed param is a fat handle, not a thin function pointer).

Full paper trail, including why the recorded blocker was a hypothesis nobody had
tested by removing the workaround:
[workaround-stthunk-opaque-carrier](workaround-stthunk-opaque-carrier.md).

## 2. `weak-upgrade-after-drop` carries a `known-leak` marker -- REMOVED

**Struck 2026-09-05, already true before that.** The blocker
(`inline-c-option-carrier-box-leaks`) is archived, the `known-leak` marker file
is gone from the tree, and `bash tests/run-leak-check.sh` reports
`81 passed, 0 failed, 0 known-open` -- which is the proof this row asked for.
Nobody struck the row when the work landed, which is the failure mode a
checklist is supposed to prevent.

## 3. The sanitizer gate is not armed -- REMOVED 2026-08-26

The blocker ([sanitizer-gate-not-armed-in-ci](sanitizer-gate-not-armed-in-ci.md))
is cleared and this entry's own instruction was carried out: `ci.yml`'s `test`
job -- the only job that runs `tur_tests` -- sets `TUR_SANITIZER_GATE: "1"` at
job level. The macOS half of the missing measurement came back zero (2703
fixtures, Apple clang 21, arm64), matching Linux, and was confirmed with a
positive control rather than inferred from silence.

The follow-on this entry floated -- flipping the *default* to armed and giving
the opt-out an env var -- was deliberately **not** taken. The default governs
every local run and every downstream harness that shells into `run.sh`, not
just CI, and the two toolchains measured here are not the whole population: the
first contributor on an unmeasured compiler would meet a red suite they did not
cause. CI is where the invariant is worth enforcing, and CI now enforces it.
Revisit if the local default ever starts costing more than it saves.

**Proof:** ran as specified -- `TUR_SANITIZER_GATE=1 bash tests/run.sh` exits 0
on macOS/arm64 (`2703 passed, 0 failed`, no `SANITIZER:` block), and a planted
overflow makes it exit 1 with the finding attributed to its fixture and phase.

## 4. `with-untrailed` callers bind a result they do not want -- REMOVED 2026-08-26

The blocker
([poly-call-in-statement-position-dropped](poly-call-in-statement-position-dropped.md))
is fixed, and both sites were reverted per this entry's own instructions: the
HAZARD block is deleted, `with-untrailed`'s docstring example is back in the
natural discarded form, and `sx2-trail-combinators` discards the result --
pinning the spelling users actually write, and going red if the discard ever
regresses. Proof ran as specified: the discarded-form write happens (the
fixture prints 55, not 0), and `poly-statement-position-effect` is green.

## 5. `ws-server` casts its hub mutex through `:int` -- REMOVED 2026-09-16

**Struck.** Swept in
[turmeric-spices#74](https://github.com/turmeric-lang/turmeric-spices/pull/74) --
the direct spelling is restored in both files and the six casts are gone. Two
blockers had to clear, not the one recorded below: the second,
`global-def-store-misses-int-ptr-bridge`, was found 2026-09-11 when the first
fix alone still failed the macOS leg, and the interim workaround there was to
ascribe `:ptr<void>` instead of `:int` -- better-typed, but still a cast.
Neither carrier is needed now. See the CLOSED note at the top of this file for
what was verified.

**Where:** `turmeric-spices`, not this tree -- `spices/ws-server`
(`broadcast_test.tur` and `fixtures/broadcast/server.tur`):

```turmeric
(def hub-mutex (:: (mutex-new) :int))
(defn hub-lock!   [] : nil (mutex-lock   (:: hub-mutex Mutex)))
(defn hub-unlock! [] : nil (mutex-unlock (:: hub-mutex Mutex)))
```

**Why:** `(def hub-mutex (mutex-new))` emitted no global at all while the use
sites still referenced it -- `'hub_hymutex_1866' undeclared`. Holding the
carrier and casting back at each borrow sidestepped it.

**What it costs:** every borrow is an unchecked `:int`-to-`Mutex` cast, so the
type checker stops helping at exactly the point where a mutex most needs it.

**Blocker:** [module-level-def-with-linear-init-emits-no-global](module-level-def-with-linear-init-emits-no-global.md)
-- **RESOLVED 2026-08-29**. The cause was not linearity (that framing was a red
herring; a non-linear `defopaque` global failed the same way) but
`def_is_opaque_type_decl` matching any `def` whose value merely had an opaque
type.

**When it lands:** it has, and it was carried out -- `(def hub-mutex
(mutex-new))` with `(mutex-lock hub-mutex)` at each borrow, and the six `::`
casts deleted.

**Proof it is no longer needed:** `tests/fixtures/module-level-def-of-opaque-value`
in this tree pins both flavours; against a `tur` carrying that fix, the direct
spelling builds and runs.

## Not on this list, and why

**`disjoined-dfs` is not a workaround.** It is a second search strategy with
its own semantics -- depth-first, incomplete, faster on goals whose left branch
terminates -- and it stays whatever happens to anything above. `disjoined`
(interleaving, complete) remains the default.

**The `TUR_SR1_SUM_BYVALUE` and `TUR_ADT_SLAB` seams are not workarounds
either.** They are measurement instruments that are deliberately off; the slab
is [shelved](multi-variant-adts-always-heap-allocate.md) rather than pending.

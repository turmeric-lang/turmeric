# Proper tail calls in Turmeric

Status: **complete** -- T1-T6 and the optional T2b (`musttail`) landed
2026-09-21 to 2026-09-23 and shipped in 0.52.0; Section 6's carve-outs stay
open by decision. One follow-on was never staged and was not done: T-D4's
audit after `match` (`handle` arms, `and`/`or`, `tco_let_simple`'s
carrier-ABI bail). It now lives in
[tail-grammar-skips-and-or-and-carrier-lets](tail-grammar-skips-and-or-and-carrier-lets.md):
`handle` bodies CPS-lower, so they are out of this emitter's reach by
construction; the last operand of `and`/`or` and a self tail call under a `let` that
binds a carrier-ABI value were refused when probed 2026-09-28, and both are
fixed (2026-09-29 and 2026-10-03; that report is archived). The CPS
backend's cross-function tail call is filed separately
([cps-self-tail-call-relies-on-sibling-call](../reported/cps-self-tail-call-relies-on-sibling-call.md),
owned by r7rs-lang-plan), and `#lang r7rs`, which the text below says does
not exist yet, has since landed on T6. Archived 2026-09-28.

- **T1 -- `^tailcall` + TUR-E0716 + the `-O0` fixture harness: DONE.** The
  annotation is a checked assertion, not a hint: a call it cannot place in tail
  position is a compile-time error naming the reason. See
  [T-D1](#t-d1----tail-position-becomes-a-thing-you-can-ask-for-and-be-refused)
  for what shipped, and `docs/guides/performance-guide.md` for the user-facing
  half.
- **T2 -- the checkless tail call: DONE (2026-09-22), and it moves no depth
  numbers.** A non-self call in tail position is now emitted as a genuine C
  `return f(args);`, with no hoist temp and no `if (tur_panicking) return ...;`.
  **Every row of Section 1's table is unchanged by it** -- measured, not
  assumed. What it bought is 6,601 fewer branch sites and 31,216 fewer lines of
  emitted C, and the structural precondition the rest of the plan was written
  against. See [T-D2](#t-d2----the-panic-check-is-redundant-at-a-genuine-tail-call-and-should-be-dropped-there),
  which has been rewritten around what landing it actually taught.
- **T3 -- `match` arms in the tail grammar: DONE (2026-09-22), and this one
  does move a number.** A self tail call in a `match` arm is now a backedge:
  `tailcall-match-arm-deep` runs 10,000,000 frames at `-O0` and returns. That
  row of Section 1's table ("Self tail call, `match` arm -- **No backedge
  emitted**") is the one line of it that T2 and T3 between them made stale. See
  [T-D4](#t-d4----extend-the-tail-grammar-starting-with-match).
- **T4 -- drop glue on a dead owned local: DONE (2026-09-23), and it moves a
  number.** A self tail call under a `ref<T>`, `rc<T>`, move-only Drop or
  by-value-ADT-with-owning-field local is now a backedge when that local is
  dead at the call: `tailcall-drop-glue-deep` runs 10,000,000 frames of each
  kind at `-O0`. The Section 1 row "body owns a `ref<T>` -- **No backedge
  emitted**" is the second stale line of that table. See
  [T-D3](#t-d3----cleanup-decides-tail-position-and-the-honest-answer-has-a-limit),
  whose "What shipped" corrects two of the plan's own four corrections.
- **T5 -- mutual tail calls fused into one function: DONE (2026-09-23), and it
  moves the two mutual rows of Section 1.** A cycle of tail calls among plain
  top-level functions becomes one C function with a `switch` at the top of a
  loop; every member keeps a thin wrapper. `tailcall-mutual-deep` runs a
  2-member, an 8-member and a mixed-arity group 10,000,000 deep at `-O0`. See
  [T-D5](#t-d5----mutual-tail-calls-are-scc-fusion-not-sibling-calls), whose
  "What shipped" records where the design text had to bend.
- **T6 -- the bounce trampoline for dynamic tail calls: DONE (2026-09-23), and
  it moves the indirect row of Section 1 for the dynamic dialect.** A Saffron
  call through an `any` in tail position bounces to a driver loop instead of
  nesting: `tailcall-dyn-deep` runs a self loop, a lambda, a capturing
  closure and a mutual pair through function values 10,000,000 deep at `-O0`
  in ~1.4 MB. It is also 2.7x faster than the nested calls it replaced
  (`benchmarks/saffron-dyn-tail-results.md`). See
  [T-D6](#t-d6----indirect-tail-calls-need-a-trampoline-and-only-dynamic-dialects-get-the-guarantee),
  whose "What shipped" records how the trampoline had to reach the CPS
  backend, which the design text never mentioned.

Prerequisite for [r7rs-lang-plan.md](r7rs-lang-plan.md), but not only for it:
T1-T3 and T5 are Turmeric features that stand on their own, and T3 closes a
silent hole in a shape people write every day.

Everything in Sections 1 and 2 was **measured on 2026-09-21** against
`./build/tur` at v0.50.0 on macOS/arm64. The transcript is in
[Appendix A](#appendix-a----probe-transcript). Read that appendix before
trusting any number here, because **the first version of this investigation
reached the wrong conclusion twice** -- see 2.4.

---

## 0. The ask

> The goal is for Turmeric to fully support tail calls.

Short answer: **mostly yes, and the pieces are worth building independently of
Scheme.** One piece cannot be promised in full generality, and Section 3's
T-D3 says exactly which and why. That limit is the same one Rust has, for the
same reason, and it is better stated up front than discovered by a user whose
loop overflows.

---

## 1. What ships today (measured)

Depth 10,000,000 unless noted. `-O2` is what `tur build` / `tur run` use by
default (`src/main.c:6213`); `-O0` is what `tur run --debug` uses.

| Shape | `-O0` | `-O2` | `--interpret` | What is actually happening |
|---|---|---|---|---|
| **Self** tail call | **pass** | pass | pass | Turmeric emits a real `__tur_tailcall:` label and `goto`. A genuine language guarantee. |
| Self tail call, **`match` arm** | ~~n/a~~ **pass** | pass | pass | ~~**No backedge emitted.** Ordinary recursive call.~~ **Fixed by T3** (2026-09-22): a real backedge, verified at 1e7 frames at `-O0`. |
| Self tail call, body owns a **`ref<T>`** | ~~n/a~~ **pass** | pass | pass | ~~**No backedge emitted.** Defer frame pushed; drop fires *after* the call.~~ **Fixed by T4** (2026-09-23) when the local is dead at the call: the frame fires before the backedge. Verified at 1e7 at `-O0`. |
| **Mutual**, 2 functions | ~~**SIGSEGV**~~ **pass** | pass | pass | ~~LLVM inlines the pair and collapses it to a loop. Not a tail call.~~ **Fixed by T5** (2026-09-23): one fused function with a dispatch loop. Verified at 1e7 at `-O0`. |
| **Mutual**, 8 functions | ~~**SIGSEGV**~~ **pass** | pass | pass | ~~Same -- LLVM inlines the whole 8-cycle.~~ **Fixed by T5**, same mechanism; 8 is the member cap. |
| **Indirect** (through a `fn` value) | SIGSEGV | **SIGSEGV at ~29,335** | pass at 1e6 | No tail-call handling anywhere. ~285 bytes of C stack per level on an 8 MB stack. **Saffron dynamic calls fixed by T6** (2026-09-23): 1e7 at `-O0`, ~1.4 MB. Typed-Turmeric `fn` values unchanged, by decision (T-D6). |

Three conclusions, in order of importance:

1. **The self-tail-call guarantee is real and solid.** It survives `-O0`, it is
   visible in the emitted C, and it is not an optimizer favor. Everything
   `docs/guides/performance-guide.md` says about it is accurate.
2. **The indirect case fails at `-O2` as well as `-O0`**, at a depth low enough
   (~29K) to be hit by ordinary programs. This is the one that matters for
   Scheme, where every procedure call is a call to a value -- and it is also the
   row most likely to bite a Turmeric user who stores a callback in a struct.
3. **The interpreter is already correct on all six rows.** `eval_apply_inner`'s
   trampoline handles direct, mutual, and indirect tail calls alike. Any staging
   can lean on `--interpret` as the conformant fallback.

---

## 2. Why -- three root causes

### 2.1 Every call site is followed by a panic check

This is the structural blocker, and it is documented in-tree at
`src/compiler/emit_fns.c:4089`:

> `panic` is NOT `noreturn` on the compiled path: it sets `tur_panicking` and
> returns, and the per-call-site `if (tur_panicking) return ...;` is what
> unwinds.

So every emitted call looks like this (from probe pB):

```c
static bool is_hyeven_qu(int64_t n) {
        bool __t277;
        if ((n) == (INT64_C(0))) {
            __t277 = true;
        } else {
            bool __ps_278 = (is_hyodd_qu((n) - (INT64_C(1))));
            if (tur_panicking) return ((bool)0);   /* <- work after the call */
            __t277 = __ps_278;
        }
        return __t277;
}
```

**No call in emitted Turmeric C is ever in C tail position.** Not this one, not
any. That forecloses every strategy built on the C compiler doing the work:
sibling-call optimization has nothing to optimize, and
`__attribute__((musttail))` would be a hard compile error on every site.

The simplest program in this investigation emitted **187** of these checks.

> **Superseded by T2 (2026-09-22), in the text but not in the numbers.** A
> direct call in tail position now emits as `return f(args);` with no check, so
> "never in C tail position" is no longer true of the emitted text. It was
> already not true of the *object code*: at `-O2` clang sank the check and
> sibling-called anyway, identically before and after T2 -- which is why
> Section 1's table is unchanged. See
> [T-D2](#t-d2----the-panic-check-is-redundant-at-a-genuine-tail-call-and-should-be-dropped-there).

### 2.2 Cleanup after the call

A call with live cleanup after it is not in tail position, and no amount of
codegen cleverness changes that. `tco_mark` already knows this and bails:

```c
/* emit_fns.c, tco_mark */
for (uint32_t i = 0; i < e->as.do_.n; i++)
    if (e->as.do_.items[i]->kind == EX_DEFER) return 0; /* defers break tail */
```

Measured, with a `ref<T>` local (auto-defer drop at scope end): the backedge
does not fire at all, and the emitted C is unambiguous about why --

```c
tur_frame_push_defer(&__frame_309, __defer_311, &__t312);
int64_t __ps_314 = (loop_hyref((n) - (INT64_C(1))));
if (tur_panicking) { tur_frame_fire_lifo(&__frame_309); return ((int64_t)0); }
__t313 = __ps_314;
tur_frame_fire_lifo(&__frame_309);        /* <- real work, after the call */
```

The conservative bail is **correct**. It is also more conservative than it needs
to be: in that probe `b` is never passed to the recursive call and is dead at
the call site, so the drop could legally be hoisted *before* it and tail
position restored. That gap is T4.

> **Closed by T4 (2026-09-23).** That exact program now emits
> `tur_frame_fire_lifo(&__frame); n = <tmp>; goto __tur_tailcall;` -- the frame
> is still pushed, and fires after the argument temps instead of after a
> recursive call. See T-D3's "What shipped".

### 2.3 `tco_mark`'s tail grammar is missing `match`

`tco_mark` recurses through `EX_CALL`, `EX_IF`, `EX_DO` and `EX_LET`/`EX_LETREC`
(the last only when every binding is a plain scalar -- `tco_let_simple`). There
is **no `EX_MATCH` case**, so:

```turmeric
(defn loop-match [n : int] : int
  (match (classify n)
    (Done)   0
    (More v) (loop-match v)))      ; tail call -- gets NO backedge
```

emits an ordinary recursive call. `match` is the idiomatic way to write a loop
over an ADT in this language, so this is not an exotic corner; it is the shape a
Turmeric programmer reaches for first, silently losing the one TCO guarantee the
language does make. This is the cheapest high-value fix in the plan (T3).

> **Fixed by T3 (2026-09-22).** That exact program now emits
> `n = <v>; goto __tur_tailcall;` in the `(More v)` arm.  It was indeed the
> cheapest high-value fix: it moved a depth number (1e7 at `-O0`, where T2
> moved none) and churned **zero** snapshots, because no existing fixture had
> a tail call in a match arm -- which is itself the measure of how silent the
> hole was.

### 2.4 What the `-O2` numbers actually measure, and why they are not a guarantee

**This subsection exists because the first pass of this investigation got it
wrong twice, in the direction CLAUDE.md warns about: control flow read, a cause
inferred, no measurement.**

The first probe ran mutual recursion at depth 1,000,000 with a **literal**
argument, saw it pass at `-O2`, and concluded "clang is doing sibling-call
optimization." Both halves were wrong:

- **The probe was void.** Disassembling it, `is_hyeven_qu` and `is_hyodd_qu`
  were not in the object file at all. They are `static`, side-effect-free, and
  called with a constant, so clang evaluated the whole recursion at compile time.
  The probe measured constant folding.
- **The mechanism was wrong.** Re-run with the depth read from the environment
  so it cannot be folded, `-O2` still passes -- but the functions are *still*
  absent from the object file. clang **inlines** the cycle and converts it to a
  loop. `main` contains:

  ```asm
  2c10: cmp  x0, #0x1
  2c14: b.eq 0x2c2c
  2c18: sub  x0, x0, #0x2      ; n -= 2  (both bodies inlined)
  2c1c: cbnz x0, 0x2c10        ; loop
  ```

  There is no sibling call. There is no call.

This matters beyond pedantry, because the two mechanisms have very different
fragility. Sibling-call optimization would apply to bodies of any size. Inlining
applies only while the cycle fits under the inliner's cost threshold -- so the
`-O2` "pass" evaporates the moment the functions do real work, and it never
applied to indirect calls at all, which is exactly what the ~29K ceiling in
Section 1 shows.

**Harness consequence, and it is a hard rule: a tail-call fixture compiled at
`-O2` measures clang, not Turmeric.** Every fixture this plan adds must be built
and run at `-O0`, or it asserts nothing. The probe battery in Appendix A is
built both ways for precisely this reason.

### 2.5 The CPS path does not rescue this today

The R7RS plan floated "route it through the CPS-IR backend, which already has
`CT_TAILCALL`." Measured: the indirect probe **is already being routed there**,
and it does not help. `step_hya__cps` emits the indirect call as an ordinary C
call, checks `tur_panicking`, threads the result through two temporaries, and
only then invokes the continuation:

```c
static int64_t step_hya__cps(int64_t n, DK *__kont) {
    ...
    int64_t __ps_315 = ((*(tur_thunk_int64_t_int64_t_t *)(...))(..., n - 1));
    if (tur_panicking) return ((int64_t)0);
    __t313 = __ps_315;
    ...
    return dk_run(__kont, (intptr_t)(__t0));
}
```

Worse, the callee is reached through its **direct-entry wrapper**, which does a
`setjmp`, a `dk_prompt` malloc and `__dk_entry_depth++` on every level. So each
"tail call" is a full nested re-entry into the DK machinery, which is where the
~285 bytes per frame come from.

`CT_TAILCALL` exists in the IR vocabulary (`emit_cps_ir.h`); this shape is not
reaching it. Treat "the CPS backend gives us tail calls" as **disproven for the
current emitter**, not as available.

---

## 3. Design decisions

### T-D1 -- tail position becomes a thing you can ask for, and be refused

**Verdict: add an annotation that is a hard error when the call cannot be a tail
call.**  **Landed** -- what follows is the design, and then what shipped.

Today TCO is invisible. You either get the backedge or you do not, nothing says
which, and the failure mode is a SIGSEGV at an unpredictable depth in
production. Every language that takes this seriously has a checked form --
Scala's `@tailrec`, Clojure's `recur`, Kotlin's `tailrec`.

```turmeric
(defn loop-match [n : int] : int
  (match (classify n)
    (Done)   0
    (More v) ^tailcall (loop-match v)))
```

A `^tailcall` call that the compiler cannot place in tail position is
`TUR-E0xxx`, naming the reason -- "a `ref<T>` local is live across this call",
"the callee is not statically known and this dialect does not trampoline
indirect calls". That diagnostic is most of the value: it converts a silent
performance cliff into a compile-time conversation.

**This lands first**, because it is also the test instrument: every later stage
is verified by a fixture that annotates a call and asserts the annotation holds
at `-O0`.

#### What shipped

`^tailcall` is a reader-level **prefix** on the expression that follows it, so
`^tailcall (loop v)` reads as `(^tailcall (loop v))`.  It had to be a prefix
rather than an extra list element because the shapes that most want it --
`match` and `handle` arms -- pair up two forms at a time, and an extra element
there silently re-pairs every clause after it.  The prefix is suppressed in
list-head position, which is what lets the explicit `(^tailcall (loop v))`
spelling read as itself (and is what the sweet-exp indentation layer produces
from a `^tailcall`-led line).  `tur fmt` normalizes to the parenthesized form.

Elaboration does nothing but set a flag: the annotation elaborates to the call
itself, unchanged.  **The check lives in `emit_fns.c`, beside `tco_mark` /
`emit_tail`**, rather than in the elaborator -- `tco_params_simple`,
`tco_let_simple` and `tco_is_self_call` ARE the tail grammar, and a second copy
of it in elaboration would be one more pair that can drift (risk TR1).  The
verifier walks the body mirroring `tco_mark`'s spine, and additionally descends
into the non-tail sub-expressions people write calls in (`if` conditions, `do`
prefixes, `let` inits, `match` arms and guards, call and builtin arguments,
`while` bodies, ascriptions, `return`), so it can say WHICH rule refused the
call.  A `^tailcall` in one of the node kinds the walker does not enumerate
goes unchecked rather than mis-reported.

The diagnostic is `TUR-E0716`, with a distinct message per reason: argument
position, not the last form of a `do`, a live `defer`/owned-local drop, a
`match` arm, a one-armed `if`, a `let` binding that is fn-typed or carrier-ABI,
a different callee (T5), an indirect callee (T-D6), a mis-saturated self call,
a CPS-lowered enclosing body (2.5), and each function-level ineligibility
(variadic, `void`, `main`, inline-C body, diverging body, a parameter a backedge
cannot reassign).  `tur explain TUR-E0716` carries the long form.

Two consequences worth stating plainly, both documented in the guide:

- The check runs **during C emission**, so `tur build` / `tur run` / `tur emit-c`
  perform it and `tur check` does not.
- **`--interpret` never reports it.**  `eval_apply_inner`'s trampoline makes
  every tail call proper, so the annotation is vacuously satisfied there.  This
  is a genuine difference between the engines, not turi lagging.

Fixtures: `tailcall-annot-self-deep` (the `-O0` instrument -- a `flags` file
carrying `--debug`, 10,000,000 frames, and no `expected.c`, because `--debug`
emits `#line` directives), `tailcall-annot-let-do` (the `let` and `do` arms,
both spellings, with a codegen snapshot pinning the backedge), and six negatives
under `tests/fixtures/errors/tailcall-*`.  The five negatives whose diagnostic is
an emit-time one carry `requires.compiled` so `run-turi.sh` skips them.

### T-D2 -- the panic check is redundant at a genuine tail call, and should be dropped there

**Verdict: do not restructure `panic`. Just stop emitting the check after a tail
call.** **Landed 2026-09-22** -- the safety argument held exactly as written,
and everything else in this section is what landing it corrected.

The tempting reading of 2.1 is that the whole panic mechanism must change --
make `panic` `noreturn`, or `longjmp` to a per-thread handler. That is a deep,
cross-cutting change to something load-bearing (`tur_frame_fire_lifo` unwinding
depends on it), and **it is not necessary.**

Consider `f` whose body is exactly `return g(x);`. If `g` panics, it sets
`tur_panicking` and returns a garbage value. `f` returns that garbage value
unexamined. `f`'s own caller is, by definition, *not* in tail position with
respect to this chain -- so it has the check, and it fires:

```c
int64_t __ps = f(...);
if (tur_panicking) return 0;    /* catches g's panic, one level up */
```

The panic propagates correctly through any number of tail calls, because a tail
call by definition does nothing with the value. **The nearest enclosing
non-tail frame does the checking, and there always is one** (the program entry
point at worst).

The argument is in fact stronger than that, which is worth stating because it
shrinks the blast radius: `tur_panic` only sets the flag and returns **when a
`catch-unwind` boundary is installed** (`tur_handler_chain != NULL`,
`emit_module.c:12340`); with no handler it prints and `abort()`s. So the
per-call-site check is unobservable outside a `catch-unwind` -- which is also
why emitted `main` has never carried one (`/* ret ctype unknown; no propagation
here */`) without anything breaking. Pinned by
`tests/fixtures/tco-tail-call-panic-propagates`, which panics beneath four
checkless tail calls and asserts both the boundary-adjacent and the
one-frame-out spelling still catch.

#### What it cost, and what it bought -- measured

The headline correction, because the stage table's "unblocks tail position at
all" invited the wrong expectation:

| | before T2 | after T2 |
|---|---|---|
| mutual recursion, `-O0`, depth 1e7 | SIGSEGV | **SIGSEGV** |
| mutual recursion, `-O2`, depth 1e7 | passes | passes |
| `-O2 -fno-inline`, disassembled | `b _is_hyodd_qu` | `b _is_hyodd_qu` |
| emitted C, one 2-function probe | 9,913 lines | 9,689 lines |
| the 149 `expected.c` snapshots | -- | **-31,216 lines, 6,601 checks removed** |

Two things to take from that. First, **`-O2` was never the thing the check was
blocking**: clang already sank it and sibling-called, identically before and
after -- so 2.1's "no call in emitted Turmeric C is ever in C tail position" was
true of the *text* and not of the *object code*. Second, and this is the one
that matters for staging, **T2 alone moves no depth number**, because `-O0`
does no tail-call optimization at all: a hand-written, pristine C tail call
SIGSEGVs at `-O0` too. The only mechanism that survives `-O0` is
`__attribute__((musttail))` (verified: 1e7 deep, clean) -- or T5's `goto`.

So T2 is a code-size win plus a precondition, and **nothing else in this plan
consumes that precondition**: T3, T4 and T5 are all `goto`-based and T6 returns
a bounce descriptor. Its one consumer is `musttail`, which T-D5 has already
ruled out as the floor (clang-only; GCC ≥ 15; and c2mir, the JIT's C compiler,
has no such attribute). Land T3-T5 on their own merits; do not sequence them
behind this.

#### The carve-outs, resolved

Both of the "verify rather than assume" items came back clean, for one shared
reason: **defer frames are block-scoped.** `ctx->frame_var` is NULL at a tail
call in an outer scope while an inner scope's frame fires at its own end, so
"a frame is open at this call" and "there is cleanup after this call" are the
same condition -- T-D2 and T-D3 agree by construction rather than by luck.

- **`with-region`**: the bracket's `tur_region_note_escape` +
  `tur_region_pop_checked` pair is emitted in the *bracket-opening* frame, and
  that whole function is CPS-lowered (already `TC_CPS_BODY`). The callee inside
  the bracket is unaffected.
- **`handle`**: likewise CPS-lowered, and refused wholesale.

Three carve-outs the original text did not list, all now enforced:

- **`panic_signal_is_break`.** Inside the stackless trampoline the signal must
  reach the driver's unwind loop as a `break`; dropping it there abandons the
  live continuation chain.
- **Pending owned boxes.** An argument that left an owned `any`, a fresh
  sum-carrier box, or a vec spill pending has its drain hanging off the hoist --
  that drain is real work after the call. The decision is therefore made *after*
  the call is emitted, where the pending marks are knowable, and `emit_tail`
  falls back to the hoisted spelling when the answer is no.
- **An open region bracket** on the call itself (`bt-scope` / `with-region`),
  whose pop must follow it.

#### What T2 does not reach

It was not the "small deletion" the stage table implied, for a structural
reason worth recording: `emit_tail` -- the only emitter that puts a `return`
inside each branch, and so the only one that can make a call a C tail call --
was gated on `tco_mark(...) > 0`, i.e. on a **self** tail call already existing.
The entire mutual/indirect population T2 targets has none, so those bodies went
down the generic path that assigns into a `__tN` temp and returns once at the
bottom. Landing T2 meant widening that gate and suppressing the now-unused
`__tur_tailcall:` label.

Widening it exposed a real defect: `emit_tail` carried hand-maintained SUBSETS
of two decisions `emit_fn_def` owned -- the result-shape return ladder (three of
its sixteen arms) and the `let` binder's straddle bridges -- and widening the
routing sent bodies into them that had never been there. Twenty-four fixtures
failed with hard `cc` errors, and `examples/datalog/datalog.tur` compiled on
Linux and **ran wrong**, which is the more instructive failure: on macOS the same
emitted C was a hard error, so the Linux leg's silent exit-2 was the same defect
wearing a warning.

That is fixed, not worked around:
[docs/archive/emit-tail-return-path-lacks-carrier-bridges.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/emit-tail-return-path-lacks-carrier-bridges.md).
`emit_fn_return_spelling` is now the one ladder both paths call, and the `let`
arm gained the bridge it was missing. The extraction was verified as a pure
no-op first (zero snapshot churn with the old routing still in place), and only
then was the routing widened.

What survives is one condition, and it is a real statement about tail position
rather than a workaround: **the callee's C return type must be exactly the
enclosing function's**, read via `emit_call_name` so a `__spec__` monomorph is
compared as itself and not as its generic. A return that has to spill or cast is
work after the call. What went away is the all-or-nothing rule that stood in for
the defect -- a body is no longer refused wholesale because one of its tail
leaves needs a bridge; that leaf keeps its hoist and its check while its
siblings become tail calls.

So, correcting the report's own first guess: merging the ladders does **not**
let carrier-returning mutual recursion reach C tail position, and should not.
That case needs T5.

Also still out of scope, deliberately: **indirect** tail calls, which go through
the fat-closure protocol rather than a named callee and are T-D6's problem.

#### T2b -- `musttail`, where the C compiler can promise it

**Landed 2026-09-23, after T5** -- which changed what it is for. The design
text framed it as a way to get `-O0` mutual recursion before T5; T5's `goto`
now covers every cycle it can fuse, with no toolchain dependency. What T2b
adds is the rest: a T2 checkless tail call that T5 did NOT turn into a jump is
spelled `TUR_MUSTTAIL return f(args);`. The case that matters for depth is a
cycle past T5's caps (9+ members or 17+ parameters, measured below); the
common case in the corpus is plain forwarding -- stdlib's `list-head` /
`list-tail` / `list-length` wrappers and the `Eq[int]` instance calls are
what moved the 153 snapshots. (A generic mutual pair is NOT an example: T5
fuses it through the carrier.)

**The toolchain split is real, and measured here.** clang 18 honours the
attribute and runs a nine-member cycle 10,000,004 deep at `-O0`; gcc 13 has
no such attribute and overflows; c2mir (the JIT) has none either. So
`TUR_MUSTTAIL`, written lazily into the preamble the first time a program
needs it (so only those programs' snapshots move), expands to
`__attribute__((musttail))` only for clang on x86-64/aarch64 and to nothing
everywhere else -- including gcc 15, which has the attribute but was not
available to test against, and wasm, where clang rejects musttail without the
tail-call feature. `-DTUR_MUSTTAIL=` turns it off. Where it is empty the call
is exactly T2's.

**musttail is a hard contract, so every condition is checked, not inferred**
(`tail_call_musttail_ok`, `emit_fns.c`):

- the call text is exactly `name(args)`;
- caller and callee have IDENTICAL recorded C signatures (the forward-
  declaration pass's ground-truth table): arity, every parameter spelling, and
  the return. T2 had proved only the return;
- both take at least one parameter. A zero-parameter function is declared
  `f()`, which C99 does not count as a prototype, and clang refuses musttail
  without one -- found by the clang run, on stdlib's `hamt.tur` and
  `option.tur`;
- no `const T *` pass-by-pointer parameter;
- **no address is taken anywhere in the caller's body, nor in the call.** This
  is the one thing an optimizer checks for itself before a sibling call and
  musttail skips: a tail call reuses the caller's frame, so an argument
  pointing into it would dangle. The emitter builds its stack objects -- a
  stack fat box for a non-retaining sink, a frame `any` box, a T4 defer frame,
  a pass-by-pointer temp -- with a `&`, so refusing any single `&` is sound,
  if blunt.

**Verified across the corpus under clang**, not only on the new fixtures: with
`CC=clang` the suite fails the same 54 fixtures with and without the macro
enabled (pre-existing clang-only failures, mostly the gcc/ASan `libturi.a`
link mismatch CLAUDE.md describes), and none of those 54 carries a musttail
diagnostic behind its link failure. Under gcc: 3105/0, 153 snapshots
re-spelled (`return f(args);` -> `TUR_MUSTTAIL return f(args);` and the macro
block, nothing else). Under the JIT harness the new fixtures and a sample of
the re-snapshotted ones pass.

`^tailcall` still refuses a call that only musttail would keep off the stack:
the annotation promises a tail call on every toolchain, and this one does not.

Fixtures: `tailcall-musttail-annot` (the snapshot -- where the attribute goes,
and the zero-arity, differing-signature and address-taken cases where it must
not) and `tailcall-musttail-deep` (1e7 at `-O0`, carrying the new
`requires.musttail` marker, which `tests/run.sh` probes with the emitter's own
gate).

### T-D3 -- cleanup decides tail position, and the honest answer has a limit

**Verdict: hoist the drop when the value is dead at the call; refuse tail
position when it is live. Do not promise general TCO in a language with
destructors.** **Landed 2026-09-23** -- see "What shipped" at the end of this
section, which revises corrections 3 and 4 below.

Three cases, and the middle one is the work:

| At the tail call, an owned local is ... | Result |
|---|---|
| absent | tail position; emit the tail call |
| **live but dead after** (not an argument, not borrowed by the callee) | **hoist the drop before the call**, then tail position |
| genuinely live across (passed by borrow, or the callee retains it) | **not a tail call.** Conservative bail + a `^tailcall` diagnostic |

Row 2 is a liveness question over the tail call's argument set, and it covers
the common case -- the `ref<T>` probe in 2.2 is exactly it. (An `rc<T>` local
reproduces the same emitted C; `elab_forms.c:1573` admits both, so either is a
faithful repro and `TC_DEFER`'s "such as a `ref<T>`" is accurate as written.)

Row 3 is the limit, and it should be stated in the guide in the same breath as
the guarantee. It is why Rust does not have guaranteed TCO, and pretending
otherwise would mean either leaking or dropping a value the callee is still
using. **Turmeric can promise proper tail calls for calls with no live owned
value across them, and that promise is worth making precisely because it is
checkable** (T-D1).

For `#lang r7rs` this limit is nearly vacuous: Scheme has no destructors, and a
Scheme local is an `any`. The one thing to watch is `__tur_any_drop` on a
heap-boxed by-value aggregate, which reintroduces row 3 through the back door;
the R7RS prelude should prefer representations that do not box.

#### Four corrections, from reading the code T4 has to change

**1. Row 2 applies ONLY to compiler-synthesized drop glue, never to a user
`defer`.** This is the correction that changes the work, not just the wording.
A written `defer` has observable order -- it runs after the call, innermost
first -- so hoisting it is a behavior change, not an optimization:

```turmeric
(do (defer (println n)) (loop-defer (- n 1)))   ;; prints 1 2 3 0
```

Hoisted, that prints `3 2 1 0`. Such a block is not a tail call at any price,
and must keep today's conservative bail. `TC_DEFER`'s current text reads "a
`defer` in this block -- an explicit one, or the drop glue of an owned local",
which lumps the two together; after T4 they have different answers and the
message has to split.

**2. The AST cannot tell them apart yet, so that is T4's first commit.** A
synthesized auto-drop is built as a plain `EX_DEFER` node (`elab_forms.c`,
~1643/1686/1845/2109) and `Expr.as.defer_` carries only
`{body, captures, n_captures}` -- no marker. Add one (`is_drop_glue`), set it at
each synthesis site, and key the hoist on it.

**3. The liveness question is smaller than "liveness."** The auto-drop is
emitted only when the binding still owns the value at scope exit -- the
elaborator already suppresses it on `is_moved` / `is_linear_consumed` /
`is_binding_consumed` (`elab_forms.c:1573`). So a binding that reached a tail
call still owning its value cannot have escaped, and the test reduces to **"the
dropped binding does not occur free in the tail call's argument expressions."**
`rc_elision.c` already has that analysis's shape (single-use proof, barrier
list) if a starting point is wanted.

**4. Do not hoist the frame -- delete it.** The lowering is: arguments into
temps, then the drop bodies in LIFO order, then the backedge. No
`tur_frame_push_defer` / `tur_frame_fire_lifo` pair at all, which is also
strictly better on the panic path (no frame left to fire). One trap: the
synthesized defers are **appended after** the tail expression in the `do` items
array, so T4 must take the last **non-defer** item as the tail, not
`items[n - 1]`.

One case to settle with a fixture before relaxing anything: a closure that
captures the dropped binding and is stored outside the scope. Check whether the
elaborator already suppresses the auto-drop there, or whether "not free in the
args" is insufficient.

#### What shipped (T4, 2026-09-23)

Corrections 1 and 2 held exactly: `EX_DEFER` carries `is_drop_glue`, set at the
four synthesis sites in `elab_forms.c` (the `ref<T>` drop, the move-only Drop
opaque's `Drop.drop`, the `rc<T>` drop, and a by-value ADT's owning-field drops),
and a written `defer` keeps the conservative bail with its own message
(`errors/tailcall-written-defer`, which prints `0 1 2 3` where a hoist would
print `3 2 1 0`). The synthesized defers are indeed appended after the value,
and the tail is the last NON-defer item. Corrections 3 and 4 did not survive
contact with the code:

**3, revised: "not free in the args" is insufficient, and the open question
above answered it.** The elaborator does NOT suppress the auto-drop when a
closure captures the binding -- `tests/fixtures/rc-auto-drop-closure-capture`
pins that as intended behavior -- so an `rc` local captured by a closure the
loop hands onward, or stores where the next activation can reach it, is live
across the backedge even though it never appears free in the arguments.
"Still owns the value, so cannot have escaped" is true of *moves*, not of
*aliases*. The shipped rule is an allowlist over every occurrence of the dropped
binding -- in the block's items AND the enclosing `let`'s initializers, which
is where a sibling binding could have aliased it: a use is dead-safe only when
it copies a plain number out (`@b`, `(.tag t)` with a scalar result). A bare
occurrence -- an argument, a capture, a borrow -- refuses, and so does any node
kind the walker does not enumerate, which it answers with the complete
`collect_free_vars` walk rather than by skipping. `errors/tailcall-owned-local-live`
now holds the live case (an `rc` passed to a helper in the recursive call's
own argument).

**4, revised: the frame stays; only its firing point moves.** Deleting the
`tur_frame_push_defer` / `tur_frame_fire_lifo` pair and inlining the drop bodies
before the backedge would have been strictly WORSE on the panic path, not
better: a panic in one of the backedge's argument expressions returns through
the per-call-site check, and `emit_panic_signal_return` fires the frame chain
-- with no frame, that exit skips the drops and leaks under `catch-unwind`.
`tailcall-drop-glue-panic` pins it under LeakSanitizer. So the lowering is: the
frame is declared and the drops pushed exactly as `emit_do_value` does, the
value is emitted in tail position with the frame open, and every exit fires the
chain -- a backedge after its argument temps and before the parameter
reassignment, a `return` after its value is in a temp (a one-item `do` wrapper
gives exactly the temp and carrier bridges the ordinary path hands the return
ladder). The frame is block-scoped, so a backedge leaves it and the next
iteration re-declares it: constant stack, and the same per-step work the
recursive version did.

Two limits, both deliberate:

- **Only a backedge goes through drop glue.** A NON-self call under an open
  frame is not a C tail call -- the frame fires after it -- so T2's checkless
  `return f(args);` is off there, and a block that carries no self tail call
  keeps the path it always took. That is also why **zero** existing snapshots
  moved.
- **A block whose items `return` or `throw` is refused** (TC_DROP_EXIT): those
  fire the chain on their own schedule, which the tail path does not re-derive.

`tco_mark` decides and records it on the block (`tail_drop_hoist`), so
`emit_tail` and the `^tailcall` verifier read one answer rather than
re-deriving it (TR1). Fixtures: `tailcall-drop-glue-deep` (1e7 at `-O0` for
each of `ref<T>`, `rc<T>`, a by-value ADT's owning field, and a backedge in a
`match` arm), `tailcall-drop-glue-annot` (the snapshot -- formerly the
negative `errors/tailcall-owned-local-live`, whose header predicted its own
move), `tailcall-drop-glue-panic` (leak-checked), and the two negatives above.

### T-D4 -- extend the tail grammar, starting with `match`

**Verdict: `tco_mark` and `emit_tail` grow an `EX_MATCH` case, and the two stay
in lockstep.** **`match` landed 2026-09-22; the rest of the order below is
still open.**

`tco_mark`'s contract is that it "mirrors `emit_tail`'s structural recursion
exactly so that `marked >= 1` predicts whether `emit_tail` will emit a
backedge". That invariant is what keeps the `__tur_tailcall:` label from being
emitted-and-unused, and any new case must be added to both or the C compiler
warns on an unused label.

Order: `EX_MATCH` (2.3, measured), then audit `handle` arms, `and`/`or`, and
`tco_let_simple`'s carrier-ABI bail, each with a fixture before it is relaxed.

#### How `match` landed, and the one decision worth reusing

**`emit_tail` does not re-implement `match`.** That was the tempting shape and
it is the one to avoid: `match` has five arm-emission shapes (ADT constructor
patterns, `any` type-narrowing, literal patterns, session offers, and the
guarded variants of those), and a parallel copy of the pattern tests in
`emit_fns.c` is precisely the drift this section opens by warning about.

Instead the existing emitter grew a hook. `EmitCtx::match_tail` names the one
`EX_MATCH` node whose arms are in tail position; the five arm-body sites
consult it and call back into `emit_tail` in place of `<tmp> = <body>;`. Every
pattern test, binder and guard stays exactly where it is, and the diff is one
statement per shape.

Two things were deliberately left in place rather than removed, and they are
what makes the change cheap:

- **The result temp stays.** It is no longer assigned by any arm, but it is
  still what `emit_tail` returns after the switch -- which is the emitter's
  existing no-arm-matched fall-through, preserved for free. A non-exhaustive
  match in tail position yields what it always did.
- **The `goto <end>` / `break` after each arm stays.** Unreachable after the
  arm's own `return`, and that is fine: it keeps the end label used, so there
  is no `-Wunused-label`, and it keeps the two paths textually identical
  everywhere but the one line.

The shapes refused are the ones the hook cannot reach, gated by one predicate
(`tco_match_tail_ok`) that BOTH `tco_mark` and `emit_tail` call, so they cannot
disagree and strand the label: a nil/never-typed match, an arm-less match, and
a session-offer scrutinee. `TUR-E0716`'s `match` message was repointed at
exactly that set -- an arm body now inherits the enclosing reason the way an
`if` branch does, instead of being refused on its own account.

**It churned zero snapshots.** Not one of the 149 `expected.c` files had a tail
call in a `match` arm, which is the sharpest available measure of how silent
the hole was: the idiomatic loop shape was untested in the codegen corpus
entirely. `tailcall-match-arm-annot` (snapshot, ex-negative -- it predicted its
own move in its header) and `tailcall-match-arm-deep` (1e7 frames at `-O0`,
with a guarded arm) are the coverage now.

### T-D5 -- mutual tail calls are SCC fusion, not sibling calls

**Verdict: fuse each tail-call strongly-connected component into one C function
with a dispatch loop.** **Landed 2026-09-23** -- see "What shipped" at the end
of this section.

Compute the SCC of the direct call graph restricted to **tail edges**. A
singleton SCC with a self-edge is today's backedge. An SCC with n > 1 members
becomes one C function with a `state` variable and a `switch` at the top of the
loop; each member keeps a thin wrapper for non-tail entry from outside the
group.

```c
static int64_t __tcg_3(int state, int64_t n) {
    __tur_tailcall:;
    switch (state) {
      case 0: if (n == 0) return 1; n = n - 1; state = 1; goto __tur_tailcall;
      case 1: if (n == 0) return 0; n = n - 1; state = 0; goto __tur_tailcall;
    }
}
static bool is_hyeven_qu(int64_t n) { return __tcg_3(0, n); }
static bool is_hyodd_qu (int64_t n) { return __tcg_3(1, n); }
```

Why this rather than `musttail`: it works at `-O0`, it needs no toolchain
support (MinGW is a supported host), it has no per-call cost, and it is a
*generalization of the mechanism already in the tree* rather than a second one.
The members' parameter lists differ in general, so the fused function takes the
union -- which is why this wants a size cap and a bail, not unbounded fusion.

`musttail` stayed available as a later fast path, and has since landed as T2b
(T-D2) -- for the calls fusion does not reach, and only where the C compiler
can promise it. It is still not the floor.

#### What shipped (T5, 2026-09-23)

The emitted shape is the sketch above with three changes, each forced by
something the sketch did not have to face:

```c
static bool __tcg_group_0(int, int64_t, int64_t);        /* at the FIRST member */
static bool is_hyodd_qu(int64_t n) { return __tcg_group_0(0, n, ((int64_t)0)); }
static bool __tcg_group_0(int __tcg_st, int64_t __tcg_s0, int64_t __tcg_s1) {
    __tcg_top:;
    switch (__tcg_st) {
    case 0: { int64_t n = __tcg_s0; (void)n;
        if ((n) == (INT64_C(0))) { return false; }
        else { int64_t __t = (n) - (INT64_C(1));
               __tcg_s1 = __t; __tcg_st = 1; goto __tcg_top; } }
    case 1: { int64_t n = __tcg_s1; ... __tcg_st = 0; goto __tcg_top; ... }
    default: break;
    }
    return ((bool)0);
}
static bool is_hyeven_qu(int64_t n) { return __tcg_group_0(1, ((int64_t)0), n); }
```

- **Concatenated slots, not a union.** Each member owns `n_params` slots of its
  own C types, and its `case` re-declares its parameters as block locals read
  from them. That is what lets every member's body be emitted by the
  unmodified `emit_tail` -- the parameters keep their names, so nothing in the
  body knows it is fused -- and it is why the cap is on total parameters (16)
  as well as members (8), the TR4 bail. A wrapper passes a typed zero for the
  slots that are not its own.
- **A self call inside a group jumps too.** The `__tur_tailcall:` label is
  function-scoped in C, so two members cannot each have one; every tail call
  into the group, including a member's own, writes slots and goes through the
  top of the `switch`. Argument temps, then the T4 frame fire, then the `any`
  drops, then the slots -- the backedge's order.
- **The definition is written at the LAST member's position; a prototype at
  the first.** A member may only ever be emitted after the globals its body
  names, so the one position every member's body is valid at is the last
  one.

Membership is deliberately narrow: a plain top-level `defn` -- no closure,
dictionary clone, instance method, inline-C, effect-colored (CPS) or
`catch-unwind` body, not variadic -- every parameter one the backedge could
reassign and that the signature spells `<ctype> <name>` with no binding
flags, and one C return type across the group. Everything outside that set is
a function `emit_fn_def` shapes in ways a `case` block does not reproduce
(its pass-by-pointer, boxed and carrier parameters; its return ladder's
earlier arms). The edges are computed by walking exactly `tco_mark`'s spine,
so an edge is a call `tco_mark` will mark, and the component is a property of
the graph rather than of which member is emitted first.

Found on the way and fixed in the same change: the TOP-LEVEL forward-declaration
pre-pass (`elab_toplevel.c`) had no `float` arm, so a call to a `: float`
function defined later in the file was typed `int` -- a spurious
`then=float else=int`, which every float-returning mutual pair hits on one
side. The `defmodule` and letrec pre-passes already had the arm.

Fixtures: `tailcall-mutual-deep` (1e7 at `-O0`: an 8-cycle, a mixed-arity
float/cstr pair with a self jump, a `match`-arm jump under T4 drop glue beside a
non-tail member call and a tail call out of the group), `tailcall-mutual-annot`
(the snapshot -- formerly the negative `errors/tailcall-mutual`, whose header
predicted the move), `tailcall-mutual-panic` (a panic inside a jump's
argument, leak-checked), and `errors/tailcall-mutual-not-fusable` (a cycle
threading a `fn`-typed parameter). `mutual-recursion` and
`tco-nonself-tail-call-no-check` re-snapshot: their even/odd pairs are groups
now.

### T-D6 -- indirect tail calls need a trampoline, and only dynamic dialects get the guarantee

**Verdict: a bounce trampoline on the uniform fat-closure representation.
Typed-Turmeric indirect tail calls stay unguaranteed, with a diagnostic.**

An indirect call cannot be SCC-fused -- the callee is a runtime value. The two
real options are `musttail` (needs exact signature identity, which typed
Turmeric does not have across arbitrary `fn` types) and a trampoline.

For the dynamic dialects the trampoline is natural, because the uniformity it
needs already exists: under Saffron and `#lang r7rs` every procedure goes
through the fat-closure protocol with `any`-shaped arguments and result. A tail
call returns a bounce descriptor instead of calling; the entry wrapper loops
until it sees a non-bounce value. Cost is one tagged compare and one branch per
bounce, paid only on tail calls.

For typed Turmeric, an indirect tail call remains a real C call. `^tailcall` on
one is an error that says so and names T-D6 -- which is a much better outcome
than today's silent ~29K ceiling.

This is the split that answers the original question honestly: **"full tail call
support" is achievable for `#lang r7rs`, and achievable-with-a-named-limit for
typed Turmeric.**

#### What shipped (T6, 2026-09-23)

The design above has the shape right -- a bounce descriptor and a loop -- and
two things wrong about where they live.

**"The entry wrapper loops" cannot be the callee's wrapper.** If every
bouncer's own entry drove, a chain of bounces would nest one driver per
function, which is the recursion again. The loop has to be at the one place
that is below the whole chain, and a bounce is only safe when the activation
making it was entered from such a loop -- a typed `vec-fold` invoking a Saffron
callback must never be handed the sentinel. So:

- **The driver** (`__tur_tb_call`) makes a call, and while the result is the
  sentinel, makes the call the callee recorded. A tail dynamic call that may
  not bounce *drives* instead. Driving is always correct -- it is the ordinary
  call, made in a loop that absorbs the callee's own bounces -- so the first
  call in a chain costs one driver frame and every later one bounces into it.
- **Permission is passed by identity.** Before entering a callee, the driver
  sets `tur_tb_armed_for` to the one function it is about to enter, and only
  if that function is a registered BOUNCER -- a function whose tail reaches a
  dynamic call; its static fat box is registered (`__tur_tb_reg_box`) or, for
  a capturing closure, its thunk (`__tur_tb_reg_thunk`), in an open-addressing
  hash probed once per driven call. The bouncer compares
  the flag with its own address on entry and clears it. Nothing else ever arms
  anything, so no caller outside a driver can see a bounce, and a stale flag
  cannot be mistaken by any function but the one it names.

**Every function that makes a dynamic call is CPS-lowered.** `cps.c` colors a
function with an unresolvable callee (`has_indirect`), and a dynamic call is
the definition of one -- so the plan's own section 2.5 measurement (the
indirect probe "is already being routed there") was the common case, not an
aside. T6 therefore has two spellings of a tail site:

- **CPS** (`emit_cps_ir.c`): `x = <dynamic call>; deliver x to KK_RET` in the
  main body of a bouncer. The direct-entry wrapper publishes its root
  continuation as `tur_tb_root` when armed; the tail site bounces iff
  `__kont == tur_tb_root`, by returning a pointer to a static sentinel box --
  exactly the boxed `any` the wrapper reads back, so a bounce allocates
  nothing. (Delivering it through `dk_run` would have cost a reaped malloc per
  bounce, and 1e7 bounces would have held them all until the chain ended.)
- **Direct** (`emit_tail`, for a bouncer the CPS backend evicts, and for
  capturing closures, which it does not lower): `__tb_may` from the entry
  check, and `__tur_tb_tail(__tb_may, ...)` at the tail.

Bouncing is declined -- the site drives -- whenever something would run after
the return that the deferred call still needs: an argument that queued an
owned box for "drop after the consuming call", an open defer frame, or `any`
scope drops.

**The cost is negative.** The plan budgeted "one tagged compare and a branch
per bounce". A bounce is four words into a thread-local descriptor and a
return; the nested call it replaces opened and tore down a DK entry (`setjmp`,
a `dk_prompt` allocation, the entry-depth bookkeeping). On
`benchmarks/bench-saffron-dyn-tail.tur` -- 10,000,000 dynamic calls, shallow
enough for the old compiler to survive -- T6 is 471 ms against 1256 ms.

`^tailcall` now annotates a dynamic call too; its only refusals are positional
(`errors/tailcall-dyn-argument`). Fixtures: `tailcall-dyn-deep` (1e7 at `-O0`,
five shapes, including the typed-`vec-fold` safety case) and
`tailcall-dyn-leak` (the same shapes, LeakSanitizer-gated).

Two pre-existing defects surfaced and are filed rather than fixed here:
[saffron-catch-unwind-around-dyn-call-fn-crashes](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/saffron-catch-unwind-around-dyn-call-fn-crashes.md)
(which is why no T6 fixture panics; since resolved -- compiled only, the
CPS entry wrapper read a panicking body's NULL result box) and
[cps-capturing-closure-env-leaks-through-dyn-call](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-capturing-closure-env-leaks-through-dyn-call.md)
(since resolved: the env is reaped at the DK entry boundary, unless `__kont`
is the trampoline root, where a bounce would run the lambda after the reap).
`#lang r7rs` does not exist yet; when it does, its procedures ride the same
fat-closure protocol, so it inherits this unchanged.

---

## 4. Stages

| Stage | Size | Benefits |
|---|---|---|
| ~~**T1** -- `^tailcall` annotation + diagnostic + `-O0` fixture harness~~ **DONE** | small | all of Turmeric; it is the test instrument for everything below |
| ~~**T2** -- drop the redundant panic check at tail calls (T-D2)~~ **DONE** | medium | 6,601 fewer branch sites, -31,216 lines of emitted C. **No depth number moved** |
| ~~**T3** -- `EX_MATCH` in the tail grammar (T-D4)~~ **DONE** | small | closed a silent hole in the most idiomatic loop shape. 1e7 at `-O0`; **zero** snapshot churn |
| ~~**T4** -- drop-glue hoisting by liveness (T-D3 row 2)~~ **DONE** | medium | owned-local loops get the guarantee when the local is dead at the call. 1e7 at `-O0`; **zero** snapshot churn |
| ~~**T5** -- SCC fusion for mutual tail calls (T-D5)~~ **DONE** | medium-large | mutual recursion is a guarantee instead of an `-O2` accident. 1e7 at `-O0`; two snapshots moved |
| ~~**T6** -- bounce trampoline for indirect tail calls under dynamic dialects (T-D6)~~ **DONE** | medium-large | **R7RS's actual prerequisite**; fixed Saffron's ~29K ceiling. 1e7 at `-O0`, and 2.7x faster than before |

**Order T4 and T5 by their own merits, not behind T2.** T2 shipped and is a
precondition for nothing else here: T3-T5 are `goto`-based and T6 returns a
bounce descriptor, so none of them consumes it. Its only consumer is T2b
(`musttail`), which has since landed too. T3 landing on its own, without T2, would have worked
identically -- which is the evidence.

**Every stage has landed** (each T-D section ends with "What shipped"). T6
was the R7RS gate; it is open.

**Harness requirement, repeated because it is the trap:** every fixture builds
at `-O0` (`requires.*` marker or an explicit `--debug` invocation), asserts a
depth well past any plausible stack (1e7), and is pinned on both back ends. A
tail-call fixture at `-O2` asserts nothing.

---

## 5. What this costs

- **T2** removed emitted code, by more than "slightly": -31,216 lines across
  the 149 `expected.c` snapshots, 6,601 branch sites gone. It costs nothing at
  runtime and changed no depth behavior at either `-O0` or `-O2`.
- **T3** costs nothing at runtime. It converts calls into `goto`s.
- **T5** converts calls into `goto`s through a `switch`: one indirect branch per
  jump, and a wrapper that zero-fills the other members' slots on entry.
- **T4** converts the call into a `goto` and keeps the defer frame, so each
  step still pays the frame's init, push and fire -- the same work the
  recursive version paid per level, minus the call and the stack frame.
- **T6** costs nothing in typed Turmeric, and in the dynamic dialect it is a
  net saving: 2.7x faster than the nested calls it replaced
  (`benchmarks/saffron-dyn-tail-results.md`, the Saffron row the plan asked
  for; there is no `r7rs` row because there is no `#lang r7rs` yet). The
  per-bounce work is a four-word descriptor write, a tagged compare, and a
  hashed registry lookup to arm the next callee. The lookup started as a
  linear scan and was measured at ~0.3 ns per registered bouncer per call --
  26x slower at 1000 bouncers -- so it is O(1) now (flat from 0 to 1000; see
  the "Registry lookup" table in the results file).
- Fixture count: roughly one per row of Section 1's table, plus the `^tailcall`
  negative cases. Call it 15-20, which is inside the budget the R7RS plan's R3
  risk sets.

---

## 6. Carve-outs

- **An owned value live across a tail call is not a tail call** (T-D3 row 3).
  Permanent, and it goes in the guide next to the guarantee.
- **Indirect tail calls in typed Turmeric** stay unguaranteed (T-D6). A
  diagnostic, not silence.
- **`musttail`** is not the floor and is not promised. T5's `goto` is the
  floor: it covers every cycle it can fuse on every toolchain. T2b (landed)
  adds `musttail` for the tail calls T5 leaves as C calls -- a cycle past
  T5's caps is the one that matters for depth -- but only where the C
  compiler can guarantee it (clang on x86-64/aarch64 today); on gcc and the
  JIT those calls stay ordinary, and `^tailcall` keeps saying so.
- **Restructuring `panic` to be `noreturn`** is explicitly *not* in this plan.
  T-D2 shows it is unnecessary; if some later need arises, it is its own
  decision with its own risks.

---

## 7. Risks

**TR1 -- `tco_mark` / `emit_tail` drift.** They must mirror each other exactly
or the `__tur_tailcall:` label is emitted unused and the C compiler warns. Every
T3/T4 case touches both. Mitigation: one fixture per new case, and the
cc-warning ratchet (`tests/check-cc-warn-ratchet.sh`) already in the tree will
catch the unused-label class.

**TR2 -- measuring clang instead of Turmeric.** This investigation made that
mistake twice (2.4) before catching it. Mitigation: the `-O0` harness rule in
Section 4, stated as a rule rather than a habit.

**TR3 -- T-D2 is subtly wrong somewhere.** ~~The argument in T-D2 is sound for
ordinary calls but was not checked against `with-region` rewind, `handle`
prompts, session-typed endpoints, or the async suspend path.~~ **Retired
2026-09-22.** `with-region` and `handle` both CPS-lower, so a body containing
either is refused wholesale; defer frames turned out to be block-scoped, which
makes "a frame is open here" and "there is cleanup after this call" the same
condition. The residual risk moved somewhere the original text did not look --
the emitted-C bridges on the way to the `return`, not the control constructs --
and is now its own report
([emit-tail-return-path-lacks-carrier-bridges](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/emit-tail-return-path-lacks-carrier-bridges.md)),
contained by T2's exact-return-type gate. `tco-tail-call-panic-propagates` pins
the panic-surfacing half.

**TR4 -- SCC fusion blows up on wide groups.** The fused function takes the
union of its members' parameters. A large SCC with disjoint signatures produces
a wide, ugly function. Mitigation: a member/parameter cap with a clean bail to
today's behavior, plus the `^tailcall` diagnostic saying the group was too wide.
**As shipped (T5):** 8 members and 16 parameters in all; past either the cycle
is ordinary recursion, and `^tailcall` on one of its calls names both caps
among the conditions a group needs (it does not single out which one failed).

**TR5 -- scope.** This is six stages, and only T6 is strictly required by R7RS.
**Closed:** all six landed.
It would be easy for this to become the work instead of a prerequisite to it.
Mitigation: T1-T3 are small and independently valuable; if the R7RS track
stalls, they should land anyway. T1-T5 have.

---

## Appendix A -- probe transcript

`./build/tur` v0.50.0, Debug build, macOS/arm64, 8 MB stack (`ulimit -s 8176`),
2026-09-21. Depth is read from the environment in every probe so that no
argument can be constant-folded -- see 2.4 for why the first attempt, which used
a literal, measured nothing.

```turmeric
(load "stdlib/str.tur")
(load "stdlib/env.tur")
(defn depth [] #fx{Proc} : int (str->int (env/get! "N")))
```

### A.1 -- the matrix

```
pA  self          -O2 n=10000000: 0        -O0 n=10000000: 0
pB  mutual x2     -O2 n=10000000: true     -O0 n=10000000: CRASH (exit=139)
pC  mutual x8     -O2 n=10000000: 0        -O0 n=10000000: CRASH (exit=139)
pD  indirect      -O2 n=10000:    1        -O2 n=1000000:  CRASH (exit=139)
```

Bisected ceiling for the indirect case at `-O2`:

```
indirect tail call: deepest OK ~= 29335, crashes by ~= 33202
```

8 MB / 29,335 is roughly 285 bytes of C stack per level.

Under the interpreter, the indirect probe passes at every depth tried:

```
turi indirect n=10000    1
turi indirect n=100000   1
turi indirect n=1000000  1
```

### A.2 -- the self backedge is real (1, and 2.1's contrast)

```c
static int64_t count_hydown(int64_t n) {
        __tur_tailcall:;
        if ((n) == (INT64_C(0))) { return INT64_C(0); }
        else { int64_t __t305 = (n) - (INT64_C(1)); n = __t305; goto __tur_tailcall; }
}
```

### A.3 -- the first probe was void (2.4)

Depth passed as the literal `1000000`:

```
$ nm mutual_O2.o | grep -i "hyeven\|hyodd"
                                      # nothing: both functions eliminated
$ objdump -d --disassemble-symbols=_main mutual_O2.o | wc -l
43
```

With the depth read from the environment instead, `-O2` still passes -- and the
functions are *still* absent, because the cycle is inlined into a loop in
`main`:

```asm
2c10: cmp  x0, #0x1
2c14: b.eq 0x2c2c
2c18: sub  x0, x0, #0x2
2c1c: cbnz x0, 0x2c10
```

### A.4 -- `match` gets no backedge (2.3)

```turmeric
(defdata Step (More [v : int]) (Done))
(defn loop-match [n : int] : int
  (match (classify n) (Done) 0 (More v) (loop-match v)))
```

```c
case 0: {
    int64_t v_1699 = (int64_t)__scrut->as.More._0;
    int64_t __ps_310 = (loop_hymatch(v_1699));     /* ordinary call */
    if (tur_panicking) return ((int64_t)0);
    __t308 = __ps_310;
    break;
}
```

### A.5 -- a `ref<T>` local kills the backedge (2.2)

```c
tur_frame_push_defer(&__frame_309, __defer_311, &__t312);
int64_t __ps_314 = (loop_hyref((n) - (INT64_C(1))));
if (tur_panicking) { tur_frame_fire_lifo(&__frame_309); return ((int64_t)0); }
__t313 = __ps_314;
tur_frame_fire_lifo(&__frame_309);
```

### A.6 -- the CPS path does not tail-call (2.5)

`pD`, whose indirect calls route through the CPS backend:

```c
static int64_t step_hya__cps(int64_t n, DK *__kont) {
    ...
    int64_t __ps_315 = ((*(tur_thunk_int64_t_int64_t_t *)(...))(..., n - 1));
    if (tur_panicking) return ((int64_t)0);
    __t313 = __ps_315;
    __t0 = __t312;
    return dk_run(__kont, (intptr_t)(__t0));
}
__attribute__((unused)) static int64_t step_hya(int64_t n) {
    __dk_entry_depth++;
    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());
    ...
    if (TUR_SETJMP(__dkjb) == 0) { __r = step_hya__cps(n, __root); }
    ...
}
```

---

## See also

- [r7rs-lang-plan.md](r7rs-lang-plan.md) -- T6 is its prerequisite; D6 there
  defers to this document
- [docs/guides/performance-guide.md](../guides/performance-guide.md) -- the
  self-tail-call section, which A.2 confirms and which T5 would let us extend
- [docs/guides/delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md) -- the CPS/DK machinery A.6 measures

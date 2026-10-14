# `#lang r7rs`: a re-entrant `call/cc` keeps a copy of the stack forever under `--interpret`

**Resolved 2026-10-14: the interpreter frees a continuation nothing can reach.**
The one direction left open below, "Reclaim images", is done. `tur
--interpret` now runs a sweep that finds which continuations a program can
still reach and frees the rest: the stack image, the saved evaluator state,
and the frames and argument lists that were kept so a re-entry could finish
them. See *2026-10-14* at the end for how it works, what it costs and where
it stays off.

| `tur --interpret`, Release | before | after |
| --- | --- | --- |
| 100,000 iterations, each storing its continuation (`(set! saved k)`) | 245 MB | 76 MB |
| the same loop storing a fresh closure instead (the floor) | 54 MB | 56 MB |
| a generator stepped 16,000 times (two captures a step) | 121 MB | 55 MB |

**Narrowed 2026-10-09: a capture no longer pins the frames made after it.**
The 2026-10-05 regression below is fixed: `frame_release` and the driver's
argument accumulators are keyed on a capture EPOCH rather than the pin, so a
program that stores one continuation runs its later loops at the usual
per-step cost (990 MB -> 148 MB on the Debug tur, against 133 MB with no
`call/cc`; `check-turi-frame-reclaim.py` pins it). What is left is what was
left on 2026-09-28 -- the images themselves, and the frames and temporaries
that were live at a capture -- plus the two smaller residues noted at the
end of the 2026-10-09 section.

**Narrowed 2026-09-28.** What is left is one case: under `tur --interpret`,
a `call/cc` whose continuation may outlive the call keeps its stack image for
the life of the process. That is a continuation that is stored, returned, or
handed to the program's own procedures. Everything else here is fixed; see
*2026-09-28* below.

- An escape-only `call/cc` copies nothing on either back end. That is the
  repro, the `for-each` early exit and the named-`let` search.
- The compiled back end no longer grows per call, for `call/cc` or for `guard`.
- The interpreter's pin was measured, and costs nothing that shows.
- The interpreter's images are still never freed, but a capture close to an
  earlier one keeps only the words that differ: a generator step went from
  69 KB to 16 KB, most of which is not the continuation. See *Interpreter
  images kept small* below.

The original title was "every `call/cc` keeps a copy of the stack forever".

**Severity:** medium. Memory grows without bound in a program that calls
`call/cc` in a loop, even when every continuation is only used to escape. A
behavior change from r7rs-lang-plan T5.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (loop i acc)
  (if (= i 100000) acc
      (loop (+ i 1) (+ acc (call/cc (lambda (k) (k 1)))))))
(write (loop 0 0))
```

Peak RSS, measured 2026-09-24:

| back end | 10 calls | 10,000 calls | 100,000 calls |
|---|---|---|---|
| compiled (`tur build`, -O2) | 10 MB | 40 MB | 366 MB |
| interpreted (`tur --interpret`, Debug/ASan) | 111 MB | 1322 MB | -- |

That is about 3.6 KB per `call/cc` compiled, and about 120 KB per call
interpreted, where the C stack under the tree-walker is deep.

## Root cause

A T5 continuation is a copy of the C stack from the `call/cc` to the
stack's base. Nothing ever frees it, and a first capture also switches off
two other kinds of reclamation for the rest of the run:

- **The image.** `r7rs-cont-capture__` (stdlib/r7rs/prelude.tur) and
  `native_r7rs_cont_capture` (src/turi/interpreter_natives.c) malloc the
  `r7k_cont` / `R7kCont` and its stack image. The continuation procedure
  can be stored anywhere, and the program has no collector to say when it is
  gone.
- **DK frames (compiled).** The first capture sets `tur_dk_pinned`
  (src/compiler/emit_dk_runtime.c), so `dk_free` and the reap sweep never
  free a CPS frame again, in any part of the program.
- **Driver temporaries (interpreter).** `turi_cont_pin` (src/turi/eval.c)
  makes `TURI_DRIVE_FREE` a no-op, so argument accumulators and the like are
  never freed again. The interpreter's heap work-stack snapshots and their
  per-re-entry copies leak too.

The pins are what make a re-entry safe: a copied stack may point at any of
that memory. The price is that a Scheme program that calls `call/cc` once
stops freeing that runtime memory.

`guard`, `raise` and the eval bridge use the one-shot escape
`r7rs-call/ec__`, which copies nothing and pins nothing.

## Compiled: resolved by the collector (2026-09-25)

*Corrected 2026-09-28:* only at 100,000 calls. Live data still grew with the
call count, through the DK reap list; see *2026-09-28* below.

The r7rs-gc collector ([docs/archive/r7rs-gc-plan.md](../archive/r7rs-gc-plan.md))
graduated and is on by default for a compiled `#lang r7rs` program: the
image is a collected object, scanned while a continuation refers to it and
reclaimed after, and the pinned DK frames are reclaimed the same way. The
repro above runs in 10 MB compiled (from 527 MB, 0.13 s from 0.40 s). **What
stays open is the interpreter**, which is unchanged: `tur --interpret` keeps
the image and the pinned driver temporaries for the life of the process
(120 KB a call in the table above). The fix directions below are the
interpreter's now.

## Two directions ruled out (2026-09-26)

*The second one landed 2026-09-28* with a wider test, which takes the
`for-each` idiom: see *2026-09-28* below.

- **Taking the image lazily cannot be done in the capture native as it is.**
  `r7rs-cont-capture__` returns before the receiver runs (the prelude's
  `r7rs-call/cc` calls `f` afterwards), so the frame its `setjmp` saved is
  dead by the time anyone could know whether a copy is needed -- even an
  in-extent `(k 1)` must restore the image. Running the receiver INSIDE the
  native (capture, call `f`, copy only if `f` returns normally, pin only
  then) would fix that, but it puts one nested C drive under every
  `call/cc`: R7RS 3.5 requires `call/cc` to call its receiver in tail
  position, and `(define (loop i) (call/cc (lambda (k) (loop (+ i 1)))))`
  runs in constant stack today and would overflow.
- **Lowering an escape-only receiver to `r7rs-call/ec__`** (no copy, no pin)
  is sound when the continuation parameter appears only in operator position
  and never inside a closure-making form (`lambda`, named `let`, `do`,
  `guard`, `delay`, a macro use). But the common escape idiom calls it from
  a `for-each` lambda -- `(call/cc (lambda (return) (for-each (lambda (x)
  (if (p x) (return x))) l) #f))` -- which that test has to refuse, so it
  would fire mostly on the synthetic repro above.

## Fix directions

- **Cheap: don't copy for an escape.** Take the image lazily, or keep the
  one-shot escape as the fast path. A continuation invoked while its
  `call/cc` is still on the stack never needs the copy. The copy is only for
  one that outlives the `call/cc`, which the procedure can find out when it
  is invoked after the return.
- **Pin less.** Pin only the DK frames and driver temporaries that a live
  image can reach, instead of everything after the first capture.
- **Reclaim images.** Free an image when its continuation procedure becomes
  unreachable. That needs the continuation to be a refcounted or traced
  object, which Scheme values are not today.
  ([dynamic-returned-closure-env-is-never-freed](../archive/dynamic-returned-closure-env-is-never-freed.md)
  was the same gap for closures.  It was closed on 2026-09-28 only for a
  closure whose single owner the compiler can prove, a `let` that minted it.
  A continuation procedure stored anywhere is still not that.)

## 2026-09-28

### Escape-only `call/cc` is the one-shot escape, on both back ends

The second direction ruled out above, lowering an escape-only receiver to
`r7rs-call/ec__`, holds up once the test lets two shapes through. Both are
safe because the closure they make cannot get out either:

- a `lambda` written as an argument of a standard procedure that calls its
  procedure arguments and keeps none of them: `for-each`, `map`, their vector
  and string twins, and `call/cc`;
- a named `let` whose name, too, only ever heads a call.

`callcc_escape_only` (src/compiler/scheme_lower.c) holds when every
occurrence of `k` heads a call, and every closure that mentions `k` is one of
those two. The procedure name has to resolve to the standard one: a
program's own `for-each`, or one bound inside the body, does not count. `k`
must not be rebound inside the body. A `lambda` anywhere else, `delay`,
`case-lambda`, an internal `define`, quasiquote or a macro use mentioning `k`
keeps the copying `call/cc`. `dynamic-wind` is left out on purpose: its
`before` thunk runs again on a re-entry, before the stack is restored, where
the escape is not live yet.

A continuation captured *inside* the body and re-entered later still finds
`k` working. The escape's liveness is part of what a capture saves and a
re-entry restores, on both back ends. `tests/fixtures/r7rs-callcc-escape-only`
case 6 is exactly that. Every case in the fixture gives the same answer as a
copy of it in which each `k` is also stored, so that nothing is converted.

Peak RSS under `tur --interpret` (Debug/ASan), 10,000 calls:

| program | before | after | control, no `call/cc` |
| --- | --- | --- | --- |
| the repro above | 320 MB | 194 MB | 148 MB |
| `for-each` early exit | 738 MB | 590 MB | 625 MB |
| named-`let` search | 713 MB | 566 MB | -- |

What stays is the interpreter's process-lifetime policy for closures. The
control loop, which makes the same closures with no `call/cc` at all, is as
large.

### Compiled: the collector did not see the reap list

The 2026-09-25 note below measured 100,000 calls and called the compiled
side resolved. At 2,000,000 calls it was not: live data grew linearly. That
was 76 bytes a call for the copying `call/cc`, 39 for the one-shot escape,
and 138 for a `guard` that never raised. The DK runtime's reap list
(src/compiler/emit_dk_runtime.c) registers each CPS entry's result boxes and
chains, and only the OUTERMOST entry's exit drains it. A loop that runs
inside one CPS entry, as a program using `guard` does, kept every inner
entry's registrations until it ended: 4,000,002 of them for 2,000,000
`guard`s. Each kept what it named alive.

Now each CPS entry records where its registrations start
(`__dk_reap_mark`, emit_cps_ir.c). Under the collector (`TUR_GC_ON`) a
nested exit forgets them (`__dk_reap_drop_to`), and the collector reclaims
whatever nothing else reaches. The outermost exit still frees by hand, as
before, and without the collector the drop does nothing. The list and the
entry depth are per-thread (and per-fiber), so the drop holds with any
number of threads
([dk-reap-list-shared-across-threads](../archive/dk-reap-list-shared-across-threads.md)). Compiled, 2,000,000
iterations:

| per iteration | before | after |
| --- | --- | --- |
| `call/cc` escape | 88 MB, 1.8 s | 10 MB, 0.9 s |
| copying `call/cc` | 162 MB, 5.4 s | 11 MB, 1.9 s |
| `guard`, normal path | 285 MB, 2.8 s | 10 MB, 1.0 s |
| `guard` that raises | 281 MB, 6.0 s | 10 MB, 1.9 s |

`tests/run-r7rs-gc.sh` `reclaim-escapes` runs a million of all three under a
256 MiB address-space limit. Without the drop that loop peaks at 397 MB.

### The interpreter's pin costs nothing measurable

`turi_cont_pin` switches off `TURI_DRIVE_FREE` for the rest of the run after
the first re-entrant capture. A program that captures once and then does
20,000 steps of list work peaked at 511 MB. The same program without the
capture peaked at 532 MB: the same, within noise. The driver temporaries the
pin keeps are small next to what the interpreter keeps anyway. So the
"pin less" direction below, a per-frame capture epoch checked at some 40
free sites, would buy nothing. It is not worth its risk.

### Interpreter images kept small

Freeing an interpreter image needs to know that its continuation procedure
is dead. The procedure is an ordinary closure, and it can be stored anywhere:
a variable, a pair, a record, a vector's malloc'd buffer, another closure's
environment. The interpreter has no collector and does not refcount closures,
so there is no point at which it can know. That is still the "Reclaim
images" direction.  It is the general case of
[dynamic-returned-closure-env-is-never-freed](../archive/dynamic-returned-closure-env-is-never-freed.md),
which was closed on 2026-09-28 only where the compiler can prove the owner.

What the interpreter can do is keep less per image. Measured on a generator
(`call/cc` re-entered twice a step, 16,000 steps, Release): 69 KB a step,
of which 48 KB was stack images, 3.8 KB the snapshots of the driver's heap
work stacks, and 3.8 KB the fresh copies of those a re-entry makes. Three
changes:

- **Delta images** (`r7k_snapshot`, src/turi/interpreter_natives.c).
  Consecutive captures of one stack range are nearly the same: 98.8% of the
  words equal the previous capture's word at the same address. So a capture
  finds a KEYFRAME, the latest whole image of the same range (the same
  bottom address and size, up to eight ranges per thread), and keeps only the
  words that differ from it. A re-entry copies the keyframe back and patches
  those words. A delta is always against a whole image, and keyframes, like
  every image, are never freed, so a delta's key stays valid after the range
  gets a newer keyframe. A capture that differs in more than an eighth of its
  words, or that has no keyframe, is kept whole and becomes the range's
  keyframe. The diff is one pass over the stack, like the copy it replaces.
- **Snapshots of the used part.** `turi_cont_state_capture` copied each
  drive's work stack at its capacity; it now copies its length. The restore
  allocates the capacity, as before.
- **Abandoned work stacks freed.** A re-entry abandons every drive of the
  current form whose frame is below the image's top: the jump copies the
  image over it or leaves it dead below. Each restored drive gets a fresh
  copy of its snapshot, so the heap stacks the abandoned drives held were
  unreachable, and kept. `turi_cont_release_drives` frees them just before
  the jump.

The same generator, peak RSS under `tur --interpret` (Release):

| steps | before | after |
| --- | --- | --- |
| 16,000 | 1131 MB, 1.15 s | 284 MB, 0.68 s |

That is 16 KB a step, from 69. About 13 KB of it is not the continuation's
at all: it is the environments and closures each step makes, which the
interpreter keeps for the life of the process whether or not the program
calls `call/cc`. `tests/fixtures/r7rs-callcc-reentry-many` re-enters
thousands of times, from ranges that alternate and vary in size, and
re-enters an old continuation after hundreds of later captures. It gives the
same answers on both back ends.

### What is left

Only a continuation that may outlive its `call/cc` still has an image, and
under the interpreter nothing frees that image. It is now kept as a delta
when it can be, so a loop of re-entries grows by the words each capture
changed rather than by the stack's depth. A capture that differs a lot
from every recent one, such as a deep walk whose captures are all at
different depths, is still kept whole. Freeing any of it needs the
continuation to be a traced or refcounted object, which interpreter values
are not, so the "Reclaim images" direction is the whole of what remains.

## 2026-10-05: the interpreter's pin now also switches off frame reclamation

The section *The interpreter's pin costs nothing measurable* above no longer
holds. Since
[turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md),
`tur --interpret` hands an activation's frames back when it returns.
`frame_release` checks `g_turi_cont_pinned` and does nothing once it is set,
because a re-entry may complete a call whose frame is in the stack image. So
a program that stores **one** continuation is back to the old ~4 KB per step
for the rest of its run:

| `tur --interpret`, Release, 60,000-element list built and summed | peak RSS |
| --- | --- |
| no `call/cc` | 47 MB |
| no `call/cc`, `TUR_TURI_FRAME_RECLAIM=0` | 167 MB |
| one stored continuation first (`(call/cc (lambda (k) (set! saved k) 0))`) | 214 MB |

`r7rs-control` (343 MB) is this case.

**Direction: a capture epoch instead of a pin.** A re-entry can only
complete activations that were live when the continuation was captured.
Keep a global capture counter, bumped by `native_r7rs_cont_capture`, and
stamp each frame with the counter's value when it is created. `frame_release`
may then free a frame whose stamp equals the current counter: no capture has
happened since the frame was made, so no image holds it. Owned frames get
the same check, each against its own stamp. The same epoch test would let
`TURI_DRIVE_FREE` free a call's argument accumulator when that call began
after the last capture. That is the "per-frame capture epoch" the 2026-09-28
section ruled out as buying nothing. With frames now reclaimed, it buys the
difference between 214 MB and 47 MB above.

## 2026-10-09: the capture epoch, done

`turi_cont_pin` still sets the pin, and now also bumps a counter,
`g_turi_cont_epoch` (src/turi/eval.c, beside the frame allocators). Every
frame is stamped with the counter when it is made (`EvalFrame.epoch`, set by
`eval_frame_new` / `eval_frame_new_call`), and `frame_release` hands a frame
back only when its stamp equals the current counter: no capture has happened
since the frame was made, so no stack image holds its activation. A frame
that was live at a capture keeps an older stamp and is kept for the rest of
the run, as every frame was before.

The driver's per-frame temporaries got the same test where the frame is at
hand. `DRIVE_PUSH` stamps each work-stack entry (`DriveCont.epoch`), and the
argument accumulators of `DK_CALL_ARG`, `DK_BUILTIN_ARG` and
`DK_MAKE_STRUCT` are freed through `TURI_DRIVE_FREE_EP` against the epoch
read at the top of the case -- read BEFORE the leaf call, because the native
that captures is itself such a leaf: the capture re-enters the case with
`top` pointing at the work stack of the capture (since freed or moved by a
later realloc), while a local is restored with the image and names the right
epoch. The first attempt read `top->epoch` after the call and was a
use-after-free in every re-entering fixture.

Why the stamp is enough: an image is a copy of the C stack and of each
drive's work stack as they were at the capture, so the only frames and
accumulators it can name are ones that existed then, and every one of those
carries a stamp older than the counter the capture left behind. A frame made
later is reached only through a closure, a generator or an effect
continuation, which `frame_escape` already marks and `frame_release` already
refuses.

| `tur --interpret`, Debug/ASan (`quarantine_size_mb=0`), 60,000-element list built and summed | before | after |
| --- | --- | --- |
| no `call/cc` | 133 MB | 133 MB |
| one stored continuation first, re-entered once (the list built and summed twice) | 990 MB | 148 MB |

`tests/check-turi-frame-reclaim.py` (ctest `tur_turi_frame_reclaim`) runs
that second program and bounds its growth over idle at twice the plain
program's growth plus 16 MB (50 MB against a bound of 84 today; 890 before).
The 32 `call/cc` / control / generator fixtures agree with their expected
output under `run-turi.sh`.

Two small residues stay with the pin, each conservative rather than wrong:

- The temporaries that are not tied to a work-stack frame -- a perform's
  argument array handed back through `pargs_heap`, the `apply_args` of a
  native-resume fold, a cloned slice -- are still never freed after a
  capture (`TURI_DRIVE_FREE`). None grows with an ordinary loop.
- The one-shot in-place resume of an effect (`ws_case_is_oneshot_resume`)
  stays off once a capture has happened: whether a capture will happen
  inside the case body cannot be known at the perform.

The images are unchanged: a continuation that may outlive its `call/cc`
still keeps its (delta) image, and only a traced or counted continuation
could free it.

## 2026-10-11: re-measured; nothing cheaper is left

Looked at again after the capture epoch. Debug/ASan `tur --interpret`,
`quarantine_size_mb=0`, a loop that stores every continuation
(`(call/cc (lambda (k) (set! saved k) 1))`) against the same loop with a
plain lambda call in its place, and the escape-only form
(`(call/cc (lambda (k) (k 1)))`, lowered to the one-shot escape):

| iterations | control | stored continuation | escape-only |
| --- | --- | --- | --- |
| 10,000 | 100 MB | 128 MB | 109 MB |
| 100,000 | 100 MB | 380 MB | -- |

The control is flat: with the epoch, a program that never captures hands
every frame back. A stored re-entrant continuation costs about 2.8 KB a
capture, linear in the count, and that is exactly the residue the 2026-09-28
and 2026-10-09 sections name: the delta image, the `R7kCont`, and the frame
that was live at the capture -- which the stored `k` can legitimately
complete, so no scope-based rule can free it. Freeing any of it needs to
know the continuation procedure is dead, and interpreter closures are neither
traced nor counted; there is no narrower rule to add. The row stays open
on that one direction ("Reclaim images") and is not a candidate for a quick
fix.

## 2026-10-14: a sweep frees what no continuation can reach

The 2026-10-11 section was right that this needed the interpreter to know
when a continuation procedure is dead, and that interpreter values are
neither traced nor counted. The fix traces them, for this one purpose.
`tur --interpret` runs a SWEEP at a capture once enough continuations are
live. It marks everything the program can still reach and frees every
continuation it did not reach (`r7k_maybe_sweep`,
src/turi/interpreter_natives.c; the marker is `turi_cont_gc_*`,
src/turi/eval.c).

### What a sweep frees

- **The continuation:** its stack image (whole, or a delta), the saved
  evaluator state with its drive-stack copies, and the `R7kCont`. A whole
  image that is still the key of a live delta, or this thread's current
  keyframe for its stack range, is kept until neither holds.
- **What a capture kept from being handed back.** A frame made before a
  capture is not released when its activation ends, because an image may run
  that activation again; neither is a work-stack entry's argument list, or a
  `call/ec` escape boundary, pushed before one. These are now recorded
  (`cont_defer_frame`, `TURI_DRIVE_FREE_OWN`). After a complete marking, each
  one nothing reached goes back to the free lists (`turi_cont_gc_release`).

The procedure value itself (`(fn [& vs] ...)` in the prelude's
`r7rs-call/cc`) and the `let` frame it captures are pool objects. Like every
closure the interpreter makes, they stay until the env is freed. That is the
interpreter's policy for closures (docs/guides/gc-guide.md, "The interpreter
is different"), not this report's; what it costs a Scheme loop is filed as
[turi-dropped-closures-kept-until-teardown](turi-dropped-closures-kept-until-teardown.md).
It is also why the stored-continuation loop above stays above the
stored-closure loop.

### How it finds what is reachable

The marker is **conservative**. Every word it reads is tried three ways:

- as a **continuation handle**;
- as a pointer into the **value pool**. The pool is a bump arena with no object
  boundaries, so an env running `#lang r7rs` now keeps an OBJECT MAP: a bit
  for every 8-byte unit where an allocation starts (value.c,
  `turi_val_starts_*`). A word anywhere inside an object marks the object,
  and all of the object's words are read in turn;
- as a **tracked collection box** (a Vec, Set or Map). Its cells are read through
  the box's TR3 scan callback.

No layout is needed for a frame, a binding, a closure or a struct. An integer
that happens to look like a pointer only keeps something alive. Memory
outside the pool that holds values is read with its type: the drivers' work
stacks and their argument accumulators, the globals, the env's in-flight
values, the defer chain, and the R7RS natives' dynamic-environment slots.

The roots are:

- the C stack from the sweep up to the thread's stack base, with the
  callee-saved registers spilled into a frame the scan covers;
- every live drive. Entries `[0, len)` are read with their type. The rest of a
  drive's inline buffer is left out of the stack scan: it holds stale entries
  of evaluations that have finished;
- the globals, the env's return, throw and abort values, and the defer chain.

A reached continuation is read the same way. That means its image (a delta
is rebuilt into the bytes it would restore), the registers saved in its
`jmp_buf`, and its saved state, its drives handled like the live ones.

**Handles.** A program no longer holds an `R7kCont` address. It holds a slot
in a table plus the slot's generation, as `(gen << 32) | ((slot + 1) << 2)`;
bit 0 is still the prelude's re-entry tag. A sweep recognises a handle
wherever one is stored. Freeing a continuation bumps its slot's generation,
so a handle that outlived its image names nothing. Restoring it is then an
error that names this report, not a jump into freed memory. That error is
what the stress test below looks for.

### Stale words: the one trap

The first version marked almost everything. Each image chained to the one
after it: of 256 continuations, a sweep freed 9. An image is the C stack as
it was at the capture. A slot the frames live then never wrote still holds
whatever an earlier, finished call left there. Very often that is the address
of a frame or binding that was on the free list at the time and has been
handed out again since, to the very next `call/cc`'s `let`, whose binding
holds the next continuation's handle. So words read **out of an image**, or
out of its `jmp_buf`, are filtered:

- A frame **made or handed out again since the capture** is skipped. Every
  frame carries the epoch it was made in, and a frame with the capture's
  epoch or later is not the frame the image saw.
- A **binding** is skipped outright. A live binding hangs off its frame, which
  the image names in its drives and its evaluator's locals, and is reached
  through it.
- A continuation **captured after** the image is skipped.

The object map marks frame and binding starts. Those are the kinds the
interpreter reuses; the object's extent tells the two apart. With the filter,
a sweep of the loop above frees 253 of 256.

### When it runs, and what it costs

A sweep runs at a capture once three things hold: 512 captures have passed
since the last sweep, the live count has doubled from what the last sweep
left, and the captures since have made about twice what the last sweep read
(counted at 1 KB a capture). A sweep walks everything reachable: some 300 KB
for a small program, mostly the prelude's globals, about 0.4 ms on a Release
`tur`. The three rules keep that a small, constant cost per capture, whether
continuations live long or the program's heap is large: a generator over a
big list would otherwise walk the list every few hundred steps. The two
capture-heavy loops above run about 10-15% slower than before. The object map
costs a Scheme program about 2-4% more memory whether or not it captures (a
`guard` loop, which never captures: 216 MB -> 225 MB). Turmeric programs keep
no map.

### Where a sweep frees nothing

A sweep does not guess. It records a DOUBT, and a sweep with a doubt frees
nothing that time. The doubts:

- a non-empty Set or Map is reachable (its entries are untyped);
- a work-stack entry is of a kind the marker does not read (effects,
  generators, STM, `catch-unwind`, native resumes);
- an effect handler, catch, reset or generator is live, or is saved in a
  reached continuation;
- a reached continuation saved a defer.

None of these comes up in a Scheme program that uses only R7RS and the SRFIs.
The sweep is off altogether:

- when the program imports a Turmeric module or a library of its own. Natives
  can then keep values in memory the marker cannot read
  (`g_scheme_lower_opened_seam`, set by the Scheme lowering);
- when the program has fibers, generators or futures (SRFI 18 threads);
- under a debugger;
- while a continuation of another env or thread is live;
- in an env whose object map started after the env had evaluated something:
  a REPL that switched to `#lang r7rs` mid-session.

`TUR_TURI_CONT_GC=0` turns it off. `TUR_TURI_CONT_GC_TRACE=1` reports each
sweep on stderr. `TUR_TURI_CONT_GC_STRESS=1` sweeps at every capture, for the
tests.

Two imprecisions are left. Neither frees anything wrongly; both only keep
something too long. A stale word in an image that points at an object
allocated fresh since the capture (rather than handed out again) is followed.
A stale word in the *live* stack keeps what it names until the next sweep.

### Tests

- `tests/fixtures/r7rs-callcc-reclaimed-images` stores a continuation in
  each kind of place a program can keep one: a vector, a nest of pairs, a
  record, a closure, a promise, a parameter object, the argument list of a
  call in progress, another continuation's image, an exception payload, the
  dynamic environment, and a SRFI 69 table. It also re-enters `map` part-way.
  Each case makes captures it drops at once, so sweeps run in between, then
  re-enters the stored continuation. Both back ends print the same.
- `tests/check-turi-callcc-reclaim.py` (ctest `tur_turi_callcc_reclaim`) runs
  that program with a sweep at every capture: 2,024 sweeps, none declined,
  2,008 continuations freed, every answer right. It then bounds a loop that
  stores every continuation: its growth must stay within twice that of the
  stored-closure loop plus 16 MB, and be at most half its growth with the
  sweep off. Debug: +26 MB, against +14 for closures and +123 with the sweep
  off.
- With one of the marker's inputs disabled on purpose (vector cells not
  read), the stress run stops at the first case with the restore's error.
  The test can see a missed reference.
- All 2,769 `run-turi.sh` fixtures and 3,796 `run.sh` fixtures pass. Under
  `run-turi.sh` with a sweep at every capture, every Scheme fixture but the
  new one passes as well. The new one times out there, at its full size,
  because 8,000 sweeps do not fit in 15 seconds; the check script runs it
  smaller.

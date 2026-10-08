# Turmeric Godot Binding -- Status Refresh, JIT Concerns, and Un-stranding Plan

> **Status:** **complete** -- Steps 0-4 landed 2026-09-07: the branch merged as
> [turmeric-godot#1](https://github.com/turmeric-lang/turmeric-godot/pull/1), and
> that repo got its **first passing CI run**, green on all four platforms.
> Step 5 landed 2026-09-10, as neither (a) nor (b) -- per J1, declarations are
> needed on every route: an exported C ABI (`native_abi.h`, 122 entry points)
> plus a staged `tg-godot` declarations module, so engine-touching scripts
> AOT-compile and run (turmeric-godot#2/#3, macOS; Windows via turmeric-godot#4
> and this repo's `88197f74f`). Step 6 ran 2026-09-08 as a stand-in-plugin probe
> on Windows, not the real shim in Godot: a JIT `libturi` links into a plugin,
> MIR runs there, and JIT'd code calls the plugin's exports. The remainder is
> tracked in open reports: five AOT gaps (variadic `godot-call`, unstaged
> facade, `godot-connect-typed`, E0711 in a hand-written `defmodule`, compile
> cost) in [godot-aot-staged-build-lacks-godot-natives](../reported/godot-aot-staged-build-lacks-godot-natives.md);
> the real shim in Godot (SConstruct cannot link `libtur_mir.a`), macOS W^X and
> fixture parity in [jit-godot-embedding-spike](../reported/jit-godot-embedding-spike.md).
> J7's open, shim-side threading half is filed as
> [godot-shim-threading-under-workerthreadpool](../reported/godot-shim-threading-under-workerthreadpool.md).
> Archived 2026-09-28.
> **Last Updated:** 2026-09-28
> **Type:** Integration / Game Engine -- post-v1.
> **Does not supersede** [godot-language-binding-plan.md](../archive/godot-language-binding-plan.md);
> that plan's v1 scope really is complete and stays archived. This one covers
> what happened after it, which no document currently describes.

---

## Why this exists

The v1 binding plan was last updated **2026-06-28** and closed as complete. In
the ten weeks since:

- A **Windows port landed in `../turmeric-godot`** across five commits and was
  never merged, never reviewed, and never built by CI.
- **CI has never passed on this repo. Not once**, on any platform, in its
  entire history.
- The compiler moved **four releases** (v0.42.2 -> v0.44.2) plus the whole
  Windows queue, and the binding has not been built against any of it.
- Two `docs/reported/` entries describe the binding, and **one of them is
  wrong** -- it reports as open a bug that was fixed a month ago.

The engineering is in better shape than the repo's signals suggest. The signals
are the problem.

---

## Where the binding actually is (2026-09-06)

### Verified working

Windows bring-up finished on branch `windows-support`, 5 commits ahead of
`main`, all pushed:

| Commit | What it established |
| --- | --- |
| `1851569` | Builds with MinGW. AOT layer ported: `cmd.exe` quoting, `WEXITSTATUS` (absent on MinGW), `mkdir_p` across drive roots, `realpath` -> `_fullpath` |
| `d715fb6` | 29 natives converted to `turi_register_default_native_typed` -- they were invisible to the elaborator, producing ~40 spurious `TUR-W0040` warnings |
| `516bf0b` | Prelude declares `ResourceHandle` / `SceneTreeHandle`. All three paddle-pong scripts load clean: 0 eval failures, 0 warnings |
| `d5fa0a0` | `#mode` directive stripped before the reader sees it -- the documented per-script mode knob had never worked on any platform |
| `0e23951` | `.gitattributes` eol=lf (the `.tur` scripts feed a reader that copies inline-C verbatim into generated C) |

Net: the GDExtension builds with MinGW, initializes in stock Godot 4.3.stable,
registers language + resource format, uninitializes clean, exits 0, and every
demo script evaluates.

`main` is fully contained in the branch. Landing it is a fast-forward.

### Stranded

- **No PR has ever been opened on `rjungemann/turmeric-godot`.** All-time count:
  zero.
- **CI has never passed.** Three runs, three failures. The current one dies in
  ~6 seconds on all four matrix legs with an identical error:

  ```
  Repository path '/home/runner/work/turmeric-godot/turmeric'
    is not under '/home/runner/work/turmeric-godot/turmeric-godot'
  ```

  `actions/checkout` is given `path: ../turmeric` and refuses to write outside
  the workspace. The consequence is not "Windows is unverified" -- it is that
  **Linux and macOS are equally unverified**, and have been since the workflow
  was written.
- **The sibling checkout is unpinned.** No `ref:`, so it floats to turmeric
  `main`. Trowel pins a released tag; this does not.

### Stale signals to clear

1. `docs/reported/godot-baked-in-prelude-fails-to-eval.md` says "Bug 1 fixed;
   bug 2 diagnosed but NOT fixed." Bug 2 -- the `SceneTreeHandle` /
   `ResourceHandle` forward reference -- was fixed by `516bf0b` on 2026-08-04.
   Both bugs are closed; per the archiving rule this belongs in `docs/archive/`.
2. `turmeric-godot/README.md` says "AOT mode and the paddle-pong demo are still
   pending." Both shipped.
3. `examples/paddle-pong-tur/aot2.{err,log}` sit untracked, timestamped
   22:09 Aug 4 -- *before* the two commits that fixed the very errors they
   record. They read as a current failure and are not one.

---

## The one real product defect

**[godot-aot-staged-build-lacks-godot-natives](../reported/godot-aot-staged-build-lacks-godot-natives.md)**
-- high, AOT-only, not platform-specific.

The AOT path stages a script into a transient project and compiles it with
standalone `tur`. Every `godot-*` name is a C++ function the GDExtension
registers into the *interpreter* env at run time, and standalone `tur` has never
heard of any of them. The staged build dies at the first one:

```
error: unknown function or operator 'godot-export'
```

Any script that touches the engine -- which is every useful script -- cannot be
AOT-compiled. The interpreter path is unaffected and works. This was found on
Windows only because that is where AOT was first driven end to end.

---

## JIT concerns

> **Audited 2026-09-07, one day after filing. Three of the ten were wrong:
> J1, J2 and J4.** All three failed the same way -- reasoned from a true premise
> to a conclusion I never checked against the tree, then stated with more
> confidence than the evidence supported. J1 and J2 were settled by running a
> three-line program; J4 by reading two paragraphs of a plan I had already
> cited. Each is corrected in place, with the original claim quoted, rather than
> quietly rewritten.
>
> Scoring the rest honestly: **J6, J8 and J10 verified** against
> `jit-guide.md:325`, `src/CMakeLists.txt:604` and `cmake/mir.cmake`
> respectively. **J3's facts hold** but its corpus figure is point-in-time, not
> standing. **J9** points at a real open report.
>
> **J5 and J7 have since been verified too, and both moved.** J5 got *worse*:
> MIR's interpreter tier is not a W^X escape hatch, because it publishes native
> shims through the same `MAP_JIT` allocator -- so a locked-down platform has no
> degraded JIT mode, only the interpreter or AOT. J7 got *better in one half and
> sharper in the other*: MIR-gen's thread-unsafety is real but **already solved**
> in-tree by `jit_lazy_gen_locked`, so the binding inherits a JIT that
> serialises generation; what is actually open is shim-side state under Godot's
> worker threads. As filed, J7 would have sent someone to re-investigate a
> solved problem -- a vague concern is not harmless, it misdirects.
>
> Final tally: **3 wrong (J1, J2, J4), 5 verified sound (J3 with a caveat, J6,
> J8, J9, J10), 2 verified-and-materially-revised (J5, J7).** Only five of ten
> survived unchanged.
>
> The pattern worth carrying forward: the wrong entries are the ones asserting
> what *would* happen; the sound ones cite a file and a line. Anything below
> phrased as a mechanism rather than a citation should be treated as a
> hypothesis until it is run.

The open spike [jit-godot-embedding-spike.md](../reported/jit-godot-embedding-spike.md)
asks whether the shim should compile in-process rather than shelling out to
`tur build --shared`. Several of its premises have changed, and several of its
open questions now have partial answers. Recorded here because the spike doc
still reads as if the Windows JIT were unexplored.

### J1 -- CORRECTED 2026-09-07: the JIT does *not* dissolve the AOT defect

> **This section was wrong when written, and the error mattered**, because it
> made the JIT look like a way to skip the work in step 5 rather than a
> different way to finish it. Corrected here rather than deleted, so the bad
> reasoning stays visible.
>
> The original claim: the natives are C++ functions in the host process, a
> subprocess can never see them, so in-process compilation "dissolves the defect
> rather than working around it."
>
> The premise is true and the conclusion does not follow. **The failure is at
> elaboration, not at symbol resolution.** Same three-line program:
>
> ```
> tur build  -> error: unknown function or operator 'godot-export'
> tur jit    -> error: unknown function or operator 'godot-export'   (identical)
> tur --interpret -> warning TUR-W0040 ... will runtime-dispatch     (defers)
> ```
>
> The runtime-dispatch fallback lives in a branch
> [elab_call.c:3726](../../src/compiler/elab_call.c) labels `eval mode`, and
> `g_interpret_mode` is set by `cmd_eval_h`, `cmd_eval_expr` and `cmd_repl` --
> not by `cmd_jit`. The JIT elaborates in compiled mode and hard-errors the same
> way. Being in the right address space does not teach the elaborator a name.
>
> **Declarations are required on every route.** What the JIT still buys is
> narrower and still real: no external toolchain on the player's machine, and no
> link step. That is an argument about *distribution*, not about *this defect*.

The work that every route needs, and that should be costed before choosing one:
~90 exported C entry points with legal names and concrete signatures, plus a
generated declarations file staged with the source. The natives cannot be linked
as they stand -- each is
`static TuriValue tg_native_export(TuriEnv *, TuriValue *, uint32_t, void *)`,
the interpreter's ABI, file-local.

The declaration half is confirmed cheap: `extern-c` takes a hyphenated name and
mangles `-` to `_`, so `(extern-c godot-export [name :cstr ty :cstr dflt :float] :void)`
emits `extern void godot_export(const char *, const char *, double);` and a
plain typed call. The staged project therefore gets *real types*, not the
interpreter path's `:int`-shaped dynamic dispatch.

### J2 -- CORRECTED 2026-09-07: `dlsym` resolution is fine; the invented problem was mine

> **Also wrong, and wrong the same way as J1** -- asserted from a plausible
> premise without checking what the compiler emits.
>
> The original claim: the natives are registered by *string name* into the
> interpreter env, `godot-export` "is not an exported C symbol, and could not be
> one -- it is not a legal C identifier", so JIT'd code needs "a dispatch shim
> that looks the name up in the env", called out as "the first real design
> question".
>
> The first half is true. The conclusion is not. The compiler **mangles `-` to
> `_`**, so a declaration
>
> ```turmeric
> (extern-c godot-export [name :cstr ty :cstr dflt :float] :void)
> ```
>
> emits an ordinary, perfectly linkable symbol:
>
> ```c
> extern void godot_export(const char *, const char *, double);
> godot_export("vel-x", "float", 240.0);
> ```
>
> `dlsym` finds `godot_export` like any other symbol. There is **no env-lookup
> dispatch shim to design** -- that was an invented problem, and it made the JIT
> route look harder than it is.
>
> What is actually required is the J1 correction's work and nothing more: the
> shim must *export* C entry points under those mangled names, because today's
> natives are file-local `static` functions in the interpreter's
> `TuriValue`-based ABI. Once they exist, all three resolution strategies (link,
> `dlopen` fixup, `dlsym(RTLD_DEFAULT)`) are ordinary.

### J3 -- The Windows sequencing gate the spike names is now cleared

The spike says to sequence `jit-windows-support-spike.md` first. That spike has
been run (2026-08-05) and its follow-on defects fixed since:

- `jit-win-prelude-shadows-user-fn` -- fixed; `jit_prelude_win_shadowed` is on
  `main` in `src/jit_engine.c`.
- `jit-c2mir-implicit-decl-truncates-pointers` -- `strtok`/`strpbrk`/`memchr`
  returning 32-bit-truncated pointers; fixed, and `emit_module.c` on `main` now
  emits explicit declarations for all three.
- `jit-s2-split-disengages-on-hoisted-inline-c-include` -- resolved, archived.
- Windows JIT corpus measured **2702 pass / 0 fail** on 2026-09-06.

The spike's "compounds risk rather than avoiding it" caveat no longer applies
the way it did.

Two caveats on the above, from re-auditing it 2026-09-07:

- **That corpus figure is a point-in-time measurement, not a standing
  guarantee.** Fixtures are added continuously, so treat it as "the corpus was
  green on that date" and re-measure before relying on it. Per
  [jit-suite-reports-pass-when-the-engine-is-disabled](../archive/jit-suite-reports-pass-when-the-engine-is-disabled.md)
  (J9), also confirm the engine actually *engaged* -- a wholesale fallback to
  `cc` reports green.
- **Those first two reports are correctly still open**, and this bullet
  originally said the opposite. Both index rows lead with "RESOLVED" / "Fixed",
  both fixes really are on `main`, and I moved both to `docs/archive/` on that
  basis -- then read the bodies and moved them back.
  `jit-win-prelude-shadows-user-fn` carries a section titled "Still open: the
  two mechanisms disagree about what is declared" (nothing keeps
  `JIT_PRELUDE_WIN` and `mangle.c`'s libc denylist in step, so a name added to
  the prelude silently re-opens the hole), and
  `jit-c2mir-implicit-decl-truncates-pointers` says its list of three functions
  is explicitly "a lower bound". A fixed *symptom* is not a resolved *report*.

### J4 -- CORRECTED 2026-09-07: the universal fallback is the INTERPRETER, not AOT

> The original claim: iOS bans JIT and Web has no JIT, both have parked plans,
> therefore "AOT must still exist" and `aot_cache.cpp` cannot be deleted.
>
> The two premises are true. **The conclusion is not**, and the parked plans I
> cited say so in as many words -- I cited them without reading them.
> [godot-binding-ios-plan.md](../upcoming/hold/godot-binding-ios-plan.md):
>
> > iOS does not permit dlopen of arbitrary `.dylib` files in App Store builds.
> > **AOT-as-shipping-shared-libs is not viable** on iOS. Practical answer: ship
> > interpreter mode only; OR statically link every AOT script into the app
> > binary at export time.
>
> So iOS cannot run today's AOT path either -- it is `dlopen`-based, and that is
> exactly what is prohibited. The plan's own recommendation is
> **interpreter-only on iOS for v1.x**.
>
> [godot-binding-web-plan.md](../upcoming/hold/godot-binding-web-plan.md) is different again:
> an AOT path there is "doable", but as per-script `.wasm` artifacts through
> Godot's own loader -- a different mechanism, not this cache.

The corrected picture, which is more useful than the one it replaces:

| | JIT | today's AOT (stage + subprocess + `dlopen`) | interpreter |
| --- | --- | --- | --- |
| Desktop | yes | yes | yes |
| iOS | no | **no** -- `dlopen` prohibited | yes |
| Web | no | not this mechanism; would be `.wasm` per script | yes |

**The thing that must survive everywhere is the interpreter.** Today's
`aot_cache.cpp` is already desktop-only, whichever way the JIT question goes --
so "can the cache be deleted?" is a desktop-scoped question about build-time
cost and toolchain dependence, not a portability constraint. That is a smaller
and more answerable question than the one J4 originally posed.

### J5 -- W^X inside a host process is a shipping question, not a dev question

Allocating executable memory inside an application the JIT does not control is
different from doing it in `tur`:

- **macOS:** Godot's export templates are pre-signed. A JIT needs
  `com.apple.security.cs.allow-jit`, and adding an entitlement to a template you
  did not sign means re-signing it. This interacts directly with T5.A of the
  [shipping-breadth plan](../upcoming/hold/godot-binding-shipping-breadth-plan.md).
- **Windows:** EDR and antivirus reaction to RWX pages in a game process.

Neither is answered by `tur`'s own JIT working, because `tur` is a developer
tool users trust differently from a game.

**VERIFIED 2026-09-07, and one hoped-for escape route is already closed.** The
entitlement/W^X exposure is documented rather than speculative: `tur jit` always
generates machine code, and the obvious fallback -- running MIR's interpreter
tier on a locked-down platform -- does not exist as an out.
[docs/guides/jit-guide.md:107](../guides/jit-guide.md) is explicit that
`MIR_set_interp_interface` "still publishes native shims through the same
`MAP_JIT` code allocator, so it carries the identical entitlement and W^X
profile as the generator and is **not an escape hatch on locked-down
platforms**" (`TUR_JIT_GEN=interp` is spike instrumentation only; an interpreter
tier was evaluated and not adopted -- `docs/archive/mir-interp-tier-plan.md`).

So on a platform that refuses W^X there is no degraded-but-working JIT mode to
fall back to. The fallback is the tree-walking interpreter or AOT, which is the
same conclusion J4 reaches from the other direction.

### J6 -- `constructor` attribute

c2mir discards it, so the embedding path must call `__tur_static_init`
explicitly. The shim's init ordering needs a defined home for that call.

### J7 -- Threading. VERIFIED 2026-09-07: the MIR half is already solved; the open half is ours

Filed as "MIR context reentrancy under concurrent compile or execution is
unverified". Now verified, and it splits cleanly in two.

**The MIR half is real and already handled -- do not go re-investigate it.**
MIR-gen genuinely is not thread-safe, and the tree measured it rather than
assumed it: "three different assertions across five runs of one fixture
(`destroy_func_cfg`, `mark_unreachable_bbs`, `undeclared reg N of func`)"
([src/jit_engine.c:352](../../src/jit_engine.c)). Turmeric therefore does *not*
use `MIR_set_lazy_gen_interface`; it reimplements the lazy path as
`jit_lazy_gen_locked` from public primitives with a process-wide mutex, where
"the double-check inside the lock is the load-bearing half" -- a plain mutex
still lets two threads past the stub generate the same function twice and trip
`_MIR_duplicate_func_insns`
([docs/guides/jit-guide.md:273](../guides/jit-guide.md)). Contention is
self-extinguishing: a function generates once, then its thunk goes straight to
code.

So a Godot host inherits a JIT that already serialises generation. Filed as
written, J7 would have sent someone to investigate a solved problem.

**The half that is genuinely open is shim-side, not MIR-side.** Godot dispatches
from `WorkerThreadPool` and the physics thread. The variant arena is
`thread_local` and its enter/leave bracketing is per-call, which is the right
shape -- but whether the *interpreter env*, the export/signal tables and the
`.godot/turmeric-cache` bookkeeping tolerate concurrent entry is untested, and
that is turmeric-godot's code, not MIR's. That is the question to settle before
the binding runs anything beyond `_ready`.

### J8 -- The no-JIT build must keep working

The JIT is opt-in at `libturi` build time. `TUR_HAVE_JIT` exists precisely for
this fallback shape, and the GDExtension statically links whatever `libturi` it
is given. A JIT path must not become a hard requirement.

### J9 -- Measurement integrity

[jit-suite-reports-pass-when-the-engine-is-disabled](../archive/jit-suite-reports-pass-when-the-engine-is-disabled.md)
is open: `run-jit.sh` can report PASS when the engine is disabled tree-wide. Any
"the JIT works in Godot" claim must confirm the engine actually *engaged* --
a TUR-W0070 fallback to `cc` looks like success from the outside. This exact
false-positive shape has already cost time once on the Windows JIT work.

### J10 -- Aggregate ABI: mostly already paid for, do not un-pay it

The MIR pin (`07ad0148`, `cmake/mir.cmake`) carries fixes that bear directly on
a Godot binding:

- `472fa4c6` -- aarch64 AAPCS64 HFA passing. MIR passed every aggregate <= 16
  bytes in `x0..x7` where a conforming compiler uses `v0..v7`. The comment names
  the exact shape: "`struct { float x, y; }` vector APIs." That is `Vector2`.
  Self-consistent within pure-JIT code, silently wrong the moment JIT'd code
  calls a natively compiled function -- data-dependent wrong answers, no
  diagnostic.
- `07ad0148` -- win64 lazy-generation wrapper ABI, which is what kept the
  default `TUR_JIT_GEN=lazy` unusable on Windows.

Two consequences. First, **do not revert the pin**: the file warns that turmeric
deleted the refusals that used to catch the HFA shape, so an older MIR
reinstates the miscall silently. Second, a mitigating design note -- the bridge
marshals through tagged `int64` arena handles rather than passing aggregates by
value, so the JIT boundary is mostly scalar and largely sidesteps this class
anyway. That is worth *confirming* rather than assuming, since it is the main
reason to expect Vector2-heavy scripts to behave.

---

## What actually happened (2026-09-07)

Steps 0-4 ran as written and the branch merged. Recording the deltas, because
two of the three CI diagnoses were things this plan did **not** anticipate, and
one prediction was simply wrong.

**The prediction that was wrong.** Step 3 said to budget for Linux and macOS
drift, since the last verified build was ~Aug 5 and the compiler had moved four
releases. **No drift materialised** -- all three desktop platforms built clean
against the pinned v0.44.2 on the first run that got past checkout. The pin was
still worth adding; the fear was not.

**Three CI cycles, three distinct diagnoses**, none reachable without running:

1. `actions/checkout` refusing `path: ../turmeric` -- as step 1 predicted. The
   finding underneath it was bigger than the fix: *no* platform had ever been
   verified, so Windows was never specially broken.
2. **MSVC rejecting `-Wextra`** (`cl : error D8021`). The workflow had no MSYS2
   at all and built with Visual Studio, while the port targets MinGW/UCRT64 --
   which cannot work when `libturi.a` is linked statically. Fixed with
   `msys2/setup-msys2`, `-G Ninja`, pacman scons, and `use_mingw=yes` (godot-cpp
   picks MSVC whenever `not use_mingw and msvc.exists(env)`).
3. **The drive letter did not survive the MSYS2 seam.** `TURMERIC_ROOT` was
   `${{ github.workspace }}/turmeric`; SCons saw
   `\a\turmeric-godot\turmeric-godot/turmeric/...` with `D:` gone. This one
   camouflaged itself: a leading `\` is drive-RELATIVE on Windows and the
   checkout was already on `D:`, so SConstruct's own `os.path.isfile()` probe
   resolved it, found the archive, and printed its reassuring "linking libturi
   from ..." line. Everything compiled. Only SCons's node layer disagreed, 21
   minutes in, at the final link. Fixed by making the path relative -- no drive
   letter to lose.

**A process note worth keeping.** After cycle 1 the Windows toolchain mismatch
was already visible in the workflow, and the temptation was to fix it blind in
the same commit. Not doing so was right: guessing at MSYS2 package names would
have produced an untested change, where one CI cycle produced
`cl : command line error D8021: invalid numeric argument '/Wextra'` -- a
one-line diagnosis. When a cycle is ~20 minutes and the alternative is a guess,
spend the cycle.

---

## Suggested sequence for un-stranding the branch

Ordered by dependency and by risk retired per unit of effort. Steps 1-3 are
cheap and remove false signals; step 4 is the actual blocker.

> Steps 0-4 are **done**. Kept as written, rather than rewritten in the past
> tense, so the reasoning can be checked against the outcome above.

### Step 0 -- Clear the false signals (minutes, no risk)

- Delete `examples/paddle-pong-tur/aot2.{err,log}`.
- Archive `docs/reported/godot-baked-in-prelude-fails-to-eval.md` to
  `docs/archive/`, noting `516bf0b` as the fix for bug 2; drop its
  `docs/reported/README.md` row.
- Update `turmeric-godot/README.md`: AOT and paddle-pong shipped; point the plan
  link at this document.

Do this first precisely because none of it is interesting. It is what stops the
next reader from re-diagnosing a solved bug.

### Step 1 -- Fix the CI checkout

Check the sibling repo into a path *inside* the workspace and point
`TURMERIC_ROOT` at it, rather than `path: ../turmeric`:

```yaml
- name: Checkout sibling turmeric
  uses: actions/checkout@v4
  with:
    repository: rjungemann/turmeric
    ref: v0.44.2          # pin; do not float to main
    path: turmeric
```

...with the `working-directory:` and `TURMERIC_ROOT` references updated to
match. **Pin the ref while here** -- an unpinned dependency is how a green build
turns red for reasons that have nothing to do with the commit under test.

### Step 2 -- Open the PR

`windows-support` -> `main`. Clean fast-forward. This is the step that has been
missing for a month, and it costs nothing.

### Step 3 -- Let CI report, then fix what it finds

This is the first time any platform will have been verified. **Expect drift:**
the last verified build was against turmeric as of ~Aug 5, and the compiler has
since moved v0.42.2 -> v0.44.2 plus the entire Windows queue. Trowel needed
real source changes for exactly this reason -- v0.44.0's top-level trace
instrumentation changed behaviour it had encoded.

Budget for Linux/macOS breakage here, not just Windows. They have had no
coverage either.

### Step 4 -- Merge

The Windows port lands, and the repo has a passing build for the first time.

### Step 5 -- Then the AOT natives defect

Highest-severity open item, and the first thing that is product-facing rather
than infrastructural. Decide deliberately between:

- (a) teach standalone `tur` the `godot-*` natives -- a second registration
  surface that will drift from the shim's, or
- (b) treat J1/J2 as the answer and compile in-process.

Do not start (b) as a refactor. Do it as the spike's own method says: a scratch
branch, one script, `tur_jit_compile_image` + `tur_jit_image_sym`, bypassing
`aot_cache` entirely.

### Step 6 -- Then, and only then, the JIT spike's question 1

Cheapest possible probe, and it gates everything else in the JIT direction:
rebuild `libturi` with `-DTUR_JIT=ON`, relink the shim, load it in Godot on
Linux or macOS. It either loads or it does not. Per J9, confirm the engine
*engaged* rather than falling back.

**Explicitly not before step 4.** Compounding an unmerged branch with an
unproven compilation path is how the current situation arose.

---

## Related

- [godot-language-binding-plan.md](../archive/godot-language-binding-plan.md) -- the completed v1 plan.
- [godot-binding-aot-plan.md](../archive/godot-binding-aot-plan.md) -- the AOT design this refreshes.
- [jit-godot-embedding-spike.md](../reported/jit-godot-embedding-spike.md) -- the open spike J1-J10 annotate.
- [jit-windows-support-spike.md](../archive/jit-windows-support-spike.md) -- run 2026-08-05; its verdict feeds J3.
- [godot-binding-shipping-breadth-plan.md](../upcoming/hold/godot-binding-shipping-breadth-plan.md) -- T5.A signing intersects J5.
- `cc-path-preamble-split-plan.md` -- the split runtime the Windows JIT spike
  named as its route around the MinGW header wall. Deliberately not linked: it
  lives on the in-flight `cc-preamble-split` branch and is not on `main` yet.

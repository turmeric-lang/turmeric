# turmeric-godot: baked-in prelude fails to eval, so no script ever loads

> **RESOLVED 2026-09-07. Both bugs fixed and shipped.**
>
> Bug 2 below still reads "NOT fixed"; that text is out of date and is kept for
> the investigation trail. It was fixed the same night it was diagnosed, by
> `516bf0b` in `../turmeric-godot` ("Fix the prelude's forward-referenced handle
> types"), which declares `ResourceHandle` / `SceneTreeHandle` in the prelude
> ahead of the facade -- fix direction 1 of the two proposed below. All three
> paddle-pong scripts then load with 0 eval failures and 0 warnings.
>
> Archived only now, and the delay was deliberate rather than an oversight:
> `516bf0b` sat on an unmerged branch for a month, so the repo's `main` still
> carried the bug and archiving would have claimed a fix that had not shipped.
> It landed with
> [turmeric-godot#1](https://github.com/turmeric-lang/turmeric-godot/pull/1)
> on 2026-09-07 -- which is also the first CI run that repo ever passed, on all
> four platforms. See step 4 of
> [godot-binding-refresh-plan.md](godot-binding-refresh-plan.md).
>
> Still open, and unaffected by this:
> [godot-aot-staged-build-lacks-godot-natives](../reported/godot-aot-staged-build-lacks-godot-natives.md).
>
> **INVESTIGATED 2026-08-04.** Root cause found -- it is **two** bugs, both in
> `../turmeric-godot`, neither Windows-specific and neither a libturi
> regression. The original report below is kept for the ruled-out list.
>
> ### Bug 1 -- 29 natives were invisible to the elaborator (FIXED)
>
> `turi_register_default_native` records only `{name, fn, ud}` for the
> *runtime*. `turi_register_default_native_typed` additionally calls
> `tur_native_sig_register`, and **that** is what makes a name visible to the
> elaborator ([src/turi/env.c:112-142](../../src/turi/env.c)). The shim had
> converted 61 registrations to the typed variant and left 29 behind, so those
> 29 did not exist as far as elaboration was concerned.
>
> This exact class of bug was found and fixed once before -- the typed variant
> was introduced *as* the remedy. See
> [docs/archive/untyped-native-registration-blocks-curated-facades.md](../archive/untyped-native-registration-blocks-curated-facades.md).
> The conversion was simply incomplete.
>
> The correlation was exact: every name that errored used the untyped call;
> none of the 61 typed ones did. Fixed by converting all 29 with
> `TUR_NRT_INT`, which
> [src/runtime/globals.h:284](../../src/runtime/globals.h) documents as
> "matches the historical untyped behavior" -- so this changes *visibility*,
> not typing, including for the five natives commented `// dynamic`.
>
> Result: all ~40 `TUR-W0040 unknown name 'godot-*'` warnings are **gone** (40
> -> 0). This confirms the original report's guess that the warnings and the
> prelude failure shared a root cause -- but only partly, because:
>
> ### Bug 2 -- prelude/facade evaluation order (OPEN)
>
> With bug 1 fixed the prelude still fails, now with a specific and much more
> useful error:
>
> ```
> [turmeric res://scripts/ball.tur:<eval>:242 ] unsupported return type keyword
>   'SceneTreeHandle': it is not a built-in type and is not bound by any parameter
> [turmeric res://scripts/ball.tur:<eval>:308 ] unsupported return type keyword
>   'ResourceHandle': ...
> ```
>
> The prelude uses `SceneTreeHandle` (`get-tree`) and `ResourceHandle`
> (`preload`) as return types but never declares them. It declares 20 other
> handle types with `defopaque` and its own comments say these two come from
> "the typed defopaque the generator already declares" / are "allowlisted so
> their Handle defopaques exist".
>
> They do exist -- `(defopaque ResourceHandle :int)` and
> `(defopaque SceneTreeHandle :int)` are at
> `src/bridge/generated_facade.cpp:57` and `:61`. But the facade is evaluated
> **after** the prelude, by construction: `turmeric_script.cpp` G3.b evaluates
> the prelude, G3.c the facade, and the ordering is deliberate ("so curated
> names take precedence over the per-class generated forms"). So the prelude
> forward-references types that do not exist yet.
>
> Fix directions, cheapest first:
>
> 1. Add `(defopaque SceneTreeHandle :int)` / `(defopaque ResourceHandle :int)`
>    to the prelude's own declaration block alongside the other 20. Duplicate
>    `defopaque` of the same name must be confirmed harmless when the facade
>    re-declares them moments later -- check that first, it is the whole risk.
> 2. Split the facade into a declarations part (evaluated before the prelude)
>    and a definitions part (after), preserving the precedence the ordering
>    exists for.
>
> Option 1 is a two-line change; option 2 is the structurally correct one.
> Neither is done here -- this was an investigation, and the fix belongs with
> someone who can test on Linux/macOS too.
>
> ### Still true
>
> The AOT path remains unexercised: script load still fails, so
> `.godot/turmeric-cache/` is still never created.


**Summary:** Every `.tur` script load in the Godot GDExtension prints
`turmeric-godot: baked-in prelude failed to eval: elaboration error` and then
fails with `Failed loading resource`. No script loads, in either execution mode.
This blocks WIN2 end-to-end verification and, if it reproduces off Windows, is a
straightforward showstopper for the shim.

**Severity:** High -- the GDExtension loads and registers correctly, but no
Turmeric script can actually run.

**Platform:** Observed on Windows (MinGW/UCRT64, Godot 4.3.stable).
**NOT verified on Linux/macOS** -- see "What is not known" before assuming this
is a Windows bug. Nothing about it looks platform-specific.

## Repro

```sh
cd ../turmeric-godot
scons platform=windows arch=x86_64 target=template_debug use_mingw=yes
cd examples/paddle-pong-tur
# import once, so res:// resources are in the database
Godot_v4.3-stable_win64.exe --headless --editor --path . --quit
# then run
TUR_BIN=/path/to/tur.exe Godot_v4.3-stable_win64.exe --headless --path . --quit
```

Output (trimmed):

```
[turmeric-godot] initialize(level=2)
[turmeric-godot] registered Turmeric script language + resource format
[turmeric-godot] libturi smoke: (+ 1 2) = 3 : int
hello from turmeric (via native)
[turmeric-godot] TurmericScript::_reload (len=1305, keep_state=no)
[turmeric res://scripts/ball.tur:<eval>:53 TUR-W0040] unknown name 'godot-vec2' ...
  ... ~40 more TUR-W0040 lines ...
turmeric-godot: baked-in prelude failed to eval: elaboration error
ERROR: Failed loading resource: res://scripts/ball.tur. Make sure resources have
       been imported by opening the project in the editor at least once.
ERROR: [pong] script load returned null
```

The Godot-level "Make sure resources have been imported" is a red herring: the
project *was* imported (`.godot/` exists and the editor pass succeeded). The
prelude failure precedes it and is the actual cause.

## What this is not

Ruled out during WIN2 bring-up, so nobody re-derives it:

- **Not the extension failing to load.** It initializes through all four levels,
  registers the script language and resource format, and `libturi` evaluates
  in-process (`(+ 1 2) = 3`).
- **Not an AOT-path problem.** It reproduces identically with the default
  interpreter mode and with AOT forced. The ~40 `TUR-W0040
  will runtime-dispatch` warnings show elaboration reaching the interpreter path
  in both cases.
- **Not caused by the Windows AOT port.** The failure is upstream of any of it:
  `.godot/turmeric-cache/` is never created, `tur build --shared` is never
  invoked, and `TUR_BIN` is never consulted.

## Where to look

`src/bridge/prelude.cpp` in `../turmeric-godot` holds the baked-in prelude; the
diagnostic is emitted where its eval result is checked. The elaboration error
itself is not printed -- only the fact that one occurred -- which is the first
thing to fix: surface the underlying diagnostic (file/line/code) instead of
`elaboration error`. Likely suspects, in order:

1. The prelude has drifted from the current `libturi` elaborator (this shim
   tracked a much older `main`; the compiler has moved ~1000 commits).
2. A prelude form that only elaborates under a flag/experiment the embedded
   path does not enable.
3. Genuinely Windows-specific elaboration divergence -- least likely, and it
   should be the last hypothesis tested, not the first.

The `unknown name 'godot-vec2' ... 'godot-call'` warnings may be the same root
cause surfacing earlier: those names are exactly what the prelude is supposed to
define. If the prelude never evaluated, the script would see all of them as
unknown -- which is precisely the observed output. Worth checking whether they
are one bug rather than two.

## What is not known

- Whether this reproduces on Linux/macOS. Nobody has run this example on any
  other platform recently; the shim's CI (`.github/workflows/build.yml`) builds
  but does not run a project. **Test that first** -- it decides whether this is
  a Windows report or a general one.
- Which prelude form fails. The diagnostic is swallowed.

## Related

- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md) -- WIN2.
- [jit-godot-embedding-spike.md](jit-godot-embedding-spike.md) -- if the JIT
  replaces the AOT path, the prelude still has to elaborate; this is upstream of
  that choice.

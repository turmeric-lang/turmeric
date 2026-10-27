# Plan: Unblock the 8 typecheck-skipped spices

> **Status:** Phase 1 Complete (Category A fixed; tidal marker removed)
> **Last Updated:** 2026-05-25
> **Type:** Compiler / Spice maintenance
> **Companion to:** [spice-aware-check-plan.md](spice-aware-check-plan.md)
> **Tracked across repos:** `turmeric` (compiler fixes), `turmeric-spices` (spice source fixes)

---

## Overview

SC8 (the per-spice CI regression matrix) was wired into
`turmeric-spices/.github/workflows/ci.yml` in commit `ecb0ec3`. It runs
`tur check` against every `.tur` file in every spice's `src/` and fails
on any non-zero exit -- a real signal now that SC4/SC5 auto-discovery
landed in the compiler.

Eight spices opted out via a `requires.typecheck-skip` marker file so
CI stays green during rollout. This plan inventories the failures,
groups them by category, suggests fix recipes, and orders the work.

**Goal:** delete every `requires.typecheck-skip` marker so the SC8
matrix runs against the whole monorepo.

**Affected spices:** `glsl`, `opengl`, `postgres`, `raylib`, `rtaudio`,
`scscm`, `sqlite`, `tidal` (8 of 21 spices, ~17 files).

---

## Failure inventory

Each row below is one type-check error surfaced by `tur check` against
the file. Sourced from the skip-marker text plus a re-run of the CI
logic locally. Run `tur check turmeric-spices/spices/<name>/src/...`
to reproduce.

| Spice    | File                                | First error                                                    | Category |
|----------|-------------------------------------|----------------------------------------------------------------|----------|
| glsl     | `src/glsl/shaders.tur`              | `__prim-keyword` arg 1: expected int, got cstr                 | D        |
| glsl     | `src/glsl/stdlib.tur`               | `unbound symbol '.'` (stdlib `dot` macro)                      | D        |
| opengl   | `src/opengl/math.tur`               | `defn: too many parameters (max 8)`                            | C        |
| postgres | `src/postgres/db.tur`               | `if condition must be bool, got int`                           | B        |
| postgres | `src/postgres/notify.tur`           | `__pq-exec-raw` arg 2: expected cstr, got int                  | B        |
| postgres | `src/postgres/stmt.tur`             | `if condition must be bool, got int`                           | B        |
| raylib   | `src/raylib/camera.tur`             | `defn: too many parameters (max 8)`                            | C        |
| raylib   | `src/raylib/models.tur`             | `defn: too many parameters (max 8)`                            | C        |
| rtaudio  | `src/rtaudio/stream.tur`            | `defn: too many parameters (max 8)`                            | C        |
| scscm    | `src/scscm/codegen.tur`             | `if condition must be bool, got int`                           | B        |
| scscm    | `src/scscm/compile.tur`             | `err?` arg 1: expected `ptr<void>`, got int                    | B        |
| scscm    | `src/scscm/expander.tur`            | `if condition must be bool, got int`                           | B        |
| scscm    | `src/scscm/lexer.tur`               | ~~`defn: 'ok' is already defined by an auto-loaded stdlib module`~~ **FIXED (Phase 1)** | A        |
| scscm    | `src/scscm/parser.tur`              | `if condition must be bool, got int`                           | B        |
| sqlite   | `src/sqlite/db.tur`                 | `if condition must be bool, got int`                           | B        |
| tidal    | `src/tidal/event.tur`               | ~~`defn: 'ok' is already defined ...`~~ **FIXED (Phase 1)**    | A        |
| tidal    | `src/tidal/notation.tur`            | ~~`defn: 'ok' is already defined ...`~~ **FIXED (Phase 1)**    | A        |
| tidal    | `src/tidal/pattern.tur`             | ~~`defn: 'ok' is already defined ...`~~ **FIXED (Phase 1)**    | A        |
| tidal    | `src/tidal/render.tur`              | ~~`unterminated list (missing ')')`~~ **FIXED (Phase 1)** -- missing `)` in `render-dur-sp` | E        |

---

## Categories

### Category A: `ok` shadow from auto-loaded `result.tur`

**Affected:** 4 files (`scscm/lexer.tur`, `tidal/{event,notation,pattern}.tur`).

`stdlib/result.tur` is auto-loaded at compiler startup (see the
`stdlib_files[]` array in `src/main.c`, comment "Bug-5 follow-up:
result.tur is auto-loaded so its `ok` / `ok?` / `ok-val` / `err` /
`err?` / `err-val` helpers are globally available"). Files that define
their own `ok`/`ok?`/`ok-val` collide.

Example from `scscm/lexer.tur:53`:

```turmeric
(defn ok [x : int] #{Unsafe} : ptr<void> ...)
(defn ok? [r : ptr<void>] #{Unsafe} : bool ...)
(defn ok-val [r : ptr<void>] #{Unsafe} : int ...)
```

The lexer is implementing its *own* result type but reusing the
stdlib's names. Same pattern in the three tidal files.

#### Fix recipe (Category A)

Two options:

1. **Rename the local definitions.** Mechanical; example for `scscm/lexer.tur`:
   - `(defn ok ...)` -> `(defn lex-ok ...)`
   - `(defn ok? ...)` -> `(defn lex-ok? ...)`
   - `(defn ok-val ...)` -> `(defn lex-val ...)`
   - Update every caller in the same spice (`grep -rn '\<ok\b' spices/scscm/`).
2. **Use stdlib's `ok` directly.** If the local `ok` is functionally
   identical to stdlib's, delete it and rely on the auto-load.

Option 1 is safer (the inline-C bodies differ -- scscm's `ok` returns
`:ptr<void>` while stdlib's is structurally a `Result`). Pick names
that reflect the local domain (`lex-`/`tidal-` prefixes).

**Effort:** small per file (rename + grep + update callers). ~1 hour
total for all 4 files.

---

### Category B: `if int -> bool` after type-system tightening

**Affected:** 7 files
(`postgres/{db,notify,stmt}.tur`,
`sqlite/db.tur`,
`scscm/{codegen,compile,expander,parser}.tur`).

The compiler now requires `if` conditions to be `:bool`, not `:int`.
Older spice code that used C-style truthy-int conditions trips the
type checker.

Example from `postgres/db.tur:170`:

```turmeric
(defn db-connect [connstr : cstr] : ptr<void>
  (let [raw (__pq-connect-raw connstr)]
    (if (!= raw 0) (__ok raw) (__err 0))))
```

Here `(!= raw 0)` returns `:int` (0 or 1) rather than `:bool`. Same
pattern in every Category B file.

The related `notify.tur` and `compile.tur` errors are likely downstream
type errors caused by the same coercion gap (a function returning an
int that callers were treating as a ptr or cstr).

#### Fix recipe (Category B)

For each `if int -> bool` site, replace the int-returning predicate
with a bool-returning one. Options, in order of preference:

1. Use stdlib's bool predicates: `(zero? raw)`, `(not (zero? raw))`,
   `(pos? raw)`, `(neg? raw)`, `(true? raw)`.
2. Use comparison operators that return bool: `(> raw 0)`, `(= raw 0)`,
   `(>= raw 0)`. Verify by reading the operator's definition -- if
   `!=` returns int, find the bool equivalent (`not=` or similar).
3. As a last resort, wrap in a coercion: `(int->bool (!= raw 0))`. Try
   to avoid this; it indicates the underlying predicate has the wrong
   return type and should be fixed at source.

For the dependent errors (`notify.tur`'s `__pq-exec-raw` cstr/int
mismatch, `compile.tur`'s `err?` ptr<void>/int mismatch), check whether
fixing the parent function's return type (probably `:ptr<void>` not
`:int`) resolves them automatically.

#### Compiler-side check (turmeric repo)

Before changing spices, verify the stdlib operators actually return
bool. Grep stdlib for `=`, `!=`, `<`, `>`, `<=`, `>=` to confirm their
return types. If any are still `:int`-typed, that's a stdlib bug worth
filing -- this category becomes much smaller if the operators are
upgraded once instead of working around them in every consumer.

**Effort:** small per file but multiplied by 7 files. ~3-4 hours total.
Larger if a stdlib-side fix is also needed.

---

### Category C: `defn: too many parameters (max 8)`

**Affected:** 4 files
(`opengl/math.tur`, `raylib/camera.tur`, `raylib/models.tur`,
`rtaudio/stream.tur`).

The compiler enforces a max-8-parameter limit on `defn`. These files
declare functions with 9+ scalar parameters because they were written
as direct bindings of C API surfaces where structs are flat
(positional `Vector3` becomes 3 floats, `Camera3D` becomes 11 floats,
etc.).

Example from `raylib/camera.tur:36`:

```turmeric
(defn camera-3d [px : float py : float pz : float
                 tx : float ty : float tz : float
                 ux : float uy : float uz : float
                 fovy : float proj : int] : int
  ```c
  ...
  cam->position = (Vector3){ (float)px, (float)py, (float)pz };
  cam->target   = (Vector3){ (float)tx, (float)ty, (float)tz };
  ...
  ```)
```

#### Fix recipe (Category C)

Two real options:

1. **Group args into a struct.** Define `vec3` (or whatever the C API
   calls it) as a Turmeric struct, take it as one parameter:

   ```turmeric
   (defstruct vec3 :x :float :y :float :z :float)
   (defn camera-3d [pos : vec3 target : vec3 up : vec3
                    fovy : float proj : int] : int
     ...)
   ```

   This is the right long-term shape but requires updating every
   caller in the spice. May also need helper constructors
   (`(vec3-new x y z)`) and accessors.

2. **Raise the compiler's max-param limit.** Defensible if 8 turns out
   to be arbitrary and most spices need more. Search for the limit in
   `src/compiler/` (likely a `MAX_DEFN_PARAMS` constant or similar).
   Raising it is a one-line change but has codegen implications --
   verify the emitted C signatures and any ABI assumptions still hold.

The plan recommends **option 1 for now** (raylib/opengl/rtaudio are
graphics/audio domains where vec3/Camera3D structs already exist
conceptually), with option 2 as a fallback if a survey of the rest of
turmeric-spices uncovers many more max-8 hits.

**Effort:** moderate. ~2-4 hours per file (API change + caller
updates). Bigger if downstream spices depend on the flat signatures.

#### Open question

Does the compiler actually need a max-8 limit? Worth checking. If it
exists for ABI reasons (e.g. matching the System V x86-64 calling
convention's 6 integer / 8 float register limit) then option 1 is the
only right path. If it's arbitrary, option 2 might be acceptable.
A 10-minute spike to read the compiler source clarifies this before
spending hours refactoring spices.

---

### Category D: glsl-specific

**Affected:** 2 files (`glsl/shaders.tur`, `glsl/stdlib.tur`).

Two unrelated errors:

1. `__prim-keyword` arg 1: expected int, got cstr (`shaders.tur:164`).
   Reading the surrounding context shows the function is called with
   `input-layout` which is bound as `:cstr`. Either `__prim-keyword`'s
   signature is wrong, or the caller should convert the string to a
   keyword first.
2. `unbound symbol '.'` (`stdlib.tur` -- error fires in
   `stdlib/macros.tur:446` from the `(defmacro dot ...)` expansion).
   Likely the GLSL stdlib has a definition or macro use that the `dot`
   macro can't expand against in this context.

Both need investigation before a fix can be written. Until we know
whether these are spice bugs or compiler/stdlib bugs, this category is
the lowest-priority and may end up split across both repos.

**Effort:** unknown. Budget 1 day for investigation + fix, more if a
compiler change is required.

---

### Category E: Cascading parser failure

**Affected:** 1 file (`tidal/render.tur`).

`unterminated list (missing ')')` -- this is almost certainly a parse
error caused by a missing token from upstream tidal modules failing to
load. Once Categories A (the `ok` shadow in event/notation/pattern)
are resolved, this error may disappear on its own. Re-check after A
ships before manually editing.

**Effort:** likely zero (auto-resolves), worst case small.

---

## Suggested ordering

Easiest first; each phase is independently mergeable.

1. **Phase 1: Category A** (`ok` renames). 4 files, ~1 hour.
   After landing: re-check `tidal/render.tur` (Category E) and likely
   delete `tidal/requires.typecheck-skip`.

2. **Phase 2: Category B** (`if int -> bool`). 7 files, ~3-4 hours.
   May want a short compiler-repo spike first to verify whether stdlib
   `!=`/`=` need a return-type fix; if yes, do that first.

3. **Phase 3: Compiler spike for Category C max-param limit.**
   10-minute read of `src/compiler/` to decide option 1 vs option 2.
   Updates this plan if option 2 is the right call.

4. **Phase 4: Category C** (struct args). 4 files, ~1-2 days.
   The largest piece of work. Can be split across multiple PRs (one
   per spice).

5. **Phase 5: Category D** (glsl investigation). Unknown effort.
   Independent of phases 1-4.

6. **Phase 6: `tur-signal` spice -- run tests and resolve issues.**
   The `signal` spice was just extracted from stdlib (see
   `docs/signal-spice-plan.md`). It does not yet carry a
   `requires.typecheck-skip` marker, so we need to verify it is
   actually clean rather than silently skipped.

   Steps:
   a. Run `tur check` against all four source files:
      ```sh
      cd ../turmeric-spices/spices/signal
      for f in src/signal/{core,dsp,envelope,synth}.tur; do
          echo "=== $f ===" && tur check "$f"
      done
      ```
   b. Run the test suite:
      ```sh
      tur run tests/signal/arrow_tests.tur
      ```
   c. If `synth.tur` triggers Category C (`too many parameters`),
      decide between raising the compiler limit (Phase 3 spike result)
      or introducing intermediate structs matching the existing type
      annotations.
   d. If `synth.tur` triggers unbound-symbol errors for `math/sin`,
      `math/PI`, `vec-of`, etc. (it references math and vec stdlib
      without explicit imports), add the missing `(import ...)` lines.
   e. Fix any remaining `if int -> bool` hits in the four files
      (Category B recipe applies).
   f. If issues cannot be resolved before the other phases land, add a
      `requires.typecheck-skip` marker with a comment so CI stays green
      and remove it when the blocking fix arrives.

   **Effort:** ~1-2 hours if only import/Category B fixes are needed;
   more if synth.tur requires Category C struct refactoring.

After every phase, delete the corresponding `requires.typecheck-skip`
markers in `turmeric-spices/spices/*/` so CI starts asserting against
those spices.

---

## Verification per spice

```sh
# From turmeric-spices working tree:
cd spices/<name>
n_fail=0
while IFS= read -r -d '' f; do
    if ! tur check "$f"; then n_fail=$((n_fail + 1)); fi
done < <(find src -name '*.tur' -print0)
echo "$n_fail failed"
# When n_fail == 0:
rm requires.typecheck-skip
```

Then run the full CI matrix locally to confirm:

```sh
# From turmeric-spices working tree:
for d in spices/*/; do
    name=$(basename "$d")
    [ -f "$d/requires.typecheck-skip" ] && { echo "SKIP $name"; continue; }
    [ -d "$d/src" ] || continue
    n_fail=0
    while IFS= read -r -d '' f; do
        if ! tur check "$f" >/dev/null 2>&1; then n_fail=$((n_fail + 1)); fi
    done < <(find "$d/src" -name '*.tur' -print0)
    printf "  %-12s %s\n" "$name" "$([ "$n_fail" -eq 0 ] && echo OK || echo "FAIL ($n_fail)")"
done
```

Push to a PR; the SC8 matrix job runs on every push to a branch with
an open PR.

---

## Risks and open questions

1. **Stdlib `result.tur` auto-load may be the wrong default for some
   spices.** Categories A and possibly some Category B errors trace
   back to auto-loaded helpers. If many more spices hit this pattern,
   consider gating auto-load behind a spice manifest opt-in
   (`:auto-load-stdlib false` or similar). Out of scope for this plan
   but worth flagging.

2. **`__prim-keyword` and `__pq-exec-raw` are inline-C bindings.**
   Type errors here may indicate a binding generator (or hand-written
   signature) that drifted from the actual C function. Worth checking
   if `tur emit-cmake` or a binding-gen pass exists that could
   regenerate them cleanly.

3. **Max-param limit decision blocks Category C.** If we choose option
   2 (raise the limit), no spice changes are needed. If we choose
   option 1 (struct args), it's the biggest single category of work
   in this plan. The Phase 3 spike resolves this before committing to
   either path.

4. **`render.tur` may surface new errors after fixing A.** Currently
   the parser failure short-circuits other diagnostics. Don't assume
   Category E is zero-effort until verified.

---

## Tracking

After this plan lands, file one GitHub issue per spice (or per
category) referencing this doc. The issue body should be the
`requires.typecheck-skip` marker content plus a checklist of files,
so contributors can pick up one spice at a time.

For maintainers wanting a quick status sweep:

```sh
ls turmeric-spices/spices/*/requires.typecheck-skip 2>/dev/null | wc -l
# 0 means we're done.
```

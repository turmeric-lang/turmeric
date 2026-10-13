# Release tarball's `include/turi/` cannot be included, and `tur` never points at it

**RESOLVED 2026-10-13.** Both halves fixed, plus the check that keeps them
fixed. Still live on `main` that day: a prefix tree staged exactly as
`release.yml` packaged it failed the repro with `'turi/eval.h' file not
found`, and with `-I`/`-L` passed by hand, one header deeper on
`compiler/diag.h`.

- **Fix direction 1 -- `tur` probes the prefix layout.**
  `resolve_autolink_flags` step 1 (`src/main.c`) checks
  `<exe_dir>/../include/turi/eval.h` and, for a program that links `-lturi`,
  prepends `-I<prefix>/include -L<prefix>/lib`. It probes exactly one level
  up, like `resolve_stdlib_root` step 3 and `locate_runtime_lib`, not as a
  walk-up: a walk-up would let a stale `/usr/local/include/turi` shadow a dev
  build under `/usr/local/src`. `TUR_SDK_ROOT` pointing at a prefix
  (`include/turi/eval.h` present) takes the same arm; the source-tree probe
  (`share/turmeric/src/turi/eval.h`) is unchanged.
- **Fix direction 2 -- ship the closure.** `scripts/stage-release.sh` stages
  the archive, and both `release.yml` packaging steps call it. Its `include/`
  is the `cc -MM` closure of the roots programs name -- `turi/eval.h`,
  `env.h`, `value.h`, `fiber.h`, and `turi/r7rs_embed.h` -- copied
  src/-relative. That is 12 headers today: the report's six, plus
  `compiler/forms.h` (added since) and `r7rs_embed.h`. The script then
  compiles the roots with nothing but `-I<dist>/include`, so a header that
  gains an include either ships it or fails the staging. The closure is
  computed rather than listed because a hand-kept list is how four of eleven
  shipped. The "shrink the surface" option, an embedding API that does not
  drag in `compiler/diag.h`, is still open as a design question. Shipping the
  closure unblocks consumers and is easy to narrow later.
- **`(import (scheme eval))` was broken by the same defect.**
  `stdlib/r7rs/eval.tur` includes `turi/r7rs_embed.h`, which no archive ever
  shipped, so `#lang r7rs` `eval` could not compile from a release either.
- **Fix direction 3 -- the check.** `release.yml` builds and runs
  `tests/release-sdk/turi-eval.tur` and `scheme-eval.tur` from the extracted
  archive (Windows: `scheme-eval` only, the path its suite already covers from
  the build tree). `tests/check-release-sdk.sh` (ctest `tur_release_sdk`)
  stages the same tree from a local build on every PR. It builds both programs
  with the staged `tur` from outside the checkout and asserts that the compile
  line takes `-I`/`-L` from the staged prefix and nothing from `src/`.
  Measured: with the `main.c` probe reverted, it fails with `turi/eval.h: No
  such file`; with only the old four headers staged, with
  `compiler/diag.h: No such file` and `turi/r7rs_embed.h: No such file`.
- **tvm's build-from-source path** (`tvm/tvm.sh`) copied the same four
  headers into a third shape. It now uses `stage-release.sh --headers` when
  the checked-out ref has it.
- **Guides:** `eval-api.md` (its compile line passed `-I.../include/turi`,
  which cannot resolve `"turi/env.h"`), `releases-and-installation-guide.md`,
  `release-process-guide.md`, `tvm-guide.md`. The turmeric-spices `CLAUDE.md`
  sentence this report names is in that repo and still describes the flat
  layout.

**Severity (at filing): high for any turi-embedding consumer; zero for everyone else.**
Every official release archive since **v0.47.0** ships a `turi` public header
set that does not compile, and a `tur` whose SDK anchoring looks for those
headers at a path the archive never contains. The net effect is that **no
program embedding `libturi` can be built against any published release** --
only against a from-source build of the compiler.

Found 2026-09-16 while running the `turmeric-spices` suites against a
v0.49.0 release tarball. `spices/notebook` fails 7 of its 10 suites on it;
all 7 are this one defect.

## Repro

```sh
# Any published release >= v0.47.0.
curl -fsSL -o tur.tgz \
  https://github.com/rjungemann/turmeric/releases/download/v0.49.0/turmeric-v0.49.0-macos-arm64.tar.gz
mkdir -p sdk && tar -xzf tur.tgz -C sdk

cat > embed.tur <<'EOF'
(defn probe [] : int
  ```c
  /* __tur_include__: #include "turi/eval.h" */
  return 0;
  ```)
(defn main [] : int (probe))
EOF

./sdk/bin/tur build embed.tur -o /tmp/embed
```

```
embed.c:2:10: fatal error: 'turi/eval.h' file not found
tur: cc invocation failed (status 256)
```

Supplying the include dir by hand does not rescue it -- it only moves the
error one header deeper:

```sh
TUR_CC_FLAGS="-I$PWD/sdk/include -L$PWD/sdk/lib" ./sdk/bin/tur build embed.tur -o /tmp/embed
```

```
In file included from sdk/include/turi/eval.h:6:
sdk/include/turi/env.h:16:10: fatal error: 'compiler/diag.h' file not found
```

Real-world instance (same root cause, no hand-written repro needed):

```sh
cd turmeric-spices/spices/notebook && tur test tests
# 10 tests, 3 passed, 7 failed -- every failure is
# "fatal error: 'turi/eval.h' file not found"
```

## Root cause -- two halves, each sufficient on its own

### 1. The shipped header set is not self-contained

`.github/workflows/release.yml:84` (and `:230` for the Windows leg) copies
exactly four headers:

```sh
cp src/turi/eval.h src/turi/env.h src/turi/value.h src/turi/fiber.h dist/include/turi/
```

Those four transitively include **six headers that are never copied**:

| Included by | Missing header |
| --- | --- |
| `turi/eval.h:9` | `runtime/globals.h` |
| `turi/env.h:16` | `compiler/diag.h` |
| `turi/env.h:48` | `runtime/arena.h` |
| `turi/env.h:49` | `runtime/buf.h` |
| `turi/env.h:50` | `compiler/symbols.h` |
| `turi/fiber.h:88` | `runtime/platform.h` |

So `include/turi/` is unusable no matter what `-I` you pass. This is not a
case of the public surface leaking one private detail -- `env.h` pulls in
the compiler's diagnostic and symbol tables, so "ship the six" and "shrink
the public headers" are materially different fixes (see below).

### 2. `tur`'s SDK anchoring probes a path the archive does not have

`resolve_autolink_flags` step 1 (`src/main.c:2502`-`2530`) walks up from the
executable looking for:

```c
snprintf(probe, sizeof(probe), "%s/share/turmeric/src/turi/eval.h", dir);  /* main.c:2517 */
```

and on a hit emits `-I<sdk_root>/src -I<sdk_root>/src/compiler
-I<sdk_root>/src/runtime` -- i.e. it expects a **source tree** staged under
`share/turmeric/src`. The archive stages only `stdlib` there
(`release.yml:85`, `cp -R stdlib dist/share/turmeric/`). The probe therefore
never hits, `sdk_root` stays empty, and **no `-I` is emitted at all**. That
is why the first repro above fails before it can even complain about
`compiler/diag.h`.

Note these two halves disagree about the intended shape: the packaging
ships a *prefix* (`include/turi/*.h`), the compiler looks for a *source
tree* (`share/turmeric/src/...`). Neither is wrong on its own; they have
simply never been reconciled, and nothing checks that they agree.

## Why this has gone unnoticed since v0.47.0

- **turmeric's own release verification cannot see it.** `release.yml` has a
  "Compile a program from the extracted archive" step, added precisely
  because `tur --version` was too weak a check. But the program it compiles
  is a `println` hello-world, which includes no turi header and links no
  `-lturi`, so it exercises the stdlib and runtime-archive paths only.
- **turmeric-spices CI cannot see it either.** `.github/workflows/ci.yml`
  checks out `rjungemann/turmeric` and builds `tur` from source on every
  run, which is the one configuration where the source tree is present and
  the probe hits.
- The only way to hit it is the documented local/agent path -- the prebuilt
  binary from `scripts/install-tur.sh` -- which had its own blocker until
  today (it hardcoded the pre-v0.47.0 flat layout and died at `chmod`, so
  nobody got far enough to reach this). Fixed in turmeric-spices
  `scripts: find the vendored tur under either tarball layout`.

## Fix directions

Either half alone unblocks the repro; doing only one leaves the other as a
trap for the next consumer.

1. **Decide which shape is canonical.** The prefix layout is what
   `release.yml` already produces and what `resolve_stdlib_root` step 3 and
   `locate_runtime_lib` already resolve natively (`<exe_dir>/../share/...`,
   `<exe_dir>/../lib`). Making `resolve_autolink_flags` probe
   `<prefix>/include/turi/eval.h` and emit `-I<prefix>/include` alongside the
   existing source-tree probe is the smaller change and keeps the
   source-tree case working for dev builds.
2. **Make `include/turi/` self-contained.** Two options, and the choice is a
   real API decision, not a packaging detail:
   - *Ship the closure.* Copy the six headers above into
     `dist/include/{compiler,runtime}/` and add `-I<prefix>/include` so the
     `"compiler/diag.h"` spelling resolves. Cheap, but it publishes the
     compiler's internal headers as part of the SDK surface.
   - *Shrink the surface.* Split the embedding API so `turi/env.h` does not
     drag in `compiler/diag.h` and `compiler/symbols.h`. Correct, larger,
     and probably wants its own plan.
3. **Make the release verify step actually cover this.** Extend the
   "Compile a program from the extracted archive" step with a second program
   that carries a `/* __tur_include__: #include "turi/eval.h" */` inline-C
   block and links `-lturi`. This is the check that turns the defect into a
   red release job instead of a downstream discovery. Without it, any fix
   here can silently rot again the next time the archive layout moves.

## Guides to update when fixed

- Any guide documenting `include/turi` against a release tarball should
  state which archive layout the include path comes from.
- The turmeric-spices `CLAUDE.md` "Getting the `tur` binary" section says the
  tarball "contains `tur`, `libturi.a`, `include/turi/*.h`, and the full
  `stdlib/` tree" and that "the stdlib lives next to the binary on purpose".
  Both sentences describe the pre-v0.47.0 flat layout and should be updated
  to the prefix layout once the above is settled.

## Not this

- It is **not** `install-tur.sh` being broken. That was a separate, real bug
  found in the same session and fixed; with a correctly installed binary the
  failure above still reproduces exactly.
- It is **not** a missing `-L`/`libturi.a`. `lib/libturi.a` and
  `lib/libturt_runtime.a` are both present in the archive and are found. The
  failure is at the C *compile* step, before any link.
- Symlinking `include/` next to the binary (`<prefix>/bin/include`) does
  **not** help -- the probe is for `share/turmeric/src/turi/eval.h`, not for
  a sibling `include/`. Measured.

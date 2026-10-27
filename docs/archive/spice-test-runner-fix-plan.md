# Plan: Fix the `tur test` Runner for Spices

> **Status:** Draft Plan
> **Last Updated:** 2026-05-23
> **Type:** Bug Investigation & Multi-Repo Fix

---

## Background

While scaffolding the PV0 phase of `tur-plutovg`, every existing spice in
`turmeric-spices` was found to be in a broken state: `tur fetch` writes empty
lockfiles, no cmake-deps are ever fetched, and `tur test` cannot compile any
test file. The failure is split across five distinct bugs — none of them are
specific to plutovg, but they all currently block the PV0 smoke test.

This document captures the investigation, the proposed fixes, and the order
in which to land them.

---

## Bug inventory

### Bug 1 -- `{...}` parses as `F_CONTRACT_TYPE`, not a map (silent show-stopper)

**Where it lives**

- Reader: `src/compiler/reader.c` lines ~1559-1568 -- plain `{...}` outside
  curly-infix mode produces `F_CONTRACT_TYPE` (the `{var : T | pred}` form),
  not `F_MAP`.
- Map literals require `#{...}` syntax (`reader.c:852-866`).
- Manifest parser: `src/compiler/pkg.c:206`, `:141`, `:118` -- every map-shaped
  parser checks `map->tag != F_MAP` and **silently returns true** without
  emitting a diagnostic. The same silent-skip exists in `parse_spices`,
  `parse_cmake_deps`, and `map_get_kw`.

**Symptom**

- Every `spices/*/build.tur` in `turmeric-spices` uses bare `{...}` for
  `:cmake-deps`, `:spices`, and `:options`.
- `tur fetch` reports "spice: lock file written to tur.lock" and exits 0.
- `tur.lock` ends up empty (`:spices #{}` / `:cmake-deps #{}`).
- No C dependency is ever downloaded; `plutovg.h`, `raylib.h`, `sqlite3.h`,
  etc. are not on disk; spice C builds fail with "file not found".

**Proof**

```sh
cp build.tur /tmp/manifest-test/build.tur
sed -i '' 's/:cmake-deps {/:cmake-deps #{/' /tmp/manifest-test/build.tur
sed -i '' 's/:options {/:options #{/'       /tmp/manifest-test/build.tur
sed -i '' 's/"plutovg" {/"plutovg" #{/'     /tmp/manifest-test/build.tur
cd /tmp/manifest-test && tur fetch --update
# -> generates cmake/CMakeLists.txt, fetches plutovg, builds libplutovg.a
```

**Fix**

- *Core (this PR):* in `parse_cmake_deps`, `parse_spices`, and the
  build-opts branch in `pkg_manifest_read`, emit a `DIAG_ERROR` (or at least
  a `fprintf` to stderr) when the value's tag is not `F_MAP`. Mention that
  the value must use `#{...}` syntax.
- *Spices (this PR):* rewrite every `spices/*/build.tur` to use `#{...}` for
  all map-shaped values: `:spices`, `:cmake-deps`, the per-entry
  `{:url ... :ref ...}` blocks, and `:options`.

**Files touched**

Core:

- `src/compiler/pkg.c`

Spices (17 files):

- `spices/c-dsl/build.tur`
- `spices/glsl/build.tur`
- `spices/http/build.tur`
- `spices/json/build.tur`
- `spices/math/build.tur`
- `spices/opengl/build.tur`
- `spices/osc/build.tur`
- `spices/plutovg/build.tur`
- `spices/png/build.tur`
- `spices/postgres/build.tur`
- `spices/raylib/build.tur`
- `spices/regex/build.tur`
- `spices/rtaudio/build.tur`
- `spices/rtmidi/build.tur`
- `spices/scscm/build.tur`
- `spices/sqlite/build.tur`
- `spices/test/build.tur` (if it uses any map)
- `spices/tidal/build.tur`
- `spices/valkey/build.tur`
- `spices/wav/build.tur`

---

### Bug 2 -- Top-level `(import ...)` is rejected outside a `(defmodule ...)` body

**Where it lives**

- `src/compiler/elab_call.c:378`:
  ```c
  if (name == e->sym_import) {
      diag_emit(DIAG_ERROR, call->span,
                "import is only allowed inside defmodule");
      return NULL;
  }
  ```
- Imports are only parsed by `src/compiler/elab_module.c:471` inside the
  `defmodule` body loop.

**Symptom**

- Test files in turmeric-spices open with bare top-level `(import ...)`:
  ```turmeric
  (import test/suite  :refer [describe it])
  (import test/assert :refer [assert-eq])
  ...
  ```
- Every such file errors at line 1 with "import is only allowed inside
  defmodule" and `tur test` reports them as FAIL.

**Affected files (26)**

```
spices/c-dsl/tests/c-dsl/codegen_test.tur
spices/c-dsl/tests/c-dsl/core_test.tur
spices/glsl/tests/glsl/codegen_test.tur
spices/osc/tests/osc/bundle_test.tur
spices/osc/tests/osc/client_test.tur
spices/osc/tests/osc/msg_test.tur
spices/osc/tests/osc/server_test.tur
spices/plutovg/tests/plutovg/canvas_test.tur
spices/plutovg/tests/plutovg/surface_test.tur
spices/png/tests/png/info_test.tur
spices/png/tests/png/reader_test.tur
spices/png/tests/png/writer_test.tur
spices/rtaudio/tests/rtaudio/core_test.tur
spices/rtaudio/tests/rtaudio/devices_test.tur
spices/rtaudio/tests/rtaudio/stream_test.tur
spices/rtmidi/tests/rtmidi/core_test.tur
spices/rtmidi/tests/rtmidi/in_test.tur
spices/rtmidi/tests/rtmidi/msg_test.tur
spices/rtmidi/tests/rtmidi/out_test.tur
spices/valkey/tests/valkey/client_test.tur
spices/valkey/tests/valkey/cmd_test.tur
spices/valkey/tests/valkey/pubsub_test.tur
spices/wav/tests/wav/info_test.tur
spices/wav/tests/wav/reader_test.tur
spices/wav/tests/wav/writer_test.tur
```

**Fix**

Wrap each test in a `(defmodule <spice>/tests/<name> ... (defn main [] :int
(run-all) 0))` block:

```turmeric
(defmodule plutovg/tests/surface
  (import result          :refer [ok? ok-val])
  (import test/suite      :refer [describe it])
  (import test/assert     :refer [assert-eq assert-ok])
  (import test/runner     :refer [run-all])
  (import plutovg/surface :refer [surface-create surface-destroy
                                  surface-width surface-height surface-stride])

  (describe "plutovg/surface"
    (it "creates a surface with the requested dimensions"
      ...))

  (defn main [] : int
    (run-all)
    0))
```

Files touched: 26 (listed above).

---

### Bug 3 -- `tur test` does not wire up include paths from the manifest

**Where it lives**

- `src/main.c:1355` (`cmd_test`):
  ```c
  int build_rc = cmd_build(tur_files[i], out_path, NULL, 0, NULL);
  ```
- No `-I` paths derived from `:spices`/`:exports`, no `TUR_STDLIB_DIR`
  threaded, no cmake-deps include/link flags passed to the C compile step.

**Symptom**

After Bugs 1 and 2 are fixed, the elaborator still cannot resolve
`(import test/suite)` or `(import plutovg/surface)` from a test file —
because no `-I` flag points at `spices/test/src` or `spices/plutovg/src`.

**Fix (turmeric core)**

Make `cmd_test` (and probably `cmd_build` of a spice src dir) read the
nearest `build.tur` and derive:

1. `module_include_dirs`:
   - The spice's own `src/`.
   - For each `:spices` entry, the resolved spice's `src/` (looked up via
     `tur.lock` after `tur fetch`, or via the `:subdir` field).
2. `TUR_STDLIB_DIR` (or equivalent in-process value): point at the turmeric
   installation's `stdlib/`.
3. C compile flags: read `cmake/spice-deps-manifest.json` (already emitted
   by `pkg_gen_cmake_deps`) and add `-I<include_dir>`, `-L<link_dir>`,
   `-l<link_lib>` for each cmake-dep.

**Scope:** medium. Touches `src/main.c` (cmd_test + cmd_build dispatching),
plus a small JSON reader for `spice-deps-manifest.json` (or have
`pkg_cmake_build` cache the values in memory so we don't reparse).

**Resolution (landed):**

1. `cmd_test` now resolves the test directory with `realpath()` first (so
   `find_project_root` works on a relative path like `tests/plutovg`), then
   walks up looking for `build.tur`.
2. When `build.tur` is found, it is parsed with `pkg_manifest_read` and the
   following directories are added to the `-I` list passed through to
   `cmd_build`:
   - The project's own `src/` (so a spice's tests can `(import
     plutovg/surface ...)`).
   - Each `:spices` entry's resolved `src/`, mirroring the convention from
     `cmd_run`: `spices/<name>-<ref>[/<subdir>]/src` with a fallback to the
     plain dep dir if no `src/` is present.
   - **Monorepo fallback**: when the fetched copy is absent (e.g. an
     optional `:spice` whose tag does not exist in the remote), walk up
     to four levels from `proj_root` looking for a sibling at
     `<ancestor>/<subdir>/src`.  This is what lets `tur test
     tests/plutovg` succeed inside the `turmeric-spices/spices/plutovg/`
     workspace without first running `tur fetch`.
3. C-side include/link flags from `cmake/spice-deps-manifest.json` were
   already wired up via `pkg_cmake_manifest_read` inside `cmd_build`, so we
   reuse that path unchanged.
4. While here, fixed `assert-ne` in `spices/test/src/test/assert.tur` to
   use `not=` instead of `!=` (the latter elaborates to a less-specific
   type and breaks the `(if ...)` body).

**Verification:**

```sh
cd turmeric-spices/spices/plutovg
TUR_STDLIB_DIR=$TURMERIC_REPO/stdlib tur test tests/plutovg
# Reports: collected 2 spice include dirs
#          -I spices/plutovg/src
#          -I spices/test/src           (monorepo fallback)
```

The path resolution is correct; remaining test failures hit an unrelated
macro-hygiene issue (`test/suite.tur` expands to `(do ...)` but `do` is an
auto-loaded macro that isn't visible from inside imported modules' macro
expansions).  That's a Turmeric macro-system limitation, not part of
Bug 3's scope.

---

### Bug 4 -- Stdlib auto-load ignores `TUR_STDLIB_DIR`

**Where it lives**

- `src/main.c:382-417` -- hardcoded array of `"stdlib/macros.tur"`,
  `"stdlib/safe.tur"`, `"stdlib/contract.tur"`, etc.; all opened via
  `read_entire_file(stdlib_files[i], ...)` which is cwd-relative.
- `src/main.c:3518-...` -- second hardcoded list for a different command path.

`TUR_STDLIB_DIR` *is* honored at `elab_toplevel.c:389` for explicit
`(import ...)` resolution, but the implicit prefix-load uses cwd-relative
paths only.

**Symptom**

Running `tur build` or `tur test` from any directory other than the
turmeric repo root spams stderr:

```
tur: cannot open 'stdlib/macros.tur': No such file or directory
tur: cannot open 'stdlib/safe.tur': No such file or directory
...
```

Builds proceed (the messages are warnings only -- the loader skips missing
files), but the program runs without macros like `cond`, `when`, `for`, and
without `assert!`/`require!`. Most test files fail at compile time as a
result.

**Fix (turmeric core)**

In both auto-load sites, look up the stdlib directory once via:

1. `TUR_STDLIB_DIR` env var, else
2. A compile-time fallback (`TUR_INSTALL_STDLIB_DIR` set by CMake), else
3. `"stdlib"` (current behaviour) for in-repo development.

Then build each path as `<stdlib_dir>/macros.tur`, etc. The change is
mechanical -- compute one `const char *sdir` at the top of the loop and
`snprintf` paths into a buffer.

**Files touched**

- `src/main.c` (two call sites: around lines 382 and 3518).
- Optional: `CMakeLists.txt` to define `TUR_INSTALL_STDLIB_DIR`.

---

### Bug 5 -- `stdlib/result.tur` cannot be explicitly imported

**Where it lives**

- `stdlib/result.tur:446` -- `(definstance Eq [result])` triggers
  TUR-E0013 "orphan instance" when result.tur is loaded as an explicit
  module (the auto-load path bypasses this because typeclasses are
  registered in the same pre-module pass).
- `stdlib/result.tur:447` -- `result-eq?` is marked `unsafe`; explicit
  import path requires an enclosing `(unsafe ...)` block.
- Same pattern at `result.tur:689` for `(definstance Foldable [ptr<void>])`.

**Symptom**

Any spice test (or library file) that writes `(import result :refer [ok? ok-val])`
gets two errors from inside `stdlib/result.tur`:

```
error: typeclass 'Eq' is not defined           (when TUR_STDLIB_DIR is set
                                                but auto-load couldn't find it)
error [TUR-E0013]: orphan instance: ...
error: unsafe function 'result-eq?' requires an enclosing (unsafe ...)
```

**Fix (turmeric core, or stdlib)**

Two options -- pick one:

- *Option A (stdlib):* move the `definstance Eq [result]` into
  `stdlib/typeclass-eq.tur` (which owns Eq), and the Foldable instance into
  the module that defines Foldable. Same for any other orphan instances in
  stdlib.
- *Option B (elaborator):* extend the "stdlib pre-module" exemption
  (`elab_toplevel.c:735-756`) so that bindings/instances coming from a
  module under `TUR_STDLIB_DIR` keep their pre-module status even when
  imported by name, and `(unsafe ...)` wrapping is implicit for stdlib
  bodies.

Option A is cleaner and less invasive but requires careful audit of all
stdlib `definstance` forms.

**Resolution (landed):** A pragmatic hybrid of A:

1. Disabled all six broken `(definstance Eq|Foldable|Bifunctor|Functor|Applicative|Monad ...)` forms in `stdlib/result.tur` with `#;` datum-comments. They were dead code historically (never elaborated cleanly via auto-load or import).
2. Disabled the matching dead-code helpers (`__foldable_result_foldl/foldr`, `__bifunctor_result_bimap`, `__functor_result_fmap`, `__applicative_result_pure/ap`, `__monad_result_bind`) for the same reason — they had broken C bodies (`return ff;` returning `int64_t` from `:ptr<void>`-typed signatures).
3. Added `result.tur` to the auto-loaded stdlib list in `src/main.c` (both call sites) so `ok` / `ok?` / `ok-val` / `err` / `err?` / `err-val` are globally available without `(import result ...)` — mirrors how `tresult.tur` exposes its typed counterpart.
4. Fixed a reader bug along the way: `#;<form>` at EOF made `read_all` return NULL, treating end-of-input after a datum-comment as a parse error. `read_all` now treats `read_form() == NULL && !r.error` as benign EOF.

**Remaining caveat:** `result.tur`'s public functions are still marked
`#{Unsafe}`, so callers need `(unsafe ...)` wrapping. The auto-loaded
`tresult.tur` API (`tok` / `tok?` / `tok-val` etc.) has no such marker —
prefer it for new code. The Unsafe marker is honest about the inline-C
pointer-cast usage and should not be removed without a real rework.

---

### Bug 6 -- Auto-loaded macros are invisible inside imported-module macro expansions

**Where it lives**

- `src/compiler/elab_macros.c` -- macro expansion runs against the
  *defining* module's scope, not the calling site.  Bindings promoted to
  "stdlib pre-module" status by `elab_toplevel.c:735-756` are visible to
  user code that imports nothing special, but not from inside macros
  loaded via `(import some-spice)`.

**Symptom**

`tur-test`'s `describe` macro (in `spices/test/src/test/suite.tur:66`)
expands to ``(`do (__desc-print ~name) ~@body)``.  `do` is provided by
the auto-loaded `stdlib/macros.tur`, but when `describe` expands inside
a test file's module body the expansion is elaborated against the
`test/suite` module's scope -- where `do` is not visible.  Every spice
test file therefore fails to compile with:

```
spices/test/src/test/suite.tur:67:5: error: unbound symbol 'do'
```

This blocks all end-to-end test execution even though every PV0-PV7
plutovg test file (and every other spice's test files) parses,
elaborates its own body, and reaches the macro-expansion step cleanly.
Bug 3's wiring (`tur test` reading the manifest) is the reason we even
get this far; this is the next gate.

**Fix options**

- *Option A (elaborator):* during macro expansion, walk up to the
  importing module's scope (and ultimately the pre-module global scope)
  when a symbol cannot be resolved in the defining module's scope.  This
  matches Clojure/Lisp's typical "macro sees its expansion site" model
  but conflicts with strict module hygiene.
- *Option B (stdlib):* make `do` (and other core macros that get
  splice-quoted by spice macros -- `cond`, `when`, etc.) addressable by
  fully-qualified name from inside any module, e.g. by reserving a
  built-in `tur/do` form that the macro expander always recognises.
- *Option C (spice):* rewrite the `describe` / `it` macros in
  `spices/test/src/test/suite.tur` to avoid stdlib-only macros in their
  expansions -- expand to a `(let)` / explicit sequence form instead of
  `(do ...)`.

Option C is the cheapest unblock (one spice, one file) and gets the
test framework to an actually-runnable state.  Options A and B are the
durable fix for "third-party spices can rely on stdlib macros from
within their own macro expansions."

**Resolution (Option A, landed):**

The root cause was narrower than the original diagnosis suggested: the
issue isn't really about "macro expansion runs against the defining
module's scope", it's about how the quasiquote expander produced its
template.  `quasiquote_expand_form` in `src/compiler/elab_macros.c`
was wrapping every literal atom (`F_SYM`, `F_INT`, ...) inside a
quasiquote in `(quote X)`.  That meant a body like ``\`(do ~@body)``
expanded to ``((quote do) body...)`` rather than ``(do body...)``.

When the result was elaborated, the call head was an `F_QUOTE` node,
not an `F_SYM`, so `elab_call`'s special-form dispatch (which keys on
`sym == e->sym_do`) was bypassed.  The elaborator instead fell into
the "dynamic call head" path, recursed into the quote, and tried to
resolve `do` as a binding -- producing the `unbound symbol 'do'` we
saw from `test/suite.tur`.

Two changes in `quasiquote_expand_form` fix it:

1. Atoms inside a quasiquote are returned as-is rather than being
   wrapped in `(quote ...)`.  They're already literal data in the
   resulting form, so the wrap was redundant and actively wrong for
   special-form symbols.
2. `F_UNQUOTE_SPLICING` items inside a list are kept wrapped (not
   pre-unwrapped) so the splice path in `substitute_params` can detect
   them and splice a list-valued rest parameter into the parent list.

A regression fixture lives at
`tests/fixtures/macro-quasiquote-special-form/`; it exercises
``\`(if ~test (do ~@body) nil)`` and ``\`(let [~var ~val] ~body)``
patterns end-to-end.

**Status:** landed.

---

## Execution order

| # | Task | Repo | Cost | Phase |
|---|------|------|------|-------|
| 1 | Add `DIAG_ERROR` to `parse_cmake_deps`/`parse_spices` for non-map values | turmeric | low | **this PR** |
| 2 | Rewrite all 17 `spices/*/build.tur` to use `#{...}` | turmeric-spices | low | **this PR** |
| 3 | Wrap all 26 test files in `(defmodule ...)` | turmeric-spices | low | **this PR** |
| 4 | Make stdlib auto-load honor `TUR_STDLIB_DIR` | turmeric | low | **this PR** |
| 5 | Verify `tur fetch --update` succeeds for every spice | turmeric-spices | n/a | **this PR** |
| 6 | Wire `tur test` to read `build.tur` and pass `-I` + cmake-deps flags (Bug 3) | turmeric | medium | **landed** |
| 7 | Resolve `import result` orphan-instance and unsafe issues (Bug 5) | turmeric stdlib | medium | **landed** |
| 8 | Add CI: run `tur fetch` + `tur test` for every spice in `turmeric-spices` | turmeric-spices | medium | **landed (`.github/workflows/ci.yml`)** |
| 9 | Fix auto-loaded macros being invisible inside imported-module macro expansions (Bug 6) | turmeric | medium | **landed** |

After items 1-8 land, every spice's `build.tur` parses, every test file
parses + elaborates, `tur fetch` succeeds, and `tur test` resolves the
right `-I` paths.  The last gate to actually running the tests
end-to-end is item 9 (Bug 6) -- the macro-hygiene issue that today
shows up as `error: unbound symbol 'do'` inside `test/suite.tur`.

---

## Verification

For items 1-5 the acceptance signals are:

- `cd turmeric-spices/spices/plutovg && tur fetch --update` produces
  `cmake/CMakeLists.txt`, downloads plutovg via FetchContent, builds
  `libplutovg.a`, and writes a non-empty `tur.lock`.
- The same is true for any other tier-2 spice in the workspace
  (`raylib`, `sqlite`, `png`).
- `tur check spices/plutovg/tests/plutovg/canvas_test.tur` -- with
  `TUR_STDLIB_DIR` set, the file type-checks (the import-only-inside-
  defmodule error disappears).
- `parse_cmake_deps` emits a clear error if someone writes `:cmake-deps {...}`
  instead of `:cmake-deps #{...}`.
- Running `tur` from any directory no longer spams the
  `cannot open 'stdlib/macros.tur'` warnings when `TUR_STDLIB_DIR` is set.

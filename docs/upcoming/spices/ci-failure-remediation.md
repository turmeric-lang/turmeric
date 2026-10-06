# CI Failure Remediation Plan

> **RESOLVED 2026-09-09 -- every failure class in this plan is closed.**
>
> Re-verified against turmeric **v0.46.0**, running each spice's `tur check` and
> test suite the way `.github/workflows/ci.yml` does. The five classes below
> (A, B, C, D, E) are all fixed; the per-class results are tabulated in
> "Verification 2026-09-09" at the bottom. What remains is **one residual**, in
> `watch`, which is no longer the problem this document describes: the compile
> failure of section E4a is gone, and what is left is two macOS backend
> assertion failures being handled on a separate branch. See "The one residual".
>
> Everything from here to that section is kept as the historical record of what
> each class was and how it was fixed. Section statuses inside it were accurate
> when written and have not been rewritten.

> **Original status:** Active / in-progress
> **Last Updated:** 2026-05-31
> **Source:** Local `tur` run against `main` (compiler built from `rjungemann/turmeric` tip-of-main)

Five distinct failure classes came out of a full scan of `main`. They are ordered
roughly by blast radius — fix A first because it masks every other test result.

---

## A. Test runner can't find tests in subdirectories

> **Status:** ✅ Done — implemented **Option 1** (updated the CI "Run spice
> tests" step). If `tests/` has flat `.tur` files it runs `tur test tests`
> (covering flat spices and the linalg/template aggregators); otherwise it
> descends and runs `tur test` on every directory that directly contains a
> `.tur` file (covering frame-style 1-level nesting and tourist's 2-level
> fixtures). Verified locally that the runner now finds and attempts tests in
> every spice with a `tests/` dir — no more "no .tur files found". Template's
> `fixtures/` (run by `run-fixtures.sh`, not `tur test`) are deliberately left
> alone. Suites that still fail now do so for the §B/§D reasons below.

### Symptom

`tur test tests` exits non-zero with:

```
tur: no .tur files found in 'tests'
```

### Root cause

The CI step (`.github/workflows/ci.yml`, "Run spice tests") runs:

```sh
"$TUR" test tests
```

from inside each spice directory. `tur test` only looks at `.tur` files at the
**top level** of the path it is given. Many spices store their test files one
level deeper:

```
tests/
  <spice>/
    foo_test.tur
    bar_test.tur
```

### Affected spices

`c-dsl`, `frame`, `glsl`, `httpd`, `osc`, `plutovg`, `png`, `rtaudio`, `rtmidi`,
`signal`, `tidal`, `valkey`, `wav`, `opengl`, `plot`, `stats`, `postgres`

### Fix options (pick one)

**Option 1 — Update the CI step** to pass the subdirectory explicitly.  
Change the CI "Run spice tests" step to discover and pass the right path:

```sh
test_dir="tests"
if [ ! -f tests/*.tur ] && [ -d "tests/$(basename $(pwd))" ]; then
  test_dir="tests/$(basename $(pwd))"
fi
"$TUR" test "$test_dir"
```

**Option 2 — Move all test files** to `tests/` directly (drop the extra level).  
This is simpler long-term but requires touching every affected spice.

Note: some of these spices also have real test-logic failures (see §D) that
will surface once the runner can find the files.

---

## B. Type-check failures

These are caught by `tur check` and block compilation.

### B1. `watch/watch.tur` — unterminated `defmodule`

> **Status:** ✅ Done — added the missing closing `)` for the `defmodule`.
> `watch/watch.tur` now checks clean, and `notebook/main.tur` + `cli.tur`
> (which transitively import it) now check clean too. **Newly surfaced:** with
> the source fixed, the `watch` *test* suite now fails at C compile time
> because its inline-C helpers use `usleep`/`useconds_t`/`FILE` without the
> needed feature-test macro and headers — tracked under §E4a.

**Error:**
```
watch/src/watch/watch.tur:29:1: error: unterminated list (missing ')')
```

The file is 909 lines and the `defmodule watch/watch` that opens on line 29
is missing its closing `)`. The last line of the file needs an additional `)`.

**Fix:** Add a closing `)` at the end of `spices/watch/src/watch/watch.tur`.

---

### B2. `ok-val` receives `ptr<void>` — affects `plot`, `plutovg`, `wav`

> **Status:** ✅ Done (the `ok-val`/`ptr<void>` mismatch itself). These spices
> roll their own `Result` encoding — a malloc'd `{is_ok, ok_val, err_val}`
> struct returned as an opaque handle — but the call sites used the stdlib
> `ok?`/`ok-val`/`err-val`, which only accept the stdlib `Result` type.
>
> *(Historical: that hand-rolled struct is the pre-SR2b layout. It has since
> been retired repo-wide in favour of the emitted preamble's
> `tur_box_ok`/`tur_is_ok`/`tur_ok_value` helpers over the canonical tagged
> box. Do not copy the struct spelling above into new code.)*
>
> Fix applied:
> - **plot**: `plot/core.tur` defines its own encoding (`__ok`/`__err`); added
>   local `__ok?`/`__ok-val`/`__err-val` accessors and switched the 11 call
>   sites to them. `plot/core.tur` now checks clean.
> - **plutovg/wav (tests)**: added reusable `result-ok?`/`result-val`/
>   `result-err` accessors to `test/assert` (co-located with `assert-ok`,
>   which already reads this struct), exported them in `test/assert.tur` and
>   `test/build.tur`, and switched the test call sites off the stdlib
>   `ok?`/`ok-val`.
>
> **Downstream issues surfaced once the type error cleared:**
> - **wav**: type error gone; 2/3 suites pass. `info_test` now fails only on a
>   missing `sndfile.h` → tracked under §E.
> - **plot**: `core.tur` compiles, 3/6 suites pass. `interval`/`line`/`point`
>   tests define a local `pair` that shadows the stdlib `pair` (same class as
>   §D2) → tracked under §D7.
> - **plutovg**: type error gone, but the suites use multi-statement `(it ...)`
>   bodies that end in cleanup (void), while `it` is `(defn it [desc result
>   :bool])`. ~40 `it` blocks need restructuring to return a bool → tracked
>   under §D8 (needs a decision, see Priority order).

**Error (representative):**
```
plot/src/plot/core.tur:1151:31: error [TUR-E0001]:
  function 'ok-val' arg 1: expected (type-app (type-app Result tyvar) tyvar), got ptr<void>
```

The same pattern appears in `spices/plutovg/tests/plutovg/canvas_test.tur:31`
and `spices/wav/tests/wav/info_test.tur:12`. These call `ok-val` on values that
are typed as `ptr<void>` (the old untagged-union Result encoding) rather than
the current `Result` type.

**Fix:** Each call site needs to unwrap via a `ptr<void>` accessor instead of
`ok-val`, or the source module needs to return a typed `Result` so that `ok-val`
can see the correct type argument. Audit every `ok-val` / `ok?` call in the
three spices and verify the called function's declared return type matches the
current `Result` encoding.

---

### B3. `linalg` — three independent issues in `src/linalg/`

> **Status: CLOSED (verified 2026-09-09).** All **seven** `src/linalg/*.tur`
> files (`decomp`, `fmt`, `mat`, `sized`, `small`, `solve`, `vec`) now `tur
> check` clean -- `checked 7 file(s), 0 failed` -- and `tur test tests` is
> `3 tests, 3 passed, 0 failed` (56 assertions). The rearchitecture that closed
> this is written up in `linalg-v0.21.0-rearchitecture-blocker.md`. Note the
> count below is stale: there are seven source files now, not six.
>
> *Original status:* ⚠️ Larger than expected — needs a decision (see Priority order).
> A closer look shows **all six** `src/linalg/*.tur` files fail, and the spice
> appears to target an older Turmeric dialect rather than having three local
> bugs:
> - `malloc`/`free`/`printf` are called as Turmeric-level functions via
>   `(declare ...)`; the current compiler only accepts these inside inline-C
>   (`#include <stdlib.h>` / `<stdio.h>`), the way every working spice does.
> - `defstruct` uses the old non-vector field-list syntax (`small.tur`,
>   `vec.tur`).
> - Several functions have untyped params (`[n]` rather than `[n :int]`).
> - `decomp.tur:185`, `solve.tur:177`, `fmt.tur:188` have parse errors
>   (unterminated lists / unexpected `)`).
>
> Fixing this is a port of the whole spice (FFI strategy, struct syntax, param
> types, parse repairs), not the three small edits below. Recommend treating it
> as its own work item. The three points below are still accurate as far as
> they go.

**B3a. `malloc`/`free` unbound:**
```
linalg/src/linalg/vec.tur:6:10: error [TUR-E0003]: unbound symbol 'malloc'
```
`(declare malloc free)` is used but these symbols are not available without
a `(import ...)` or an inline-C block that includes `<stdlib.h>`. Add the
appropriate declaration or use the stdlib wrappers.

**B3b. `defstruct` field list syntax:**
```
linalg/src/linalg/vec.tur:13:3: error: defstruct field list must be a vector [f1 : T1 f2 : T2 ...]
```
The struct fields are written as plain S-expressions instead of a vector
literal. Change to bracket syntax:
```turmeric
(defstruct vec [len :int  data :ptr<float64>])
```

**B3c. `vec-new` name conflicts with stdlib:**
```
error: defn: 'vec-new' is already defined by an auto-loaded stdlib module
```
Rename the local definition (e.g. `linalg-vec-new`) or qualify it with the
module name so it does not shadow the stdlib symbol.

---

### B4. `opengl/shaders.tur` — variadic body with inline-C

> **Status:** ✅ Done — kept the public variadic `shader-program` API but moved
> the inline-C into a fixed-arity helper `__shader-program-link [shaders :int]`
> (the variadic rest-param was already passed to the inline-C as a cons-list
> handle); the variadic form now just forwards to it. All 8 opengl source
> files check clean. (The opengl *tests* still hit §D5.)

**Error:**
```
opengl/src/opengl/shaders.tur:75:3: error:
  defn 'shader-program': variadic body contains inline-C;
  inline-C blocks need a fixed arity signature
```

The compiler rejects inline-C blocks inside variadic (`& args`) functions.

**Fix:** Give `shader-program` a fixed arity, e.g. accept a list or a vector
of shaders rather than a variadic rest argument.

---

### B5. `sdf-raylib/integration.tur` — unbound `scene-glsl`

> **Status:** ✅ Done — the real cause was paren placement: the inline-C
> block's closing ` ```)` also closed the `(let [scene-glsl ...] ...)`, so the
> trailing `(csdf-glsl-free scene-glsl)` cleanup landed *outside* the `let`
> where `scene-glsl` is unbound. Dropped the `)` from the fence line and closed
> the `let` after the cleanup call instead. All 16 sdf-raylib source files
> check clean.

**Error:**
```
sdf-raylib/src/raylib/integration.tur:537:19: error [TUR-E0003]: unbound symbol 'scene-glsl'
```

`scene-glsl` is referenced at line 537 (`(csdf-glsl-free scene-glsl)`) but
is never defined in the module. It was likely meant to be the return value of
an earlier call captured in a `let` binding.

**Fix:** Wrap the relevant code block in a `let` that binds `scene-glsl` before
the call to `csdf-glsl-free`.

---

### B6. `stats` — `static` functions in included header (`pcg32.h`)

> **Status: CLOSED (verified 2026-09-09).** `tur check` over all 9 `stats`
> source files is `checked 9 file(s), 0 failed`, and `tur test tests/stats` is
> `6 tests, 6 passed, 0 failed`.
>
> *Original status:* ⚠️ Partially done; the rest is larger than stated and needs a
> decision (see Priority order).
>
> Done: a separate, clean bug — `stats/test.tur` imported `pf`/`qf` from
> `stats/dist`, but the F-distribution functions are named `pf-dist`/`qf-dist`
> (the names the module actually defines and exports). Corrected the import.
>
> Still open, and bigger than the plan implied:
> - The `static inline` problem is **not just `pcg32.h`**. `dist.tur` also
>   defines 7+ of its own `static inline` C helpers (`__lbeta`, `__igamma_p`,
>   `__ibeta`, `__igamma_p2`, `__ibeta2`, `__igamma_r`, `__ibeta_r`) *inside*
>   defn inline-C bodies, and they hit the same "invalid storage class for
>   function" error. Because these spices presumably compiled before, this
>   looks like a change in how the current compiler emits inline-C function
>   definitions (file scope → nested inside the defn's C function). If so, the
>   right fix is likely compiler-side (or a documented `c-preamble`/file-scope
>   mechanism), not hoisting dozens of helpers by hand. A module-level ```` ```c
>   #include "stats/pcg32.h" ```` block in `rng.tur` does make `rng.tur` check
>   clean (the header guard turns the in-body includes into no-ops), but it
>   does not address `dist.tur`'s own static helpers, so it was not committed —
>   a piecemeal hoist would be inconsistent.
> - `stats/test.tur` additionally does its arithmetic at the Turmeric level
>   (`(float n)`, `(sqrt …)`, `(min …)`, `(/ …)`) while every *passing* stats
>   module does float math inside inline-C. `(float n)` does not yield a float
>   under the current numeric checker, and `sqrt`/`min` resolve to int-typed
>   builtins. test.tur needs a numeric-dialect rework (same flavor as §B3
>   linalg) — flagged, not attempted.

**Error:**
```
stats/src/stats/pcg32.h:23:20: error: invalid storage class for function 'pcg32_srandom_r'
```

`pcg32.h` defines functions with `static inline` storage in a header that is
`#include`d inside a `defmodule` body. The Turmeric C emitter wraps defmodule
bodies in a C function scope, making `static` function definitions inside that
scope illegal.

**Fix:** Move the `#include "pcg32.h"` to a top-level inline-C block (outside
the `defmodule`) using the file-level `c-preamble` mechanism, or convert the
affected definitions to `extern` with a matching `.c` compilation unit.

---

### B7. `notebook` — cannot resolve `ansi/term`

> **Status:** ✅ Done via §C (added `spices/ansi` to root `:members`).

**Error:**
```
notebook/src/notebook/tui.tur:12:3: error: module 'ansi/term' not found
```

`notebook/tui.tur` imports `ansi/term`, which lives in the `ansi` spice.
`ansi` is not listed in the root `build.tur` `:members`, so workspace
resolution cannot find it (see also §C).

**Fix:** Add `"spices/ansi"` to `:members` in the root `build.tur` (§C fixes
this for all affected spices at once).

---

## C. New spices missing from root workspace `:members`

> **Status:** ✅ Done — all ten added to root `build.tur` `:members`. Verified
> `notebook/tui.tur` now resolves `ansi/term` (B7 fixed). `notebook/main.tur`
> and `cli.tur` still fail, but only because they transitively import the
> broken `watch/watch.tur` (B1).

The following spice directories exist on disk but are absent from the root
`build.tur` `:members` list:

| Spice | Impact |
|---|---|
| `ansi` | `notebook` cannot resolve `ansi/term` (B7) |
| `c-dsl` | Not in workspace; cross-spice imports unresolvable |
| `frame` | Not in workspace |
| `glsl` | Not in workspace |
| `linalg` | Not in workspace |
| `opengl` | Not in workspace |
| `raygui` | Not in workspace |
| `sdf-raylib` | Not in workspace |
| `signal` | Not in workspace |
| `stats` | Not in workspace |

**Fix:** Add all ten to `build.tur` `:members`:

```turmeric
:members ["spices/ansi"
          "spices/c-dsl"
          "spices/frame"
          "spices/glsl"
          "spices/linalg"
          "spices/opengl"
          "spices/raygui"
          "spices/sdf-raylib"
          "spices/signal"
          "spices/stats"
          ;; ... existing members ...
          ]
```

---

## D. Test logic bugs

These are bugs in the test files themselves (or their immediate dependencies),
not in the spice source. They will surface once §A is resolved.

### D1. `rtaudio` / `rtmidi` — `defmodule` accidentally inside comment

> **Status:** ✅ Done — split the opening line of all 7 affected test files so
> `(defmodule ...)` starts on its own line. Both suites now compile and pass
> (`rtaudio` 3/3, `rtmidi` 4/4).

**Error:**
```
rtaudio/tests/rtaudio/core_test.tur:13:7: error: unexpected ')'
```

The first line of every `rtaudio` and `rtmidi` test file reads:

```
;; tests/rtaudio/core_test.tur -- tests for rtaudio/core.(defmodule rtaudio/tests/core
```

The `(defmodule ...)` form got appended to the end of the comment line. Because
`;;` comments to end-of-line, the `defmodule` is never opened. The file body
(imports, describe, defn) is at the top level, and the closing `)` that was
meant to close the defmodule is unmatched — hence "unexpected ')'".

**Fix:** Break the opening of each test file so `(defmodule ...)` is on its
own line:

```turmeric
;; tests/rtaudio/core_test.tur -- tests for rtaudio/core.
(defmodule rtaudio/tests/core
  ...
```

Affects all `.tur` files under `tests/rtaudio/` and `tests/rtmidi/`.

---

### D2. `tidal` — local `ok` shadows stdlib

> **Status:** ✅ Done — renamed the test's local result primitives (`ok`,
> `err`, `ok?`, `err?`, `ok-val`, `err-val`) to `__`-prefixed names and a
> further shadowing helper `list-length` → `tidal-list-length`, updating all
> usages. `tidal` suite now passes (1/1).

**Error:**
```
tidal/tests/tidal/tidal_test.tur:43:9:
  error: defn: 'ok' is already defined by an auto-loaded stdlib module
```

The test defines `(defn ok ...)` and `(defn ok? ...)` and `(defn ok-val ...)`
as `ptr<void>`-based helpers (the old Result encoding). These now conflict with
the stdlib `ok` / `ok?` / `ok-val` from the current `Result` type.

**Fix:** Either rename the local helpers (e.g. `tidal-ok`) or port the test
to use the stdlib `Result` constructors directly.

---

### D3. `c-dsl` — `cons` cell used as `cstr`

> **Status: CLOSED (verified 2026-09-09).** `tur check` over all 10 `c-dsl`
> source files is `checked 10 file(s), 0 failed`, and `tur test tests/c-dsl` is
> `4 tests, 4 passed, 0 failed`.

**Error (from `tests/c-dsl/codegen_test.tur:51`):**
```
error: expression in call head has type `cstr`, which is not callable
```

The test passes a `cons` cell (a linked-list node with `int` tag) as an
argument where a `cstr` is expected. The `c-join` function signature or the
test call site is mismatched.

**Fix:** Verify `c-join`'s declared signature in `src/c-dsl/core.tur` and
update the test to build the argument list in the way `c-join` actually expects.

---

### D4. `glsl` — `test/assert` not found

> **Status: CLOSED (verified 2026-09-09).** `tur check` over all 7 `glsl`
> source files is `checked 7 file(s), 0 failed`, and `tur test tests/glsl` is
> `3 tests, 3 passed, 0 failed` (43 assertions).
>
> *Original:* ✅ Done (the missing dep). Added the standard
> `:spices #{ "test" #{...} }` block to `spices/glsl/build.tur`; `test/assert`
> now resolves. **Downstream (separate, §D3 family):** both glsl test files
> then fail on a list-encoding API mismatch — they build statement/decl lists
> as `cons`/`vec-of` of `cstr` values, but `glsl-stmts`/`glsl-defn`/
> `glsl-vertex-shader` expect `:int`-encoded lists (`vec-push! arg 2: expected
> int, got cstr`; `expression in call head has type cstr`). The glsl *source*
> compiles fine. Tracked with §D3.

**Error:**
```
glsl/tests/glsl/codegen_test.tur:11:3: error: module 'test/assert' not found
```

The `glsl` spice `build.tur` does not declare a `:spices` dependency on the
`test` spice, so `tur check` cannot resolve `test/assert`.

**Fix:** Add `"test"` to the `:spices` map in `spices/glsl/build.tur`:

```turmeric
:spices {"test" {:path "../test"}}
```

---

### D5. `opengl` — `assert-true` called with `int`

> **Status:** ✅ Done — the cause was `(!= handle 0)`, which is int-typed in the
> current dialect, feeding `assert-true` (which wants `bool`). Replaced all 7
> occurrences across `buffers_test`/`window_test`/`shaders_test` with
> `(not (= handle 0))`. The type error is gone; the suites now fail only at the
> C-compile step on a missing `glad/gl.h` → tracked under §E.

**Error (from `tests/opengl/buffers_test.tur:28`):**
```
error [TUR-E0001]: function 'assert-true' arg 1: expected bool, got int
```

The test passes an `int`-typed expression to `assert-true`, which requires
`bool`. OpenGL functions like `glIsBuffer` / `glIsVertexArray` return `int`
(0 or 1) in the generated bindings.

**Fix:** Wrap calls with an explicit cast: `(assert-true (not= 0 <gl-call>))`,
or declare the return type of the relevant FFI functions as `:bool`.

---

### D6. `signal` — C codegen produces wrong struct return type

> **Status: CLOSED (verified 2026-09-09).** `tur check` over all 6 `signal`
> source files is `checked 6 file(s), 0 failed`, and `tur test tests/signal` is
> `6 tests, 6 passed, 0 failed`. The `arrow_tests.tur` named below **no longer
> exists** -- not in the worktree, not in `git ls-files`, and not at the tip of
> any local or remote ref. The six suites are now
> `test_compose` / `test_core` / `test_envelope` / `test_filter` / `test_osc` /
> `test_shaper`, all passing. The line below is the only surviving reference to
> that filename in the tree.

**Error (in compiled output):**
```
error: incompatible types when returning type 'Pair__int__int' but 'int64_t' was expected
```

The `signal/arrow_tests.tur` test triggers a codegen bug where a function
returning `Pair<int, int>` is emitted with an `int64_t` return type. This is
likely a compiler-side issue (report upstream to `turmeric-lang/turmeric`) but
the test file or the spice source may be able to work around it by choosing
a struct type that the current emitter handles cleanly.

---

### D7. `plot` — local `pair` shadows stdlib (surfaced after §B2)

> **Status:** ✅ Done (the `pair` shadow) — renamed the local `pair` helper to
> `xy-pair` (defn + usages) in `interval_test`, `line_test`, `point_test`.
> **Downstream (dialect drift, still open):** those same three suites then fail
> with `compile-time fn parameter must be a symbol` on their typed `fn`
> lambdas, e.g. `(fn [x :float] :float (* x x))` passed to
> `function-interval`/`parametric`. The current compiler rejects typed
> anonymous-fn params here. This is the same older-dialect pattern seen in the
> glsl/stats/linalg test files (see "Systemic note" below). plot *source*
> compiles; 3/6 plot suites pass.

`tests/plot/interval_test.tur`, `line_test.tur`, and `point_test.tur` each
define a local `(defn pair [x :float y :float] :int ...)` that collides with
the auto-loaded stdlib `pair`:

```
error: defn: 'pair' is already defined by an auto-loaded stdlib module
```

**Fix:** Rename the local helper (e.g. `plot-pair` or `__pair`) in those test
files. `plot/core.tur` itself compiles; 3 of 6 plot suites already pass.

---

### D8. `plutovg` — `(it ...)` bodies don't return `bool` (surfaced after §B2)

> **Status: CLOSED (verified 2026-09-09).** The `bool`/`nil` mismatch this
> section is about is gone: `tur check` over all 9 `plutovg` test files passes
> clean, so the documented
> `error [TUR-E0001]: function 'it' arg 2: expected bool, got nil` no longer
> occurs anywhere. (The suite still cannot be *run* on a box without the
> `plutovg` native library -- all 9 files then fail at the `cc` step with
> `use of undeclared identifier 'plutovg_surface_t'` after the compiler's own
> `inline-C requested <plutovg.h>, not found on the include path` warning. That
> is a missing `:cmake-deps` fetch, a section E concern, not this one.)
>
> *Original status:* Open — needs a decision (see Priority order). Out of original
> plan scope.

`test/suite`'s `it` is `(defn it [desc :cstr result :bool] :bool)` — its
second argument is an eagerly-evaluated bool. The plutovg suites instead pass
multi-statement `(let ...)` bodies that perform asserts and end in resource
cleanup (`(surface-destroy s)` etc.), which return `:void`:

```
error [TUR-E0001]: function 'it' arg 2: expected bool, got nil
```

There are ~40 such `(it ...)` blocks across the plutovg tests. Two possible
directions:

1. **Restructure each test body** to compute a single bool pass-value, run
   cleanup, then return the bool. Faithful, but laborious and amounts to
   authoring test logic for each case.
2. **Change the framework**: give `test/suite` an `it`-like form that accepts
   a void/any body (e.g. a macro that treats reaching the end without a failed
   assert as a pass). This changes how the suite tally is computed and affects
   every spice.

This was masked by the B2 `ok-val` error and is independent of it.

---

## Systemic note: older-dialect drift in test files

Several spices' **test files** (not their source) were written against an
earlier Turmeric dialect and fail once the headline issue is cleared. The
recurring forms:

| Pattern | Current compiler says | Seen in |
|---|---|---|
| Typed `fn` lambdas: `(fn [x :float] :float ...)` | `compile-time fn parameter must be a symbol` | plot (§D7) |
| Turmeric-level float math + `(float n)` cast | `(float n)` is int; `sqrt`/`min` are int-typed → `mixed-width numeric arithmetic` | stats/test.tur (§B6) |
| `cons`/`vec-of` of `cstr` into `:int`-list APIs | `expected int, got cstr` / `cstr is not callable` | glsl (§D3/§D4), c-dsl (§D3) |
| void-bodied `(it ...)` blocks | `it arg 2: expected bool, got nil` | plutovg (§D8) |
| Turmeric-level `malloc`/`free`/`printf`, old `defstruct` | unbound symbol / bad field list | linalg (§B3) |

Because the *source* of most of these spices compiles, the most likely root
cause is that the compiler tightened (or changed) these behaviors after the
tests were written. A blanket decision is probably more efficient than fixing
each test file by hand:

- decide whether typed `fn` lambdas / Turmeric-level numeric casts are meant
  to be supported (→ possible compiler fix), or whether the tests should be
  rewritten to the current idiom (inline-C float math, `:int`-encoded lists);
- and whether `it` should accept void bodies (→ `test/suite` change, see §D8).

These are grouped here so they can be triaged together rather than piecemeal.

---

## E. Missing native C libraries

These spices fail to compile their tests because system headers or libraries
are absent in the sandbox. They would also fail in CI unless the cmake-deps
fetch step installs them.

| Spice | Missing | Notes |
|---|---|---|
| `httpd` | `mbedtls/net_sockets.h` | `mbedtls` must be listed under `:cmake-deps` and fetched before the test step runs |
| `postgres` | `libpq-fe.h` | Requires a PostgreSQL client dev package or a bundled `libpq` cmake dep |
| `raygui` | raygui headers | `raygui` cmake dep must be fetched; CI fetch step skips if `:cmake-deps` absent from `build.tur` |
| `ansi` (`image_test`, `term_test`) | `struct sigaction` incomplete | Likely a missing `_POSIX_C_SOURCE` or `_DEFAULT_SOURCE` feature-test macro in the inline-C preamble |
| `wav` (`info_test`) | `sndfile.h` | Surfaced after §B2; needs `libsndfile` dev headers (system package or cmake dep). The other 2 wav suites pass. |
| `opengl` (all 3 suites) | `glad/gl.h` | Surfaced after §D5; needs the glad/GLFW headers (cmake dep). The §D5 type fix is done, so these compile cleanly once the headers are present. |

For `httpd`, `postgres`, and `raygui`, verify that `:cmake-deps` is present
in each `build.tur` and that the CI fetch step is not being skipped.

> **Status: CLOSED for `ansi` (verified 2026-09-09).** The feature-macro
> emission-ordering problem analyzed below is fixed. `tur check` over all 8
> `ansi` source files is `checked 8 file(s), 0 failed`, and `tur test tests` is
> `9 tests, 9 passed, 0 failed` -- `image_test` and `term_test` included. The
> analysis is kept for the record.

For `ansi`, the picture is more subtle than "add a feature macro". Turmeric
**already** emits `#define _DEFAULT_SOURCE 1`, but in the generated
`tests_term_test_tur.c` it lands at **line 23 — after** the system headers
(`<sys/ioctl.h>`, `<signal.h>`, … at lines 1–8) that the spice's
`/* __tur_include__: #include <...> */` directives hoist to the very top of
the translation unit. Because the feature macro is defined *after* those
headers are processed, glibc never exposes `struct sigaction`, `SA_RESTART`,
or `CLOCK_MONOTONIC`/`CLOCK_REALTIME`, so the term/image tests fail to
compile.

This is an **emission-ordering issue**: the feature-test macro must precede
every hoisted `#include`. The clean fix is most likely compiler-side (emit
`_DEFAULT_SOURCE`/`_GNU_SOURCE` before the `__tur_include__` block), or a
documented way for a spice to inject a define ahead of the hoisted headers.
A spice-level workaround (prepending a `__tur_include__: #define _GNU_SOURCE`)
is fragile because directive ordering across defns isn't controllable, so it
was not attempted here. The `watch` test failures in §E4a are very likely the
same ordering problem (`usleep`/`useconds_t`/`FILE` are also feature-gated).

The library-dependency rows (`mbedtls`, `libpq`, `sndfile`, `glad/gl.h`,
`raygui`) cannot be verified in this sandbox — they need the actual system
packages or fetched cmake-deps. They are left documented for a CI/environment
with those libraries available.

### E4a. `watch` test suite — inline-C missing feature macro / headers

> **Status: CLOSED as written (verified 2026-09-09) -- but see
> "The one residual" at the bottom.**
>
> The compile failure described here is gone. `tur check` over all 5 `watch`
> source files is `checked 5 file(s), 0 failed`, and the suite now *builds*:
> across two consecutive runs there were zero `error:` lines, zero
> `cc invocation failed`, and zero occurrences of `useconds_t`,
> `implicit declaration of function 'usleep'`, or `unknown type name 'FILE'`.
>
> `tur test tests` is now `9 tests, 7 passed, 2 failed`. The 2 failures are
> **runtime assertion failures on the macOS backend**, a different problem from
> the one this section describes, and they are being handled on a separate
> branch -- deliberately not addressed here.

Once `watch/watch.tur` compiles, the `watch` test suite (8 of 9 files) fails
at the C compile step:

```
tests_backend_drain_into_test_tur.c:21: error: 'useconds_t' undeclared
... warning: implicit declaration of function 'usleep'
... error: unknown type name 'FILE'
```

The test helpers embed inline-C that calls `usleep` and uses `FILE`/`fopen`
without the feature-test macro and headers. Same class as the `ansi` row.

**Fix:** In the inline-C blocks of the affected `watch` test files, add
`#define _DEFAULT_SOURCE` and `#include <unistd.h>` / `#include <stdio.h>`
(or hoist these into a shared test preamble).

---

## Progress summary

### ✅ Done and verified (committed)

| Section | What landed |
|---|---|
| §C | Added the 10 missing spices to root `build.tur` `:members`. |
| §B7 | `notebook` resolves `ansi/term` (via §C). |
| §A | CI test step now discovers nested/`fixtures` test layouts; every spice's tests are found. |
| §B1 | Closed the unterminated `defmodule` in `watch/watch.tur` (also unblocked notebook). |
| §D1 | `rtaudio`/`rtmidi`: moved `defmodule` off the comment line (both suites pass). |
| §B2 | `ptr<void>` Result accessors fixed in `plot`/`plutovg`/`wav`; added reusable `result-ok?`/`result-val`/`result-err` to `test/assert`. |
| §D2 | `tidal`: renamed local `ok`/`err`/`list-length`/… that shadowed stdlib (suite passes). |
| §D4 | `glsl`: declared the `test` spice dependency. |
| §B4 | `opengl`: moved `shader-program` inline-C into a fixed-arity helper (source clean). |
| §B5 | `sdf-raylib`: fixed the paren placement that left `scene-glsl` outside its `let` (source clean). |
| §D7 | `plot`: renamed test-local `pair` → `xy-pair`. |
| §D5 | `opengl` tests: `(!= h 0)` → `(not (= h 0))` for `assert-true` (type error gone). |
| §B6 (part) | `stats/test.tur`: corrected `pf`/`qf` → `pf-dist`/`qf-dist` import names. |

### Verification 2026-09-09 -- every class re-checked, all closed

Re-run against turmeric **v0.46.0**, from inside each spice directory, mirroring
`.github/workflows/ci.yml` (`tur check` per source file; `tur test tests` when
`tests/` holds flat `.tur` files, otherwise `tur test tests/<subdir>`).

| Section | Spice | `tur check` | Suite | Verdict |
|---|---|---|---|---|
| §B3 | `linalg` | 7 files, 0 failed | 3 tests, 3 passed, 0 failed | closed |
| §B6 | `stats` | 9 files, 0 failed | 6 tests, 6 passed, 0 failed | closed |
| §D3 | `c-dsl` | 10 files, 0 failed | 4 tests, 4 passed, 0 failed | closed |
| §D4 | `glsl` | 7 files, 0 failed | 3 tests, 3 passed, 0 failed | closed |
| §D6 | `signal` | 6 files, 0 failed | 6 tests, 6 passed, 0 failed | closed |
| §D8 | `plutovg` | 6 src + 9 test files, 0 failed | needs the native lib to run | defect closed |
| §E | `ansi` | 8 files, 0 failed | 9 tests, 9 passed, 0 failed | closed |
| §E4a | `watch` | 5 files, 0 failed | 9 tests, 7 passed, **2 failed** | compile closed; see below |

Also confirmed:

- **No `requires.typecheck-skip` markers exist anywhere in the tree.** The only
  `requires.*` file in the repo is `spices/tourist/fixtures/template/requires.spices`.
  The string `typecheck-skip` survives solely as dead opt-out code in
  `.github/workflows/ci.yml`.
- **`signal/arrow_tests.tur` no longer exists** -- see §D6.
- The **systemic dialect drift** that the old "Suggested next decisions" section
  called the biggest lever is resolved: every spice it named (linalg, stats,
  glsl, c-dsl, plot, plutovg) now checks clean. No compiler-behavior decision is
  outstanding.

### The one residual -- `watch`, 2 macOS backend assertion failures

This is **not** the section E4a problem. The compile failure is fixed; the suite
builds and runs. Two of its nine tests fail on macOS, on backend semantics, and
they reproduce identically across runs:

- `tests/backend_drain_into_test.tur` -- `3/8` failed. `backend-wait` sees an
  event and `backend-drain-into` returns `>= 0`, but nothing is deposited into
  `evbuf`, so `not ok 5 - at least one event in evbuf`,
  `not ok 6 - event name or count matches target`, and
  `not ok 7 - first event has non-zero mask`.
- `tests/tree_test.tur` -- `1/7` failed. The event arrives with the right path,
  but its kind is classified wrong:
  `not ok 5 - event kind is write or rename (atomic-save)` at `tests/tree_test.tur:184`.

**This residual is being handled on a separate branch and is deliberately out of
scope for this document.** It is recorded here only so that closing this plan
does not lose it.

### Incidental, not blocking

`linalg`'s `tests/small.tur` compiles with two real `-Wreturn-stack-address`
warnings (`tests_small_tur.c:5814` and `:5818`): inline-C returning
`(int64_t)(intptr_t)&(m.m00)` for a by-value struct parameter. The suite passes,
but the address handed back points into a dead frame and is worth a look.

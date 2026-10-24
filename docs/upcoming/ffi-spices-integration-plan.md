# FFI x spices: closing the gaps between the dynamic FFI and the package layer

> **Status:** re-audited 2026-10-24 against `main` (289d688d): S1, S2, S4
> implemented (2026-08-17) and still in the tree; S3 satisfied -- its
> jit-ffi-c2mir-plan F4/F5 landed 2026-08-18 (follow-ons 2026-08-21, aarch64
> HFA fixed 2026-08-26), and that plan graduated in 0.38.0 and is archived.
> Re-verified 2026-10-24: `tests/turi/repl-spice-linklibs.sh` (S1) and the
> `s2-variadic-*` cases of `tests/turi/repl-spice-call.sh` (S2) pass on the
> JIT build.
> **Remaining: S5 only. Decided 2026-10-24: Option A** -- give variadic
> exports `__ffi` shims, then delete the shape table, after closing a CI
> coverage gap first; the ordered steps are in S5. Independent of the
> raylib web playground plan, which cannot use the table in the browser
> anyway (measured under emcc, see S5).  Its old blocker
> moved on 2026-10-02: `TUR_JIT` now
> defaults ON on 64-bit x86-64 and arm64 (MIR vendored under `external/mir/`),
> so default builds and release binaries carry the c2mir thunk provider.  But
> `-DTUR_JIT=OFF` and the hosts the default leaves off (ppc64, s390x, riscv64)
> are still supported configurations, and on those the shape table
> (`src/runtime/ffi_dispatch_thunk.c`, `tools/gen_ffi_dispatch.py`) is still
> the live non-JIT rung.  Deleting it now means deciding that a non-JIT build
> loses the interpreter's dynamic FFI -- a decision, no longer a blocker.
> As of 2026-10-24 the table, its generator and the `--max-arity` advice
> (`src/turi/ffi_thunk.c:360-365`) are all still present; see S5.
> Proposed 2026-08-17.
> **Track:** post-v1, incremental -- each phase is independently landable.
>
> **Implementation notes:**
> - S1 went further than the fix shape: `:build-opts :link-libs` turned out
>   to be parsed and consumed NOWHERE (schema + docs only), so
>   `drv_collect_build_aux` now appends it for every consumer -- the cc link
>   line gets it too, not just the REPL hook.  A hook build failure also no
>   longer fails the whole spice load: the loader falls back to the
>   subprocess path (which handles vendored :c-sources and static-only
>   cmake deps the engine cannot).  Test:
>   `tests/turi/repl-spice-linklibs.sh` (hermetic -- builds its own .so),
>   ctest `tur_repl_spice_linklibs`.
> - S2 surfaced a latent loader bug: the exports-manifest parser pushed an
>   extra arg slot for the `&` marker even though `fd->n_params` already
>   includes the rest formal (`(:int :int & :int)` parsed as arity 3, and a
>   :float rest slot would have classed 'f' where the callee takes a
>   pointer) -- harmless only while variadic exports were rejected
>   outright.  Fixed alongside the marshaller; the rest-list builder
>   mirrors the compiled EX_CONS_LIST emission (right-folded malloc'd
>   cells, float heads as IEEE-754 bit patterns, never freed -- matching
>   compiled semantics).  Tests: `s2-variadic-*` in
>   `tests/turi/repl-spice-call.sh`, green on all three dispatch rungs
>   (shape table, JIT thunk, in-process hook).
> - S4: the constants convention is documented in
>   developing-spices-guide.md ("Constants: #defines do not survive
>   linking").
> **Builds on:** [jit-ffi-c2mir-plan.md](../archive/jit-ffi-c2mir-plan.md) (F1-F5
> implemented; graduated 2026-08-21),
> [docs/guides/developing-spices-guide.md](../guides/developing-spices-guide.md),
> [docs/guides/ffi-guide.md](../guides/ffi-guide.md).

## 0. Summary

The dynamic FFI (JIT call thunks, thunk-backed extern-c, `call-ptr`) and
the spice layer (`:cmake-deps`, `:build-opts :link-libs`,
`:c-sources`/`:c-includes`, per-export `__ffi` shims) each work; this
plan is the survey of where they meet and don't. One genuine defect
(S1), one pre-planned marshalling gap that this work makes more visible
(S2), two phases that are cross-references into the jit-ffi plan's own
trailing phases (S3), and two small documentation/cleanup items (S4,
S5). Ordered by value.

## 1. What exists today (verified against the tree, 2026-08-17)

- A spice wraps a C library with `:cmake-deps` (fetch+build via CMake,
  flags recorded in `cmake/spice-deps-manifest.json`) and/or
  `:build-opts :link-libs`; `tur build`/`tur run` inject `-I/-L/-l`
  from the manifest (`drv_collect_build_aux`, `src/driver/cc.c`).
- Stdlib-style bare sources instead embed `/* __tur_autolink__: ... */`
  markers in inline-C; `scan_autolink_markers` (`src/driver/cc.c`)
  collects them from the emitted C.
- The REPL loads a spice either by subprocess `tur build --shared` +
  dlopen, or -- in JIT builds -- in-process via the `TurSpiceJitHook`
  (`repl_jit_build`, `src/driver/jit.c`).
- REPL calls into spice exports resolve through the FFI ladder
  (`src/turi/ffi_thunk.c`): JIT thunk (any signature) -> per-export
  `__ffi` shim -> generated shape table.

## 2. Phases

### S1 -- thread `:cmake-deps` / `:link-libs` flags into the REPL's in-process JIT hook

**The defect.** `repl_jit_build` collects only `__tur_autolink__`
markers (then `src/main.c:3920-3922`; today `src/driver/jit.c:723`) before handing the TU to
`tur_jit_compile_image`. A spice that declares its C dependency the
*recommended* way -- `:cmake-deps` or `:build-opts :link-libs`, with no
marker in any source -- compiles fine in-process, but nothing dlopens
the dependency, so `MIR_link`'s `dlsym(RTLD_DEFAULT)` resolver cannot
see its symbols and the load fails (or worse, resolves against an
unrelated homonym already in the process). The subprocess path is
unaffected because `tur build` injects the manifest flags into cc.

**Fix shape.** In `repl_jit_build`, call the same
`drv_collect_build_aux`/`pkg_cmake_manifest_append_cc_flags` pair
`cmd_build` uses and append the collected `-L`/`-l` entries to the
autolink string handed to `tur_jit_compile_image`. Two engine-side
follow-ons in `jit_load_autolink` (`src/jit_engine.c:670`):

- It currently ignores `-L` entirely and probes `lib<name>.so` by
  soname only. Track `-L` dirs and try `<dir>/lib<name>.so` (and
  `.dylib` on macOS) before the bare soname.
- A static-only cmake dep (`.a`) cannot be dlopened; when resolution
  fails, fail the hook cleanly so the loader falls back to the
  subprocess path -- the behavior today's marker-less spices already
  get, minus the mystery.

**Test.** A fixture spice under `tests/turi/` with `:build-opts
{:link-libs ["-lm"]}` (universally present) whose export calls a libm
symbol the process does not otherwise pull in; assert it is callable at
the REPL with the JIT hook active.

**Done (2026-08-17; re-verified 2026-10-24).** `repl_jit_build` calls
`drv_collect_build_aux` and appends the cmake/`:link-libs` flags to the
autolink string (`src/driver/jit.c:728-766`), failing the hook to the
subprocess path when the spice vendors `:c-sources`; `jit_load_autolink`
honours `-L` dirs and probes `<dir>/lib<name>.so` / `.dylib` before the bare
soname (`src/jit_engine.c:670`).  Test: `tests/turi/repl-spice-linklibs.sh`,
ctest `tur_repl_spice_linklibs` (`CMakeLists.txt:1652`).

### S2 -- variadic spice exports (cons-list marshalling)

`ffi_native_shim` still rejects variadic exports outright
(`src/turi/ffi_thunk.c`: "cons-list marshaling lands in a later RP") --
a promise predating the thunk engine. Thunks make this deliverable now:
a variadic call site *has* a concrete arity and class list at the
moment of the call, which is exactly a signature string -- render
"the fixed prefix + the walked rest-list classes", get a thunk, call.
No shape table entry could ever cover this; the thunk cache handles the
per-call-shape variety naturally. Non-JIT builds keep the clean
rejection.

**Done (2026-08-17; re-verified 2026-10-24).** The paragraph above
describes the tree before the fix: `ffi_native_shim`
(`src/turi/ffi_thunk.c:167`) now accepts a variadic export, checks "at
least" the fixed arity, and builds the rest list with
`ffi_build_rest_list` (`src/turi/ffi_thunk.c:118`).  It went further than
"non-JIT builds keep the rejection": the call also works through the
shape-table rung, within its arity ceiling.  The compiled side emits no
`__ffi` shim for a variadic export (`emit_ffi_export_shims`,
`src/compiler/emit_module.c`), so on a non-JIT build the shape table is the
only rung a variadic export has -- which matters for S5.

### S3 -- struct-by-value and callbacks for spice APIs (cross-reference)

These are jit-ffi-c2mir-plan F4/F5, not new work, but the spice layer
is where they pay off; record the dependency here so neither plan
forgets the other:

- **F4 (struct-by-value)** retires the `'?'` class, which today makes a
  spice export returning/taking a small struct silently shim-less and
  REPL-uncallable. The signature registry F4 needs ("single source of
  truth shared between elaborator and turi") should be designed against
  the spice **exports manifest**, which already serializes per-export
  type information -- extending it with layout hashes is the natural
  transport.
- **F5 (callbacks)** is what lets a spice expose C APIs that take
  function pointers (`qsort`, reactor-style registrations, zmq socket
  monitors) with Turmeric closures from the REPL. Today's reactor
  spice-side workaround (fat-closure handles through `:int`) is the
  measure of the hole.

**Satisfied (2026-08-18; note 2026-10-24).** F4 and F5 landed in the
archived jit-ffi-c2mir-plan.  F4 did not build the shared signature
registry suggested above: it encodes layout inline in the signature string
instead, so the "extend the exports manifest with layout hashes" transport
was never needed -- superseded, not open.  No spice in turmeric-spices
(origin/main bdeea3f) uses `callback-ptr` yet; adopting it is per-spice
work, not an item of this plan.

### S4 -- constants do not survive dynamic linking: make wrapping them a documented convention

`#define ZMQ_REP 4` never reaches a symbol table, so both the dynamic
FFI and REPL users of a spice must restate constants (see the libzmq
example in the FFI guide). No mechanism needed -- a spice should export
them as plain defns/defs next to its externs -- but the convention
belongs in developing-spices-guide.md's C-wrapping section, with a
sentence on keeping them adjacent to the `extern-c` block they belong
to so drift is reviewable.

**Done (2026-08-17).** developing-spices-guide.md, "Constants: `#define`s do
not survive linking -- re-export them" (line 569).

### S5 -- shape-table retirement bookkeeping

jit-ffi-c2mir-plan section 2.4 demoted the generated table
(`tools/gen_ffi_dispatch.py`, `src/runtime/ffi_dispatch_thunk.c`) to
the non-JIT fallback rung. Once JIT builds are the default REPL
configuration, the table (and its generator, and the `--max-arity`
regeneration advice in the ladder's error message) can be deleted.
Blocked on the default flipping; record it so the 381-entry file does
not outlive its reason.

**State on 2026-10-24.** The default flipped on 2026-10-02
(`CMakeLists.txt:145-178`), so the blocker is gone, but nothing has been
deleted: `src/runtime/ffi_dispatch_thunk.{c,h}` (still 381 shapes: arity
0-6 over i/f, three return classes), `src/runtime/ffi_dispatch.{c,h}`,
`tools/gen_ffi_dispatch.py`, and the regeneration advice in the ladder's
error (`src/turi/ffi_thunk.c:360-365`) are all live.

#### How a spice export is called (the ladder)

`ffi_native_shim` (`src/turi/ffi_thunk.c`) tries, in order:

1. **JIT thunk** -- c2mir synthesizes an exact-signature trampoline (any
   arity, any int/float mix). JIT builds only; skipped for record-taking
   exports.
2. **`__ffi` shim** -- the spice's own library carries a per-export
   trampoline with the real C types. Emitted since 2026-07-22
   (interpreter-arbitrary-arity-ffi) on every library build: the `--shared`
   module path always (`emit_module.c`, `emit_ffi_export_shims`), the REPL's
   in-process single-file build when it sets `g_emit_ffi_export_shims`
   (`src/driver/jit.c:704`).
3. **Shape table** -- the 381 generated trampolines. The table's only caller
   is this rung (`ffi_thunk.c:354`); plain `extern-c` under `--interpret`
   never used it (it needs the JIT; see `requires.jit`).

Rung 3 is reached in exactly three cases: a spice library built before
2026-07-22 (no shims); a **variadic** export on a build without the JIT,
because the emitter skips variadic exports (`emit_module.c`, "Skipped ...:
variadic exports"); and a JIT build whose thunk synthesis failed. Builds
without the JIT: Emscripten and Apple's embedded platforms (which cannot load
a native spice library at all), 32-bit hosts, and ppc64 / s390x / riscv64
unless configured `-DTUR_JIT=ON`.

#### Measured 2026-10-24 on a `-DTUR_JIT=OFF` build

All five REPL spice suites pass on a JIT-less build today: `repl-spice-call`
17/17 (the arity-12 and int/float-split calls through rung 2, the three
`s2-variadic-*` cases through rung 3), `-load` 9/9, `-errors` 5/5, `-reload`
4/4, `-linklibs` 1/1. They also pass on the JIT build.

**The gap.** No CI job runs them without the JIT. The JIT-less job
(`ci.yml`, "Configure (Debug, JIT off)") runs only `tests/run-turi.sh`, and
every other job has the JIT, where rung 1 answers scalar calls before rung 2
is tried -- so in CI rung 2 is reached only by record-taking exports. After
this phase rung 2 is all a JIT-less build has, and nothing would notice it
breaking.

#### Decision (2026-10-24): Option A -- variadic shims, then delete

Options weighed: (A) give variadic exports shims, then delete the table;
(B) delete as is, losing variadic exports from the REPL on JIT-less builds;
(C) keep the table permanently as the JIT-less rung (~200 KB of generated C
and its generator, maintained for a rare case). A, because the gap B would
open is small to close: a variadic export's C function is its fixed
parameters plus **one int slot holding the rest list** -- the S2 path already
counts that slot in `n_args` and builds the list (`ffi_thunk.c`, "S2: a
variadic export's n_args INCLUDES the trailing rest-list slot") -- so its
shim is an ordinary shim with one more int parameter.

#### Steps, in order

- [ ] **1. Cover the JIT-less ladder in CI.** Add the REPL spice suites
      (`tests/turi/repl-spice-{call,load,errors,reload,linklibs}.sh`) to
      the JIT-less CI job. They are green there today, so this is a pure
      addition. The three `s2-variadic-*` cases are this phase's
      acceptance test: deleting the table without step 3 makes exactly
      them fail.
- [ ] **2. (Optional) Exercise rung 2 on JIT builds too.** A test-only
      switch that skips rung 1 for scalar exports, run with the
      spice-call suite, so the shim rung is covered on macOS and Windows,
      not just the Linux JIT-less job.
- [ ] **3. Emit `__ffi` shims for variadic exports.** Stop skipping them in
      `emit_ffi_export_shims` and read the rest slot as an int parameter.
      Acceptance: the `s2-variadic-*` cases pass on the JIT-less build
      with the table still present (so they pass through rung 2, not 3).
      No fixture snapshot moves: shims are emitted only in library builds
      (`--shared`, the REPL's in-process build), never in the single-file
      output `expected.c` records.
- [ ] **4. Delete the table.** Remove `src/runtime/ffi_dispatch{,_thunk}.{c,h}`,
      `tools/gen_ffi_dispatch.py`, the `tur_ffi_dispatch_unit` test
      (`tests/runtime/ffi-dispatch-unit.c`, its `add_executable` in
      `src/CMakeLists.txt`, its ctest entry and the two ctest part lists in
      `CMakeLists.txt` that name it) and the generated files' warning
      carve-outs in `src/CMakeLists.txt`. Replace rung 3 in `ffi_thunk.c`
      with an error for a library that has no shim: "rebuild the spice".
      Update what refers to the old rung: `tests/turi/repl-spice-errors.sh`
      (asserts the `gen_ffi_dispatch.py --max-arity` hint is in the binary),
      `docs/guides/repl.md` (the `--max-arity 8` advice),
      `docs/guides/compiler-internals.md` (the runtime file listing), and the
      comments in `spice_loader.{c,h}`, `ffi_thunk.h` and
      `tests/unit/experiments_user_config.c`. Archived plans keep their
      history.
- [ ] **5. Archive this plan** -- S5 was the last open item.

#### The raylib web playground: no dependency (measured 2026-10-24)

[raylib-web-playground-plan](raylib-web-playground-plan.md) (draft) step A0
once planned to call baked spice exports from the browser interpreter -- a
JIT-less build -- through this shape table. That was checked under emcc
6.0.13 and the table cannot serve it: exports keep their real C types, the
table calls every int-class argument through an `int64_t` function-pointer
cast, and wasm32 traps on the mismatch (`RuntimeError: null function or
function signature mismatch`) for every export with a `bool` or `cstr`
parameter -- 15 of the raylib spice's 84 defns, the input predicates and
every `load-*` among them. The `__ffi` shims work for all of them. That
plan now carries the shims in its static image (its A0/A1), so it needs
nothing from the table, and step 4 here can land independently of it.

Cross-referenced from aot-compiled-repl-plan.md "Open questions" item 1.

## 3. Explicitly not doing

- **A dlclose lifecycle for spice-loaded dependencies.** Autolinked and
  `dlopen`'d libraries stay mapped for the process lifetime, matching
  the interpreter's process-lifetime posture for closures and natives;
  spice `(reload)` already retires old images without unmapping. A
  library that must be unloaded mid-session is out of scope -- the
  thunk cache holds no per-target state, but user-held `dlsym` pointers
  dangle on dlclose and no marshalling layer can fix that.
- **Auto-generating extern-c declarations from C headers.** A binding
  generator is a tool, not a language feature, and c2mir's preprocessor
  is not a header-parsing service; revisit only if a spice author
  actually asks.

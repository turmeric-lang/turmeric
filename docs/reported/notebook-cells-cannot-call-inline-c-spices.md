# Notebook cells can import `plot`, `linalg`, `stats` and `frame` but cannot call them: their work is inline C, which the interpreter does not run

**Severity:** medium (feature gap). The notebook is the literate-programming
entry point, and four of the five example notebooks
([notebook-docs-examples-plan](../upcoming/notebook-docs-examples-plan.md) C1)
are walkthroughs of spices whose functions are inline-C bodies. Since
[notebook-eval-no-module-base-dir](../archive/notebook-eval-no-module-base-dir.md)
was fixed (2026-10-10) every import in them resolves; the first call into the
spice then fails, cleanly, with the interpreter's inline-C refusal.

Filed 2026-10-10, closing that report. **Narrowed the same day** (see the
end): stats, frame, plot and linalg cells now run their spices compiled;
what is left is a cell's `defn` handed to compiled code as a callback.

## Repro

From `turmeric-spices/spices/notebook`, with a `tur` built from `main`:

```
tur build . -o /tmp/tur-nb
/tmp/tur-nb check examples/stats-walkthrough.tur.md
```

```
[] ERROR: eval: inline-C not supported in interpreter mode (function uses a native C implementation; run it with `tur build`/`tur run` instead of `--interpret`)
```

The same for `linalg-walkthrough` (`linalg/fmt`'s `mat-print`, `linalg/solve`),
`frame-walkthrough` and `data-analysis` (`frame/csv`), and `plot-walkthrough`
(`plot/core`, plus an inline-C `defn` written in the cell itself).
`math-walkthrough`, which calls nothing native, checks clean.

A pure-Turmeric spice works end to end -- `(import linalg/mat :refer
[mat-identity mat-rows])` in one cell and `(mat-rows (mat-identity 3))` in the
next answers `3` (the notebook's `tests/session_test.tur` case 5).

## Root cause

A notebook cell runs in libturi's tree-walking interpreter
(`spices/notebook/src/notebook/session.tur`, `session-eval` ->
`turi_eval_typed`). The interpreter runs an inline-C body only when
`try_exec_simple_inline_c` (`src/turi/eval.c`) recognises it as one of a dozen
small shapes (a flat constructor, a field accessor, a `free`, ...). Real spice
code -- stats' distribution engine, linalg's formatter and solvers, frame's CSV
reader -- is loops and pointer work, which it declines by design (refusing
rather than guessing; see
[turi-inline-c-silent-miscompiles](../archive/history/turi-inline-c-silent-miscompiles.md)).

The route that runs such a body is the `repl-jit-inline-c` experiment
(`--enable=repl-jit-inline-c`, BETA, aot-compiled-repl-plan C1): the
interpreter compiles the defn through the real emitter and the in-process MIR
engine on its first call. An embedder cannot reach it:

- the compile hook is installed by the `tur` binary only
  (`turi_set_inline_c_jit_hook(&g_repl_inline_c_jit_hook)`, `src/main.c`), and
  the hook's implementation lives in `main.c` too;
- the experiment gate is `g_opt_repl_jit_inline_c`, set by `--enable=` /
  `build.tur` / the user config, none of which a `tur build`-ed program reads;
- its supported subset is "a defn whose whole body is inline-C, fixed arity,
  scalar or pointer signature, calling nothing else", which several of these
  bodies exceed (stats' `random-n` calls the `static inline` `pcg32_*`
  helpers its module defines in a separate block).

Running the examples also surfaced content bugs that are the examples' own.
Fixed with the import work: `math-walkthrough` imported a module that never
existed (`stdlib/math`), `linalg-walkthrough` referred `mat-print` /
`vec-print` from `linalg/mat` / `linalg/vec` instead of `linalg/fmt`, and
`stats-walkthrough` / `plot-walkthrough` called `(rng-make 42)` where
`rng-make` takes a seed and a stream -- which `tur check` accepted, passing the
partial application to `rnorm`'s `rng : int`. `plot-walkthrough` also called
the `#fx{Unsafe}` `image-hook-record-path` from a top-level form without
`(unsafe ...)` (fixed with the narrowing below).

## Fix directions

1. **Ship the JIT hook in libturi.** Move `g_repl_inline_c_jit_hook` out of
   `main.c` into tur_core beside `inline_c_jit.c` (it needs the emitter and
   `jit_engine.c`, both already linked into a `-lturi` program that links
   `libtur_mir.a`), and give the embed API a per-env switch
   (`turi_env_set_inline_c_jit(env, true)`) the notebook calls next to
   `turi_env_set_toplevel_imports`. Widening the C1 subset to bodies that call
   sibling static helpers is the second half.
2. **Run the cell compiled.** The notebook already knows the cell's source
   and its search path; it could `tur build` a cell that names an inline-C
   function into a shared object and call it, the way `tur repl`'s spice
   auto-discovery binds a spice's exports (`.tur-repl-cache/`). Heavier, but
   it runs every body the compiler accepts.
3. **Say so in the notebook.** Until one of the above lands, `nb check`
   could name the function and the spice rather than echo the interpreter's
   generic message, and the example notebooks could be marked as needing it.

## Narrowed 2026-10-10: a cell's spices run compiled

Direction 2, in the shape `tur repl` already had. libturi gained
`turi_env_attach_spice` and `turi_env_attach_spice_for_module`
(`src/turi/env.c`): build the spice that provides a module with
`tur build --shared` (cached under its `.tur-repl-cache/`), `dlopen` it, and
bind its exports as natives (`tur_ffi_install_spice_bindings_owned`,
`src/turi/ffi_thunk.c`). The notebook's `session-eval` attaches the spice of
every module a cell imports before evaluating the cell (`notebook/` modules
excepted -- they are the host's own code), and links `-rdynamic`, which the
image needs to resolve the runtime it shares with libturi. Running
`stats-walkthrough` this way prints `dnorm`/`pnorm`/`qnorm` (0.398942,
0.975002, 1.95996), the `rnorm` summaries and the Welch t-test table;
every `frame-walkthrough` cell prints its table, group-by and `agg`
included (a cell's `vec-of` and `cons` build the C layouts frame reads).
Pinned by ctest `tur_embed_attach_spice` (`tests/turi/embed-attach-spice.c`).

The examples' own bugs went with it: they called `str-append` (a macro-time
helper, not a runtime function) and `float->str` (which does not exist), and
spelled a column name `(cast "x" :int)`, which the elaborator rejects.

What it took beyond the API -- each a defect the `tur repl` path shared:

- **Opaque handles had no FFI shim.** `ffi_shim_class_for_kind` classified a
  `defopaque` parameter as a non-scalar and declined the export, and
  `FnDef.param_types` carries the ADT kind without its def, so a type-aware
  test has to read the function type's full argument types
  (`ffi_shim_class_for_type`, `src/compiler/emit_module.c`).
- **A generic export got a shim calling a function that is never defined.**
  linalg/sized's `lamat-set! [m n]` is generic only in phantom indices; with
  no instantiation there is no definition, and the shim's reference left the
  `.so` failing `dlopen` (`undefined symbol: linalg__sized__lamat_hyset_ex`).
  Shims now skip what `emit_abi_fn_skip_generic` skips, and the manifest
  keeps only symbols the emitted C defines (`manifest_keep_defined`,
  `src/main.c`) -- before that the loader rejected the image as a "stale
  exports.manifest".
- **A struct parameter was bound as an int.** The manifest spells it `:any`,
  which the loader reads as class `i`; calling linalg's `mat-print-prec`
  through it was an ASan SEGV. The binder now takes only exports with a
  shim.
- **Importing a module replaced its compiled exports.** The interpreter kept
  a pre-bound native only when the defn's body *is* inline C; frame's
  `read-csv-string` is a Turmeric wrapper over private helpers, so the import
  swapped in a closure the interpreter refused. A native bound from a spice
  image now wins over its own module's defn, found by its qualified name
  (`EX_FN_DEF`, `src/turi/eval.c`).
- **Nested module names split at the first slash.** `parse_manifest_line`
  (`src/turi/spice_loader.c`) read `stats/rng/rng-make` as module `stats`,
  name `rng/rng-make`, so no export of a nested module had its own name --
  in `tur repl` too.

### Later the same day: plot

- **Static cmake-deps are built position-independent.** plot's image did
  not link -- `libplutovg.a` was not `-fPIC` (`relocation R_X86_64_TPOFF32
  ... recompile with -fPIC`). The CMake `tur fetch` generates
  (`pkg_gen_cmake_deps`, `src/compiler/pkg.c`) now defaults
  `CMAKE_POSITION_INDEPENDENT_CODE ON`, so any spice's static dep can go
  into a `--shared` image.
- **`notebook/image` runs as a session native.** It is the notebook's own
  module, so there is no image to attach; the session registers
  `image-hook-record-path` itself.
- **The inline-C constructor emulation stored 0 for what it could not
  evaluate.** `plot-walkthrough`'s scatter and histogram cells build points
  with an inline-C `defn` that puns doubles through a union (`ux.d = x;
  p->head = ux.i;`): `try_exec_simple_inline_c` skipped the `ux.d`
  statement, could not evaluate `ux.i`, kept 0, and six points drew as one
  at (0, 0) -- silently. A field value it cannot evaluate, or an assignment
  it would skip, now declines the body (`ic_exec_constructor`,
  `src/turi/eval.c`; `tests/run-interp-inline-c-constructor.sh`). The
  examples build their points with `cons` and `float->bits` instead.
- **A `:cstr` or `:bool` export returned its bare word.** The binder read
  every int-class result back as an integer, so `(test->str r)` was an
  address; the manifest's return tag now picks a string or a boolean
  (`TurSpiceExport.ret_tag`).
- **A spice whose `src/` imports a `:spices` dep could not be imaged** --
  in `tur repl` either. The loader built `<root>/src`, a directory with no
  `build.tur`, so the manifest's deps never reached the include path
  (notebook: `module 'ansi/term' not found`); and a `--shared` build of the
  root compiled the deps only for their headers yet listed their exports in
  the manifest, which the loader then rejected as stale. Shared builds now
  list only the project's own modules, and the loader builds the root with
  the new `tur build --shared --bundle-deps`, which links the deps' code in.
- Two plot bugs the walkthrough showed, fixed in the spice (compiled
  programs had them too): every x tick was labelled with a y value (the
  label buffers were block-local and the y block reused the x block's
  storage), and a discrete histogram's category labels were the bar
  pointers printed as text.

### Later still: linalg -- records across the FFI

linalg's API passes `defstruct mat` / `lavec` / `lufac` by value, so none of
it had a shim and every call reached inline C interpreted. A by-value record
slot is now FFI class `s`, spelled `:record` in the manifest
(`ffi_shim_class_for_type`, `src/compiler/emit_module.c`): the slot carries a
pointer to the record's C bytes, and the shim hands the callee that pointer
or the record it points at -- `const T *` or by value, by the emitter's own
rule (`type_struct_pass_by_ptr`) -- and copies a record result into a buffer
the caller supplies. The manifest does not say WHICH record; the module's
own defn does, so when an import keeps such a native the interpreter notes
the layout of its signature (`ffi_note_export_records`, `src/turi/eval.c`,
reusing the aggregate extern-c machinery: `agg_sig_build`, then
`tur_eval_agg_to_bytes` / `tur_eval_agg_from_bytes` per call in
`ffi_native_shim`). Called before its module is imported, such an export
says so instead of guessing. The aggregate layout also learned that a
`:heap` ADT field (`mat`'s `(Vec float)`) or an opaque one is a word, as the
emitter declares it. Every `linalg-walkthrough` cell runs: the product, the
solves and the least-squares fit print what numpy gives.

## Still open

1. **A cell's `defn` as a callback.** plot's `function` takes its callback
   as an untyped (so `:int`) parameter; a cell's `defn` is an interpreter
   closure with no C address to pass. `plot-walkthrough`'s first cell and
   the FFI callback path (`tur_ffi_cb_ctx_new`, which needs the parameter's
   function type) both want plot to type it `(fn [float] float)`.
2. **stats' frame helpers crash, compiled too** -- a layout bug of stats',
   not the notebook's:
   [stats-frame-interop-reads-a-stale-frame-layout](stats-frame-interop-reads-a-stale-frame-layout.md).
   The examples' `ols-frame` cells are left in place, marked `eval=false`.

(The original filing's last example bug -- `plot-walkthrough` calling the
`#fx{Unsafe}` `image-hook-record-path` outside `(unsafe ...)` -- is fixed in
the example.)


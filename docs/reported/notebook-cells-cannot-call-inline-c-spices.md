# Notebook cells can import `plot`, `linalg`, `stats` and `frame` but cannot call them: their work is inline C, which the interpreter does not run

**Severity:** medium (feature gap). The notebook is the literate-programming
entry point, and four of the five example notebooks
([notebook-docs-examples-plan](../upcoming/notebook-docs-examples-plan.md) C1)
are walkthroughs of spices whose functions are inline-C bodies. Since
[notebook-eval-no-module-base-dir](../archive/notebook-eval-no-module-base-dir.md)
was fixed (2026-10-10) every import in them resolves; the first call into the
spice then fails, cleanly, with the interpreter's inline-C refusal.

Filed 2026-10-10, closing that report.

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
partial application to `rnorm`'s `rng : int`. Still open: `plot-walkthrough`
calls the `#fx{Unsafe}` `image-hook-record-path` from a top-level form without
`(unsafe ...)`.

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

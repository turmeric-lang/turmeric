# plot's typed API has no FFI shim, so notebook plot cells fall back to the interpreter and stop at its first inline-C body

**Severity:** medium (regression). Since 2026-10-10
([notebook-cells-cannot-call-inline-c-spices](../archive/notebook-cells-cannot-call-inline-c-spices.md))
a notebook cell calls plot compiled, through the spice image the session
attaches. plot's Renderer-typeclass rework (turmeric-spices#113) gave its
public API real types, and those types have no FFI class. So the
constructors and `plot-write-png` get no `__ffi` shim, the binder leaves
their interpreted defns in place, and the first inline-C body they reach is
refused. Every cell of `plot-walkthrough` and the last cell of
`data-analysis` fail. The `linalg`, `stats` and `frame` walkthroughs are
unaffected and run.

Filed 2026-11-28, found while closing the callback item of that report (a
cell's `defn` as a C callback now works; it is not what fails here).

## Repro

From `turmeric-spices/spices/notebook`, with a `tur` built from `main`:

```
tur build . -o /tmp/tur-nb
ASAN_OPTIONS=detect_leaks=0 /tmp/tur-nb check examples/plot-walkthrough.tur.md
```

```
[] ERROR: eval: inline-C not supported in interpreter mode (function uses a native C implementation; run it with `tur build`/`tur run` instead of `--interpret`)
```

The image itself builds and binds 154 plot exports
(`tur build --shared --bundle-deps spices/plot`). The ones a cell calls are
not among them:

```
plot/decor/axes       -> plot__decor__axes      :: () -> :any
plot/point/points     -> plot__point__points    :: (:any :any :cstr) -> :any
plot/line/function    -> plot__line__function   :: (:fn :float :float :int :any :cstr) -> :any
plot/core/plot-write-png -> plot__core__plot_hywrite_hypng :: (:any :any :cstr) -> :ptr
```

(`ASAN_OPTIONS=detect_leaks=0` because a Debug `libturi` is ASan-built and
the images' own allocations are reported at exit; it does not change the
cell result.)

## Root cause

`ffi_shim_class_for_type` (`src/compiler/emit_module.c`) admits scalars,
opaque handles (`LineStyle`, `PlotOpts` -- these are fine), by-value records
and, since 2026-11-28, `^fat` callbacks. Two of plot's slot types are none
of those, so the export gets class `'?'` and no shim:

- **`AnyRenderer`**, `(exists [a] [(Renderer a)] a)`: every constructor's
  result and the element type of `plot-write-png`'s first parameter. In C a
  `void *` box.
- **`(Vec T)`** parameters: `(Vec Point)` for `points` and `lines`,
  `(Vec Bar)` for the histograms, `(Vec float)` for `density`, and
  `(Vec AnyRenderer)` for `plot-write-png`. In C a vec pointer (or the
  int64 word).

Callbacks are not part of it: `function`'s callback is a supported `:fn`
slot, which a cell's `defn` crosses.
Installing the JIT FFI provider in the notebook would not help either: an
export without a `__ffi` shim is never bound in an embedder
(`tur_ffi_install_spice_bindings_owned`, `src/turi/ffi_thunk.c`), whatever
call path it would then take.

Both are pointer-sized words in C, so the shim itself would be trivial. What
is not trivial is the interpreter side:

- a cell's `(vec-of (Point 0.0 0.1) ...)` holds interpreter records, while
  compiled code reads a `(Vec Point)` whose elements are boxed C records
  (`container_elem_form`, `src/compiler/types.c`) -- a word class would hand
  plot pointers to the wrong layout;
- an `AnyRenderer` made by an interpreted constructor is the interpreter's
  pack, not the C box, so the two cannot be mixed either; and
- turning such results into bare words would take away what a cell can do
  with a value that interprets fine today.

## Fix directions

1. **`AnyRenderer` as a word, end to end.** Class an existential result or
   argument `'i'`: a cell gets the compiled box and only ever hands it back
   to plot. Once every constructor and `plot-write-png` are bound, no
   interpreter-made pack reaches compiled code. A cell that `open`s one
   would then fail, which no example does.
2. **Marshal `(Vec T)` at the boundary.** Build a C vec from the cell's vec
   per call, packing each record element with the `:record` machinery
   (`tur_eval_agg_to_bytes`) into the element box the compiled code expects,
   and the reverse for a `(Vec T)` result. Words and floats need only the
   element copy. Check first whether the interpreter's vec of words already
   has the C layout: the 2026-10-10 frame work relied on a cell's `vec-of`
   and `cons` building the layouts frame reads.
3. **Interim:** mark the plot cells `eval=false` in the example notebooks,
   as the function cell already was, so `nb check` passes on what does run.

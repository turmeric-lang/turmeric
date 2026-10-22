# Generators: shared captures, `yield` checks, exhausted reads

**RESOLVED 2026-10-21** (items 1-4 of generator-forms-gaps). Pinned by
`gen-capture-mut-shared` and `gen-unwrap-exhausted` (all three engines) and
`errors/gen-yield-type-mismatch`, `errors/gen-yield-in-fn-literal`,
`errors/gen-set-captured-mut-struct`. Item 5 (a type for a generator
parameter) stays open in
[generator-forms-gaps](generator-forms-gaps.md).

## What was wrong, and what changed

1. **A compiled generator copied what it captured; the interpreter shared it.**
   Rule chosen: a scalar `^mut` the body names is shared, as it already is for a
   `fn` (the `TurMutCell` rewrite, `elab_let_mut_to_cell`), so `cell_fn_mentions`
   (`src/compiler/elab_forms.c`) now treats a `gen` body as a capturing scope.
   Every other captured local is still copied; a `set!` of a captured `^mut` of
   another type inside the body is an error (`gen_sets_binding`) rather than a
   silent write to the copy. `collect_free_vars` (`elab_core.c`) also records an
   `EX_SET` target as a free variable, so a body that only assigns no longer
   leaves the local undeclared in the emitted C.
2. **`yield` payload types were unchecked.** Each later `yield` is compared with
   the first (`elab_yield`); integer widths may mix, any other difference is an
   error.
3. **`yield` inside a nested `fn` literal** emitted C that did not build while
   the interpreter ran it. `elab_fn` hides the generator context while a literal
   is elaborated, so the yield is a diagnostic that says why.
4. **`gen-unwrap` of an exhausted generator** segfaulted compiled and read 0
   interpreted. It is now a panic with the same text and `file:line` on both
   (`EX_GEN_UNWRAP` in `emit_expr.c` and `eval.c`, and the stdlib/native
   fallbacks). The compiled read sits in the `else` of the null test because
   `tur_panic` returns when a `catch-unwind` handler is active.

**Also resolved 2026-10-21** (item 2 of the narrowed report): a generator
captured by a `fn` literal did not build. `__tur_gen_hdr_t`, the header
`gen-next` / `gen-done?` read a generator through, was emitted with the first
generator's functions -- after any lambda lifted from the function creating it
-- so the lambda named an undeclared type. It is one line of the fixed preamble
now (`emit_module.c`); snapshots and the split runtime regenerated. Pinned by
`gen-captured-by-fn` (all three engines).

The cell a generator captures is never freed (`closure_binding_escapes` treats an
`EX_GEN` as an escape), which is safe and no worse than the generator state
itself, which has never been freed.

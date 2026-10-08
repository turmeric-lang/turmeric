# TUR-W0030 names synthesized lambdas (`__fn_38`), which a user cannot act on

**Severity: low (diagnostic quality), blocking a default.** 47 of 358
`TUR-W0030` warnings over the fixture corpus name a compiler-synthesized
lambda rather than anything in the user's source, so the warning's own advice
-- "add `#{...}` to the function" -- names no function the user wrote. Filed
2026-10-01 while measuring whether `--strict-effects` could default on.

**Status: RESOLVED 2026-10-02** by directions 1 and 3 together; see
[Resolution](#resolution).

**Original status: OPEN.** Not a wrong answer, and harmless while the flag is opt-in.
It is filed because it is the thing standing between `--strict-effects` and a
default: shipping 13% unactionable warnings in a default-on lint teaches
people to ignore the category.

## Minimal repro

Any `fn` literal that performs an effect its enclosing `defn` does not
declare. From the corpus (`stdlib/backtrack-dfs.tur`):

```sh
./build/tur --strict-effects check stdlib/backtrack-dfs.tur
```

```
stdlib/backtrack-dfs.tur:107:3: warning [TUR-W0030]: function '__fn_38'
  performs effects {Bt} but has no effect-row annotation
  (add #{...} or handle all effects inside the function)
```

There is no `__fn_38` in the source. The span is right -- it points at the
`fn` literal -- but the name is an elaborator-internal gensym, so the
message cannot be grepped, cannot be matched against the file, and reads as a
compiler-internals leak.

## Measured incidence

Over `tests/fixtures/` plus `stdlib/` (2750 files that check clean without the
flag), with `--strict-effects`:

| | count |
| --- | --- |
| files gaining at least one W0030 | 202 (7.3%) |
| W0030 warnings total | 358 |
| naming a synthesized lambda (`__fn_N`) | 47 |
| naming any compiler-internal `__` symbol | 50 |
| naming a real user-written function | 308 |

## Root cause

`src/passes/effect_check.c:1511-1526` emits the warning with
`fn->binding->name->name`, falling back to `"<anonymous>"` only when there is
no binding at all. A `fn` literal *does* get a binding -- a synthesized one --
so the fallback never fires and the gensym is printed instead. The
`--lint-effects` copy of the same message
(`effect_check.c:1558-1578`) has the identical bug.

## Fix directions

1. **Describe the lambda instead of naming it.** `"anonymous function at
   line 107"`, or `"the function passed to 'dfs-or'"` when the elaborator
   knows the call it is an argument to. Two call sites to change, both in
   `effect_check.c`.
2. **Suppress W0030 for synthesized bindings entirely**, and rely on the
   enclosing `defn`'s own warning. Note the corpus says this would hide
   genuine cases: in `backtrack-dfs.tur` the enclosing `defn` warns too, but
   that is not guaranteed in general.
3. **Mark synthesized bindings** with a flag the diagnostic layer can read,
   which fixes this class wherever else a gensym reaches a message rather than
   only here. Widest fix; worth a grep for other `binding->name->name` uses in
   diagnostics first.

Direction 1 is the cheap correct one. Direction 3 is the one that stops this
recurring.

## Resolution

**Fixed 2026-10-02 by directions 1 and 3 together.** Direction 3's flag already
existed -- `Binding.is_synthesized`, added for the LSP so completion and
outlines skip `__fn_N` and `__inst_*` -- but nothing in the diagnostic layer
read it. So a synthesized binding now also carries a **`diag_label`**, set at
the two mint sites while the source facts are still to hand, and
**`binding_fn_describe`** (`src/compiler/expr.c`) is how a diagnostic names a
function:

| Binding | Described as |
| --- | --- |
| a `defn` | `function 'boom'` (unchanged) |
| a lifted `fn` literal | `anonymous function in 'dfs-or'` -- the enclosing `defn`, from `e->current_fn_name` at lift time; plain `anonymous function` at top level |
| an instance method | `method 'io-show' of instance IOShow [int]` |
| a `defclass` default body | `default body of method 'greeting' in class Greet` |
| a rank-2 forwarding wrapper | `rank-2 wrapper for 'cb'` |
| a future synthesized binding with no label yet | `compiler-generated function` -- never the gensym |

The report's repro now reads:

```
stdlib/backtrack-dfs.tur:107:3: warning [TUR-W0030]: anonymous function in
  'dfs-or' performs effects {Bt} but has no effect-row annotation (add
  #fx{Bt} after its parameter vector, or handle those effects inside it)
```

Every function-naming diagnostic in `src/passes/effect_check.c` goes through
the helper -- `TUR-E0009` (both forms), `TUR-W0030`, `TUR-W0031`,
`TUR-W0032` -- not only the two `W0030` sites, which are now one (see
[strict-effects-and-lint-effects-are-indistinguishable](strict-effects-and-lint-effects-are-indistinguishable.md)).
Two message fixes rode along, both about actionability:

- `TUR-W0030` spells the row to add (`#fx{Bt}`) in the current syntax --
  its old advice, `add #{...}`, was the retired spelling, which itself warns
  -- and for an instance method or a default body points at the `defclass`,
  which is where its row comes from.
- `TUR-W0031` is no longer reported on an instance method. Its row is copied
  from the class method, so the warning ("declares effect 'Write' but never
  performs it") described a claim the instance never made and could not
  change. Found by `tests/fixtures/typeclass-effect-row` once W1 made its
  `Write` resolve.

**Measured after the fix,** over the same `stdlib/` + `tests/fixtures/`
corpus with `--strict-effects` (3,823 files): 342 `TUR-W0030` in 202 files,
61 of them on `fn` literals, every one described as *anonymous function in
'...'*. **Zero** effect diagnostics (`TUR-E0009`, `W0030`-`W0032`) print a
`__` name. The sweep turned up two more mint sites beyond the report's two,
both now labelled the same way:

| Mint site | Was printed as | Now |
| --- | --- | --- |
| a `defclass` default body (`elab_typeclasses.c`) | `function '__default_Greet_greeting'` | `default body of method 'greeting' in class Greet` |
| a rank-2 forwarding wrapper (`make_poly_wrapper_ex`, `elab_call.c`) | `function '__poly_15'` | not reported -- see below |

A `__poly_N` wrapper forwards to a function the user passed to a rank-2
parameter and performs exactly what that function does; the function gets its
own `TUR-W0030` (or is annotated), and the wrapper has no source form anyone
could annotate. So `--strict-effects` skips it rather than describing it -- it
was a duplicate of the warning on the function itself. Each mint site records
which kind it is (`Binding.synth_kind`, a `SynthKind`), which is what lets the
lint skip a wrapper and point an instance method or default body's fix at the
`defclass`.

**Pinned by** `tests/run-flags.sh` `strict-effects-lambda-name` (a `fn`
literal that performs an effect is described by its enclosing `defn`, and no
diagnostic contains `__fn_`) and `tests/fixtures/errors/typeclass-effect-row-default-bad`,
whose `expected.diag` used to match the `__default_` gensym and now matches
the description.

**Not swept here:** diagnostics outside the effect pass. About 30 emit sites
elsewhere print a binding name directly (24 in `elab_call.c`, a few in
`borrow_check.c`, `elab_core.c`, `elab_memory.c`); most cannot see a
synthesized binding, but the ones that can should take
`binding_fn_describe` as they are touched.

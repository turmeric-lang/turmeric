# A second `defn` of the same name in one file passes the front end

**RESOLVED 2026-09-30**, the day after it was filed. Pinned by
`tests/fixtures/errors/defn-redefine-same-file` (same shape),
`defn-redefine-same-file-result-type` and `defn-redefine-same-file-arity`
(the misleading-`println` case). `tests/turi/repl-smoke.sh` also checks that
redefinition on a later turn still works, and that two defns of one name in
one turn do not.

## Resolution

The report's first direction: mirror `def`, keyed on the **form** that
elaborated the binding.

- `Binding` gains `defn_claim` (`src/compiler/expr.h`). It holds the span of
  the `(defn ...)` form Pass 2 elaborated into a Pass-1 forward declaration.
  Line 0 means unclaimed.
- `elab_defn` (`src/compiler/elab_fns.c`, at the forward-declaration check)
  claims the binding the first time. A later defn of the name from a
  *different* form in the *same file* now gets
  `defn: 'f' is already defined`, plus a note at the first definition. It
  is not exempt as a prior REPL/playground turn (`elab_prior_turn_global`),
  and not a stdlib load.

Three choices in that rule, and why:

- **Compared by span, not by `Form *`.** The same source form can be
  elaborated twice: the speculative deferral in `elab_toplevel.c` (a defn
  that fails is rolled back and re-elaborated after the other forms), or a
  macro expansion that carries its call site's span. It must still be
  recognised as itself.
- **Same file only.** A binding that arrived from an import, an `extern-c`
  or a native stub has no claim, and keeps exactly the old behaviour. Loading
  one helper file twice also stays as it was (same offsets). The fix covers
  what the report describes and nothing wider.
- **User code only.** A stdlib load (`in_stdlib_load`) keeps its existing
  rules (MF3). This change does not touch them.

The pass-1 question the report raised (does it collapse the two forward
declarations into one?) answered itself: it does. Pass 1 skips a name that is
already in scope, so both defns land on one binding, and Pass 2 is the only
place that can tell them apart.

## The report as filed

**Severity: low.** It is a user mistake, but no Turmeric diagnostic reports
it. `tur check` exits 0, `--interpret` silently runs the later definition, and
`tur build` / `tur jit` fail with raw C-compiler or c2mir errors. The
same mistake with `def` gets a clean `def: 'x' is already defined`.

Filed 2026-09-29 while investigating
[aot-compiled-repl-plan](../upcoming/aot-compiled-repl-plan.md). Measured
on `main` at `c6ba4162`, Release `-DTUR_JIT=ON`.

## Repro

```turmeric
(defn f [] : int 1)
(defn f [] : int 2)
(defn main [] : int (println (f)) 0)
```

| Path | Result |
| --- | --- |
| `tur check` | no output, exit 0 |
| `tur --interpret` | prints `2` (the later definition wins, silently) |
| `tur build` | `error: redefinition of 'f'` from `cc`, in `/tmp/tur-build/...c` |
| `tur jit` | `Repeated item declaration f` from c2mir, then the cc fallback fails the same way |

Change the second definition's result type to `float` (`2.5`). `check` still
exits 0, `--interpret` prints `2.5`, and `build` reports `conflicting types
for 'f'`.

Change the second definition's arity to `[x : int]`. The later definition
then replaces the earlier one for every caller. The zero-argument call
`(println (f))` fails with a misleading error: `operator lookup failed for
'println': got 1 arg(s), first arg type (fn [int] : int)`. That error points
at `println`, not at the duplicate definition.

For comparison, `(def x 1)` followed by `(def x 2)` in one file reports
`def: 'x' is already defined` at the second form.

## Root cause

`elab_defn` looks up the name before defining it
(`src/compiler/elab_fns.c:5805`). It accepts **any** existing global
function binding as "pass 1's forward declaration of this form":

```c
/* Forward declarations have TY_FN type (from pass 1) */
if (existing->type.kind == TY_FN && existing->is_global) {
    /* This is a forward declaration - proceed with the real definition */
} else {
    diag_emit(DIAG_ERROR, ..., "defn: '%s' is already defined", ...);
```

(`src/compiler/elab_fns.c:5829`). The first `defn`'s real binding has exactly
that shape, so the second `defn` sails through. The emitter then writes two C
functions with the same name.

`def` gets this right (`src/compiler/elab_fns.c:11720-11733`). It
accepts only the binding pass 1 declared for *this* form
(`is_forward_def`), or a global from an **earlier REPL/playground turn**
(`elab_prior_turn_global`, `src/compiler/elab_core.c:2505`). Anything else is
reported as a duplicate "within one turn (or one file)".

## The looseness is partly load-bearing

Redefining a `defn` across REPL turns must keep working. On `main` this
transcript prints `1`, `2` and `9`:

```text
(defn f [] : int 1)
(f)
(defn f [] : int 2)
(f)
(defn f [x : int] : int x)
(f 9)
```

That case is exactly what `elab_prior_turn_global` identifies. The fix must
not tighten it.

## Fix directions

- Mirror `def`. Pass 1 marks the forward declaration it creates for each
  `defn` (a flag like `is_forward_def`, or the declaring `Form*`). In pass 2,
  `elab_defn` accepts an existing binding only if it is:
  - that same form's forward declaration;
  - a prior-turn global (`elab_prior_turn_global`);
  - or a binding from `in_stdlib_load` / `bare_fat_spec_active`, which have
    their own rules just above.

  Everything else becomes `defn: 'f' is already defined`, with a note
  pointing at the first definition.
- Check that pass 1 itself does not silently collapse the two forward
  declarations into one before pass 2 can see both.
- Things to watch when running the suite:
  - The interpreter's native-stub preload (`src/turi/preload.c:140`,
    e.g. `(defn none? ...)`) defines stubs that later stdlib loads
    redefine. Those arrive as separate turns, but confirm it.
  - Any fixture that defines a name twice on purpose.
- Pin it with `tests/fixtures/errors/duplicate-defn-same-file` (same arity),
  plus cases for a different result type and a different arity. Also add a
  REPL test asserting the three-turn transcript above still prints `1`, `2`,
  `9`.

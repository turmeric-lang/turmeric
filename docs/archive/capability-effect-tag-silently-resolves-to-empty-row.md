# An undeclared capability tag in `#fx{...}` silently resolves to the empty row

**Severity: medium (silent loss of a checked guarantee).** A `#fx{...}` row
naming an effect that no `defeffect` in the compile declares is dropped at
resolution with **no diagnostic**, so the annotation becomes decorative and
`#fx{}` on its callers becomes a promise the compiler has quietly stopped
checking. Filed 2026-10-01, found while scoping `^capability` for `println`.

**Status: RESOLVED 2026-10-02** by fix direction 1 -- see
[Resolution](#resolution) at the end. Direction 2 was not taken here; it is
the plan's W2.

**Original status: OPEN.** The behaviour is already *documented* as a trap in
[effects-system-guide.md:375](../guides/effects-system-guide.md), which is how
we know it has bitten before -- `#fx{Bt}` "sat decorative on the trail
mutators for a month" before `Bt` was declared. This report is the filing the
guide note never got, because the failure mode is worse than the note implies:
it is not only that the tag does nothing, it is that a *second* function's
`#fx{}` silently stops meaning anything.

## Minimal repro

`IO` is declared in `stdlib/effects.tur:29`, which is **not** autoloaded. So
in a file that does not pull it in:

```turmeric
(defn maybe-io    [] #fx{IO} : int 0)
(defn claims-pure [] #fx{}   : int (maybe-io))
(defn main        [] : int 0)
```

```sh
./build/tur --dump-effects check repro.tur
```

Observed:

```
defn maybe-io    : #{}
defn claims-pure : #{}
```

Exit 0, no warning, no error. Expected: either `maybe-io : #{IO}` and a
`TUR-E0009` on `claims-pure`, or -- failing that -- a diagnostic naming `IO`
as an unresolved effect.

The second line is the real defect. `claims-pure` declares `#fx{}` while
calling something annotated as an authority-holding function, and that is
exactly the check `^capability` exists to perform:

> **Propagated from the declared row.** [...] A `#fx{}` (pure) caller of an
> `#fx{FS}`-tagged function therefore fails effect-row checking with
> `TUR-E0009`, exactly like the built-in `#fx{Unsafe}`.
> -- effects-system-guide.md:340-348

With the tag dropped, the caller's `#fx{}` passes. The annotation that was
supposed to *buy* the check is what silently removed it.

## Root cause

Effect-row resolution maps an uppercase name in `#fx{...}` to a declared
effect and drops what it cannot find, rather than diagnosing it. `Unsafe` is
immune because it is compiler-known (`EFFECT_NAME_UNSAFE`,
`src/passes/effect.h:164`); every `^capability` tag in
`stdlib/effects.tur` depends on that file being in the compile.

This is why `Bt` is declared in `trail.tur` rather than `effects.tur` --
the guide says so outright (`effects-system-guide.md:355-358`): the module
that uses it is autoloaded and `effects.tur` is not. That is a workaround for
this defect, applied once, by hand, for one tag.

## Why it matters more now

Tagging `println` with `#fx{IO}` (see
[effect-row-honesty-plan](../upcoming/effect-row-honesty-plan.md)) walks
straight into this. `println` is a builtin available in every program;
`IO` is declared in a module most small programs never import. A `println`
tagged `#fx{IO}` would be a no-op in precisely the files that need it, and
every `#fx{}` in them would be an unchecked promise. The tag cannot be hung on
a builtin until resolution is honest.

## Fix directions

1. **Diagnose an unresolved effect name** (preferred). An uppercase name in
   `#fx{...}` with no `defeffect` in the compile is a hard error naming the
   tag, with a "did you import the module that declares it?" hint.

   **The sizing sweep is done (2026-10-01): zero undeclared effect tags** in
   `stdlib/` plus `tests/fixtures/` -- 146 files carry a `#fx{...}`, 28
   distinct names, every effect among them resolves. So this breaks no
   existing effect annotation.

   It does break something else. **Three names in `#fx{}` are not effects at
   all** but compiler attributes, interned by name in the elaborator rather
   than declared by `defeffect`, so a naive rule rejects them:

   | Marker | Interned at | Used by |
   | --- | --- | --- |
   | `Construct` | `src/compiler/elab_core.c:2398` | `ok`/`err` (`stdlib/result.tur:39,57`), `some` (`stdlib/option.tur:33`) |
   | `ByVal` | `src/compiler/elab_core.c:2400` | m5 by-value accessor marker |
   | `NonExhaustive` | `src/compiler/elab_structs.c:3568,3607` | `match` exhaustiveness opt-out |

   (`Unsafe` is a fourth, via `sym_effect_unsafe`, `elab_core.c:2243`.)
   Allowlist all four, and assert the allowlist is exhaustive against the
   interned set so a future attribute cannot silently re-become a dropped
   effect.
2. **Make the five stdlib capability tags compiler-known**, as `Unsafe`
   already is, so `IO`/`FS`/`Net`/`Proc`/`Rand` resolve without an import and
   `Bt`'s hand-placement stops being special. Does not fix `#fx{Typo}`;
   complements direction 1 rather than replacing it.
3. **Warn only**, as a transitional step, if direction 1's sweep comes back
   large. Weakest option: the whole problem is that silence here reads as
   success.

Directions 1 and 2 are independent and both wanted: 1 makes a typo loud, 2
makes the common tags work where they are needed.

## A related wart, worth its own change

`#fx{}` is doing two jobs: effect rows and compiler attributes. Every other
effect-system language keeps these apart -- in Koka, Unison, OCaml 5, Effekt
and the Haskell effect libraries an effect label is an ordinary type-level
name, so an unknown one is a plain unbound-identifier error and this failure
mode cannot arise; attributes live in separate pragma syntax (`{-# ... #-}`,
`[@@...]`, `@ann`).

Turmeric already has that separate syntax: `^<lowercase>`, ~1,100 uses in
`stdlib/` (`^fat`, `^borrow`, `^mut`, `^linear`, `^multishot`, `^private`,
`^deprecated`, and `^capability`/`^extends` on `defeffect` itself). The case
convention agrees -- effects uppercase, attributes lowercase -- and the three
markers above are uppercase only because `#fx{}` demanded it. The clean
spelling is `^construct` / `^byval` / `^non-exhaustive`.

Not folded into direction 1 because `#fx{NonExhaustive}` is **documented
user-facing syntax**, with its own section at
[`sum-types-guide.md:241`](../guides/sum-types-guide.md) and a line in that
page's description, so it needs a deprecation cycle rather than a rename.
Tracked in
[effect-row-honesty-plan](../upcoming/effect-row-honesty-plan.md) section 6
Q1.

## Resolution

**Fixed 2026-10-02 by direction 1: an undeclared effect name is
`TUR-E0026`.** The repro above now fails at `maybe-io`'s row:

```
repro.tur:1:25: error [TUR-E0026]: unknown effect 'IO' in effect row: no
  defeffect declares it ('IO' is declared in stdlib/effects.tur, which is not
  autoloaded; add (load "stdlib/effects.tur"))
```

and with `(load "stdlib/effects.tur")` added, `claims-pure` fails `TUR-E0009`
as it always should have.

**Mechanism.** An `ERK_UNRESOLVED` row now carries the span of the `#fx{...}`
form it was read from (`effect_row_unresolved` takes it; all nine
construction sites pass one). Every resolution in `effect_check_pass` goes
through `resolve_declared_row` (`src/passes/effect_check.c`), which asks
`effect_row_unknown_names` (`src/passes/effect.c`) for exactly the names
`effect_row_resolve` would drop and reports each one before resolving --
once per row object, since a row reached through two holders is resolved
twice. The message adds a did-you-mean against the declared effects (edit
distance <= 2), names `stdlib/effects.tur` for the thirteen effects it
declares, and calls out a name that starts with neither case of letter
(`#fx{|e}`) as neither an effect nor a row variable. All five row positions
are covered: `defn`, `fn` literal, fn-typed parameter, record field, class
method. The interpreter reports it identically (it shares the pass); like
`TUR-E0009` there, it does not stop the run, because `eval.c` ignores the
pass's return code -- a pre-existing interpreter behaviour, unchanged here.

**No allowlist was needed -- and so W0 is not a prerequisite.** The table
above (and the plan's Q1) assumed `Construct`, `ByVal` and `NonExhaustive`
reach resolution. They do not: the `defn` row parser plucks `Construct` and
`ByVal` out before the row is built (`elab_fns.c`, the M2a/M5 pluck in
`elab_defn`), and `#fx{NonExhaustive}` is a `match` marker that
`elab_match` splices out -- it never becomes an effect row. `stdlib/result.tur`
and `stdlib/option.tur` compile unchanged. The attribute split (W0) stays
worth doing as a syntax cleanup, on its own merits.

**The sizing sweep was wrong: four rows had never resolved.** Run with the
real compiler over `stdlib/` plus `tests/fixtures/` (3,823 files), the check
found:

| Site | What it was | Fix |
| --- | --- | --- |
| `stdlib/typeclass-show.tur` `show-string-fputs` | `#fx{FS}` on an `fputs(stdout)` -- reached by every program that loads `typeclass-show.tur` (116 of the sweep's hits) | Tag removed. `FS` was never in scope, so it was always `#fx{}`; and stdout is not `FS`. It now matches `println` (untagged) until W2/W4 tag both |
| `tests/fixtures/typeclass-effect-row` | `#fx{Write}` with no `Write` declared | Declares `Write` |
| `tests/fixtures/effect-fat-callback-capturing` | `#fx{Write}` x3, undeclared | Declares `Write` |
| `tests/fixtures/effect-capturing-closure-thin-param` | `#fx{Write}` undeclared, and `#fx{|e}` (meant as the row variable `e`) | Declares `Write`; `|e` -> `e` |

All three fixtures still print their expected output with the rows now real,
so the shapes they were written to pin (a row-variable param, a concrete-row
param under the fat protocol) are exercised for the first time. The earlier
sweep missed these because `--dump-effects` prints `defn` rows only and the
`typeclass-show.tur` site is reached through a load. **turmeric-spices:** of
777 files, 649 check cleanly and none reports `TUR-E0026`; the 128 that fail
do so for other reasons and report no `TUR-E0026` either.

**Pinned by** `tests/fixtures/errors/unknown-effect-in-row` (every position,
the did-you-mean, `|e`), `errors/unknown-capability-tag-pure-caller` (this
report's repro) and `errors/loaded-capability-tag-pure-caller` (its
`TUR-E0009` twin with `effects.tur` loaded). The guide's trap note
(`effects-system-guide.md`, Capability effect tags) now documents the error
instead.

**Not done here:** direction 2 -- making `IO`/`FS`/`Net`/`Proc`/`Rand`
compiler-known so they resolve without a load. That is the plan's W2, still
the prerequisite for tagging `println` (W4). Until then a small program that
writes `#fx{IO}` gets a precise error telling it what to load, which is the
honest version of what used to be a silent no-op.

## Update 2026-10-02 -- direction 2 landed too

effect-row-honesty-plan W2 made `IO` / `FS` / `Net` / `Proc` / `Rand`
compiler-known, so the repro above no longer needs a load and no longer hits
TUR-E0026: it fails with TUR-E0009 on `claims-pure`, the check the tag exists
to buy (`tests/fixtures/errors/builtin-capability-tag-pure-caller`, renamed
from `unknown-capability-tag-pure-caller`). W4 then tagged `println` itself
`#fx{IO}`, and `show-string-fputs` -- the stdout writer whose decorative
`#fx{FS}` the W1 sweep found -- now says `#fx{IO}` as well. Both directions of
this report are done.

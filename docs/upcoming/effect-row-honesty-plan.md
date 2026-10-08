# Effect-row honesty -- answer WP8 as Option A, and make `#fx{}` mean something

> **Status: PROPOSED 2026-10-01, all three open questions resolved the same
> day** (section 6 -- Q1 and Q2 by measurement, Q3 by the author).
> **W0-W4 DONE 2026-10-02** (W1 first; W0 turned out not to be its
> prerequisite), and **W5's two gating defects fixed the same day.** What
> remains is W5 itself -- the `--strict-effects` default -- which this plan
> recommends against, now with the post-W4 measurement it asked for (see W5).
> Written in response to
> two questions about the security guide -- whether `--no-proc-macros` should
> default on "because Rust defaults `procMacro.enable = false`", and whether
> `--strict-effects` should default to true -- plus the observation that
> Haskell's `putStrLn` / `Debug.Trace` split is the shape Turmeric's
> `Write` / `println` pair already has, with the defaults inverted.
>
> **Type:** effects system, diagnostics, guide corrections
> **Touches:** `src/compiler/builtins.c` (the 14 `println` rows),
> `src/passes/effect.h` + effect-row resolution, `src/passes/effect_check.c`,
> `stdlib/effects.tur`, `docs/guides/security-guide.md`,
> `docs/guides/effects-system-guide.md`, and the
> [security-audit-plan](security-audit-plan.md)'s WP8 and open question 3.
>
> **Answers:** security-audit-plan open question 3 (`#fx{Unsafe}` semantics)
> as **Option A**, and supersedes WP8's first bullet.
> **Files alongside:**
> [capability-effect-tag-silently-resolves-to-empty-row](../archive/capability-effect-tag-silently-resolves-to-empty-row.md),
> [strict-effects-w0030-names-synthesized-lambdas](../archive/strict-effects-w0030-names-synthesized-lambdas.md),
> [strict-effects-and-lint-effects-are-indistinguishable](../archive/strict-effects-and-lint-effects-are-indistinguishable.md).

## 1. The thesis

An effect row should mean what it says. Today three separate things stop that
from being true, and they are usually discussed separately:

- `#fx{Unsafe}` is described in the security guide as "documentation and a
  lint" when the compiler in fact enforces it as a hard error at every call
  site. The description undersells a real mechanism and invites readers to
  treat the marker as advisory.
- `#fx{}` can be a promise the compiler has silently stopped checking, because
  an undeclared capability tag resolves to the empty row with no diagnostic.
- `#fx{}` does not exclude printing, because `println` is a compiler builtin
  with an empty row -- so the most common effect in any program is invisible
  to the effect system entirely.

Fix those three and `#fx{}` becomes a claim worth making. Until then,
defaulting `--strict-effects` on mostly adds noise to unannotated code without
making any annotation mean more than it does now.

## 2. What is already built (measured 2026-10-01)

Worth stating up front, because the mechanism gap is much smaller than the
discussion suggests.

**`#fx{Unsafe}` is already enforced, not advisory.** Three probes with
`--dump-effects`:

| Probe | Result |
| --- | --- |
| `(defn wrapper [] : int (poke p))` where `poke` is `#fx{Unsafe}` | **hard error:** `unsafe function 'poke' requires an enclosing (unsafe ...)` |
| `(defn tainted [] #fx{Unsafe} : int (poke p))` | `tainted : #{Unsafe}` -- propagates from the *declared* row, no block needed |
| `(defn caller [] : int (unsafe (tainted p)))` | `caller : #{}` -- `(unsafe ...)` satisfies the call site **and erases the row** |

That is exactly Rust's `unsafe fn` + `unsafe {}` split. The row erasure is the
feature: it is how a safe abstraction gets built over an unsafe primitive.

**The Haskell split exists too, under different names:**

| Haskell | Turmeric | Where |
| --- | --- | --- |
| `putStrLn :: String -> IO ()` | `(perform (Write s))`, `Write ^extends IO` | `stdlib/effects.tur:109` |
| `runIO` / `main :: IO ()` | `(with-write ...)` -- handles `Write` by calling `println` | `stdlib/effects.tur:175` |
| `Debug.Trace.trace` | `println` -- builtin, row `#{}` | `src/compiler/builtins.c:133-147` |

**But the defaults are inverted.** Haskell makes `putStrLn` ergonomic and
`Debug.Trace` an import you go out of your way for; the asymmetry *is* the
discipline. Turmeric makes `println` a builtin with 14 type overloads used in
every guide example, while `Write` needs an effect performed and a handler
wrapped around the call. Measured in `turmeric-spices`:

| | count |
| --- | --- |
| files calling `println` | 553 |
| `println` calls | 2,743 |
| files touching `Write` at all | 15 |

Nobody uses the tracked path, so the tracked path tracks nothing.

**This is why `--strict-effects` looks free.** Over 1,004 spice files that
check clean, `--strict-effects` adds **zero** warnings. That is not evidence
the corpus is disciplined -- it is that printing, the dominant effect, is
invisible to the lint. Defaulting the flag on against that measurement would
be reading the number backwards.

## 3. Decision: Option A for `#fx{Unsafe}`

security-audit-plan open question 3 asks whether `#fx{Unsafe}` means "pointer
arithmetic only" or "may corrupt memory on bad input". **Adopt Option A:
pointer arithmetic only -- it describes a body, not an input contract.**

Reasoning:

- It is what is built, and what every call site in `stdlib/` already assumes.
- `(unsafe ...)`-as-discharge is load-bearing. Option B needs the obligation
  to keep propagating past a wrapper, which means either changing what
  `(unsafe ...)` does at every existing call site or minting a second marker.
  Neither fits WP8's one-day, decision-shaped scope.
- The two readings are mutually exclusive on one marker: A's whole point is
  that a validating wrapper discharges; B's is that it does not. You cannot
  make `(unsafe ...)` mean both "I verified this" and "noted, still
  dangerous".

**Consequence to state plainly:** `#fx{Unsafe}` is a *discipline*, not a
queryable boundary. You cannot ask "which functions here can corrupt memory on
bad input?", because every competently written wrapper has deliberately erased
the answer. The M-1/M-5 deserializers therefore do **not** get retro-tagged,
and the security guide keeps promising nothing on their behalf.

Option B stays a legitimate future feature under a *different* name (a
propagating `#fx{Tainted}`-style marker). It is out of scope here.

## 4. Work items

**Re-sequenced 2026-10-01 at the author's direction:** the syntax question and
the effect/attribute split come *first*, before the capability work. That is
the better order, and not only on preference -- **doing the split first deletes
W1's allowlist entirely.** The allowlist only existed as a workaround for
tightening resolution while attributes still shared the brackets. Empty the
collision in W0 and W1's rule becomes the clean one every other effect
language has.

Ordered by dependency: W0 -> W1 -> W2 -> W4; W3 is independent and can land
any time.

### W0 -- Split attributes out of `#fx{}` -- DONE 2026-10-02

> **Done:** `(defn ^construct some ...)`, `(defn ^byval name ...)`,
> `(match ^non-exhaustive x ...)`.  `^construct`/`^byval` sit before the
> defn's name with `^deprecated`/`^reflect` (one parse loop, any order; one
> shared `elab_defn_name_index` for the pre-name scans, and `gendocs.py` taught
> the carets).  `#fx{NonExhaustive}` still opts out, with **TUR-D0004** (an
> error under `--Werror=deprecated`); one fixture per spelling.  Left in a row,
> `Construct`/`ByVal`/`NonExhaustive` are TUR-E0026 with a hint naming the new
> spelling, and an unknown pre-name `^attr` is now an error rather than being
> taken as the function's name.  Turned out not to be W1's prerequisite -- see
> W1 -- so it landed after it.

Move the three compiler attribute markers onto the `^attr` syntax the language
already has, leaving `#fx{...}` holding effect names and row variables only.

| Today | After | Sites |
| --- | --- | --- |
| `#fx{Construct}` | `^construct` | 4 code (`stdlib/result.tur:39,57`, `stdlib/option.tur:33`), 2 guide |
| `#fx{ByVal}` | `^byval` | 4 code (m5 fixtures), 0 guide |
| `#fx{NonExhaustive}` | `^non-exhaustive` | 1 code, **6 guide** |

**This is a 9-site code change.** `Unsafe` stays in `#fx{}` -- it is a genuine
effect (it appears in inferred rows, propagates, and participates in
`TUR-E0009`), unlike the other three, which are elaborator pragmas that merely
borrowed the brackets.

Why `^attr` and not a new `#attr{}`: `^<lowercase>` is already the established
attribute marker -- ~1,100 uses in `stdlib/` alone (`^fat` 327, `^borrow` 195,
`^mut` 127, `^linear`, `^unique`, `^multishot`, `^tailcall`, `^affine`,
`^private`, `^deprecated`, `^atomic`, `^persistent`, `^thread-local`), and
`defeffect` itself takes `^capability` and `^extends`. The case convention
already encodes the distinction: effect names and row variables inside
`#fx{}` are uppercase and lowercase respectively; attributes are lowercase
with a caret. The three markers are uppercase *only* because `#fx{}` required
it.

Transition, because one of them is published syntax:

- `#fx{NonExhaustive}` has its own documented section at
  [`sum-types-guide.md:241`](../guides/sum-types-guide.md) and a line in that
  page's front-matter description. So: **dual-accept** both spellings, emit
  `^deprecated`-style guidance on the `#fx{}` form, and rewrite the guide to
  teach `^non-exhaustive`. `^deprecated` and the TUR-W0050 retired-flag
  machinery are both precedent.
- `Construct` and `ByVal` are internal (zero spice uses, zero or no user-facing
  guide text), so they can move outright.

**Exit:** `#fx{...}` contains nothing but effect names and row variables;
`grep -r '#fx{Construct}\|#fx{ByVal}' --include=*.tur` is empty; the
`NonExhaustive` dual-accept has a fixture for each spelling.

### W1 -- Make effect-row resolution honest -- DONE 2026-10-02

> **Done:** an undeclared name is `TUR-E0026`, in every row position, with a
> did-you-mean and a load hint for the `stdlib/effects.tur` names. Two
> corrections to what this section assumed, both measured with the real
> compiler rather than `--dump-effects`:
>
> - **W0 was not a prerequisite.** `Construct` and `ByVal` never reach
>   resolution -- the `defn` row parser plucks them before building the row --
>   and `#fx{NonExhaustive}` is a `match` marker, never a row. So the rule
>   below landed with no allowlist and no W0. W0 stays worth doing as the
>   syntax cleanup it is.
> - **The sweep was not zero.** Four rows had never resolved: a stdlib
>   `#fx{FS}` on `show-string-fputs` (a stdout writer; tag removed, it matches
>   `println` until W4) and three fixtures' undeclared `Write` and `#fx{|e}`.
>   All fixed in the same change; turmeric-spices has none. Details in the
>   archived report's Resolution.

Filed as
[capability-effect-tag-silently-resolves-to-empty-row](../archive/capability-effect-tag-silently-resolves-to-empty-row.md).
An uppercase name in `#fx{...}` that no `defeffect` in the compile declares is
dropped silently, so `#fx{IO}` checks as `#fx{}` -- and a caller's `#fx{}`
then passes a check it should fail.

- Diagnose an unresolved effect name as a hard error naming the tag, with a
  "did you import the module that declares it?" hint.
- **No allowlist** -- W0 removed the reason for one. The rule is simply: an
  uppercase name in `#fx{...}` resolves to a declared effect or is an error.
  That is the rule Koka, Unison, OCaml 5, Effekt and the Haskell effect
  libraries all get for free by keeping effect labels in the ordinary
  namespace (section 6, Q1).

**The sizing sweep is already done** (Q1): zero undeclared effect tags in
`stdlib/` plus `tests/fixtures/`, so after W0 this breaks nothing and needs no
warn-then-error transition.

**Exit:** the repro in that report errors instead of printing two `#{}` rows,
and `stdlib/` still compiles.

### W2 -- Make `IO` available where `println` is -- DONE 2026-10-02

> **Done:** the five tags are registered, with parents and `is_capability`,
> wherever `Unsafe` is -- the elaborator's env, `PASS_EFFECT_LOWER`'s and the
> interpreter's session env (`effect_env_register_builtin_capabilities`).
> `stdlib/effects.tur` keeps its declarations, because the docs live on them;
> a `defeffect` of one of these names is accepted only when it matches the
> built-in exactly, and anything else is an error naming the expected form.
> **`Bt` stays in `trail.tur`:** it is declared next to the mutators it
> annotates, in an autoloaded file, and since W1 an undeclared name is an
> error rather than a silent drop -- so its placement is ordinary now, not a
> workaround, and it did not need to become compiler-known.

`IO` lives in `stdlib/effects.tur:29`, which is not autoloaded. `println` is
available everywhere. Tagging the builtin without fixing that would make the
tag a no-op in exactly the small programs that need it -- and, after W1, a
hard error in every program that does not import `effects.tur`.

Follow the `Unsafe` precedent: it is compiler-known via
`EFFECT_NAME_UNSAFE` (`src/passes/effect.h:164`). Make the five stdlib
capability tags (`IO`, `FS`, `Net`, `Proc`, `Rand`) compiler-known the same
way. This also retires the hand-placement of `Bt` in `trail.tur`, which exists
only to dodge this problem.

**Exit:** `#fx{IO}` resolves in a file that imports nothing.

### W3 -- Guide corrections -- DONE 2026-10-02

> **Done:** all three items.  The rust-analyzer sentence and the
> `#fx{Unsafe}` paragraph in the security guide (each claim re-probed), the
> audit plan's M-7 row, open question 3 and WP8 decision bullet; and the
> effects guide's "Printing: `println` or `(perform (Write s))`?" section,
> written against W4's semantics -- with W4, `println` maps to `putStrLn`
> (tracked) rather than `Debug.Trace`, and `Write` to an interpretable output
> effect.

Two factual errors, both in `docs/guides/security-guide.md` (plus W0's
`sum-types-guide.md` rewrite, which lands with W0 rather than here):

1. **`:102-104` is wrong about rust-analyzer.** It says `--no-proc-macros` is
   "what rust-analyzer ships as `procMacro.enable = false`". rust-analyzer
   ships `procMacro.enable = **true**`, and has since
   [changelog #69](https://rust-analyzer.github.io/thisweek/2021/03/22/changelog-69.html)
   (2021-03-22): *"enable proc macros by default (use
   `rust-analyzer.procMacro.enable` to disable them)."* It also defaults
   `cargo.buildScripts.enable = true`, so it **runs `build.rs`** on a tree you
   merely opened. Rewrite the sentence to say the flag *is* that setting
   turned off, not that upstream ships it off.
   (`docs/guides/macros-guide.md:328` is already correct -- it names the
   setting without claiming a default. Leave it.)
2. **`:432-438` undersells `#fx{Unsafe}`.** "documentation and a lint, not a
   boundary" is wrong on the first clause: the call-site rule is a hard error.
   Replace with Option A as decided in section 3 -- what the marker does
   enforce (call-site acknowledgment), what `(unsafe ...)` discharges, and why
   row erasure means it is not a queryable boundary. This is WP8's "one
   paragraph in the security guide" exit.

Also correct the audit plan's own M-7 row (`:166`), which describes only the
default-off lint half and reads as though nothing is enforced.

3. **Say which print path to reach for** (from Q3). Nothing today tells a
   reader when to use `println` versus `(perform (Write s))`. The effects
   guide should state it: `println` for output you are not trying to control,
   `perform (Write s)` when a handler should be able to intervene -- mapped
   onto the `Debug.Trace` / `putStrLn` pair in section 2, which is what makes
   the split legible.

**Note on the `--no-proc-macros` default while here:** the premise for
flipping it was the false rust-analyzer claim, so the author's 2026-09-30
decision (LSP does not pass it by default) stands unchanged. If it is
revisited, the flag is **global** (`src/main.c:9589`, `:12420`), so flipping
its default would refuse `defmacro*` in `tur build` too. rust-analyzer's
setting is scoped to the language server; `cargo build` always runs proc
macros. The faithful analogy is a **per-subcommand** default -- deny for
`check`/`lsp`/`run --list`, allow for `build`/`run` -- which does not exist
today. Out of scope for this plan; recorded so the next person does not
re-derive it.

### W4 -- `^capability` on `println` -- DONE 2026-10-02

> **Done:** `BuiltinSpec.effect` (the declared capability, by name); all 14
> `println` rows say `"IO"`; the effect pass merges it in the `EX_BUILTIN` arm
> and, for Saffron's dynamic `println`, the `EX_DYN_OP` arm.  The exit holds on
> both engines.  Three things the item did not list, all needed:
>
> - **Capability filters on the "can this perform an effect at runtime?"
>   gates** -- `fn_effect_may_escape` (cps_ir.c), `callee_effect_free`
>   (emit_cps_ir.c), the poly-wrap `__cps` twin gate (emit_expr.c) and
>   `serial_receiver_with_escaping_effect` (emit_effects.c) now ask
>   `effect_row_is_runtime_pure` instead of "is the row empty".  Without it
>   every printing int->int function would have grown a `__cps` twin.  No
>   codegen snapshot moved.  CPS *coloring* needed nothing: it seeds on control
>   operators, not rows.
> - **Lint filters** -- TUR-W0032 ignores capability tags (a row-polymorphic
>   HOF that prints is not "always concrete"), and TUR-W0033 strips them from
>   the handled body's row (IO is Write's parent, so a body that merely prints
>   would otherwise make every Write clause look reachable).
> - **Fixtures:** nine annotated fixtures printed under a row that did not say
>   so; each now names IO (or, for the two whose point was a *pure* closure,
>   uses one).  New: `errors/println-in-pure-fn` (four routes, run under both
>   engines), `errors/println-in-pure-fn-saffron`, `println-io-row`.  Hover
>   renders `(println : (fn [int] #fx{IO} : nil))`.  `show-string-fputs`
>   (typeclass-show.tur), the only other stdout writer, says `#fx{IO}` too.
>
> **turmeric-spices: unaffected** -- the same 649 of 777 files check clean
> before and after.  Partly for a bad reason: module members are not
> effect-checked at all (only top-level functions are), and every spice-test
> `main` that prints under a row sits inside a `defmodule`.  That gap is filed
> as [module-members-skip-effect-row-checking](../archive/module-members-skip-effect-row-checking.md);
> closing it is the next step toward `#fx{}` meaning something everywhere.
>
> **Closed 2026-10-02:** module members (and defns inside a top-level `do`)
> are effect-row checked.  Re-measured over the same 777 files, exactly three
> spice tests go from clean to `TUR-E0009` -- each a `#fx{Unsafe}` that
> prints and now needs `#fx{Unsafe IO}`.

With W1 and W2 landed, tag the `println` builtin `#fx{IO}`.

Why `^capability` and not `Write`: the effects guide draws exactly this
distinction at `effects-system-guide.md:322` -- capability tags are for
"side effects [that] are not expressed with `perform` at all [...] they happen
inside inline-C or `extern-c` calls". A builtin that writes to stdout is that
case. And capability tags already have the right properties: justified by
annotation alone (no `TUR-W0031` over-annotation noise), propagated from
declared rows, and a `#fx{}` caller of one is a hard `TUR-E0009`.

- 14 overload rows under the single name `println` in
  `src/compiler/builtins.c:133-147`; there is no other print builtin.
- The builtin table has **no effect-row field** today -- that is the actual
  compiler work. Adding one is reusable for any future effectful builtin.
- Keep `println` ergonomic. This is explicitly *not* a migration: the guide's
  discipline is opt-in ("a function with no effect-row annotation is never
  checked"), so unannotated code is unaffected. What changes is that `#fx{}`
  starts meaning *actually pure, does not even print* -- available to whoever
  asks for it.

**Exit:** `(defn f [] #fx{} : int (do (println "x") 0))` is a `TUR-E0009`;
the same function unannotated still compiles.

### W5 -- Then, and only then, revisit the `--strict-effects` default

Two filed defects gated this, both found while measuring it. **Both fixed
2026-10-02:** no effect diagnostic names a gensym (a lambda is *anonymous
function in 'dfs-or'*), and `--strict-effects` is the one flag, promotable
with `-Werror=strict-effects`, with `--lint-effects` a deprecated alias. What
is left of W5 is the default itself, after W4:

- [strict-effects-w0030-names-synthesized-lambdas](../archive/strict-effects-w0030-names-synthesized-lambdas.md)
  -- 47 of 358 warnings name `__fn_38`-style gensyms. A default-on lint that
  is 13% unactionable teaches people to ignore the category.
- [strict-effects-and-lint-effects-are-indistinguishable](../archive/strict-effects-and-lint-effects-are-indistinguishable.md)
  -- the two flags emit the same warning, neither can become an error, and the
  comment claiming otherwise is wrong. Decide which flag survives before
  giving either a default.

**And re-measure after W4.** Every number in section 2 was taken with printing
invisible. Once `println` carries `#fx{IO}`, the corpus-wide cost of
`--strict-effects` is a different and much larger number, and the current
measurement says nothing about it.

> **Re-measured 2026-10-02, after W4**, over `stdlib/` plus `tests/fixtures/`
> (3,830 files):
>
> | | before W4 | after W4 |
> | --- | --- | --- |
> | `TUR-W0030` warnings | 342 | **3,252** |
> | files with at least one | 202 (5%) | **2,418 (63%)** |
> | warnings whose row is only `{IO}` | -- | 2,909 (89%) |
> | warnings on `main` | -- | 2,355 (72%) |
> | `TUR-W0032` | 1 | 1 |
>
> So a default-on `--strict-effects` would now flag most programs, and mostly
> for one thing: an unannotated `main` that prints.  That is the prediction
> this section made, and it settles the recommendation below.  If W0030 is
> ever defaulted on, exempting `main` (the program's entry, whose row nobody
> calls into) would remove 72% of it on its own -- a question for whoever
> reopens W5, not a change this plan makes.

**Recommendation: do not default it on in this plan.** The honest version of
"meaningful effect reporting" is W1-W4 -- making `#fx{}` a claim the compiler
actually checks. A default-on W0030 is a *style* lint over unannotated code,
which is a separate question with a worse cost/benefit -- 63% of the corpus
after W4, above -- and it buys nothing for security: a warning never fails a
build (`diag.c:110`), and the opt-in `-Werror=strict-effects` is the way a
project that wants the gate already gets it.

## 5. What this does not do

- **No security promise moves.** Option A means the effect system stays out of
  the security guide's promises, exactly as M-7 says. W3 makes the guide
  *accurate* about why, not stronger.
- **No deserializer retagging.** That was Option B's bill.
- **No `--strict-effects` default**, and no `--no-proc-macros` default.
- **No Option B marker.** A propagating taint marker is a real future feature
  under its own name; nothing here forecloses it.

## 6. Open questions -- all three resolved 2026-10-01

### Q1. W1's blast radius -- RESOLVED by measurement

> **Correction 2026-10-02:** not zero. Checked with the real compiler (W1's
> `TUR-E0026`), four rows had never resolved -- see W1. The resolver itself
> is the better detector than a `--dump-effects` diff: it sees every row
> position and every loaded file (the `typeclass-show.tur` site is reached
> only through a load). The paragraph below is kept as written.

**Zero undeclared effect tags in the corpus.** Over `stdlib/` plus
`tests/fixtures/`: 146 files carry a `#fx{...}` tag, 28 distinct names, and
every one that is an effect resolves. Eight files initially looked like drops;
all eight are detector artifacts -- docstring text, the `defeffect`
declaration sites themselves, and rows written in a *type* position
(`[run : fn #fx{Write}]` in a struct field) which `--dump-effects` does not
print because it prints `defn` rows only.

So the blast radius is not existing breakage. **It is that `#fx{}` is a shared
namespace**: three names in it are not effects at all but compiler attributes,
interned by name in the elaborator rather than declared by `defeffect`:

| Marker | Interned at | Used by |
| --- | --- | --- |
| `Construct` | `src/compiler/elab_core.c:2398` | `ok`/`err` (`stdlib/result.tur:39,57`), `some` (`stdlib/option.tur:33`) |
| `ByVal` | `src/compiler/elab_core.c:2400` | m5 by-value accessor marker, fixtures |
| `NonExhaustive` | `src/compiler/elab_structs.c:3568,3607` | `match` exhaustiveness opt-out |

(`Unsafe` is a fourth, via `sym_effect_unsafe`, `elab_core.c:2243`.)

A naive "hard error on an unknown name in `#fx{...}`" therefore breaks
`stdlib/result.tur` and `stdlib/option.tur` on the first compile.

**What other effect languages do.** They do not have this problem
structurally. In [Koka](https://arxiv.org/pdf/1406.2061), Unison, OCaml 5,
Effekt and the Haskell effect libraries, an effect label is an ordinary
*type-level name* resolved by normal scoping, so an unknown effect name is
simply an unbound identifier -- a plain type error. None of them carries a
special "unknown effect" rule, because none of them built a bespoke
sub-namespace with its own ad-hoc resolution. Their collective answer to W1 is
"resolve effect names the way every other name is resolved." Equally: none of
them puts compiler attributes inside the effect syntax -- attributes are
separate pragma syntax (`{-# ... #-}`, `[@@...]`, `@ann`).

**Turmeric already has that separate syntax, and it is `^attr`.** Not a new
`#attr{}`: `^<lowercase>` is the established attribute marker, ~1,100 uses in
`stdlib/` alone (`^fat` 327, `^borrow` 195, `^mut` 127, `^linear`, `^unique`,
`^multishot`, `^tailcall`, `^affine`, `^relevant`, `^private`, `^deprecated`,
`^atomic`, `^persistent`, `^thread-local`). `defeffect` itself takes
`^capability` and `^extends`. The case convention corroborates the split:
effect names are uppercase, attributes lowercase -- the three markers above are
uppercase *only* because `#fx{}` required it. The clean target spelling is
`^construct` / `^byval` / `^non-exhaustive`.

**Decision (author, 2026-10-01): do the split FIRST, as W0, and drop the
allowlist.** The allowlist was only ever a workaround for tightening
resolution while attributes still shared the brackets; emptying the collision
first makes W1's rule the clean one. See W0 for the 9-site change and the
`NonExhaustive` deprecation cycle.

### Q1b. Should the effect row stop being `#fx{...}` altogether?

Asked 2026-10-01. **Answer: no -- keep `#fx{}`.** Three things decide it.

**The spelling is not what causes the bug.** Worth separating, because it is
easy to assume the bespoke syntax is the problem. It is not: the silent drop
comes from bespoke *resolution* -- names in the row are matched against
declared effects by an ad-hoc lookup that drops misses. Where the row *sits*
is independent. W1 fixes the resolution without touching a single `#fx{`.

**`#fx{}` is already the considered answer, and recently.** The row used to be
bare `#{...}`, with `@{...}` as an alternate sugar; `fx-row-syntax-rename-plan`
(now `docs/archive/history/`) moved it to `#fx{...}` to join the
`#map{}` / `#set{}` / `#row{}` / `#refine{}` / `#r{}` reader-literal family and
free the bare `#{` slot, and retired `@{...}` in the same pass precisely
because "two ways to spell the same thing was the original problem." Its
siblings in the *same signature slot* -- `#reads <sym>` and `#writes [...]`
(`src/compiler/reader.c:1452`) -- share the shape. A third spelling would
re-create the problem that plan closed.

**The migration is ~10,600 sites across two repos.** Measured:

| | `#fx{` sites | files |
| --- | --- | --- |
| turmeric `.tur` | 748 | 227 |
| turmeric-spices `.tur` | **9,732** | 600 |
| `docs/guides/` | 157 | 29 |

Spices dominates because `#fx{Unsafe}` sits on every inline-C function. So
re-spelling the row costs ~1,000x what the split (9 sites) costs, for a
conceptual gain the split already delivers.

**What the alternative would look like, for the record.** The version worth
sketching is the Koka/Unison one -- fold the row into the *type*, so effect
labels become ordinary type-level names:

```turmeric
;; today
(defn log-msg   [msg : cstr]            #fx{Write} : nil ...)
(defn run-twice [f : (fn [] #fx{e} int)] #fx{e}    : int ...)
(defstruct Action :copy [run : fn #fx{Write}])

;; row folded into the return type (sketch)
(defn log-msg   [msg : cstr]              : nil / {Write} ...)
(defn run-twice [f : (fn [] int / {e})]   : int / {e}     ...)
(defstruct Action :copy [run : (fn [] nil / {Write})])
```

Its real appeal is structural: effect names in the type grammar are resolved
by ordinary scoping, so the silent drop becomes *impossible* rather than
*fixed*. Its costs, beyond the 10,600 sites:

- **The terminator problem is already documented against it.** `#writes`' own
  rationale (`reader.c:1452-1461`) records that the return-type marker `:`
  reads as a symbol, so a greedy token run in signature position cannot be
  delimited -- which is why `#writes` takes a bracketed vector. Any
  unbracketed effect-name run hits the same wall, so the sketch still needs
  braces and a separator; it buys grammar placement, not fewer delimiters.
- **`/` and `!` are both poor separators here.** `!` is Turmeric's
  established mutation suffix (`vec-push!`, `set!`), and `/` is the module
  path separator (`tur/fs`, `log/error`). A third meaning for either is worse
  than a reader tag.
- Rows are overwhelmingly concrete: 468 uppercase effect names against 29
  lowercase row variables across turmeric's `.tur` files. The type-level
  framing pays off most for row polymorphism, which is 6% of uses.

So: the structural property is worth having and W1 delivers it; the syntax
change that would also deliver it is not worth 10,600 sites. If the row is
ever revisited, it should be for a reason the type grammar actually needs --
effect aliases, or rows in more type positions -- not for this bug.

### Q2. Interpreter parity -- RESOLVED, nothing to do

Both halves check out:

- **One table.** The interpreter consults the same builtin table --
  `src/turi/eval.c:80` includes `builtins.h`, calls `builtin_lookup`
  (`:13155`), and runs `builtins_init`. `src/turi/docstrings.c:215` holds only
  a doc string for `println`, not a second registration. So an effect-row
  field on the builtin row is **one edit covering both paths**.
- **The check already runs on both.** Measured with a declared capability
  effect and a `#fx{}` caller: `tur check` and `tur --interpret` emit the
  *identical* `TUR-E0009`, same span, same text.

W4's exit criterion therefore needs no interpreter-specific clause; add an
`--interpret` fixture to keep it honest.

### Q3. `with-write` -- RESOLVED: keep both (author, 2026-10-01)

`Write` + `with-write` stays alongside `#fx{IO}` on `println`, because the two
are not redundant: **`Write` is handleable and `#fx{IO}` is not.** A capability
tag can only be declared, so it cannot intercept, redirect or capture output;
`(handle ... (Write [s] k) ...)` can. That is a real capability no tag
replaces, and it is why `with-write`'s 15 files are not the measure of its
worth.

**Follow-on doc item:** nothing today tells a reader which to reach for. The
effects guide should say it plainly -- `println` for output you are not trying
to control, `perform (Write s)` when a handler should be able to intervene --
mapped onto the Haskell pair in section 2 (`Debug.Trace` vs `putStrLn`), which
is the analogy that makes the split obvious. Fold this into W3.

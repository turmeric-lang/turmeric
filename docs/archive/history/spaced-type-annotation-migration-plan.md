# Spaced Type Annotation Migration Plan

## Motivation

Turmeric currently writes type annotations as **fused** `name:type` (e.g. `(defn f [x :int y :int] :int ...)`). This collides visually with keyword literals (`:vertex`, `:refer`, `:spices`) and with map-key keywords (`#map{:name n}`), and it reads poorly compared to the spaced form used in most ML-family languages and in our own sweet-exp examples:

```turmeric
;; current (fused)
(defn classify [x : float] : cstr ...)

;; target (spaced)
(defn classify [x : float] : cstr ...)
```

The reader already emits a distinct AST node (`F_TYPE_ANN`) for the spaced form, and most elaboration sites already accept it. The migration is therefore primarily a **mechanical rewrite of source files** plus targeted compiler work to close the few remaining elaboration gaps.

This plan covers both `/Users/rjungemann/Projects/turmeric` (compiler + stdlib + fixtures) and `../turmeric-spices` (spice sources). Docs in this repo (`docs/guides/**`, `docs/upcoming/**`, `CLAUDE.md`, `README.md`) switch to the new syntax once it is universally accepted.

## Current State (as of this plan)

### Reader

`src/compiler/reader.c` (the `read_keyword` path, ~lines 456–509) already distinguishes two cases when it sees a `:`:

- Followed immediately by an identifier char → `F_KEYWORD(":int")` (fused).
- Followed by whitespace / `(` / `[` → reads the next form and wraps it as `F_TYPE_ANN(inner)` (spaced).

So `:int` and `: int` are already distinct token shapes downstream; no lexer change is needed.

### Elaboration sites that already accept `F_TYPE_ANN`

Confirmed via grep on `src/compiler/`:

- `defn` / `fn` parameter lists and return types — `elab_fns.c:995, 1005, 304, 2476, 2964, 3074`.
- `defstruct` fields and pattern destructure — `elab_structs.c:205, 626, 706, 811, 1805, 2151`.
- Typeclasses / `definstance` — `elab_typeclasses.c:618, 667, 763, 1878, 2008`.
- Module-level `defn` signatures (export validation) — `elab_module.c:743, 762, 779`.
- Macro expanders — `elab_macros.c:88, 240, 683, 844, 870`.

### Elaboration sites that still only see `F_KEYWORD`

- **`let` / `loop` bindings** — `elab_forms.c:1440` only checks `F_KEYWORD` for the optional type slot in a binding pair. Spaced annotations in `(let [x : int 5] ...)` will misparse as three binding forms instead of `name + type-ann + value`.
- **Arity counting in some call sites** — `elab_forms.c:1274` already counts both, so this is fine. But anywhere a hand-rolled walk only looks for `F_KEYWORD`, it must be widened.

### Sweet-exp

The sweet-exp reader feeds the same `F_TYPE_ANN` path, so `defn classify [x : float] : cstr` already works (CLAUDE.md examples). One existing fixture (`tests/fixtures/t-expression-sweet-exp/input.tur:8`) still uses the fused form — that file gets rewritten in Phase 3.

### Quantitative scope

- `stdlib/*.tur` — ~105 files, ~1.6k annotation sites.
- `tests/fixtures/**/*.tur` — ~1.4k files, ~4k annotation sites (many are `expected.c` snapshots — those are **not** touched; only the `.tur` inputs).
- `../turmeric-spices/**/*.tur` — ~1.8k files, ~25k annotation sites.

Total source annotation sites to rewrite: ~30k. This is large but very mechanical.

## Corner Cases (must be resolved in prerequisite phases)

Each item is a concrete obstacle to a flat global rewrite. The plan addresses every one before Phase 3 begins.

### CC-1: `let` / `loop` binding type slots

Two distinct sub-cases, only discovered while executing Phase 1a:

- **Named-let** (`elab_named_let`, `elab_forms.c:1440`) already documents a type slot in its binding format (`[name1 [:type1] init1 ...]`) but only matched `F_KEYWORD`, so `(let loop [n :int 10] ...)` worked while `(let loop [n : int 10] ...)` errored with "binding name must be a symbol". **Resolved in Phase 1a.**

- **Plain let / let\*** (`elab_let` at `elab_forms.c:156`, `elab_letstar` at line 1080) **does not parse a type slot at all** — the loop reads `name`, then `init`, with no optional keyword/type-ann in between. `(let [x :int 5] ...)` is therefore *not* currently valid syntax in plain let; the comment about "type annotations carry through verbatim" applies only to named-let. The codemod cannot emit `(let [x : int 5] ...)` until plain let learns to parse it. **Resolved in Phase 1a-bis (new sub-phase below).**

This was previously misstated in the plan as a single fix at line 1440.

### CC-2: Variadic rest type — `& rest :type`

`& rest` is followed by a colon-prefixed type. The variadic handler in `elab_fns.c` reads the form **after** `&`'s parameter name; it already passes through `F_TYPE_ANN` in the type-form unwrap at line 894. **Audit and add a regression fixture in Phase 1b** to confirm `& rest : int` and `& rest : Route` both elaborate, especially for the homogeneity / polymorphic-rest paths.

### CC-3: Complex / parenthesised types

`[f : (-> a b)]`, `[xs : (list int)]`. The reader wraps the inner list form inside `F_TYPE_ANN` when the `:` is spaced. `elab_fns.c:864-894` already unwraps. **Audit + fixture in Phase 1b** (and confirm the same for `defstruct` field types).

### CC-4: Effect / contract annotations `#{...}`

These are reader-distinct (`F_CONTRACT_TYPE`) and never start with `:`; no conflict, but the rewrite pass must **not** insert spaces inside `#{Unsafe}` etc. **Tooling rule documented in Phase 2.**

### CC-5: Keyword literals that look like types

`:vertex`, `:fragment`, `:refer`, `:spices`, `:path`, `:url`, `:exports`, `:members`, `:else`, `:tag` etc. These are real keyword values, **not** type annotations, and must remain fused. The codemod (Phase 2) cannot blindly insert a space after every `:` — it must understand position.

Distinguishing rule: a colon-prefixed token is a **type annotation** only when it sits in one of the recognised type-bearing positions:

- inside a `defn` / `fn` / `defmacro` parameter vector, after a parameter name;
- in the slot immediately after the parameter vector of `defn` / `fn` (return type);
- inside a `defstruct` field vector, after a field name;
- inside a `let` / `loop` binding pair, between name and value;
- in `definstance` / typeclass method signatures (same shapes as `defn`).

Everywhere else (`:refer`, `#map{:name n}`, manifest fields, `cond` `:else`, ADT tags) it is a keyword literal and stays fused. The codemod implements a structural matcher, not a regex.

### CC-6: Reader-quoted code and macros

Macros that **build** forms with `(quote :int)` or `(list 'foo ':int)` continue to work as today (the symbol/keyword is constructed at runtime; the reader sees `':int` syntactically). The codemod skips inside `quote` / `quasiquote` unless the user explicitly opts in. **Documented in Phase 2.**

### CC-7: Bare `:` token (defgadt and similar)

`reader.c:492-500` returns `F_SYM(":")` for a colon with no following form. Some `defgadt` constructor syntax uses a bare `:` separator. The codemod must leave bare `:` alone. **Documented in Phase 2; covered by Phase 1c audit.**

### CC-8: Sweet-exp neoteric `f(x :int)`

In neoteric, parameter vectors still parse identically to s-expression mode. Already-spaced sweet-exp files (e.g. CLAUDE.md examples) are the target style; the codemod rewrites fused forms inside neoteric and sweet-exp blocks just as for s-exp.

### CC-9: Generated docstring tables (`stdlib/docstrings.tur`)

`stdlib/docstrings.tur` is autogenerated by `tools/gendocs.py --emit-tur`. The generator emits the docstring **text** verbatim, including any `:int` substrings that appear inside `Parameters:` lines. After Phase 3, regenerate this file rather than hand-editing it.

### CC-10: Fixture `expected.c` snapshots

Codegen output does **not** contain Turmeric type annotations — it's lowered C. No rewrite needed; just regenerate snapshots per the CLAUDE.md rule once if the migration accidentally perturbs any codegen path (it should not, but the smoke-test guards against it).

### CC-11: Spice manifests (`build.tur`)

`build.tur` files use `:spices`, `:path`, `:url`, `:exports`, `:members`, `:refer` — all keywords, none are type annotations. They are left untouched. The codemod must skip top-level `build.tur` keyword maps (covered by CC-5's structural matcher).

### CC-12: Docs that show *both* styles for teaching purposes

`docs/guides/sweet-exp-guide.md`, `docs/guides/type-annotations-guide.md` (if it exists), and any "old-vs-new" examples may intentionally show both. The plan calls out a single "legacy fused form" mention in the type-annotation guide and otherwise standardises on spaced.

## Phased Plan

### Phase 0 — Decision lock & tracking issue (this doc)

- Land this plan in `docs/upcoming/`.
- Open a tracking issue listing each phase as a checkbox.
- No code changes.

### Phase 1 — Compiler readiness (close the elaboration gaps)

Each sub-phase is independently mergeable.

**Phase 1a: named-let binding type slot** *(DONE)*

- Widened the predicate at `elab_forms.c:1440` to accept both `F_KEYWORD` and `F_TYPE_ANN`. The form is passed through unchanged into the synthesised `(fn [...])`, whose elaborator already handles both shapes — no unwrap needed at this layer.
- Added fixture `tests/fixtures/named-let-loop-spaced-types/` (mirrors `named-let-loop` using `: int`). Codegen verified byte-identical to the fused-form fixture.
- Suite: 1310 pass / 1 pre-existing fail (`tur-apply-t-fatshim-float`, unrelated, confirmed via `git stash` baseline).

**Phase 1a-bis: plain `let` / `let*` binding type slot** *(DONE — commits 0c9cfd30, 6bc00459)*

Plain `(let [name init ...])` and `(let* ...)` currently have no type-annotation grammar at all. To support `(let [x : int 5] ...)` (and the fused form `(let [x :int 5] ...)` for symmetry with named-let), `elab_let` must learn an optional type slot between the binding name and its initializer.

Concrete work:

- In `elab_let` (around `elab_forms.c:486` after the `name = cur->as.sym; i++` step), insert an optional type-annotation consume:
  - If `items[i]` is `F_KEYWORD` or `F_TYPE_ANN`, capture its type form (unwrapping `F_TYPE_ANN` via `items[i]->as.list.items[0]`); advance `i`.
  - Otherwise, no annotation; leave `i` untouched.
- Resolve the captured type form via the existing type-elaboration helper used by `defn` parameter annotations (`elab_fns.c:894` pattern), and reconcile against `init->type`:
  - If both present, require a unify / subtype match against the inferred init type; emit a diagnostic on mismatch (reuse the diagnostic code used for `defn` param type mismatch, or add a new one if its phrasing doesn't fit).
  - Pass the annotated type to `binding_new` instead of `init->type` when present, so down-stream users (move tracking, borrow checks, substructural annotations) see the user-stated type.
- Mirror the same change in `elab_letstar` (`elab_forms.c:1080`), which is currently described as "structurally identical to elab_let".
- Audit `let*` desugaring in `elab_forms.c:1128–1169` to confirm the rewritten `let` form still parses correctly when a type slot is present.
- Decide whether the annotation belongs **before** or **after** the `^mut` / `^persistent` / `^linear` / `^unique` / `^affine` / `^relevant` prefix annotations that the parser already consumes at line 197. Recommendation: annotation **after** the name (matching defn/named-let), substructural markers **before** the name (matching today). I.e. `[^mut x : int 5]`, not `[x ^mut : int 5]`.

Fixtures (all happy-path unless noted):

- `tests/fixtures/let-typed-binding-spaced/` — `(let [x : int 5] (println x))`.
- `tests/fixtures/let-typed-binding-fused/` — same with `:int` (proves the new code accepts the existing fused form too).
- `tests/fixtures/let-typed-binding-complex-spaced/` — `(let [f : (-> int int) some-fn] (f 3))` (validates `F_TYPE_ANN` wrapping a list type form).
- `tests/fixtures/letstar-typed-binding-spaced/` — `let*` variant.
- `tests/fixtures/let-typed-binding-with-mut/` — `(let [^mut x : int 5] ...)` to lock in the annotation ordering.
- `tests/fixtures/errors/let-type-annotation-mismatch/` — `(let [x : int "hello"] ...)` must produce a type-mismatch diagnostic, not a silent coercion.

Each happy-path fixture also gets a fused-form sibling (or a single fixture with an `expected.c` snapshot, with a paired fused-form fixture whose snapshot must match) to prove **codegen equivalence** with `:int`.

Exit criteria: `bash tests/run.sh` green; plain `let` and `let*` both accept fused and spaced type annotations; the negative fixture produces a clear diagnostic.

**Phase 1b: Audit & regression-fixture sweep** *(DONE)*

For every site listed under "Elaboration sites that already accept `F_TYPE_ANN`", add (or confirm existing) one fixture using the spaced form:

- `defn` param + return: `(defn f [x : int] : int ...)` — `defn-spaced-typeann/`.
- `defn` complex param type: `(defn f [g : (-> int int)] : int ...)` — `defn-spaced-compound/`.
- Variadic rest: `(defn f [& rest : int] : int ...)` — `variadic-rest-spaced-int/`; and `(defn f [& rs : Route] : int ...)` — `variadic-rest-spaced-opaque/`.
- `fn` literal: `(fn [x : int] : int ...)` — `fn-spaced-typeann/`.
- `defstruct`: `(defstruct P [x : int y : int])` — `defstruct-spaced-typeann/`.
- `definstance` / typeclass method — `definstance-spaced/`.
- Sweet-exp: a `.tur.sweet`-style fixture combining several of the above — `sweet-exp-spaced-typeann/`.

The variadic-rest sub-task revealed a real elaboration gap: `elab_defn`/`elab_fn` only matched `F_KEYWORD` for the `& rest :type` slot. Both call sites in `elab_fns.c` were widened to also accept `F_TYPE_ANN{inner: F_SYM/F_KEYWORD}`. Codegen verified byte-identical between fused and spaced variants at fixture-creation time for both the int and opaque cases.

**Phase 1c: Bare-`:` and quoted-form regression** *(DONE)*

- `defgadt` bare-`:` constructor syntax: `defgadt-spaced-typeann/`.
- Quoted keyword literals (`(quote :int)`, `':float`): `quoted-keyword-type-ann/`. Confirms the reader round-trips `:T`-shaped keywords inside `quote` without confusing them with a spaced type annotation. The `(list ':int)` shape from the plan would require runtime list construction over `Sym` values, which is not currently a supported runtime path; the bare-quote shapes cover the same reader concern.

**Exit criteria for Phase 1**: Every type-annotation position the codemod will touch has at least one passing spaced-form fixture, and `bash tests/run.sh` is green with leak detection on.

### Phase 2 — Codemod tooling *(DONE)*

Landed `tools/spaced-types-rewrite.py`, a structural rewriter operating on a
trivia-preserving token tree (not regex):

- Tokenizer recognises strings, `;`-comments, ` ```c ... ``` ` inline-C
  fences, `#{...}` effect sets, `#map{...}`/`#set{...}`/`#vec{...}`
  data-literal dispatches, quote/quasiquote/unquote prefixes, and the
  `#lang sweet-exp` directive — each as opaque tokens that the walker
  steps over rather than into.
- Form tree is just paren/bracket/brace pairing with trivia kept as
  in-line children, so serialisation round-trips byte-for-byte when no
  rewrite fires.
- Type-bearing position walker covers `defn`/`fn`/`defmacro` param vec
  and return slot (including `& rest :T`), `defstruct` field vec,
  `let`/`let*`/`loop`/named-let binding vec (including the type slot
  enabled in Phase 1a-bis), and `defclass`/`defprotocol`/`definstance`
  method signatures.
- Sweet-exp handled by an implicit-form walker that scans the top-level
  token sequence for unparenthesised `defn`/`fn`/`let`/etc. and applies
  the same per-form rewriters (gated on a `#lang sweet-exp` directive).
- Skip rules: `#{...}`, `#map{...}`/`#set{...}`/`#vec{...}`, string
  literals, comments, ` ```c ... ``` ` blocks, and (by default) any form
  inside `quote`/`quasiquote`. `--rewrite-quoted` opts back in.
- `build.tur` files are skipped by name (CC-11). `--no-skip-build-tur`
  opts back in.
- CLI: `--write` (default), `--dry-run` (unified diff), `--check`
  (exit 1 if any file would change).

Test corpus under `tests/codemod/spaced-types/` has 14 hand-written
before/after pairs covering CC-1 through CC-13. Run with
`bash tests/codemod/run-spaced-types.sh`.

Smoke-tested end-to-end: `python3 tools/spaced-types-rewrite.py --check
stdlib/` reports 98 files / 2352 fused annotation sites needing rewrite
across stdlib; `--write` on `stdlib/option.tur` produced byte-identical
`emit-c` output to the fused-form original (codegen-invariance held). The
named-let-loop and t-expression-sweet-exp fixtures also round-tripped
through the rewriter with byte-identical codegen.

Carry-over for Phase 6: the `--check` mode is the CI hook called out in
that phase.

#### Original design notes

- New tool: `tools/spaced-types-rewrite.py` (or `tur fmt --spaced-types` if we prefer to land it inside the compiler — open question; Python keeps the migration off the critical path).
- Input: a `.tur` or `.tur.sweet` file.
- Behaviour:
  1. Read the file via the same reader the compiler uses (shell out to `tur` with a `--dump-forms` flag, **or** reimplement a minimal s-exp/sweet-exp tokenizer that preserves whitespace and comments). The compiler-driven path is preferred for fidelity.
  2. Walk forms, matching the *type-bearing positions* enumerated in CC-5.
  3. For each fused `F_KEYWORD` at such a position whose name begins with `:`, rewrite it to spaced form (`name :T` → `name : T`).
  4. Preserve all comments, blank lines, and existing horizontal alignment in `let` binding columns. The reader does not preserve these — so the rewriter operates on **token streams with trivia**, not on AST nodes alone.
- Skip rules:
  - Inside `quote` / `quasiquote` forms (unless `--rewrite-quoted`).
  - Inside `build.tur` top-level manifest maps.
  - Inside `#{...}` effect / contract sets.
  - Inside string literals and `;`-comments (trivially).
- `--check` mode: exits non-zero if any fused annotation remains; used in CI later.
- `--dry-run` mode: prints a unified diff.

Add a unit-test corpus under `tests/codemod/spaced-types/` with hand-written before/after pairs covering every CC-N case.

### Phase 3 — Mechanical rewrite (this repo) *(DONE)*

Landed across four commits (see `git log`):

1. `Phase 3 step 1: rewrite tests/fixtures` — 1338 files, 6196 rewrites.
   Also extended the elaborator to close the spaced-form gaps the rewrite
   surfaced (variadic `& rest : T`, defn/fn/defclass/definstance return
   slots, top-level forward-decl peek, letrec init-fn return peek).
   Pinned to fused form (kept as the fused-syntax regression coverage):
   `named-let-loop`, `variadic-types-int`, `variadic-types-opaque`,
   `defn-spaced-typeann` (intentional mix), `hamt-lisp-*` (4),
   `instance-closure-return-*` (5), `linear-lref-param-kw`,
   `set-duplicate-elements`, `session-*` (3),
   `typeclass-effect-row-caller`, `errors/typeclass-effect-row-*` (2),
   `scheduler-multithread`.
2. `Phase 3 step 2: rewrite stdlib` — 96 files, 2070 rewrites.
   Excluded `stdlib/docstrings.tur` (auto-generated; step 4) and
   `stdlib/httpd.tur` (hits a pre-existing if-branch widening gap that
   only surfaces under `load`). Also added bare `ptr` → TY_PTR_VOID in
   `typekind_from_symbol` so `: ptr` (F_SYM) resolves the same as
   fused `:ptr`. Regenerated 105 fixture `expected.c` snapshots to match
   the new stdlib codegen.
3. `Phase 3 step 3: rewrite examples/` — 9 files, 456 rewrites
   (datalog, guestbook, minikanren, snake).
4. Step 4 (`stdlib/docstrings.tur` regen): `python3 tools/gendocs.py
   stdlib --emit-tur stdlib/docstrings.tur` produced byte-identical
   output (the generator still emits fused form for the doctable's
   internal `(defn doc-lookup [name :cstr] :cstr ...)` signature; that
   keyword-typed inline-C stays valid and is intentionally left alone).
   No commit needed.

Final suite: **1337 pass / 8 fail**. All 8 failures
(`future-capturing-closure`, `hkt-for-comprehension`,
`hkt-for-comprehension-vec`, `instance-closure-return-*` × 5) reproduce
on `origin/main` with identical diagnostics, i.e. they are unrelated
pre-existing failures.

#### Original mechanical-rewrite checklist (kept for reference)

1. `tests/fixtures/**/*.tur` — run the codemod, then `bash tests/run.sh`. Any FAIL is a codemod bug; fix the tool, not the fixture. Snapshot `expected.c` files are untouched.
2. `stdlib/**/*.tur` (excluding the autogenerated `docstrings.tur`).
3. `examples/` and any other in-repo source trees.
4. Regenerate `stdlib/docstrings.tur` via `tur run docs --emit-tur` (per CLAUDE.md).
5. Run the full suite: `bash tests/run.sh` and the dedicated-runner ctest targets. Zero `FAIL`.

Commit each step separately so a regression bisects cleanly.

### Phase 4 — Mechanical rewrite (`../turmeric-spices`) *(DONE)*

Landed on the `spaced-types` branch in `../turmeric-spices` as commit
`645710a` ("Migrate to spaced type annotations"). Same codemod invocation
as Phase 3, scoped to `spices/`:

- 341 tracked `.tur` files modified, 4101 lines (+4101 / -4101 --
  whitespace-only changes).
- `build.tur` manifests skipped automatically by the codemod (CC-11).
- Codegen-invariance spot-checked on three representative files
  (`ansi/src/ansi/style.tur`, `math/src/math/vec3.tur`,
  `regex/src/regex/regex.tur`): `tur emit-c` output byte-identical for
  fused and spaced.
- Pre-existing per-spice build failures (`math` malloc shadowing,
  `regex`/`c-dsl`/`stats` per-file C compile issues) reproduce on the
  sibling repo's `main` unchanged.

The branch is local; opening a PR upstream is left for whoever drives
the migration on that repo's side.

#### Original phase notes

1. Run codemod on `spices/**/*.tur` and `spices/**/*.tur.sweet`.
2. Run that repo's test suite.
3. Open a PR there referencing this plan.

This phase can run concurrently with Phase 3 after Phase 1+2 land.

### Phase 5 — Documentation *(DONE)*

Built `tools/spaced-types-rewrite-md.py`, a small wrapper that walks
`turmeric` / `tur` / `scheme` / `clojure` / `lisp` code fences inside
markdown files and pipes the contents through `rewrite_source` from the
main codemod. Prose and non-Turmeric code blocks are left alone.

Landed as a single commit covering 72 files / 555 line changes across
`CLAUDE.md`, `README.md`, `docs/guides/**`, and `docs/upcoming/**`.
Preserved fused examples on purpose:

- The "current (fused)" motivation block in this plan (line 8), which
  exists to contrast against the target spaced form.
- The "old fused-keyword style is still accepted" demonstration in
  `docs/guides/type-annotations-guide.md`.

`docs/guides/type-annotations-guide.md` already existed and already
called out that the legacy fused form remains a valid reader input but
should not be used in new code, so the plan's "new short guide" step
was a no-op.

#### Original phase notes

- `CLAUDE.md` — the function-arity, sweet-exp, indentation, and docstring sections. All `:int` examples become `: int`.
- `README.md` — any code snippets.
- `docs/guides/**` — every guide. Walk one by one; many already use spaced in sweet-exp examples.
- `docs/upcoming/**` — new plans authored from now on use spaced. Existing in-flight plans optional.
- A single new short guide: `docs/guides/type-annotations-guide.md` (only if not already present) explaining the spaced form is canonical, with a one-line "legacy fused form `name:type` is still accepted by the reader but should not be used in new code".

### Phase 6 — Enforcement

- Add a CI step that runs `tools/spaced-types-rewrite.py --check` over the repo. Fused annotations in `.tur` / `.tur.sweet` cause CI to fail.
- Mirror the CI hook in `../turmeric-spices`.
- Optionally: lift the codemod into `tur fmt` so editors can auto-fix on save.

### Phase 7 — (Optional, deferred) Reader deprecation

Not part of this plan's commitment, but worth noting: once the ecosystem is fully migrated and CI prevents regressions, the reader could emit a deprecation warning when it sees fused `F_KEYWORD` in a recognised type-annotation position. We are explicitly **not** removing the fused form — it remains valid syntax. Revisit after several release cycles.

## Validation Strategy

- **Per-phase**: `bash tests/run.sh` green with ASan + LSan on (per CLAUDE.md leak-detection policy).
- **Phase 1**: side-by-side `emit-c` diff between fused and spaced fixtures must be empty (proves the migration is codegen-invariant).
- **Phase 3 / 4**: full suite green; spot-check a handful of representative spices (`linalg`, `httpd`, `raylib`).
- **Phase 6**: CI red on intentionally-introduced fused annotation in a throwaway branch.

## Open Questions

1. **Codemod language**: Python tool (fast to land, lives outside compiler) vs. `tur fmt` subcommand (canonical, but pulls migration onto the compiler's release cadence). Suggested: start with Python, port into `tur fmt` during Phase 6 if it earns its keep.
2. **`let` rewrite ergonomics**: spaced `let` with aligned binding columns is wider. Do we want the codemod to re-align? Suggested: no — leave realignment to a future `tur fmt` pass, keep this migration's diffs minimal.
3. **Sweet-exp `[x : int]` vs `[x : int 5]`**: in sweet-exp, the spaced form is already common. No special handling expected, but Phase 1b should add a sweet-exp let fixture once Phase 1a lands.

## Out of Scope

- Removing the fused form from the reader.
- Reformatting beyond the colon-space change (no indentation, no comment reflow, no alignment).
- Changing keyword-literal syntax (`:refer`, `:else`, etc.).
- Touching `expected.c` snapshots (they have no Turmeric syntax).

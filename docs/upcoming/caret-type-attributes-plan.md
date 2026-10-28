# Plan: spell type-declaration flags as `^attr`, not `:keyword`

> **Status:** P0, P1 and P2 IMPLEMENTED (2026-10-27, 2026-10-28); P3 is next. The three open questions
> are decided (see "Decisions"): attributes go before the name, `^move` stays,
> and `^heap` moves with the rest. Both spellings are accepted today; the
> keyword one is not yet warned about (P3). A spelling change only: every form
> here means exactly what it means today, and the checker's rules for linear,
> affine, copy and heap values do not move.

## The problem

The same word has two spellings depending on where it sits:

```turmeric
(defopaque Chan : ptr<void> :linear)        ; every Chan is linear
(defn close [^linear c : Chan] : unit ...)  ; this binding is linear
(defn open [] : ^linear Chan ...)           ; this result is linear
(exists :linear [a] [(Show a)] a)           ; this existential is linear
```

There are two conventions for flags on a definition, and the type declarations
are the only forms that use keywords:

| Form | Flags today | Where |
| --- | --- | --- |
| `defn` | `^deprecated ["msg"]`, `^reflect`, `^construct`, `^byval`, before the name | `elab_defn_name_index`, `src/compiler/elab_toplevel.c:2767` |
| `def` | `^mut`, `^persistent`, `^private`, before the name | `src/compiler/elab_forms.c`, `elab_fns.c:15681` |
| `defeffect` | `^private` before the name; `^extends Parent`, `^capability` after the signature | `src/compiler/elab_effects.c:1455`, `:1654` |
| `defdynamic` | `^private` before the name | `src/compiler/elab_dynvars.c:71` |
| `match` | `^non-exhaustive` | `src/compiler/elab_structs.c:3576` |
| parameters, `let`, return types | `^linear`, `^affine`, `^relevant`, `^unique`, `^mut`, `^borrow`, `^fat` | `elab_fns.c:9995`, `elab_types.c:1915` |
| **`defopaque`** | **`:linear`, `:affine`, `:sealed`, `:non-null`, after the base type** | `src/compiler/elab_structs.c:1010` |
| **`defstruct`** | **`:copy`, `:move`, `:linear`, `:heap`, `:no-auto-ctor`, after the name** | `src/compiler/elab_structs.c:690` |
| **`defdata` / `defgadt`** | **`:copy`, `:move`, `:linear`, `:affine`, `:heap`, `:no-auto-ctor`, after the name** | `src/compiler/elab_structs.c:1789` |
| **`exists`** (type) | **`:linear`** | `src/compiler/elab_types.c:1429` |

Three costs follow from that:

1. **A reader has to learn two spellings for one idea.** `^linear` and `:linear`
   both mean "used exactly once". The difference is only how far the claim
   reaches (one binding, or every value of the type), and the form already says
   that.
2. **A flag and a type look identical.** In `(defopaque Chan :ptr<void> :linear)`
   the first keyword is a type and the second is a flag. Nothing lexical tells
   them apart, so every editor pack has to guess
   ([editor-packs-do-not-colour-annotations-and-generic-types](../reported/editor-packs-do-not-colour-annotations-and-generic-types.md),
   "An open design question"). A `^linear` is already one token with one
   meaning, and Try Turmeric colours it as a modifier.
3. **The type position is split.** `: ^linear Chan` is legal as a return
   type, while the type form `exists` takes `:linear`.

## The rule this plan adopts

- **`^name` is an attribute of the thing being declared or bound.** Most take
  no argument. A few take one (`^deprecated "msg"`, `^extends IO`), and that
  stays as it is.
- **`:name` is data or a named argument**: map keys, `:refer [...]`, manifest
  keys, and the type annotation `: T`.

Under that rule, every flag on a type declaration is a `^` attribute.

## Target syntax

The attributes go **before the name**, as on `defn`, `def`, `defdynamic` and
`defeffect`'s `^private`:

```turmeric
(defopaque ^linear Chan : ptr<void>)
(defopaque ^affine ^sealed Future : ptr<void>)
(defopaque ^non-null Handle : ptr<void>)
(defstruct ^copy Point [x : int y : int])
(defstruct ^linear Socket [fd : int])
(defstruct ^heap Node [next : ptr<Node> val : int])
(defdata ^copy Shape (Circle int) (Square int))
(defdata ^linear Token [A] (Token A))
(exists ^linear [a] [(Show a)] a)
```

In sweet-exp the same: `defopaque ^linear Chan : ptr<void>`.

The full set is `^copy`, `^move`, `^linear`, `^affine`, `^heap`, `^sealed`,
`^non-null` and `^no-auto-ctor`. On a declaration they mean exactly what the
keyword means today. The mutual exclusions carry over: `^linear` with
`^affine`, and `^copy` with `^move`.

### Why before the name, not in the keyword's old slot

- **A type-parameter vector already reads `^` as a kind.** `[^f a]`, `[^^f]`
  and `[^&cols]` declare kinds (`elab_structs.c:1821`), so any slot next to the
  vector is a trap. Before the name never touches the vector.
- **It matches the definitions that already take `^`.** `(defn ^deprecated f ...)`
  and `(def ^mut x ...)` put the attribute first. That is the convention a
  reader has already met.
- **It reads the way the type position does.** `(defopaque ^linear Chan ...)`
  and `: ^linear Chan` say the same thing in the same order.

The alternative is to keep the slot and change only the sigil, as in
`(defopaque Chan : ptr<void> ^linear)`. That is the cheaper migration, since no
tool that finds a declaration's name has to change. It was rejected
(2026-10-27) because
`defstruct` and `defdata` put their flags next to the type-parameter vector,
so the kind collision above would be one typo away, and because it keeps the
type declarations unlike every other definition. `defeffect`'s trailing
`^extends` / `^capability` are attributes of the effect's signature and stay
where they are.

## Phases

### P0 -- one shared parser, accepting both spellings

- Add `sym_caret_copy`, `sym_caret_move`, `sym_caret_heap`, `sym_caret_sealed`,
  `sym_caret_non_null` and `sym_caret_no_auto_ctor` to the interned table
  (`elab_core.c:2493`). `^linear` and `^affine` already exist.
- Write one helper, say `elab_type_decl_attrs(e, form, &idx, &out)`. It reads
  the leading `^` attributes into a flags struct and reports an unknown `^name`
  or a contradictory pair, naming the declaration form. `elab_defstruct`,
  `elab_defdata` / `defgadt` and `elab_defopaque` all call it. The trailing
  keyword loops stay for now and fill the same struct, so both spellings reach
  identical code.
- Add one helper that returns the name index of a type declaration, as
  `elab_defn_name_index` does for `defn`. Every pre-pass that reads the name
  calls it. The comment at `elab_toplevel.c:2759` records the bug this
  prevents: a module pre-declare scan that skipped none of the `^` attributes.
  Known callers:
  - the forward-type pre-pass, `elab_toplevel.c:3396` (reads `items[1]`);
  - `elab_defstruct` lowering to `defdata`, `elab_structs.c:895`. It can forward
    the parsed flags rather than rebuilding keyword forms;
  - `scheme_lower.c:5247` (`TYPE_HEADS`);
  - the hoisting of nested type definitions, `elab_call.c:5041`.
- `exists` accepts `^linear` beside `:linear` (`elab_types.c:1429`).
- **Fix one gap on the way.** `defstruct`'s keyword loop has no `:affine`,
  though `defdata` and `defopaque` do. From reading the loop, an `:affine`
  there ends the loop and is then parsed as part of the fields. With one
  shared parser, `^affine` works on all three. Pin it with a fixture.

**No `--enable` gate.** This adds a second spelling that elaborates to the
same thing. It has no semantics in flux and no cost to impose, which is what
the experiment rule in CLAUDE.md is for. If review disagrees, an
`EXPERIMENTS[]` row is cheap to add.

**Fixtures** (new):
- `caret-type-attrs` runs every attribute on every form. Its output matches
  the keyword spelling of the same program.
- `errors/caret-type-attrs-linear-and-affine` and
  `errors/caret-type-attrs-unknown` pin the diagnostics.
- `caret-type-attrs-defstruct-affine` pins the gap fixed above.
- `caret-exists-linear` pins the type form.

#### As implemented (2026-10-27)

- `elab_type_decl_name_index` and `elab_type_decl_attrs` are in
  `src/compiler/elab_structs.c`, declared in `elab_internal.h` with a
  `TypeDeclAttrs` struct and `TDA_*` mask bits. Each form passes the mask of
  attributes it takes, so `defstruct ^sealed` and `defopaque ^heap` are errors
  that list what the form does take. The mutual exclusions (`^linear` with
  `^affine`, `^copy` with `^move`) are checked there.
- `elab_defstruct`, `elab_adt_def` (`defdata` / `defgadt`) and `elab_defopaque`
  read their name at the returned index and start from the parsed flags. Their
  keyword loops are unchanged apart from `defstruct` gaining `:affine`, and set
  the same variables.
- `defstruct` lowers to `defdata` by building a caret-first form, so every
  `defstruct` now goes through the new parser.
- The forward-type pre-pass in `elab_toplevel.c` finds the name (and a
  `defopaque`'s phantom arity) past the attributes.
- Two sites on the plan's list needed no change: `scheme_lower.c`'s
  `stdlib_names_of` already skips a `^` before the name, and
  `elab_hoist_nested_type_def` takes the elaborated expression, not the form.
- `exists` accepts `^linear` beside `:linear`.
- Verified: 8 new fixtures (5 of them `errors/`); the full `tests/run.sh`
  (4183 passed, 0 failed); the interpreter harness on the new fixtures
  (`caret-exists-linear` is skipped there, as it holds user inline-C). The
  emitted C for `^non-null`, `^no-auto-ctor` and `^move ^heap` is identical to
  the keyword spelling's.
- Not verified: that `defstruct :affine` failed before this change. That claim
  comes from reading the old loop; no pre-change build was run. The fixture
  passes with both spellings now.
- Diagnostics still say `:non-null` and `:linear` where they mention a keyword
  (for example the "requires a pointer base type" message). P3 rewords them.

### P1 -- tools that read declarations

Each of these finds a declaration by its head and then its name, or reads the
flags. Each has to skip or understand the leading `^` attributes:

- `tools/gendocs.py`: `def_re` (line ~593), plus the `struct_ann` detection for
  `defstruct` (~1107) and `defopaque` (~1113). The rendered declaration shows
  the attributes as written.
- `src/lsp/lsp_docs.c`, `src/cli/lsp_lite.c:259` and `src/turi/docstrings.c:242`,
  for the signature strings.
- `src/compiler/fmt.c`. It must keep the attributes on the head line, and must
  not reflow them under the name.
- `web/main.js` `CARET_MODIFIERS` and the matching list in
  `tools/genguides.py`. **These lists are required, not cosmetic.** Any `^name`
  not on them is coloured as a kind, so `^copy` would read as a type variable
  until it is added.
- The editor-packs report gains a line: the new attributes join each pack's
  modifier set.

#### As implemented (2026-10-27)

- **`tools/gendocs.py`.** `_parse_def_line` reads the `^attrs` before the name
  of a `defopaque` / `defstruct` / `defdata` / `defgadt` into `lead_attrs`, so
  the name is the symbol after them (it was `^copy`). The signature is shown as
  written: a leading `^` flag renders before the name and a trailing keyword
  where it was written, so the existing keyword pages do not change. The
  linear / affine badge reads both spellings through `_decl_flags`. The
  `defstruct` and `defdata` keyword scans also learned `:affine` / `:linear`,
  which they had missed. Regenerating `stdlib/docstrings.tur` and the API pages
  from the current stdlib gives byte-identical output. Pinned in
  `tests/tools/check_gendocs_parse.py` (6 renderings, 10 flag cases).
- **`src/lsp/lsp_docs.c`** keyed an entry by `^copy` for
  `(defstruct ^copy Point ...)`, and did the same for `(defn ^construct some
  ...)` before this plan. It now steps over the `^attrs`, and over the message
  of a `^deprecated`, and it also scans `defopaque` and `defgadt`. Pinned in
  `tests/lsp/docscanner_test.c`.
- **`src/turi/repl.c`** `detect_type_form` named a struct `^copy` in the
  REPL's `#<struct-type ...>` line; it now skips the attributes. Checked by
  hand in `tur repl`; there is no REPL harness to pin it in.
- **`tools/check_turi_native_parity.py`**'s name regex skips the attributes.
  (It still reads a `^deprecated "msg"` defn's name as the message; that
  predates the plan and the script is informational.)
- **`src/compiler/fmt.c` needed a fix, found in P2.** P1 first recorded it as
  needing none: the check used declarations that fit on one line. A
  declaration too long for one line is laid out by the generic call printer,
  which puts the head and its *first argument* on the opening line, so the
  first attribute took that place and the name moved to a line of its own
  (`(defdata ^copy` / `  MdKind` / `  (MdKDocument)`). `tur_fmt_tests` caught it
  as `fmt-bootstrap-stdlib` once the stdlib was migrated. `fmt_call` now treats
  the leading `^attr`s and the name of a `defopaque` / `defstruct` / `defdata` /
  `defgadt` as the header; a form without attributes lays out as before.
  Pinned by `fmt-type-decl-attrs-stay-with-name` in `tests/run-fmt.sh`.
- **No change needed:** `src/cli/lsp_lite.c` holds a list of builtin names,
  not declarations; `src/turi/docstrings.c:242` is a `defstruct` signature
  string with no flag in it.
- **Highlighters.** `CARET_MODIFIERS` in `web/main.js` and in
  `tools/genguides.py` gained `copy`, `move`, `heap`, `sealed`, `non-null` and
  `no-auto-ctor`. Checked: the docs highlighter run in Chromium colours all
  eight flags `hl-ann` and a kind in the type-parameter vector `hl-kind`; the
  regex `main.js` builds from the list matches them and not `^copyx`. Cases
  were added to `web/tests/docs-highlight.spec.js` and
  `web/tests/editor-highlight.spec.js`.
- **Not run:** the two Playwright specs themselves. `web/node_modules` is not
  installed in this environment, and `editor-highlight.spec.js` also needs the
  dev server and the wasm build. The Monaco editor's colouring of the new
  names is therefore checked through its regex only.
- The editor-packs report notes the new names. None of the four packs keeps a
  closed modifier list, so they need nothing for these names.

### P2 -- migrate the tree

A rewrite tool in the style of `tools/spaced-types-rewrite.py`, which moves the
trailing keywords to leading `^` attributes. It works on forms rather than
lines, since many declarations span lines. Rough counts of single-line forms
today, across `.tur`, `.sweet` and `.md`:

| Keyword | Sites |
| --- | --- |
| `:copy` | ~600 |
| `:heap` | ~260 |
| `:linear` | ~70 |
| `:move` | ~65 |
| `:affine` | ~17 |
| `:non-null` | ~15 |
| `:sealed` | ~12 |
| `:no-auto-ctor` | 3 |
| `exists :linear` | 6 |

Run it over `stdlib/`, `tests/fixtures/`, `docs/guides/` and `examples/`, then
regenerate the codegen snapshots in the same PR. The spelling does not reach
emitted C, so there should be no snapshot churn, and a moved snapshot means
the rewrite changed something it should not have.

Update these by hand:
- the `;;@form` lines and docstrings in `stdlib/forms/datatypes.tur` (`defstruct`,
  `defdata`, `defopaque`), then `tur run docs`;
- `substructural-types-guide`, `opaques-guide`, `structs-guide`,
  `sum-types-guide`, `gadts-guide` and `type-annotations-guide`. The
  substructural guide gains a short section that says one spelling now covers
  bindings and types;
- `turmeric-spices`, separately, once P3 ships a release that warns.

#### As implemented (2026-10-28)

- **The tool** is `tools/caret-type-attrs-rewrite.py`. It reuses the
  tokenizer of `spaced-types-rewrite.py`, which keeps every byte of trivia, and
  edits tokens in place: each flag keyword of a `defopaque` / `defstruct` /
  `defdata` / `defgadt` becomes a `^attr` before the name (after any already
  there), and `(exists :linear ...)` becomes `(exists ^linear ...)`. A flag
  that stood alone on its line takes the line with it. It reads `.tur`,
  `.tur.sweet` (a head at the start of a line) and `.md` (the `turmeric` and
  `sweet-exp` fences, with their inline-C blocks, and `(def...)` code spans).
  `--write`, `--dry-run` and `--check`; a second run changes nothing. Eleven
  cases in `tests/codemod/caret-type-attrs/`, run by
  `tests/codemod/run-caret-type-attrs.sh`: each form, a phantom-parameter
  `defopaque` in fused and spaced form, flags on either side of a vector,
  macro templates (`~name`), a name ending in `'`, flags on their own lines,
  a half-migrated form, sweet-exp, Markdown, and text that must not move.
- **Applied to** `stdlib/`, `tests/fixtures/`, `docs/guides/`, `examples/`,
  `benchmarks/` and `tools/check-guide-pairs.tur`: 425 files. Left alone on
  purpose: `docs/archive/`, `CHANGELOG.md`, the two older codemod corpora,
  `docs/reported/` (its repros show the spelling they were filed with) and the
  other open plans.
- **Checked, not assumed.** Every changed file differs from its original only
  in flag tokens, and no flag was lost or duplicated (strip every `:flag` and
  `^flag` from both versions and compare). `tur emit-c` of all 3,239 fixture
  inputs, examples and benchmarks, before and after: identical except 12
  files whose only difference is a source-line constant, because joining a
  multi-line declaration moved lines in `stdlib/zip.tur` and the markdown
  modules (`tur_panic_at_site("zip.tur", 215 -> 209`). No snapshot needed
  regenerating; `tests/run.sh` and `tests/run-turi.sh` pass.
- **By hand (about 60 files):** the 20 comment and heredoc lines the tool does
  not see; the programs embedded in strings in `tests/*.py`, `tests/*.sh`,
  `tests/turi/*.c`, `web/main.js` and `tutorials/eavt.yaml` (14 sites, each an
  exact replacement); `stdlib/forms/datatypes.tur` (the `defstruct`,
  `make-struct`, `defdata` and `defopaque` reference text and `;;@form` lines,
  so `stdlib/docstrings.tur` changed in 7 entries); and the guides' prose. A
  mechanical pass turned 157 backticked mentions such as `` `:heap` node `` into
  `` `^heap` node `` (reviewed line by line; `package-manager-internals-guide`,
  where `:heap` is a manifest value, and `developing-spices-guide`, which
  quotes an old compiler error, were skipped); the sentences about *where* the
  flag goes (`sum-types`, `gadts`, `introducing-saffron`, `opaques`, `structs`)
  and the `r7rs-implementation-guide` table were reworded. The substructural
  guide has its new section, "On a type declaration".
- **Left for P3**, because they quote compiler text: the two
  `errors/conv-with-copy-error-on-*` fixtures and their `expected.diag`; the
  diagnostics and comments in `src/compiler/` that spell a keyword; and
  `tests/tools/check_gendocs_parse.py`, which tests the keyword spelling on
  purpose.
- **Run after the change:** `tests/run.sh` (4183 passed, 0 failed),
  `tests/run-turi.sh` (3103 passed, 0 failed, 1015 skipped), the Docs lint
  script, the guide-pair checker (980 pairs), the spaced-types check, and the
  ctest targets for the formatter, gendocs, the LSP scanner, the flag, project
  and representation harnesses, and the type, region, refinement and Saffron
  fuzzers.
- **Not done:** `turmeric-spices` (another repo); the Playwright specs (see
  P1); and `tur_generic_spec_matrix`, which does not finish inside the 12-minute
  cap even when it runs alone, so there is no result to report. The one line
  of it that changed, `(defstruct ^heap MxH [...])`, was checked on its own:
  the emitted C is identical to the keyword spelling's.

### P3 -- warn on the keyword spelling

A new `TUR-W` warning on a trailing flag keyword on `defopaque`, `defstruct`,
`defdata`, `defgadt` or `exists`. It names the `^` spelling, for example
"write `(defopaque ^linear Chan ...)`". `tur fmt`, which formats in place,
learns the same rewrite as the P2 tool, so a user's own code migrates with one
command. The warning lives for at least one minor release.

### P4 -- drop the keyword spelling

Remove the keyword loops. A trailing `:linear` is then an error that names
the `^` form, because the declaration's arity no longer fits.

## Attributes beyond the type declarations (proposed P5)

> Status: survey and proposal, 2026-10-28; nothing decided. The decisions
> below P4 cover the type declarations only.

The rule this plan adopts -- `^name` is an attribute and goes before what it
marks -- has a few exceptions outside the four type declarations. A survey of
`stdlib/`, `tests/fixtures/`, `examples/`, `benchmarks/` and `tools/` (every
`^` symbol and flag keyword that is a direct child of a definition-like form,
and its position relative to the name), plus the elaborator's own lists:

### Not following the rule

| Form | Today | Where it is read | Uses |
| --- | --- | --- | --- |
| `defeffect` | `^extends Parent` and `^capability` **after** the signature: `(defeffect FS [] :nil ^extends IO ^capability)`. `^private` already goes before the name. | `src/compiler/elab_effects.c:1646` (scan from `name_idx + 3`) | 10 each, all in `stdlib/effects.tur` |
| `defn` | `#[no-unwind]` and `#[used]`: before the name, but a third spelling for a flag with no value. | `elab_fns.c:9250`, `elab_toplevel.c:2453`, `:2773` | 2 fixtures (`panic-no-unwind*`) |
| `defn` | `(export-as "c_name")`: before the name, but a list where every other pre-name attribute with an argument is `^attr arg` (`^deprecated "msg"`). | `elab_fns.c:9264`, `elab_toplevel.c:2462`, `:2779` | 3 fixtures, one of them an `errors/` case |
| `import` | `:for-macros`, a keyword with no value. | the import parser | 5 |

### Already following it

These looked like candidates in the survey, but each leads the thing it marks,
which is the rule:

- `^tailcall` before a call: `(if c acc ^tailcall (f x))`.
- `^fat` before a return type, `(defn pfail-raw [] ^fat :ptr<void> ...)`, and
  `^linear` before a `defdynamic`'s type, `(defdynamic *r* ^linear :int 0)`
  (refused, `errors/dynvar-linear-type`). Both qualify the type that follows,
  as `: ^linear Chan` does.
- A handler case's `(Eff [p] ^linear k)` / `^multishot k`: before the
  continuation it marks.
- `(match ^non-exhaustive x ...)`, and the binding annotations (`^mut`,
  `^borrow`, `^fat`, `^linear`, ...) before the name they bind.
- Keywords with a value stay keywords: `defn`'s `:pre` / `:post` contracts,
  `import`'s `:as` / `:refer`, `defpackage`'s keys.

### Proposal

1. **`defeffect`: move `^extends Parent` and `^capability` before the name**,
   beside `^private`: `(defeffect ^capability ^extends IO FS [] : nil)`. This
   is the one real trailing case. `^extends` takes an argument, so the
   name-finding has to skip `^extends X` the way `elab_defn_name_index` skips
   `^deprecated "msg"`; `tools/gendocs.py`'s `defeffect` pattern (one optional
   `^\w+` before the name) and its `_spaced_decl` would follow. The migration
   is 20 sites in one file, which the P2 tool can learn, and both spellings can
   be read for a release as in P0-P4.
2. **`defn`: spell `#[no-unwind]` and `#[used]` as `^no-unwind` and `^used`.**
   Same meaning, same place, one sigil for a flag. Two fixtures to move.
3. **`defn`: spell `(export-as "c_name")` as `^export-as "c_name"`**, the
   shape `^deprecated "msg"` already has. Lower value than 1 and 2: the list
   form is unambiguous today, and the change is only for uniformity.
4. **`import ... :for-macros`: leave it.** `import` is a directive, not a
   declaration, its other options are keywords with values, and a lone
   `^for-macros` among `:as` and `:refer` would make that form inconsistent
   instead.

Each is independent: P5 can take 1 alone. The P0 machinery (an interned
symbol per attribute, a shared name-index helper, the P1 tools, the P2
rewrite tool, the P3 warning) applies to each without new design.

### Open questions for P5

1. Move `defeffect`'s `^extends` / `^capability` before the name?
   (Recommended: yes.)
2. Fold `#[no-unwind]` / `#[used]` into `^no-unwind` / `^used`?
   (Recommended: yes.)
3. `(export-as "c")` -> `^export-as "c"`? (Recommended: optional, low value.)
4. Leave `import`'s `:for-macros` as a keyword? (Recommended: leave it.)

## Out of scope

- **Any change to what the attributes mean.** That includes how an `^affine`
  binding of a `^linear` type is checked, whether `^relevant` / `^unique`
  should exist on type declarations, and whether a `^linear` type with a
  `^copy` binding is an error. Those questions predate this plan, and folding
  them in would turn a mechanical rename into a semantics change. Note them as
  follow-ups if P0 surfaces them.
- **Keywords that carry a value**: `:refer [...]`, manifest keys such as
  `:spices` and `:build-dir`, map literals, `:as`. Those are named arguments.
- **Attributes on other forms** -- `defeffect`'s trailing `^extends` /
  `^capability`, and `defn`'s `#[no-unwind]` / `#[used]` / `(export-as ...)` --
  are not part of P0-P4. They are surveyed in "Attributes beyond the type
  declarations" below, which proposes a P5 for them.
- **Inline-C and `extern-c` C types such as `^int`.** They are already `^`, and
  their meaning (a C type) is separate.

## Decisions (2026-10-27)

1. **The attributes go before the name.** The trailing slot was rejected for
   the reasons in "Why before the name". So the name-finding helper and the
   tooling work in P1 are in scope.
2. **`^move` stays.** It is the default, so writing it is optional, but a
   declaration can still state its kind explicitly, as the guides' tables do.
   It is a full member of the set, not a deprecated alias: the P3 warning
   rewrites `:move` to `^move` and does not drop it.
3. **`^heap` moves with the rest.** It is about representation, not about how
   often a value is used, but it is a flag with no value on the declaration, so
   the rule puts it on `^`. The change covers every flag on a type
   declaration, not only the substructural ones.

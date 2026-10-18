# Reader Envelopes: `stringed` Graduation + Literate `.tur.md`

> **Status:** On hold. Part 1 (graduate `stringed`) is done -- in v0.49.0,
> then superseded by the decommission of the layer axis. Part 2 (the envelope
> axis and literate `.tur.md`) and Part 3 (the `turmeric/at-exp` base dialect)
> are not implemented; checked 2026-10-18, nothing in `src/` handles either.
> The plan sat in `docs/upcoming/hold/` until the 2026-10-16 flatten. Literate
> `.tur.md` is one of the consumers the rest of
> [markdown-library-plan](markdown-library-plan.md)'s M5 waits on.
> **Introduced:** 2026-09-08
> **Supersedes:** `reader-layers-decommission-and-at-exp-literate-plan.md`
> (never committed; its literate design put literate mode on the base-dialect
> axis, which this plan replaces -- see "Why literate is neither a layer nor a
> base").

## Overview

Two pieces, independent of each other:

1. ~~**Graduate the `stringed` layer.**~~ **Done in v0.49.0, and superseded**
   by the decommission of the whole layer axis -- see
   [`lang-layers-decommission-plan.md`](../archive/lang-layers-decommission-plan.md).
   Part 1 below is a pointer, not a design.
2. **Add a third reader axis -- the *envelope* -- and put literate `.tur.md`
   on it.** An envelope is a source-to-source pass that runs *before* base
   dialect selection and yields ordinary Turmeric source. It composes with
   every base rather than replacing one, which is exactly the property
   literate mode needs and which neither existing axis provides.

Part 3 keeps the `turmeric/at-exp` base-dialect research from the superseded
draft, because it is real work that is still wanted and because it is the
clean counterexample that makes the axis distinction concrete: at-exp *is* a
base dialect, literate is not.

---

## The three axes

Today the reader has two axes:

| Axis | Selected by | Cardinality | Mechanism |
|---|---|---|---|
| **Base dialect** | `#lang <base>` or extension | exactly one | `ReaderType` (`diag.h:388`), branches inside the reader + `sweet_preprocess` |
| **Layer** | `#lang <base> <layer>*` | a set | `LANG_LAYERS[]` bitset, `reader_hook` registers a `#`-dispatch |

This plan adds a third:

| Axis | Selected by | Cardinality | Mechanism |
|---|---|---|---|
| **Envelope** | file extension (`.tur.md`) | at most one | source-to-source pass producing base-dialect source |

The three are orthogonal: an envelope wraps a base, and layers ride along
regardless. `foo.tur.md` containing a ```` ```turmeric/sweet ```` fence is
envelope=literate, base=sweet, layers={}.

### Why literate is neither a layer nor a base

The instinct that literate mode must work with **both** `turmeric` and
`turmeric/sweet` is right, and it rules out both existing axes:

- **It cannot be a `LANG_LAYERS[]` layer.** The only power a `LAYER_READER`
  row has is its `reader_hook` (`lang_layers.h:38`), which is handed a
  `ReaderMacroRegistry` and can do exactly one thing: register a `#`-dispatch
  macro. It never sees the source text. Literate mode is a whole-buffer
  transform, so expressing it as a layer would mean inventing a third
  `LangLayerKind` whose hook has a completely different signature and runs at
  a completely different point -- a table that shares a name and nothing else.
  CLAUDE.md's layer contract ("flips on a `#`-dispatch ... additive and
  commutative") would no longer describe half its own rows.
- **It should not be a base dialect either** -- which is what the superseded
  draft proposed. `ReaderType` is a single field (`diag.h:437`); a
  `READER_LITERATE` base is mutually exclusive with `READER_SWEET`, so a
  literate file could only ever hold plain s-expressions. Recovering
  sweet-exp would take a `turmeric/literate-sweet` base, and then
  `turmeric/literate-at-exp`, `turmeric/literate-neoteric`... the
  combinatorial explosion the base axis exists to avoid.

The envelope axis is the shape the requirement actually describes: *one* new
concept, `N` bases x 1 envelope, no product table.

### The design trick that makes it cheap: blank, don't extract

The obvious implementation of a literate pass is "extract the code blocks and
concatenate them," which shifts every byte and forces an offset map
(`SweetMap`-style) so diagnostics point back at the `.tur.md`. Composing that
map with the sweet-exp map for a sweet-based literate file is real work.

Instead: **overwrite prose bytes with spaces, keep newlines, keep code bytes
in place.** The output buffer is byte-for-byte the same length as the input,
with identical newline positions, so every offset, line, and column is already
correct in original coordinates. Consequences:

- **No envelope offset map at all.** `diag_register_file` keeps the original
  `.tur.md` as the file contents and every span already points at the right
  line and column. No `xform_map`, no composition with `SweetMap`.
- **The base dialect composes for free.** `sweet_preprocess` (`reader.c:4267`)
  receives a buffer whose coordinates are the original file's, so its own
  `SweetMap` stays correct without any awareness that an envelope ran.
- **The reader needs no changes.** It sees whitespace between forms, which it
  already skips.
- The pass is a line-classifying scan with a small state machine (in-fence /
  out-of-fence) and no recursive grammar.

Cost: the intermediate buffer is as large as the Markdown file, most of it
spaces. For source files this is irrelevant.

---

## Part 1 -- Graduate the `stringed` layer -- DONE, and superseded

Part 1 landed in v0.49.0, as part of something larger: rather than graduating
the one layer and leaving an empty `LANG_LAYERS[]` behind, the whole layer
axis was decommissioned. `#s"..."` is an unconditional reader macro
(`reader_macros_install_builtins`), `#lang` takes a single base dialect, and
`stringed` is a warned no-op for one minor line.

The full argument, the surface audit and the migration window are in
[`lang-layers-decommission-plan.md`](../archive/lang-layers-decommission-plan.md).
The design that used to be written out here is deleted rather than kept
alongside it: two descriptions of the same finished change is exactly the
drift the archiving rule exists to prevent.

Parts 2 and 3 below are unaffected. Neither is a layer -- that is the point
they were making -- so neither depended on the axis and neither changes. The
decommission does simplify Part 2 slightly: an envelope now composes with a
base and nothing else.

## Part 2 -- The envelope axis and literate `.tur.md`

### What a literate `.tur.md` is

A Markdown file that *is* a compilation unit. Prose is prose; fenced blocks
tagged with a Turmeric base dialect are the program.

````markdown
# Squares

This module computes squares.  Everything outside a tagged fence is prose and
is invisible to the compiler.

```turmeric
(defn square [x : int] : int
  (* x x))
```

Multiple blocks concatenate in file order, so a later block sees the earlier
one's definitions:

```turmeric
(defn main [] : int
  (println (square 7))
  0)
```
````

Same file with sweet-exp, changing nothing but the fence tag:

````markdown
```turmeric/sweet
defn square [x : int] : int
  *(x x)
```
````

### Why this and not the notebook spice

The notebook spice (`turmeric-spices/spices/notebook`) already reads `.tur.md`
and evaluates its cells -- it has cell extraction (`src/notebook/cell.tur`),
fence-attribute parsing, session-backed eval, caching, and HTML/TUI rendering.
It uses the same ```` ```turmeric ```` fence tag, including attribute form
(```` ```turmeric {math=true} ````).

What it does **not** give is `.tur.md` as a *compilation unit*: `tur build`,
`tur check`, `(import)` of a `.tur.md` module, fixtures, `emit-c`, LSP. A
notebook is a document you run cell-by-cell in a session; a literate module is
a program you compile. This plan is the second thing, and it should share the
first thing's surface syntax so one file can be both -- open a literate module
in `nb tui` and it just works.

**Coordinate the fence grammar with the spice.** Anything this plan adds
(recognized tags, an attribute meaning "not part of the program") must be
something the notebook's parser already tolerates or is changed to tolerate,
in the same window. Two `.tur.md` dialects that disagree about which fences
are code would be worse than either alone.

### Why fence tags, not `#lang`

Base dialect selection inside a `.tur.md` comes from the **fence info
string**, not a `#lang` line:

- `#lang turmeric/sweet` at the top of a Markdown file renders as an `<h1>`
  reading "lang turmeric/sweet". The whole point of the format is that it
  renders.
- The fence tag is where a Markdown author already declares a language, it is
  what every renderer and editor uses for highlighting, and it is what the
  notebook spice already parses.
- `detect_lang_layered` (`reader.c:4551`) requires `#lang` on the first line
  of the buffer, which a Markdown file's first line is not.

MVP: **one base per file.** The first tagged fence sets it; a later fence
with a different tag is a hard error (`TUR-E03xx`, "conflicting base dialect
in literate file"). Per-block dialects are deferred -- see Open questions.

### Which fences are code

- ```` ```turmeric ````, ```` ```turmeric/sweet ````, ```` ```turmeric/neoteric ````,
  ```` ```turmeric/curly-infix ```` -- code, in the named base. Reuse
  `lang_base_from_name` (`reader.c:4532`) for the tag, so the recognized set
  cannot drift from `#lang`'s. That also means `sweet-exp` (the legacy alias)
  is accepted, and a future `turmeric/at-exp` works with no extra wiring.
- Any other tag (` ```c `, ` ```sh `, ` ```json `, untagged) -- prose.
  Deliberately: a literate module shows shell commands and C output as
  illustration all the time.
- **Indented (4-space) Markdown code blocks -- prose.** They carry no language
  tag, so treating them as code would silently compile examples.
- Tags carrying attributes (```` ```turmeric {id=setup} ````) -- code; the
  attributes are for the notebook and are ignored here. One attribute is worth
  reserving from day one: an opt-out (`{example}` / `{no-compile}`) for a
  Turmeric block that is *illustration*, not part of the program. Without it,
  a literate doc cannot show a snippet of bad code. Confirm the spelling with
  the notebook spice before shipping.

The scanner also has to respect Markdown's real fence rules or it will
mis-slice files: a fence may be ```` ``` ```` **or** `~~~`, may use more than
three characters, and is closed only by a run of the same character at least
as long. Blocks inside blockquotes and list items are indented; MVP can
require code fences at column 0 and diagnose an indented one, but must not
silently misread it.

### Implementation

**E1 -- The envelope concept.** New `SourceEnvelope` enum in `diag.h` next to
`ReaderType` (`diag.h:388`):

```c
typedef enum SourceEnvelope {
    ENVELOPE_NONE = 0,     /* plain source; the only value before this plan */
    ENVELOPE_LITERATE_MD,  /* .tur.md -- fenced Turmeric inside Markdown */
} SourceEnvelope;
```

Add a `SourceEnvelope envelope;` field to `SourceFile` (`diag.h:437`),
defaulting to `ENVELOPE_NONE`. Zero-initialization makes every existing
construction site correct without edits.

**E2 -- Extension dispatch.** `reader_type_from_extension` (`reader.c:4665`)
currently answers one question with one return value. Split it:
`source_format_from_extension(path, &envelope, &base)` returning both, with
`reader_type_from_extension` kept as a thin wrapper so the ~9 existing call
sites (`main.c:176`, `main.c:7355`, `main.c:7615`, `eval.c:12414`,
`wasm_lsp.c:146`, `lsp.c:2111`, ...) compile unchanged and are converted one
at a time.

For `.tur.md`: envelope = `ENVELOPE_LITERATE_MD`, base = *unresolved* (it
comes from the first fence, which needs the buffer). Have the envelope pass
report the base it found, and set `file->reader_type` from that before the
reader branches on it.

**E3 -- The pass.** `literate_md_blank(arena, src, len, &out_base)` returning
a same-length buffer:

- Scan line by line with an in-fence / out-of-fence state machine.
- Out of fence: blank every byte except `\n` (and `\r`).
- On an opening fence, classify the info string. If it names a base, record
  it (first one wins; a conflicting later one is the E2 error) and enter
  code mode; otherwise enter prose-fence mode.
- In code mode: copy bytes verbatim.
- In prose-fence mode: blank as prose.
- The fence lines themselves are always blanked -- including the opening one,
  so the info string never reaches the reader.
- If no tagged fence exists, the output is entirely blank. That reads as an
  empty program, which is right for a `.tur.md` that is pure prose, but a
  file the user asked to *build* with zero code blocks deserves a warning.

**E4 -- Wire into the reader.** In `read_all_with_registry_from`
(`reader.c:4347`), run the envelope **before** the `READER_SWEET` branch at
`reader.c:4357`:

```
if (file->envelope == ENVELOPE_LITERATE_MD) { ... }   /* new, first */
if (eff_file->reader_type == READER_SWEET) { ... }    /* existing, on the result */
```

Because the pass is length- and newline-preserving, the envelope's
`SourceFile` copy needs **no** `orig_src`/`xform_map` -- spans are already in
original coordinates. Register the blanked buffer as the file contents so the
reader's `r.src` and the diag registry agree, exactly as the sweet branch
does; diagnostics rendering a source line will print the blanked line, so if
that turns out to matter, keep the original as `orig_src` with a null map and
teach the renderer to prefer it. Verify which reads better before deciding.
A `TUR_LITERATE_DUMP=1` env var mirroring `TUR_SWEET_DUMP` is worth having.

**E5 -- Interpreter path.** `eval.c:12103` guards incremental re-read on
`reader_type != READER_SWEET`, because a whole-buffer transform makes a
non-zero start offset meaningless. The blanking pass does **not** have that
problem -- offsets are preserved -- so literate files could in principle stay
incremental. Do not exploit that in the MVP: add
`envelope == ENVELOPE_NONE` to the same guard, get it correct, and revisit.
`eval.c:12414` and `eval.c:12417` are where the extension sets
`env->reader_type` and will need the envelope alongside.

**E6 -- The rest of the toolchain.** Every place that decides "is this a
Turmeric source file" by extension needs `.tur.md`:
`main.c:7325` (`tur fmt`), the directory walk at `main.c:7427`, the
`tur run` single-file dispatch at `main.c:11892`, project-mode build descent
in `pkg.c`, and module resolution for `(import)`. `tur fmt` on a `.tur.md`
is genuinely hard (reformat inside fences, leave prose alone) -- **skip it in
the MVP and diagnose rather than mangle**.

### Phasing

- **L0** -- `SourceEnvelope`, the `SourceFile` field, extension dispatch,
  passthrough pass. `.tur.md` is recognized and compiles as if empty.
- **L1** -- The blanking pass with ```` ```turmeric ```` fences only, plain
  base. `tur run foo.tur.md` and `tur build foo.tur.md` work. Fixtures.
  **This is the MVP and the point at which the axis has paid for itself.**
- **L2** -- Base selection from the fence tag via `lang_base_from_name`;
  `turmeric/sweet` literate files. This is the step the whole design exists
  for, so do not let L1 ship alone for long.
- **L3** -- Full Markdown fence grammar (`~~~`, longer runs, indented fences
  diagnosed), the `{example}` opt-out attribute, conflicting-tag error.
- **L4** -- Toolchain surface: `(import)` of a `.tur.md` module, project-mode
  descent, LSP, `tur check`.
- **L5** -- Docs (`syntax-guide.md`, a literate guide), notebook-spice
  coordination, `tur lang-envelopes` listing if the axis grows a second
  member.

### Open questions

- **Per-block base dialects.** Attractive (a doc could show the same function
  in both syntaxes) but it breaks the cheap composition: `sweet_preprocess`
  runs on the whole buffer, so mixed bases would need per-block preprocessing
  and offset bookkeeping the blanking trick currently avoids. Deferred, and
  the "first tag wins, conflicts error" MVP rule keeps the door open --
  relaxing an error later is compatible; tightening is not.
- **Does the envelope axis need a registry?** With one member, a `switch` is
  honest and a `LANG_ENVELOPES[]` table would be ceremony. Add the table when
  a second envelope appears, not before. If one never appears, that is a
  finding: the axis was really just "`.tur.md` support," and it should be
  said plainly rather than dressed up.
- **Ordering within a file.** MVP concatenates blocks in file order. Racket's
  `scribble/lp` supports out-of-order `chunk` weaving. Explicitly out of
  scope; if it is ever wanted it is a separate feature on top.
- **`#lang` inside a fence.** Should ```` ```turmeric ```` + a `#lang` line as
  the fence's first line be honored? It would give per-block layers. It also
  gives two ways to say the same thing. Lean: error in the MVP, revisit with
  per-block bases.
- **Empty-program warning.** A `.tur.md` with no tagged fence compiles to
  nothing. Warn on an explicit build of such a file; stay silent when it is
  merely walked.

### Risk

Low for L1-L2 and mostly bounded by the Markdown scanner. The blanking design
removes the two things that would have made it risky -- offset mapping and
composition with `sweet_preprocess`. The real exposure is scope creep across
E6: every extension check in the tree is a place `.tur.md` can be half-wired,
producing "works with `tur run`, invisible to `tur build .`" confusion. Land
E6 deliberately, one surface at a time, and say in the docs which surfaces are
live.

---

## Part 3 -- `turmeric/at-exp` base dialect (carried forward)

Retained from the superseded draft. Unchanged in substance: at-exp **is** a
base dialect, and it is the clean contrast that makes Part 2's axis argument
concrete.

### What it is

A port of Racket's `at-exp` reader: `@cmd[datum...]{body...}` reads as
`(cmd datum... "body"...)`. Spec:
https://docs.racket-lang.org/scribble/reader.html

| Input | Reads as |
|---|---|
| `@foo{blah blah}` | `(foo "blah blah")` |
| `@foo[1 2]{3 4}` | `(foo 1 2 "3 4")` |
| `@foo[1 2 3]` | `(foo 1 2 3)` |
| `@foo{bar @baz{3} blah}` | `(foo "bar " (baz "3") " blah")` |
| `@{blah blah}` | `("blah blah")` |
| `@foo` | `foo` |
| `@;{...}` / `@;line` | comment |

### Why it IS a base dialect

It takes over `@`, which is currently the deref prefix (`read_at`,
`reader.c:3412`), and it changes how `{`/`}` read inside a body (balanced
braces are literal text, not curly-infix). Those are whole-reader semantics,
not an additive `#`-dispatch and not a wrapper around another reader. A file
opts in knowing deref must be written `(deref x)`.

Contrast with literate mode, which takes over nothing: it *delimits* regions
and hands each to an unmodified base reader. That is the test for which axis
a feature belongs on -- **does it change how the base reads a byte, or does it
decide which bytes the base sees?**

### Implementation sketch

Follow the `sweet_preprocess` precedent: `READER_AT_EXP` in the `ReaderType`
enum, a row in `lang_base_from_name` (`reader.c:4532`), entries in
`reader_type_name` (`reader.c:4678`) and `reader_type_is_implemented`
(`reader.c:4690`), and an `at_exp_preprocess` producing s-expression source
plus a `SweetMap`-style offset map (this one *does* need a map -- it is not
length-preserving). A third branch alongside `READER_SWEET` in
`read_all_with_registry_from`, and the same `eval.c:12103` guard.

Because at-exp is a base, it composes with the Part 2 envelope for free: a
```` ```turmeric/at-exp ```` fence inside a `.tur.md` needs no new wiring.

### Phasing

- **A0** -- Enum, name recognition, passthrough preprocessor.
- **A1** -- `@cmd{body}` and `@cmd[datum]{body}`, literal bodies, no nesting;
  `@;` line comment. Covers templating, the common case.
- **A2** -- Nested `@` forms, `@"string"` merging, `@|expr|` escapes,
  `@;{...}` block comments.
- **A3** -- Newline and indentation rules (hardest; needs line/column
  accounting).
- **A4** -- Alternative body delimiters (`|{...}|`, `|--{...}--|`). Defer
  until there is a user need.
- **A5** -- Docs and a fixture per row of the spec table.

---

## Dependency order

Part 1 is done (v0.49.0). Parts 2 and 3 are independent of it and of each
other -- different files, different axes -- and either can land first; Part
2's fence dispatch picks Part 3 up automatically if that lands later.

None of the three needs an `--enable` experiment. Per CLAUDE.md, the
experiment gate covers elaboration/checker gates and codegen knobs; reader
selection is `#lang`/extension, and a graduation is the removal of a gate,
not the addition of one.

## Summary

| Piece | Axis | Selected by | Composes with | Needs an offset map? |
|---|---|---|---|---|
| `stringed` graduation | (axis removed, v0.49.0) | -- always on | everything | n/a |
| Literate `.tur.md` | **envelope** (new) | `.tur.md` extension | every base | **No** -- blanking preserves offsets |
| at-exp | base dialect | `#lang turmeric/at-exp` | envelopes | Yes |

## Out of scope

- Autoloading `stdlib/string.tur` (zero-ceremony owned Strings) -- a separate
  autoload-list decision with real ABI cost.
- Per-block base dialects in a `.tur.md`.
- Chunk weaving / out-of-order code assembly (`scribble/lp`).
- Rendering literate prose to documentation -- that is the notebook spice's
  job, and the shared fence grammar is what connects the two.
- `tur fmt` on `.tur.md`.
- Alternative at-exp body delimiters.

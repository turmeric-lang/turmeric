# tur/markdown -- CommonMark + GFM in Turmeric, and the string helpers it needs

> **Status:** in progress. Phases S and J are done: S1 (#1200), S2 (#1209),
> S3 (#1210), S4 (#1213) and S5 (#1216); J1-J3 (#1201, #1202, #1207) and J4
> (#1208). Phase U is done too: U1 (#1217) and U2 (#1220). Phase M is under
> way: M1, the block parser (#1229), M2, the inline parser (#1240), and M3,
> the GFM extensions and heading ids (#1246), are built (section 6.4, "M1 as
> built" to "M3 as built"). All of CommonMark and all of the GFM extension
> examples pass. M4 is built too (section 6.5, "M4 as built"): this
> repository's docs pipeline renders with `tur/markdown`, and python-markdown
> is left only on the spices site's deploy, which runs turmeric-spices's own
> generators.
> Written 2026-10-15; its open questions were settled with the author the same
> day (section 8). It started as "would
> `tools/gendocs.py` be a candidate to move to Turmeric?" A straight gendocs port
> turned out not to remove any dependency (see section 9). The Markdown renderer
> did look worth doing, and so did the string helpers that both spikes below kept
> having to write by hand.
> **Type:** stdlib (`stdlib/string.tur`, `string-slice.tur`, `cstr.tur`,
> `char.tur`, new `stdlib/markdown.tur` + `stdlib/markdown/`, new
> `stdlib/unicode.tur`), tools (`tools/genguides.py`, `tools/genspices.py`,
> `tools/requirements.txt`), guides (fence and list fixes), CI (the docs jobs).
> **Track:** runs alongside v1, not on it. tur-signal, the v1 gate, calls none of
> this. Phase S is additive and can land any time. Phases U and M are fine to
> finish after v1, but they touch no compiler code, so they can proceed in
> parallel without blocking it.
> **Related:** [`notebook-docs-examples-plan.md`](spices/notebook-docs-examples-plan.md)
> (the notebook spice's `notebook/cmark.tur`),
> [`hold/reader-envelopes-and-stringed-graduation-plan.md`](reader-envelopes-and-stringed-graduation-plan.md)
> (literate `.tur.md`; asks that its fence rules agree with the renderer),
> [`security-audit-plan.md`](security-audit-plan.md) (C-5, the `markdown` pin;
> W-4, docs as a trust boundary),
> [`../guides/strings-guide.md`](../guides/strings-guide.md) (the
> `cstr` / `String` / `StringSlice` model the helpers follow).

## 0. Summary

The docs site renders Markdown with `python-markdown` 3.11. That is the only
third-party Python package on the docs path, and it is pinned with hashes
because it feeds a released artifact (security-audit C-5). The guides are not
written for it, though. They are written for GitHub, which means CommonMark plus
the GFM extensions. `tools/genguides.py` carries about 230 lines of workarounds
for the gap, and the live site still shows 54 lists as run-on paragraphs.

This plan:

1. **Phase S** adds string helpers to stdlib: byte tests against `Char`
   literals, search that returns `Option`, split/join/lines, trims, and
   builder-based concatenation. Both spikes, and `stdlib/re.tur`, show these
   are what pure-Turmeric text processing is missing today.
2. **Phase U** adds the Unicode and HTML-entity tables CommonMark needs.
3. **Phase M** writes `tur/markdown`: a pure-Turmeric CommonMark 0.31.2 + GFM
   parser and HTML renderer, tested against the spec's examples. It then moves
   the docs pipeline onto it, deletes the workarounds and the `markdown` pin,
   and offers it to the other consumers (notebook spice, literate `.tur.md`,
   terminal rendering).
4. **Phase J** makes `tur run` the only task runner the project needs, which
   removes the `just` binary as a dev dependency (section 10). It is
   independent of S, U and M.

A few fixes in section 2 don't depend on any of this and can land now.

## 1. Evidence

### 1.1 What ships today vs. CommonMark + GFM

Method: every `docs/guides/*.md` page rendered two ways, with the block structure
compared by tag counts. One side is what ships (genguides' preprocessing, then
`python-markdown` with `fenced_code`, `tables`, `toc`). The other is
`cmarkgfm` 2025.10.22 (GitHub's cmark-gfm) on the raw source.

| Finding | Number |
|---|---|
| Lists the live site renders as a run-on paragraph with literal `- ` bullets (a list straight after a paragraph line, which python-markdown won't start) | **54 lists on 23 guide pages** (e.g. "Advantages" in `cloudflare-deployment-guide.md`; 10 in `snake-game-tutorial.md`) |
| Guides with a `turmeric` / `sweet-exp` block containing a column-0 ```` ``` ```` from inline C (genguides widens these fences before rendering) | 31 |
| ...of which CommonMark (and GitHub, today) closes the block early and renders broken | **16** (28 blocks) |
| Pages whose block structure still differs once those fences are widened in source | **3**, and only in how adjacent block quotes split (`image-dumps-guide.md`, `repl-tutorial.md`, `web-continuations-tutorial.md`) |
| Raw HTML a CommonMark parser finds | **3 in the guides, none intentional**: `ptr<void>` inside two of the broken fences above, and `<source>` after a code span that `` \` `` ended early (`developing-spices-guide.md:1283`, hidden on the live site today). **0** in the 51 spice READMEs |
| Heading ids that would change under GitHub's slug rule instead of python-markdown's (mostly the ` -- ` in titles: `a -- b` is `a-b` vs `a----b`) | **403 of 3,050** |
| Links in the guides that point at a `#fragment` | 214 |

So CommonMark + GFM is the right target, with one exception: **keep
python-markdown's heading-id rule** so existing anchors and inbound links keep
working.

The workarounds in `tools/genguides.py` that exist only because of
python-markdown: `dedent_indented_fences` (fences inside list items),
`unquote_blockquote_fences` (fences inside block quotes), `widen_nested_fences`,
the info-string marker strip in `build_guide_body`, `render_task_lists`, the
`LAZY_OL` tweak, and `unrendered_fences` (a detector for fences that never
rendered).

### 1.2 Spikes

Two throwaway programs, compiled with the Debug `tur`. Neither is committed.

- **Docstring scanner** (~100 lines): walks `stdlib/`, pulls `;;;` summaries
  using gendocs' attachment rule. Compiles in 0.5s, runs in 16ms, and agrees with
  `gendocs.py` on 2,332 of the 2,389 definitions both found. The other 57 are
  module docstrings and r7rs `define` forms the spike skipped.
- **Markdown block subset** (~230 lines): a typed tree (`defdata Block` with a
  recursive `(Vec Block)`), CommonMark's rules for fenced code (length, `~`
  vs backtick, info-string restriction, closing rules), ATX headings, block
  quotes as a recursive container, paragraphs, and an HTML renderer with escaping
  through `StringBuilder`. It processes all 160 guides in **48ms**. Its counts of
  fenced code blocks, ATX headings and block quotes match cmark-gfm on
  **160 of 160** pages.

The language is ready for this. Most of the friction was in the string surface,
which is why Phase S comes first. The rest is listed in section 7.

### 1.3 What already exists

- **`turmeric-spices/spices/notebook/src/notebook/cmark.tur`**: a "block-level
  CommonMark subset parser", 2,084 lines, of which **1,345 are inline C**. Nodes
  are `:int` handles. It has no link reference definitions, no HTML blocks, and
  only a few hard-coded entities. As inline C, it can't run under `--interpret`
  or in the web REPL. Its renderers (`render-md.tur`, `render-html.tur`) bring
  the total to 4,702 lines.
- **Strings:** `cstr` has five primitives (`cstr-len`, `cstr-nth`, `cstr-sub`,
  `cstr-eq?`, `cstr-free`). `String` has `starts-with?` / `ends-with?` /
  `contains?`, `concat`, `substring`, `trim`, case mapping and `StringBuilder`.
  `StringSlice` has zero-copy views. There's no search that returns a position,
  no split, join or lines, no one-sided trim, and no literal replace. `=` doesn't
  accept `Char`, so a byte can't be compared to `#\,` without
  `(char=? (int->char b) #\,)`.
- **Consequence:** `stdlib/re.tur`, the one stdlib text processor written in
  plain Turmeric, compares raw byte numbers (`44` for `,`, `125` for `}`, `58`
  for `:`). `json.tur` (31 inline-C blocks), `csv.tur` (14), `parsec.tur` (23)
  and `path.tur` (7) fall back to C.

## 2. Quick wins, independent of the library

Each of these is small, fixes something live today, and stays correct after the
switch. **All four are in
[#1197](https://github.com/turmeric-lang/turmeric/pull/1197)** (merged 2026-10-15), which
was cut from `main` and doesn't depend on this plan. What it actually does,
where that differs from the first draft of this section:

1. **Blank line before the run-on lists:** 52 of the 54. CommonMark renders them
   the same with or without it (cmark-gfm's output is unchanged on every page),
   and python-markdown needs it. Two are left on purpose: `macros-guide.md` and
   `value-representations-guide.md` continue a list item with less than
   python-markdown's four spaces of indent, and a blank line there would turn
   GitHub's tight list loose. M4's renderer fixes them without touching the
   source. Three more lines that only *look* like ordered lists (`3) per element
   class`, `404) -- send the HTML`) are wrapped prose and stay as they are.
2. **Widen the outer fence** of the **28 blocks on 16 guides** that a CommonMark
   parser closes early, not all 31 guides that `widen_nested_fences` touches.
   The other 15 only nest runs CommonMark already reads as content. Most of the
   28 are sweet-exp blocks whose inline C closes on a line indented two spaces,
   which is still a closing fence. The site's HTML is byte-identical on every
   page but one, and `widen_nested_fences` is now a no-op ahead of its removal
   in M4. `tools/check-guide-pairs.py` learned longer fences and now **fails on
   any three-backtick turmeric / sweet-exp / lisp block a CommonMark parser
   would close early**. The spice READMEs have none.
   - Along the way: `style-guide.md`'s Inline-C Style example had its Good pair
     and Bad example spliced into one another. It's now a parse-checked pair
     followed by a `no-check` Bad block.
3. **Pin the unpinned install.** `.github/workflows/release-cut.yml`'s
   deploy-spices job ran `python3 -m pip install markdown` with no version or
   hash, which contradicts C-5. Its workspace is the `turmeric-spices` checkout,
   which has no requirements file, so pointing at `tools/requirements.txt` alone
   would have broken it. The job now sparse-checks-out just that file from this
   repo, into `.turmeric-pins/` outside the deployed `web/dist`, and installs
   with `--require-hashes`.
4. **Fix the code span at `developing-spices-guide.md:1283`.** It wrote
   `` \` `` inside a code span, but backslash escapes don't work in code spans,
   so the span ended early and the browser hid `<source>` as an unknown tag.
   It's now a double-backtick span. The other two accidental tags went away with
   item 2.

## 3. Phase overview

| Phase | Theme | Breaking? | Track | Size (approx.) |
|---|---|---|---|---|
| **S1** | Byte tests with `Char` | No | Any time | ~150 lines + fixtures |
| **S2** | Search: positions as `Option`, run lengths, predicates | No | Any time | ~300 lines |
| **S3** | Split / join / lines, owned and zero-copy | No | Any time | ~250 lines |
| **S4** | One-sided trims, literal replace, repeat, pad, parse-int | No | Any time | ~200 lines |
| **S5** | Builder pushes and a concatenation macro | No | Any time | ~150 lines |
| **U1** | Unicode classes, case folding, UTF-8 decoding | No | Before M2 | generator change + ~150 lines |
| **U2** | HTML entity table | No | Before M2 | generator + table |
| **M1** | Block parser | No | Parallel / post-v1 | ~1.5k lines |
| **M2** | Inline parser | No | Parallel / post-v1 | ~1.5k lines |
| **M3** | HTML renderer, GFM extensions, heading ids | No | Parallel / post-v1 | ~0.8k lines |
| **M4** | Docs pipeline cut-over; drop the `markdown` pin | No (site output changes as listed) | After M3 | tool + CI |
| **M5** | Notebook spice, literate `.tur.md`, terminal rendering | Spice API (notebook) | After M4 | per consumer |
| **J1** | Justfile recipes stop calling `just` | No | Any time | ~10 lines |
| **J2** | Guard: the Justfile stays inside `tur run`'s subset | No | With J1 | one check |
| **J3** | Docs, messages and skills say `tur run`, not `just` | No | After J1 | ~100 mentions |
| **J4** | `release` and `tsan` build in their own trees; no bootstrap script | No | After J1 | ~20 lines + guide fixes |

Each S phase is one PR. U is two, U1 and U2 (split when it was built: the
two halves share no code). M1 to M3 are one PR each, with the spec
ratchet (section 5.1) rising each time. M4 is its own PR. M5 is one PR per
consumer, and the notebook one lands in `turmeric-spices`. J1 and J2 are one PR;
J3 is one more (mechanical, docs only); J4 is its own PR.

## 4. Phase S -- string helpers

### 4.1 Design rules

- **Not found is `(None)`, never `-1`.** CLAUDE.md, "No Lazy `:int` Stand-Ins".
  Positions are `(Option int)`. Lengths and counts stay `:int`.
- **Borrow inputs, allocate only where the name says so.** Search and predicates
  never allocate. Anything returning a `String` returns an owned one (rc 1), and
  anything returning a slice returns an owned slice, per the strings guide's
  short rules.
- **Write them in Turmeric first, over `string/byte-at`, `string/len` and
  friends.** Then they behave the same under `--interpret` and in the web REPL
  with no native-parity work. Move a helper to `src/runtime/tur_string.c` (plus
  its `src/turi/string_native.c` native and its two `src/turi/native_caps.c`
  rows) only when a benchmark shows it matters.
- **Coverage per type.** `String` and `StringSlice` get the full set. `cstr`
  gets only the non-allocating tests and search. `cstr` is what literals,
  `fs/read-lines` and `fs/read-text` hand you, so scanning it shouldn't require
  a copy first.
- **Naming** follows the existing prefixes: `string/`, `slice/`, `cstr-`,
  `builder/`.
- **Every function** gets a full `;;;` docstring, an ASCII-only fixture under
  `tests/fixtures/`, and coverage in `tests/run-turi.sh`. Allocating helpers also
  get a `requires.leak-check` marker and `tests/run-leak-check.sh` coverage,
  since ownership is where these go wrong.

### 4.2 S1 -- byte tests with `Char`

The most common shape in `re.tur` and in both spikes is "is the byte at `i` this
character, and is `i` in range":

```turmeric
;; today (stdlib/re.tur:231)
(if (if (< p1 len) (= (cstr-nth input p1) 44) false) ...)

;; with S1
(if (cstr-byte-is? input p1 #\,) ...)
```

- `string/byte-is? [s : String i : int c : Char] : bool`, plus `slice/byte-is?`
  and `cstr-byte-is?`. They return false when `i` is out of range.
- `string/char-at [s : String i : int] : (Option Char)`, plus `slice/char-at`
  and `cstr-char-at`. These return the byte as a `Char`, for ASCII-oriented
  scanning. UTF-8 decoding is Phase U's job.
- **`=` on `Char` stays as it is** (decided, section 8). `(= #\, #\,)` is a
  `TUR-E0006`, because `=`'s overloads are keyed by primitive type kind and
  `Char` is a `defopaque` over `:int`. `=` rejects `String` for the same reason.
  `(eq? c #\,)` already works through `Eq [Char]`, which is the same idiom the
  strings guide prescribes for `cstr`. The fixtures and docstrings for S1 use
  `eq?`, and `byte-is?` covers the common case without a comparison at all.

### 4.3 S2 -- search

Non-allocating:

- `string/index-of [s : String needle : String] : (Option int)`
- `string/index-of-from [s : String needle : String start : int] : (Option int)`
- `string/last-index-of [s : String needle : String] : (Option int)`
- `string/index-of-char [s : String c : Char] : (Option int)`, plus
  `string/index-of-char-from`
- `string/starts-with-at? [s : String prefix : String at : int] : bool`, a
  prefix test at an offset. Both spikes hand-wrote it.
- `string/span-while [s : String start : int pred : (fn [Char] bool)] : int`:
  the first index at or after `start` where `pred` fails, or `(string/len s)`.
  The `char.tur` predicates plug in directly. It covers the spikes' hand-written
  `skip-ws`, `indent-of` and `name-end`:

  ```turmeric
  (string/span-while line 0 space?)          ;; indentation width
  (string/span-while line start alnum?)      ;; end of a name token
  ```

- `string/run-length [s : String start : int c : Char] : int`: how many `c`
  in a row start at `start`. Covers fence runs and ATX `#` counts.

Every one of these also gets a `slice/` twin. `cstr` gets `cstr-starts-with?`,
`cstr-ends-with?`, `cstr-index-of`, `cstr-index-of-char` and
`cstr-span-while`.

### 4.4 S3 -- split, join, lines

Owned results:

- `string/split [s : String sep : String] : (Vec String)`
- `string/split-char [s : String c : Char] : (Vec String)`
- `string/lines [s : String] : (Vec String)`, splitting on LF, CRLF and CR, the
  three line endings CommonMark recognises.
- `string/join [parts : (Vec String) sep : String] : String`, which borrows its
  parts.
- `string/release-all [v : (Vec String)] : void`, which releases every element
  and then the vector. Without it, every `split` caller has to write the loop.

Zero-copy, which is what the Markdown parser wants for its input lines:

- `string/line-slices [s : String] : (Vec StringSlice)`
- `slice/split-char [sl : StringSlice c : Char] : (Vec StringSlice)`
- `slice/release-all [v : (Vec StringSlice)] : void`

Files:

- `fs/read-string [path : cstr] : (Result String IoError)`. `fs/read-text`
  returns a `cstr` today, and `markdown/parse` takes a `String`.

Written over `vec-push!`, these inherit its region escape note (CLAUDE.md,
"Region Store Hooks"). If any of them moves to C, it has to call
`TUR_REGION_NOTE` before the store.

### 4.5 S4 -- trims and transforms

- `string/trim-start`, `string/trim-end` (`string/trim` already exists), and the
  zero-copy `slice/trim`, `slice/trim-start`, `slice/trim-end`, which return
  sub-slices.
- `string/replace-all [s : String from : String to : String] : String`, a
  literal replace. `re/replace-all` is regex-based and works on `cstr`.
- `string/repeat [s : String n : int] : String`
- `string/pad-start [s : String width : int c : Char] : String`, plus
  `string/pad-end`
- `string/parse-int [s : String] : (Option int)`, plus `slice/parse-int`.
  CommonMark needs it for ordered-list start numbers, and the existing
  `str->int-checked` takes a `cstr` and returns `(Either int int)`.

As built: an empty `from` matches nothing, so `replace-all` returns a copy (the
rule `string/split` has for an empty separator). `parse-int` is strict: an
optional sign and then digits, the whole string, with `(none)` on overflow. It
counts down, so the most negative int parses. S5 replaced the byte loops
behind `replace-all` and the pads with `tur_sb_push_range` and
`builder/push-repeat!`.

### 4.6 S5 -- building

The spike's output line was eight nested two-argument `str-concat` calls, each
allocating, with every intermediate leaked:

```turmeric
;; today
(println (str-concat path (str-concat "\t" (str-concat (int->str pre) ...))))
```

- `builder/push-slice!`, `builder/push-int!` (writes decimal digits, no
  temporary), `builder/push-repeat! [b : StringBuilder c : Char n : int]`.
- **`builder/push!` through a one-method typeclass**, with instances for `cstr`,
  `String`, `StringSlice`, `Char`, `int` and `bool`. Floats are left out until
  float formatting is decided separately (and probed with `7.1`, per CLAUDE.md).
- **A concatenation macro, `string/of`** (name decided, section 8), that expands to one
  builder, a `builder/push!` per argument, and `builder/finish`. It borrows its
  arguments and creates no temporaries, so the only thing to release is the one
  `String` it returns:

  ```turmeric
  (string/of path "\t" pre "\t" headings)   ;; => owned String
  ```

As built: `builder/push-int!`, `builder/push-repeat!` and `builder/push-slice!`
are C primitives (`tur_sb_push_int`, `tur_sb_push_repeat`, `tur_sb_push_slice`
in `src/runtime/tur_string.c`) with interpreter natives, and so is the internal
`tur_sb_push_range` that S4's `replace-all` now copies through. The class is
`BuilderPush`. Its method, `builder/-push-into! [x b]`, takes the value first,
because a class method dispatches on its first argument; `builder/push! [b x]`
is the constrained `defn` over it. `string/of` mints its builder with `gensym`,
so it captures none of the caller's names. It borrows its arguments, so a call
that returns a fresh `String`, such as `(int->string n)`, still leaks as an
argument; the `int` instance is what makes that unnecessary.

## 5. Phase U -- Unicode and entity tables

CommonMark needs things the stdlib can't do today outside `#lang r7rs`:

- **Unicode whitespace** (category Zs, plus tab, LF, FF, CR) and **Unicode
  punctuation** (categories P and S, as of spec 0.31) for the left- and
  right-flanking rules on emphasis delimiters.
- **Full Unicode case folding** for matching link reference labels.
- **UTF-8 decoding at a byte index**, to classify the code point on either side
  of a delimiter run.
- **The HTML5 named-entity table**, about 2,100 names with their semicolons.

Plan:

- Generalise `tools/gen-r7rs-unicode.py`. Its UCD input is already pinned through
  `tools/fetch-ucd.sh`. It would also emit a general `stdlib/unicode.tur`:
  `unicode/space? [c : Char] : bool`, `unicode/punct? [c : Char] : bool`,
  `unicode/fold-case [s : String] : String`, and
  `utf8/decode-at [s : String i : int] : (Option (Pair Char int))` (the code
  point, and the next byte index). The tables follow the existing r7rs pattern
  (decided, section 8): one static C table emitted twice, as inline C for
  compiled code and as an interpreter native in `src/turi/`, with a sync check
  like `tests/check-r7rs-unicode-sync.sh` keeping the two copies identical. The
  r7rs prelude can then read the same tables instead of carrying its own.
- A new `tools/gen-html-entities.py` turns WHATWG's `entities.json`, pinned by
  digest, into `stdlib/markdown/entities.tur`, which is generated and committed
  like the r7rs tables.

**U1 as built.** `stdlib/unicode.tur` is hand-written Turmeric over the
generated tables rather than generated itself. It loads
`stdlib/r7rs/unicode.tur`, so there is one copy of the tables, not two:
`tools/gen-r7rs-unicode.py` gained two classes in `r7rs_uc_prop` (op 5,
CommonMark whitespace; op 6, CommonMark punctuation), built from the blank,
punctuation and symbol runs it already had. That needed no new native,
because the interpreter's `r7rs-uc-prop__` passes the op through.
`unicode/fold-case` is the existing full fold (`r7rs-uc-string__` op 2),
adopted into a `String`. UTF-8 decoding is plain Turmeric over
`string/byte-at`, with U+FFFD for each maximal invalid subpart, and it has a
mirror the plan did not list, `utf8/decode-before`, because the flanking
rules look left as well as right. The r7rs prelude is unchanged.

**U2 as built.**
- **Source.** `html.spec.whatwg.org` is not reachable through the build proxy,
  which allows GitHub. WHATWG has frozen the entity list, and CPython's
  `html.entities.html5` is generated from the same `entities.json`. So
  `tools/gen-html-entities.py` reads CPython's copy by default, or `--json
  entities.json`, and checks the extracted table against a pinned SHA-256
  digest and count. A source that differs fails without writing anything, and
  both sources give byte-identical output.
- **What is kept.** The 2125 names that end in `;`, stored without it: CommonMark
  reads no entity without its semicolon.
- **The r7rs pattern.** One C table plus a binary search is written twice, into
  `stdlib/markdown/entities.tur` (inline C) and `src/turi/html_entities.inc`
  (interpreter natives). `tests/check-html-entities-sync.sh` (ctest
  `tur_html_entities_sync`) keeps the two copies identical.
- **The API.** The public function is `markdown/entity-lookup [name : String] :
  (Option String)`. It builds its result with `utf8/push-char!`, a UTF-8 encoder
  this phase adds to `stdlib/unicode.tur`.
- **Interpreter coverage.** `tests/run-turi.sh` lists `entities.tur` among the
  loaded modules whose inline C is all native-backed, so the interpreter runs
  its fixture too.

## 6. Phase M -- `tur/markdown`

### 6.1 Target and conformance

- **CommonMark 0.31.2 plus the GFM extensions**: tables, strikethrough,
  extended autolinks, task list items. **Not** bug-compatibility with
  python-markdown, apart from heading ids (6.3).
- **Spec examples as fixtures.** CommonMark 0.31.2 has 652 examples, and the GFM
  spec adds 24 extension examples. A runner, `tests/run-markdown-spec.sh` (ctest
  `tur_markdown_spec`), renders each example and compares it with the expected
  HTML after the spec's own normalisation (attribute order, whitespace between
  tags). It runs compiled and under `--interpret`. A committed ratchet file lists
  the passing example numbers, and that list may only grow. M1 to M3 each raise
  it, and M3 ends at all examples passing.
- **Where the examples live** (decided, section 8):
  [`turmeric-lang/commonmark-spec`](https://github.com/turmeric-lang/commonmark-spec),
  which keeps the share-alike text out of this repository. It was created and
  populated on 2026-10-15; the first commit is `a029e939`. It holds:
  - `commonmark/0.31.2/spec.json`: all 652 examples, byte-identical to
    upstream's `test/spec_tests.py --dump-tests`.
  - `gfm/0.29-gfm/extensions.json`: the 24 GFM extension examples (8 table,
    2 strikethrough, 11 autolink, 1 tagfilter, 2 tasklist), each tagged with
    its extension. GitHub's renderer passes all 24 after normalisation.
  - `SOURCES.json`, the upstream commit and SHA-256 behind each file, and
    `tools/extract.py`, which regenerates or `--check`s them from those pinned
    commits.
  - The CC-BY-SA 4.0 legal code, the attribution, and a list of the changes
    made.

  It doesn't include upstream's `normalize.py` (BSD-2-Clause), so M1's runner
  brings its own normalisation. It's consumed the way `turmeric-spices`
  already is:
  - Locally, clone it next to this checkout (`../commonmark-spec/`). When
    it's absent, the runner PASS-skips with a message saying how to get it,
    the same as `requires.spices`.
  - In CI, the jobs that run the suite check it out with `actions/checkout`
    (as `release-cut.yml` already does for `turmeric-spices`) at a commit SHA
    pinned in this repo, in `tests/markdown-spec/PINNED`. Nothing reaches the
    network at configure or test time.
  - The ratchet file, which lists the passing example numbers per spec version,
    lives in this repo, next to `PINNED`.

### 6.2 Module layout and API

- `stdlib/markdown.tur` (module `tur/markdown`, the public surface), with
  internals in `stdlib/markdown/{block,inline,html,entities,slug}.tur`. It lives
  in stdlib, not a spice (decided, section 8). The module itself has no inline
  C. Its only native dependency is `stdlib/unicode.tur`, whose tables ship with
  interpreter natives. So it runs compiled, under `--interpret` and in the web
  REPL, which the notebook's parser can't.
- **Real types, no `:int` handles.** `defdata Block` covers paragraph, heading,
  code block (info, body, fenced?), block quote, list (ordered?, start, tight?,
  items), HTML block, thematic break and table. `defdata Inline` covers text,
  code, emphasis, strong, strikethrough, link (destination, title, children),
  image, raw HTML, soft break and hard break. Every block carries a source span
  (start and end line), because the tools that use this need line numbers for
  their messages.
- **Options are structs**, per the arity guide:
  `(defstruct MdOpts [tables : bool strikethrough : bool autolinks : bool
  tasklists : bool math : bool])` and `(defstruct HtmlOpts [raw-html : RawHtml
  heading-ids : SlugMode])`. `math` (`$...$` / `$$...$$`, off by default) exists
  for the notebook.
- **Raw HTML** (decided, section 8): `RawHtml` is an ADT with three cases.
  `Pass` passes raw HTML through. `TagFilter`, the **default**, is GitHub's
  behaviour: raw HTML passes through, except that the opening `<` of nine tags
  is escaped (`title`, `textarea`, `style`, `xmp`, `iframe`, `noembed`,
  `noframes`, `script`, `plaintext`). `Escape` renders all raw HTML as text.
  Pass-through keeps one hazard: Turmeric type syntax that slips outside code
  (`ptr<void>`, `<name>`) parses as a tag, and the browser hides it. Since no
  guide uses raw HTML on purpose, M4's renderer tool reports every raw-HTML node
  it emits, and the docs build fails on any in `docs/guides/` that the page
  doesn't explicitly allow.
- **Functions:**
  - `markdown/parse [src : String opts : MdOpts] : Doc`
  - `markdown/render-html [d : Doc opts : HtmlOpts] : String`
  - `markdown/headings [d : Doc] : (Vec HeadingInfo)`: level, text and id, for
    tables of contents
  - `markdown/code-blocks [d : Doc] : (Vec CodeBlockInfo)`: info string, body
    and span, for fence consumers (section 6.6)
  - `markdown/escape-html`. Not `escape`, which is a special form (section 7).
  - `Doc` owns its tree; `markdown/doc-free` releases it.

### 6.3 Heading ids

The default reproduces python-markdown's `toc` rule. Take the heading's text,
apply NFKD and drop non-ASCII, remove everything but word characters, spaces and
`-`, lowercase it, and collapse each run of spaces and `-` to a single `-`.
Duplicate ids get `_1`, `_2`, and so on. With this rule, the 214 fragment links
and every inbound link to the site keep resolving. GitHub's rule is available as
a `SlugMode`, for READMEs that are read on GitHub.

### 6.4 M1 to M3

- **M1, block parser:** line splitting, tabs (the spec's partial tab
  expansion), container matching (block quotes, list items and their content
  columns), lazy continuation lines, all the leaf blocks (thematic break, ATX
  and setext headings, indented and fenced code, the seven kinds of HTML block,
  link reference definitions, paragraphs), and tight vs. loose lists.
- **M2, inline parser:** backslash escapes, entity and numeric references, code
  spans, the delimiter-run algorithm for emphasis, links and images (inline;
  full, collapsed and shortcut references; nested brackets), autolinks, raw
  HTML, hard and soft breaks.
- **M3, rendering:** the HTML renderer (including the spec's URL escaping),
  the GFM extensions, heading ids, and `markdown/headings`.
- **Size:** about 3 to 5k lines in total. commonmark.js is roughly 3k lines of
  JavaScript, and the notebook's parser plus renderers come to 4.7k.
- **Performance target:** all guides plus the spice READMEs parse and render in
  under a second compiled. The block-subset spike took 48ms.

**M1 as built.**
- **The ratchet.** Every example in the block sections passes on the compiled
  engine, the JIT and the interpreter, except those whose expected HTML needs
  inline syntax: emphasis, code spans, links, inline raw HTML, escapes and
  entities in text. Link reference definitions are parsed and taken out of
  their paragraphs; they show up only once M2 resolves links. That is 347 of
  the 652 CommonMark examples and 2 of the 24 GFM ones, all byte-identical to
  the spec's HTML. The 676 run leak-clean under LeakSanitizer.
- **The tree is an arena, not a `defdata Block` of `String`s** (the deviation
  from 6.2). Blocks are `MdNode` values in a `(Vec MdNode)`, linked by index,
  and their text is a `(Vec String)` pool that `markdown/doc-free` releases.
  Two compiler leaks ruled out the planned shape:
  - a struct or sum with a `String` field is a heap box nothing frees
    (`docs/archive/opaque-field-struct-box-never-freed.md`, fixed since);
  - an `Option` of a struct passed down a recursive walk leaks its box
    (`fresh-option-arg-to-self-call-leaks.md`).

  Nothing in a node owns memory, so the parser builds and frees the tree with
  no ownership bookkeeping.
- **The public view is typed.**
  - `MdBlockId` is a one-int struct, not an `:int`.
  - `markdown/block` returns `MdBlock`, a by-value sum carrying the kind's
    facts: `(MdHeading level)`, `(MdList ordered start tight)`,
    `(MdCodeBlock fenced)`.
  - `markdown/children` returns a `(Vec MdBlockId)` rather than first-child /
    next-sibling Options, for the second leak above.
  - Text comes out through `markdown/block-text`, `markdown/code-info` and
    `markdown/block-lines`, each a fresh `String` or a pair of line numbers.
- **Names.** The types are `MdDoc`, `MdOpts` and `MdHtmlOpts`, prefixed because
  stdlib files are `load`ed into one namespace. `MdHtmlOpts` has `raw-html`
  only; M3 adds the heading-id mode. `MdRawTagFilter`, the default, is
  implemented for HTML blocks, and M2 applies it to inline raw HTML.
- **Inline content until M2.** A paragraph's or heading's text is written
  escaped, with soft breaks, and hard breaks after two trailing spaces.
- **Where the code is.**
  - `stdlib/markdown/block.tur` is the port of commonmark.js's `blocks.js`
    (about 1,300 lines).
  - `stdlib/markdown/html.tur` is the renderer.
  - `stdlib/markdown/inline.tur` holds what link reference definitions are
    made of: unescaping, the label / destination / title scanners, label
    normalisation, and the HTML tag scanner behind HTML block kind 7. M2
    builds the inline parser on the same scanners, and renames this file
    `scan.tur`.
- **The runner.** `tests/markdown-spec/run.py` generates one program holding
  every example, so a crash costs only its own example, and runs it on each
  engine. It normalises HTML with its own rules, written from the spec's
  description since upstream's `normalize.py` is not in the spec repository.
  `tests/run-markdown-spec.sh` wraps it, and is ctest `tur_markdown_spec` in
  the `aux` part. CI fetches the spec repository at `tests/markdown-spec/PINNED`.
- **Compiler friction found and filed:**
  - `forward-call-with-rc-argument-rejected.md`: a call to a later `defn`
    taking an `rc<T>` is a type error, so `block.tur` defines callees before
    callers, and the renderer recurses through one function;
  - `vec-push-of-with-expression-emits-invalid-c.md`, worked around with a
    `let`;
  - the two leaks above.

**M2 as built.**
- **Conformance.** All 652 CommonMark examples pass on the compiled engine,
  the JIT and the interpreter, byte-identical to the spec's HTML. So do 3 of
  the 24 GFM ones: the tag-filter example joins now that the filter applies to
  inline HTML, and the rest are M3's extensions. The 676 run leak-clean under
  LeakSanitizer.
- **Checked against commonmark.js too.** The spec's examples show only the
  cases someone wrote down, so the HTML was also compared with commonmark.js
  0.31.2's:
  - over every Markdown file in this repository (2,218 files, 30 MB), which
    came out identical;
  - over 180,000 random documents built from Markdown's punctuation, tags,
    entities, percent escapes, and non-ASCII letters and spaces. 20,000 of
    them were also run under ASan, UBSan and LeakSanitizer, which found
    nothing.

  Every difference is a place where commonmark.js matches JavaScript's `\s`
  or calls `trim()`, both of which take in U+00A0, and the spec says spaces
  and tabs: the trimmed ends of a paragraph or heading, the end of an HTML
  block kind 7 start line, the space before a closing tag's `>`, and a fence's
  first info word. One difference is deliberate: raw HTML inside an image's
  alt text is escaped, as cmark does, where commonmark.js writes it into the
  attribute unescaped.

  The comparison is ctest `tur_markdown_diff` (`tests/run-markdown-diff.sh`):
  - Every tracked Markdown file is rendered, plus the spice READMEs and 30,000
    random documents from fixed seeds.
  - commonmark.js and its dependencies are pinned by
    `tests/markdown-spec/oracle/package-lock.json`.
  - CI's `aux` part installs node with the web job's `setup-node` pin, then
    runs `npm ci`. Without them the test skips.
  - Node was already a CI dependency through the web job. What this adds is
    four small npm packages, for tests only.
- **The inline tree is an arena too.** Inlines are `MdInl` values in a
  `(Vec MdInl)` on the `MdDoc`, linked by index, and `MdNode.inl` is a
  block's root.
  - A text inline is a byte range of a pool `String`, usually the block's
    trimmed text, so taking delimiters off a run moves an end index.
  - A code span, a character reference, and a link's destination and title
    each get a pool `String` of their own.
  - After the scan, adjacent text inlines over consecutive bytes are joined,
    so an unmatched `*` leaves `a*b` one text inline. commonmark.js leaves it
    as three.
- **The parser.** `stdlib/markdown/inline.tur` (about 730 lines) is the port
  of commonmark.js's `inlines.js`: one left-to-right scan with a delimiter
  stack and a bracket stack, both index-linked `Vec`s, then
  `processEmphasis` with its fourteen openers-bottom slots.
- **The scanners.** M1's `inline.tur` is now `scan.tur`, holding the scanners
  both parsers share. It gained:
  - the inline HTML scanners: open and closing tags, comments, processing
    instructions, CDATA and declarations;
  - the URI and email autolink scanners;
  - URL normalisation, as commonmark.js's mdurl does it: an existing `%XX`
    stays, and any byte outside the URL-safe set is percent-encoded. Link
    reference definitions' destinations get it too.
- **The public view.**
  - `MdInlineId` is a one-int struct.
  - `markdown/inline` returns an `MdInline`: `MdText`, `MdCode`, `MdEmph`,
    `MdStrong`, `MdLink`, `MdImage`, `MdHtmlInline`, `MdSoftBreak` or
    `MdHardBreak`. It is `MdHtmlInline` because `MdRawHtml` is the renderer's
    option type. Strikethrough joins with M3.
  - `markdown/inlines` and `markdown/inline-children` return a
    `(Vec MdInlineId)`, as `markdown/children` does for blocks.
  - `markdown/inline-text`, `markdown/link-dest` and `markdown/link-title`
    each return a fresh `String` or `(none)`.
- **Speed.** M1's 3.1 MB corpus parses and renders in 0.17 s compiled, inlines
  included; blocks alone took 0.11 s. This repository's 30 MB of Markdown
  takes 2.0 s.
- **No new compiler friction.** M1's two workarounds held: callees are
  defined before their callers, and the inline renderer recurses through one
  function.

**M3 as built.**
- **Conformance.** All 676 examples, the 652 CommonMark ones and the 24 GFM
  ones, pass on the compiled engine, the JIT and the interpreter. All but two
  are byte-identical to the spec's HTML. The two task-list examples differ
  only in attribute order, which the spec's normalisation ignores; ours is
  cmark-gfm's output byte for byte.
- **GFM is cmark-gfm 0.29.0.gfm.13's, quirks included.** That is the renderer
  GitHub runs, and where the GFM spec's prose and cmark-gfm differ, cmark-gfm
  is what readers see. The quirks that show:
  - A table's header is the last line of a paragraph. The lines above it stay
    a paragraph, and cmark-gfm never reads link reference definitions out of
    it.
  - With strikethrough on, `~` is a "skip character". A `*` or `_` run reads
    past the tildes beside it when it decides whether it can open or close,
    so `_a_~b` is not emphasis.
  - Task boxes follow cmark-gfm's generated scanner, which differs from the
    `.re` source it was generated from:
    - `[X]` counts;
    - an ordered marker is digits and then any one character;
    - the box is looked for from the start of the line, so an item inside a
      block quote gets none;
    - the item is checked when `[x]` or `[X]` appears anywhere on its line.
  - Email autolinks are found in a pass after the inline parse, over adjacent
    text joined first. cmark-gfm's "inside a link" is one flag, which a nested
    autolink's exit clears.
- **Checked against cmark-gfm.** `tur_markdown_diff` has a second half. It
  builds cmark-gfm from a checkout at the commit
  `tests/markdown-spec/oracle/cmark-gfm.pin` names, with the bundle driver
  `oracle/gfmrender.c`. CI fetches the checkout next to the repository, as it
  does commonmark-spec.
  - It renders this repository's Markdown, and 30,000 random GFM documents of
    table rows, tildes, URLs, email addresses and task boxes, both ways.
  - cmark-gfm's core is CommonMark 0.29, so a document counts only when the
    two agree on it with the extensions off, both as written and with its
    tildes removed. That leaves out about 0.2%.
  - 0 differ. Before the comparison became a test, it ran over 450,000 more
    random documents, also with 0 differences. Every difference it found
    along the way was one of the quirks above, or a CommonMark 0.29 difference
    the filter now leaves out.
  - 20,000 GFM documents also ran under ASan, UBSan and LeakSanitizer, which
    found nothing.
- **Heading ids (section 6.3).** `MdHtmlOpts` has a second field,
  `heading-ids : MdSlugMode`, which is `(MdNoIds)`, `(MdSlugPython)` or
  `(MdSlugGitHub)`.
  - `markdown/default-html-opts` writes no ids. Section 6.3's "default" is
    read as the default rule, the one the docs use. The default output stays
    the CommonMark spec's and cmark-gfm's, which have none, so the spec runner
    and the cmark-gfm comparison compare like for like. M4 asks for
    `(MdSlugPython)`.
  - python-markdown's rule needs NFKD, so `stdlib/markdown/slug-data.tur` is a
    new generated table, in U2's pattern.
    - `tools/gen-md-slug-data.py` writes it from the Unicode Character
      Database that `tools/fetch-ucd.sh` pins (Unicode 16.0.0).
    - The same C is `src/turi/md_slug_data.inc`, for the interpreter's
      natives, and ctest `tur_md_slug_data_sync` keeps the two the same.
    - It also holds the combining marks, which GitHub's rule keeps.
  - **Checked against python-markdown 3.11**, configured as `genguides.py`
    configures it, over every heading in this repository's 2,286 Markdown
    files.
    - The 26,860 headings both parsers find, matched by text, get the same id.
    - 2,216 files get the same id sequence. The other 70 differ only where
      the parsers find different headings: python-markdown reads `#map{` at
      the start of a line as a heading, and its `fenced_code` does not work
      inside a list. That is M4's list to work through.
    - In `docs/guides/`, 157 of 174 files already match.
  - The slug functions alone match python-markdown's `slugify` and
    github-slugger 2.0's `slug` on 20,000 random strings each. Over every
    code point, the GitHub rule differs from github-slugger only on
    characters newer than the Unicode version github-slugger was built from.
- **The API** keeps M1's and M2's shape, ids and accessors, rather than 6.2's
  `(Vec HeadingInfo)` and `(Vec CodeBlockInfo)`. It was written while a
  struct with a `String` field still leaked its box
  (`opaque-field-struct-box-never-freed`, fixed since by #1243). Records of
  that kind can go on top now, if a consumer in M4 or M5 wants them.
  - `markdown/headings` and `markdown/code-blocks` return a
    `(Vec MdBlockId)`: every block of the kind, in document order.
  - `markdown/heading-text` returns a heading's plain text, and
    `markdown/heading-ids` its ids under an `MdHtmlOpts`, one per heading.
    The level, contents, info string and lines come from M1's accessors.
  - `MdBlock` gains `(MdTable columns)`, `(MdTableRow header)` and
    `(MdTableCell MdAlign)`, and `(MdItem)` becomes `(MdItem MdTask)`.
    `MdInline` gains `(MdStrikethrough)`.
- **Size.** About 1,700 lines of Turmeric: the GFM blocks in `block.tur`,
  autolinks and strikethrough in `inline.tur`, and `slug.tur`. The generated
  table comes on top.
- **Compiler friction:**
  - `duplicate-defn-across-loaded-files-reaches-the-c-compiler.md`. A helper
    named like one in `scan.tur` compiled to C with two definitions, where
    the same clash within one file is a diagnostic.
  - `tur fmt` lines up a `cond`'s bodies past its widest test. A test that
    wraps pushes every body far to the right, so the new code keeps each test
    to one line, using helpers.
  - Emitted C is compiled as C99, which has trigraphs: `??!` in a string
    literal is `|`. The NFKD table's text had one, which shifted every byte
    after it. The generator now escapes each `?`.

### 6.5 M4 -- docs pipeline cut-over

1. **Prerequisites:** section 2's fence and list fixes have landed.
2. **A renderer tool.** `tools/md/render.tur` is built once with `tur build`
   and runs in batch mode: given a list of `.md` files, it writes each page's
   HTML fragment and its headings (JSON) to an output directory.
   `tools/genguides.py` and `tools/genspices.py` call it in place of
   `md_lib.Markdown(...).convert`. The rest of the post-processing stays in
   Python for now, because it's site layout rather than Markdown: syntax
   toggles, mermaid unwrapping, table scroll wrappers, the inline Contents box.
3. **A diff gate.** A one-off script renders every guide and spice README both
   ways and reports structural differences. The expected ones are the run-on
   lists, if any are left after section 2, and the block-quote splitting on the
   three pages above. Anything else gets investigated before switching.
4. **Delete the workarounds** listed in 1.1, about 230 lines.
5. **Drop the `markdown` pin** from `tools/requirements.txt`, its `pip install`
   steps in `ci.yml`, `release.yml` and `release-cut.yml`, and the `markdown`
   stub in `tests/tools/check_gendocs_parse.py`. One side effect:
   `gendocs.py --emit-tur`, which regenerates the tracked
   `stdlib/docstrings.tur`, stops needing a pip package. Today it fails without
   `markdown`, because it imports genguides for page layout.
6. **CI cost** (decided, section 8): `ci.yml`'s `web-smoke` job and
   `release-cut.yml`'s Try Turmeric deploy already build a native `tur` for
   `tur run docs`, so they cost nothing extra. The two that don't get an
   in-job build with ccache, the way those two do it: `release.yml`'s `docs`
   job and `release-cut.yml`'s spices-site job. Both stay independent of the
   release builds and run in parallel with them.

**M4 as built.**

- **The renderer tool.** `tools/md/render.tur`, about 220 lines, is
  `render --list=FILE`. For each `X.md` the file names it writes `X.html`, the
  fragment, and `X.json`: the headings (level, id and content HTML) and every
  raw-HTML block and inline it passed through, with the line each starts on.
  It renders with the GFM extensions, the tag filter and `(MdSlugPython)`.
  - `tools/mdrender.py` drives it. It builds the tool with `tur build` on
    first use and caches the build in the system temp directory, keyed on the
    `tur` binary and every stdlib source. Then it writes a run's pages to a
    scratch directory and runs the tool once over all of them. It takes
    `tur` from `$TUR`, `./build/tur` or `PATH`.
  - All 173 guides render in 0.35 s. The first build of a session takes
    about 8 s with the Debug `tur`.
  - The headings come back as the toc tokens python-markdown used to give:
    the nesting is redone in Python, and a heading's `name` is its HTML with
    the tags stripped. The library gains `markdown/heading-html` for that
    HTML.
  - Front matter and a hand-written contents section are blanked out rather
    than cut, so the raw-HTML report's line numbers are the file's.
  - ctest `tur_md_render` (`tests/check-mdrender.sh`) checks the tool and the
    driver together.
- **The generators.** `genguides.py` and `genspices.py` each make one batch
  call, and genguides' process pool is gone. The site-layout post-processing
  stays in Python: the syntax toggles, mermaid, the table scroll wrappers and
  the Contents box. `render_task_lists` becomes `mark_task_items`, which only
  adds the `task-item` class the CSS draws the renderer's checkboxes by.
- **The workarounds** from 1.1 are deleted: `dedent_indented_fences`,
  `unquote_blockquote_fences`, `widen_nested_fences` (and its block reader),
  the info-string marker strip, `render_task_lists`, the `LAZY_OL` tweak and
  `unrendered_fences`. genguides loses 325 lines and gains 87.
  `tools/check-guide-pairs.py` loses its blockquoted-fence check, which
  existed only because python-markdown could not render a quoted fence.
- **The diff gate.** A one-off script, not committed, rendered every guide
  and spice README both ways. It compared block structure, visible text,
  heading ids and names, and the page chrome.
  - **Headings did not move.** Every heading id and name is the same on all
    173 guides. So every anchor still resolves, the Contents boxes and
    sidebars are byte-identical, and so are the docs pack's index entries
    for the guides. M3 counted 17 guides whose headings python-markdown
    found differently, but it ran python-markdown without genguides'
    workarounds. With them, as on the live site, it found CommonMark's
    headings.
  - **51 guides changed structure; 122 did not.** In each change the new
    page is the one GitHub shows:
    - **Lists**, most of the changes. CommonMark nests a list at two spaces
      of indent, where python-markdown wanted four. It also keeps a fence,
      table or paragraph inside the item it is indented under; genguides
      used to dedent a fence out of the item, which split the list. The two
      lists section 2 left alone (`macros-guide.md`,
      `value-representations-guide.md`) render as GitHub's with no source
      change.
    - **Block quotes** split as on GitHub, on the three pages 1.1 names.
    - **Extended autolinks** turn bare URLs into links (3 pages).
    - **`\|` in a table cell** is a pipe. python-markdown kept the
      backslash on 16 pages.
    - **A wrapped line starting `128.`** (or `121.`) stays text.
      python-markdown read it as a new ordered list and dropped the number
      (`thread-pool-guide.md`, `sicp-guide.md`).
    - **Emphasis** nests differently in one paragraph
      (`stateful-refinements-guide.md`).
  - **It found source bugs, which GitHub renders broken too.** All are
    fixed here:
    - **A `|` inside a code span in a table cell** splits the cell under
      GFM; python-markdown's tables extension respected the backticks.
      4 guides now write `\|`.
    - **`<dir>` starting a list item's continuation line** opens an HTML
      block, since `dir` is a block-level tag name
      (`build-driver-internals-guide.md`). The raw-HTML report found it.
    - **A sweet-exp block at column 0** after the list item holding its
      turmeric twin ended the list and broke the toggle
      (`data-literals-guide.md`, 2 pairs).
    - **A wrapped line starting `+ `** became a nested list
      (`syntax-guide.md`).
    - **A code span meant to show Ctrl+`** did not (`devcontainer-guide.md`).
  - **Spice READMEs: 45 of 52 are unchanged.** The other 7 render as on
    GitHub: run-on lists become lists (`linalg`, `stats`), fences stay in
    their items, and `scscm` gains its task-list checkboxes and
    strikethrough. One source bug is left in turmeric-spices:
    `signal/README.md` puts `|g| < 1` in table code spans.
- **Raw HTML.** The guides' build fails on any raw-HTML node, naming its file
  and line, unless the guide's front matter says `raw-html: allow`. No guide
  does. In a spice README it is a warning, since the README lives in the
  spices repository. Neither has any today.
- **The pin.** No `pip install` is left on this repository's docs path.
  - `ci.yml`'s web-smoke job, `release.yml`'s docs job and `release-cut.yml`'s
    deploy-web job no longer install it. `tests/tools/check_gendocs_parse.py`
    drops its `markdown` stub. `gendocs.py --emit-tur` needs no pip package.
  - `release.yml`'s docs job now builds a native `tur`, as decision 5 says.
    It uses web-smoke's configure line and ccache settings, so restoring
    web-smoke's cache from `main` makes the build mostly cache hits.
    `release-cut.yml`'s deploy-web job already built one.
  - **One user is left: `release-cut.yml`'s deploy-spices job.** It runs the
    turmeric-spices repository's own copies of `genguides.py` and
    `genspices.py`, which still import python-markdown. So the pin moves to
    `tools/spices-site-requirements.txt`, which says so, and that job
    installs it from there.
  - **Not done:** moving those copies onto `tur/markdown`. That is
    turmeric-spices work. The job then needs a `tur` (item 6), and the pin
    and its install go.
- **Speed.** A `genguides.py` run takes about as long as before, about 10 s
  here. That is now mostly `git log` dating the pages.

### 6.6 M5 -- other consumers

- **Notebook spice:** replace `notebook/cmark.tur` with `tur/markdown`, keeping
  thin adapters for the spice's own API (`md-node-*`, `md-source-span`). This is
  a spice API change, so it goes through the spice's usual versioning.
- **Literate `.tur.md`** (the hold plan): its envelope pass, in C, decides which
  fences are program text. A ctest feeds the spec's fenced-code and container
  examples through both the envelope's extractor and `markdown/code-blocks`,
  and requires them to agree. It reads the examples from the same sibling
  checkout as 6.1 and skips the same way when that checkout is absent. That is the plan's own "two `.tur.md` dialects
  that disagree" concern, as a test.
- **Fence scanners:** `tools/check-guide-pairs.py` and `tools/doctest.py` find
  fences with their own scans. `markdown/code-blocks` replaces those whenever
  these tools move to Turmeric.
- **Terminal rendering:** `markdown/render-ansi` for `tur doc`, the REPL's
  `(doc ...)` and spice READMEs on the command line. Later.

## 7. Compiler friction the spikes found

1. **A `let`-bound `(vec-new)` doesn't take its element type from where it's
   used.** It copies the enclosing function's return type instead, or stays an
   unresolved `(Vec A)` when there isn't one to copy. Not yet filed under
   `docs/reported/`.

   ```turmeric
   (defn fill [acc : (Vec cstr)] : int
     (do (vec-push! acc "x") (vec-len acc)))

   (defn go [out : (Vec int)] : (Vec int)
     (let [inner (vec-new)]
       (println (fill inner))    ;; TUR-E0001: expected (Vec cstr), got (Vec int)
       out))
   ```

   Without the `(Vec int)` return type, the error reads `got (Vec A)`. The
   workaround is a typed helper, `(defn new-lines [] : (Vec cstr) (vec-new))`.
2. **`=` doesn't accept `Char`** (`TUR-E0006`). Left as it is (section 8):
   `eq?` already works, and S1's helpers cover the common case.
3. **`escape` is a special form**, so a user's `(defn escape ...)` can never be
   called by its bare name. `TUR-W0042` says so clearly. The Markdown module
   names its function `markdown/escape-html`.
4. **`import` only works inside `defmodule`**, so even a one-file tool needs a
   `defmodule` wrapper. Minor; noted for completeness.
5. **A named `let` can't declare its result type** (found writing S2). It
   desugars to an unannotated `fn` (`elab_named_let`, `src/compiler/elab_forms.c`),
   so the loop call is typed `int`. A loop that returns `bool` or `(Option int)`
   then fails with "if branches have mismatched types". S2's loops are
   self-recursive helper `defn`s with declared result types instead, which the
   self-tail-call lowering turns into loops just the same. A `: T` after the
   binding vector, `(let loop [i 0] : (Option int) ...)`, would fix it.

## 8. Decisions

The questions this plan first left open, settled with the author on 2026-10-15.

| # | Question | Decision | Why |
|---|---|---|---|
| 1 | Where do the CommonMark spec examples (CC-BY-SA 4.0) live? | [`turmeric-lang/commonmark-spec`](https://github.com/turmeric-lang/commonmark-spec) (created and populated 2026-10-15), consumed as a sibling checkout pinned by SHA; tests skip when it is absent (6.1) | Keeps the share-alike text out of this repo; same pattern as `turmeric-spices`; nothing touches the network at configure or test time |
| 2 | Should `=` accept `Char`? | **No.** `=` stays primitive-only; `eq?` is the idiom (4.2) | `(eq? c #\,)` already works through `Eq [Char]`; `=` rejects `String` for the same reason; S1's `byte-is?` covers the common case. Making `=` fall back to `Eq` would be its own plan, behind `--enable` |
| 3 | Name of the concatenation macro | `string/of` (4.6) | Matches the `string/` prefix of what it returns; `str` is already a type name (the borrowed-view tier) |
| 4 | How are the Unicode tables generated? | The r7rs pattern: one C table emitted twice, inline C plus an interpreter native, with a sync check (section 5) | Fast static lookups; works compiled, interpreted and in the web REPL; the generator and UCD pin already exist |
| 5 | How do the docs CI jobs get a `tur`? | Build in-job with ccache in `release.yml`'s `docs` job and `release-cut.yml`'s spices-site job; the other two docs jobs already build one (6.5) | Matches the existing jobs; keeps the docs jobs parallel to the release builds |
| 6 | Raw HTML | Pass through with GFM's tag filter by default (`RawHtml` is `TagFilter`); `Pass` and `Escape` are available; the docs build fails on any raw HTML in a guide that doesn't allow it (6.2) | GitHub's behaviour, closest to today's site; the build check catches the accidental-tag hazard pass-through keeps |
| 7 | Stdlib or spice? | Stdlib: `stdlib/markdown.tur` + `stdlib/markdown/` (6.2) | The repo's own docs build can't depend on the spices checkout, and literate `.tur.md` is a compiler feature |
| 8 | A bootstrap script for a machine with no `tur` (J4)? | **No.** The first build stays the two CMake commands; J4 only gives `release` and `tsan` their own build trees (section 10) | Two commands need no wrapper, and a script would be one more entry point to keep in step with CI |

## 9. Related work outside this plan

- **Docstring extraction.** `tools/gendocs.py` parses `;;;` blocks with
  line-based regexes. `src/lsp/lsp_docs.c` uses a different rule for which
  definition a block belongs to: the LSP drops a block at a `;;` line, gendocs
  keeps it. 62 stdlib definitions currently show another function's
  documentation; for example, `tur doc env/get-raw` prints `env/get`'s full
  docs. The proposed fix is one extractor in the compiler that both use, plus a
  check that each summary names its own definition. That belongs in its own plan
  or report.
- **No general sort in stdlib.** Surfaced by the same investigation. Not a
  string helper, so not covered here.
- **Python stays a dev dependency** whatever happens here: about 30 ctest
  targets, `tests/run.sh`'s parity checks, the fuzzers, `tools/ci/` and
  `tools/release_prepare.py` all use it. This plan removes the only third-party
  package on the docs path, not Python itself. The Python generators that write
  committed compiler C (`gen-runtime-split.py`, `gen_ffi_dispatch.py`,
  `gen-r7rs-*.py`) should stay out of Turmeric, because they would need a
  working `tur` to produce `tur`'s own source.

## 10. Phase J -- `tur run` in place of `just`

`tur run` is the compiler's built-in, Justfile-compatible task runner
([`tur-run-guide.md`](../guides/tur-run-guide.md); `tur just` is an alias).
It reads the same `Justfile`, so a contributor who has `tur` doesn't need the
`just` binary. That makes it the one non-Python tool on the dev path that can
go away completely. It is worth doing, and most of it has already happened.
**J1 and J2 landed in [#1201](https://github.com/turmeric-lang/turmeric/pull/1201)
and most of J3 in #1202; #1207 does its last line.** What follows is the state
they started from, as measured on 2026-10-15:

- **CI already doesn't use `just`.** No workflow installs it or runs it.
  Recipes run as `./build/tur run <recipe>` (for example
  `tur run regen-snapshots -- --check` and `tur run docs`).
- **`tur run` handles the whole Justfile.** `./build/tur run --list` parses
  all 75 recipes and prints no unsupported-feature notes. `tur run` now
  implements recipe attributes (`[unix]`, `[group]`, `[confirm]` and the
  rest), modules, imports and backtick substitution; only an unknown
  attribute such as `[parallel]` is refused. The guide still listed those as
  unsupported; #1201 corrected it.
- **The one hard dependency is the Justfile calling `just` itself.** `debug`,
  `release` and `tsan` run `[ -f build/CMakeCache.txt ] || just configure`,
  `reconfigure` runs `just configure`, and the Windows build runs
  `just configure-windows`. So `tur run build` on a tree with no CMake cache
  still fails without `just` installed.
- **The rest is wording**, where a reader or a message is told to type `just`:
  - 43 mentions in 17 guides, 2 in AGENTS.md and 1 in CLAUDE.md
  - the Justfile's own comments and error messages, e.g. "run 'just build'
    first"
  - `tools/run-doctests.sh`, the comment in `tools/requirements.txt`, and
    comments in `release-cut.yml`
  - `tools/gendocs.py`, which writes `Run: just docs` into the header of the
    generated `stdlib/docstrings.tur`
  - the `cut-*-release` commands under `.claude/commands/`
  - one compiler message: `tur docs` suggests "Or build it: just docs"
    (`src/main.c`)

### J1 -- recipes stop calling `just`

Replace each self-call with a hidden dependency recipe, so the same file works
under both runners:

```
CONFIGURE := "cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5"

debug: _configured
    cmake --build build -j --config Debug

_configured:
    @[ -f build/CMakeCache.txt ] || {{CONFIGURE}}
```

`release` and `tsan` depend on `_configured` the same way. `reconfigure`
becomes `rm -rf build` followed by `{{CONFIGURE}}`. `build-windows` is one
bash script, so it inlines the configure step from a matching
`CONFIGURE_WIN`. `tur run` already
supports dependencies and `_`-prefixed hidden recipes. Verify by running
`tur run build` in a fresh worktree with no `build/` and no `just` on `PATH`.

### J2 -- keep the Justfile inside the subset

Once nothing installs `just`, a recipe that uses a feature `tur run` defers
would only fail when someone runs it. A small check catches that up front. It
can be a ctest target or a step in an existing CI job:
- `./build/tur run --list` prints no `tur run: note:` line;
- no recipe body calls `just ` again.

### J3 -- say `tur run`

Mechanical: change each mention listed above to `tur run <recipe>`, and
regenerate `stdlib/docstrings.tur` for its new header. #1201 already did the
Justfile's own messages. Leave `docs/archive/` alone, because those files are
history. Plans in `docs/upcoming/` get updated when they're next touched.

**One exception: Windows bootstrap.** On a fresh Windows box there is no
`tur` yet, and `setup-windows` is what installs `just` (the Justfile's
"Windows setup" notes say so). There, `just` really is the runner, so those
notes and the Windows guide's bootstrap steps keep it.

J3 was split around the PRs open at the time: #1202 did the wording outside
two files, #1201 the `tur run` guide, and #1207 the header `tools/gendocs.py`
writes into `stdlib/docstrings.tur` (which #1200 was regenerating).

### J4 -- separate build trees for `release` and `tsan`; no bootstrap script

`tur run` needs a `tur`, so the very first build of a checkout can't go through
it. That build is two CMake commands, which the README and CLAUDE.md already
document. They work on Linux and macOS, and inside an MSYS2 UCRT64 shell on
Windows: CI's Windows job builds and tests that way, with Ninja and no `just`.
**Decided (2026-10-15): no bootstrap script.** A `scripts/bootstrap.sh` (plus
a PowerShell wrapper) was considered, to drop `just` from the Windows setup
notes and define the configure step once, but two commands do not need a
wrapper, and it would be one more entry point to keep in step with CI.

What J4 keeps is the bug that came up while measuring it. **`tur run release`
built Debug.** The recipe built the shared `build/` tree, configured as Debug,
and passed `--config Release`. Single-config generators (Makefiles, Ninja)
ignore that flag, so the guides' "release builds strip contracts" was false:
contract elision is `#ifdef NDEBUG` in the compiler, and only the Release
configuration defines it. `tsan` had the same problem, and `test-tsan` then
ran ctest over the ASan-instrumented `build/` with `TUR_TSAN=1`, which cannot
work (ASan and TSan cannot share a process; `tests/run-tsan.sh` refuses it).

The fix splits the trees:

- `release` configures and builds `build-release/` as Release, the directory
  the README, CLAUDE.md, `tvm` and the benchmark scripts already use.
- `tsan` configures and builds `build-tsan/` as Release, the tree the nightly
  TSan workflow and `tests/run-tsan.sh` use. The fixture programs carry
  `-fsanitize=thread`; `tur` must only not be ASan-instrumented. The TSan
  CMake configuration, which instruments `tur` too, does not link every
  target, and is reported separately
  (`docs/reported/tsan-cmake-config-fails-to-link.md`).
- `test-tsan` runs `tests/run-tsan.sh` against `build-tsan/tur`, the selection
  the nightly TSan workflow runs, under the 12-minute suite timeout.
- `_configured` takes the directory and build type, so all three build
  recipes share one configure-if-missing step.
- The guides that describe these recipes say where the binary lands, and that
  it is the Release `tur` that strips contracts.

### What stays

- **The file stays named `Justfile`.** `tur run` reads that name, and upstream
  `just` still works for anyone who has it.
- **`tools/just-vs-tur-run.sh` stays** as an optional local parity check. It
  isn't wired into CI today, and it skips itself when `just` is absent.
- **A fresh clone with no `tur` at all still bootstraps with CMake**, as
  CLAUDE.md already documents. With a released `tur` on `PATH`, J1 makes
  `tur run build` work from a fresh clone too.

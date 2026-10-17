---
title: Markdown Guide
category: Data Structures and Libraries
description: Parse CommonMark and GitHub Flavored Markdown into a document tree with `tur/markdown`, render it as HTML, walk its blocks and inlines, and what is implemented so far
---

# Markdown -- `tur/markdown`

`stdlib/markdown.tur` reads Markdown into a document and writes the document
out as HTML. It follows the [CommonMark spec](https://spec.commonmark.org/0.31.2/),
version 0.31.2, and is checked against the spec's own examples. On top of that
it has GitHub's four extensions -- tables, strikethrough, extended autolinks
and task list items -- as [cmark-gfm](https://github.com/github/cmark-gfm),
GitHub's renderer, implements them.

It is pure Turmeric apart from the Unicode and HTML-entity tables, which the
interpreter carries as natives. So the same program runs compiled, under
`tur jit` and under `tur --interpret`.

## What works today

The library is being built in phases (`docs/upcoming/markdown-library-plan.md`,
Phase M):

| Phase | Covers | State |
| --- | --- | --- |
| M1 | Block structure: paragraphs, ATX and setext headings, thematic breaks, indented and fenced code, the seven kinds of HTML block, block quotes, lists (tight and loose), link reference definitions, tabs, lazy continuation lines | Done |
| M2 | Inline syntax: emphasis and strong emphasis, code spans, links and images (inline and by reference), autolinks, raw inline HTML, backslash escapes, entities, hard and soft breaks | Done |
| M3 | The GFM extensions (tables, strikethrough, extended autolinks, task lists), heading ids, `markdown/headings` and `markdown/code-blocks` | Done |

All of CommonMark is in, and so are the extensions: every one of the
CommonMark spec's 652 examples passes, and so does every one of the 24 GFM
extension examples.

## Parse and render

`markdown/parse` takes the source as a `String` and returns an `MdDoc`.
`markdown/render-html` writes the document as a fresh `String`, and
`markdown/doc-free` releases the document.

```turmeric
(load "stdlib/markdown.tur")

(defn main [] : int
  (let [src  (string/from-cstr "# Notes\n\n- one\n- *two*\n\nSee [the spec](/spec).\n\n    code\n")
        doc  (markdown/parse src (markdown/default-opts))
        html (markdown/render-html doc (markdown/default-html-opts))]
    (do
      (println (string/to-cstr html))
      (string/release html)
      (markdown/doc-free doc)
      (string/release src)
      0)))
```
```sweet-exp
#lang sweet-exp
load "stdlib/markdown.tur"

defn main [] : int
  let [src  string/from-cstr("# Notes\n\n- one\n- *two*\n\nSee [the spec](/spec).\n\n    code\n")
       doc  markdown/parse(src markdown/default-opts())
       html markdown/render-html(doc markdown/default-html-opts())]
    do
      println $ string/to-cstr html
      string/release html
      markdown/doc-free doc
      string/release src
      0
```

```
<h1>Notes</h1>
<ul>
<li>one</li>
<li><em>two</em></li>
</ul>
<p>See <a href="/spec">the spec</a>.</p>
<pre><code>code
</code></pre>
```

The output's layout is the spec's: one line per block tag, and a tight list's
items without `<p>`. The paragraph matters here: without it, the indented line
after the blank line would continue the second item, making the list loose,
rather than start a code block.

Any `String` parses, because Markdown has no syntax errors. Lines may end in LF,
CR or CR LF, and a NUL byte reads as U+FFFD.

The document keeps its own copy of the source, so `src` can be released
straight away. Nothing else needs freeing: `markdown/doc-free` releases every
block, every inline and every string in the document.

## Options

`markdown/parse` takes an `MdOpts`, which says which extensions to recognise
beyond CommonMark:

| Field | Extension |
| --- | --- |
| `tables` | GFM tables |
| `strikethrough` | GFM `~~strikethrough~~` |
| `autolinks` | GFM extended autolinks |
| `tasklists` | GFM task list items |
| `math` | `$...$` and `$$...$$` |

- `(markdown/default-opts)` turns the four GFM extensions on and math off.
- `(markdown/commonmark-opts)` turns everything off.

`math` is not implemented yet: setting it changes nothing. It is there for the
notebook, which comes later in the plan (section 6.6).

`markdown/render-html` takes an `MdHtmlOpts` with two fields. The first,
`raw-html`, says what to do with HTML written into the document:

| Value | Raw HTML is |
| --- | --- |
| `(MdRawTagFilter)` | written through, except that the `<` opening `title`, `textarea`, `style`, `xmp`, `iframe`, `noembed`, `noframes`, `script` or `plaintext` is escaped, as GitHub does. The default, `(markdown/default-html-opts)`. |
| `(MdRawPass)` | written through unchanged, as the CommonMark spec does |
| `(MdRawEscape)` | written as text |

The second, `heading-ids`, says whether each heading gets an `id` attribute,
and by which rule:

| Value | Headings get |
| --- | --- |
| `(MdNoIds)` | no id, as the CommonMark spec and cmark-gfm write them. The default. |
| `(MdSlugPython)` | python-markdown's: `## Hello, World!` is `hello-world`, accents and ligatures become ASCII, and a repeat gets `_1`, `_2`... |
| `(MdSlugGitHub)` | GitHub's: `hello-world` too, but letters past ASCII stay, and a repeat gets `-1`, `-2`... |

```turmeric
(markdown/render-html doc (MdHtmlOpts (MdRawTagFilter) (MdSlugPython)))
```
```sweet-exp
markdown/render-html doc MdHtmlOpts(MdRawTagFilter() MdSlugPython())
```

[Headings and code blocks](#headings-and-code-blocks) has the rules in full.

`(markdown/escape-html s)` is the escaping the renderer uses for text: `&`,
`<`, `>` and `"` become entities.

## The GFM extensions

Each one follows cmark-gfm 0.29.0.gfm.13, the version GitHub runs, down to its
quirks. `tests/run-markdown-diff.sh` holds the two to the same HTML (see
[Conformance](#conformance)).

- **Tables.** A table starts at a delimiter row (`| --- | :-: |`) under a line
  with the same number of cells, which becomes the header. A colon on the left,
  right or both ends of a delimiter cell aligns its column. Each line after
  that is a row, until a blank line or the start of another block. A short row
  gets empty cells, and a long one is cut to the header's width. Write `\|` for
  a pipe inside a cell, inside a code span too.
  - The lines of a paragraph above the header stay a paragraph.
  - As in cmark-gfm, link reference definitions in that paragraph are not read:
    `[x]: /u` directly above a table is text.
- **Strikethrough.** `~one~` or `~~two~~`: a run of one or two tildes, closed by
  a run of the same length. Three or more are text. With strikethrough on, a
  `*` or `_` run's neighbour is read past any tildes beside it, so `_a_~b` is
  not emphasis.
- **Extended autolinks.** Links that need no `<...>`:
  - `www.` followed by a domain, after a space, the start of a line, or one
    of `*`, `_`, `~` and `(`. The link gets `http://`.
  - `http://`, `https://` or `ftp://` followed by a domain.
  - Email addresses. They get `mailto:`; one written with `mailto:` or `xmpp:`
    keeps it.

  A trailing `?`, `!`, `.`, `,`, `:`, `*`, `_`, `~`, quote, unmatched `)` or
  entity-like `&name;` is left out of the link. A domain with an underscore in
  its last two labels makes no link. Inside a link's text there is no second
  link.
- **Task list items.** A list item that opens with `[ ]`, `[x]` or `[X]` and a
  space gets a checkbox. Two of cmark-gfm's quirks come with it:
  - The item is checked when its first line holds `[x]` or `[X]` anywhere, so
    `- [ ] see [x]` is checked.
  - The box is looked for from the start of the line, after nothing but spaces
    and the list marker, so an item inside a block quote (`> - [ ] a`) gets no
    checkbox.

`(markdown/commonmark-opts)` turns all four off, and `MdOpts` turns each on or
off by itself.

## Walking the tree

The document is a tree of blocks. `markdown/root` is the document block, and
`markdown/children` returns the blocks directly inside one as a fresh
`(Vec MdBlockId)`, which the caller frees with `vec-free`. `markdown/block` says
what a block is:

| `MdBlock` | Block |
| --- | --- |
| `(MdDocument)` | the root |
| `(MdBlockQuote)` | a block quote |
| `(MdList ordered start tight)` | a list; `start` is the first number (1 for a bullet list) |
| `(MdItem task)` | a list item; `task` is `(MdNotTask)`, or `(MdUnchecked)` or `(MdChecked)` for a task list item |
| `(MdParagraph)` | a paragraph |
| `(MdHeading level)` | a heading, level 1 to 6 |
| `(MdThematicBreak)` | a thematic break |
| `(MdCodeBlock fenced)` | an indented or fenced code block |
| `(MdHtmlBlock)` | an HTML block |
| `(MdTable columns)` | a table, holding its rows |
| `(MdTableRow header)` | a table row, holding its cells; `header` is true for the first |
| `(MdTableCell align)` | a table cell; `align` is `(MdAlignNone)`, `(MdAlignLeft)`, `(MdAlignCenter)` or `(MdAlignRight)` |

Three more accessors read a block's details:

- `markdown/block-text` returns a leaf block's text as `(some s)`, a fresh
  `String`, or `(none)` for a container block. That is a paragraph's or
  heading's source text (trimmed, with its inline syntax still in it), a code
  block's contents, or an HTML block's HTML. The next section has the parsed
  form.
- `markdown/code-info` returns a fenced code block's info string, such as
  `"python"`.
- `markdown/block-lines` returns the source lines a block spans, as
  `(pair first last)`.

This prints each top-level heading's level and text:

```turmeric
(load "stdlib/markdown.tur")

(defn print-heading [d : MdDoc id : MdBlockId] : void
  (match (markdown/block d id)
    (MdHeading level)
    (match (markdown/block-text d id)
      (Some t) (do (println level) (println (string/to-cstr t)) (string/release t))
      (None) nil)
    _ nil))

(defn print-headings [d : MdDoc] : void
  (let [kids   (markdown/children d (markdown/root d))
        ^mut i 0]
    (do
      (while (< i (vec-len kids))
        (do (print-heading d (vec-get kids i)) (set! i (+ i 1))))
      (vec-free kids))))
```
```sweet-exp
load "stdlib/markdown.tur"

defn print-heading [d : MdDoc id : MdBlockId] : void
  (match (markdown/block d id)
    (MdHeading level)
    (match (markdown/block-text d id)
      (Some t) (do (println level) (println (string/to-cstr t)) (string/release t))
      (None) nil)
    _ nil)

defn print-headings [d : MdDoc] : void
  let [kids   markdown/children(d markdown/root(d))
       ^mut i 0]
    do
      while {i < vec-len(kids)}
        do print-heading(d vec-get(kids i)) set!(i {i + 1})
      vec-free kids
```

A list's items are its children, and an item's blocks are its children in
turn, so the same loop, made recursive, walks the whole tree.

## Headings and code blocks

Two functions list blocks of one kind from anywhere in the document, block
quotes and list items included, in the order they come in the text. Each
returns a fresh `(Vec MdBlockId)`, which the caller frees with `vec-free`.

- `markdown/headings` lists the headings.
- `markdown/code-blocks` lists the code blocks, fenced and indented.
  `markdown/code-info`, `markdown/block-text` and `markdown/block-lines` read
  each one's info string, contents and source lines.

Two more read the headings:

- `markdown/heading-text` returns a heading's content as plain text, for a
  table of contents: the text of its inlines, code spans included, with raw
  HTML and images' alt text left out and each run of whitespace one space. It
  is `(none)` for a block that is not a heading.
- `markdown/heading-ids` returns the ids `markdown/render-html` gives the
  headings under an `MdHtmlOpts`, one per heading in the same order as
  `markdown/headings`. It is a fresh `(Vec String)`, released with
  `string/release-all`, and empty under `(MdNoIds)`.

This prints a table of contents with python-markdown's ids:

```turmeric
(load "stdlib/markdown.tur")

(defn print-toc [d : MdDoc] : void
  (let [hs     (markdown/headings d)
        ids    (markdown/heading-ids d (MdHtmlOpts (MdRawTagFilter) (MdSlugPython)))
        ^mut i 0]
    (do
      (while (< i (vec-len hs))
        (do
          (match (markdown/heading-text d (vec-get hs i))
            (Some t) (do (println (string/to-cstr (vec-get ids i))) (println (string/to-cstr t)) (string/release t))
            (None) nil)
          (set! i (+ i 1))))
      (vec-free hs)
      (string/release-all ids))))

(defn main [] : int
  (let [src (string/from-cstr "# Install\n\n## From `source`\n\n## Install\n")
        doc (markdown/parse src (markdown/default-opts))]
    (do
      (print-toc doc)
      (markdown/doc-free doc)
      (string/release src)
      0)))
```
```sweet-exp
load "stdlib/markdown.tur"

defn print-toc [d : MdDoc] : void
  let [hs     markdown/headings(d)
       ids    markdown/heading-ids(d MdHtmlOpts(MdRawTagFilter() MdSlugPython()))
       ^mut i 0]
    do
      while {i < vec-len(hs)}
        do
          (match (markdown/heading-text d (vec-get hs i))
            (Some t) (do (println (string/to-cstr (vec-get ids i))) (println (string/to-cstr t)) (string/release t))
            (None) nil)
          set!(i {i + 1})
      vec-free hs
      string/release-all ids

defn main [] : int
  let [src string/from-cstr("# Install\n\n## From `source`\n\n## Install\n")
       doc markdown/parse(src markdown/default-opts())]
    do
      print-toc doc
      markdown/doc-free doc
      string/release src
      0
```

```
install
Install
from-source
From source
install_1
Install
```

**The rules.** Both start from the heading as the renderer writes it, with
its comments and tags taken out and its character references decoded. So a
link keeps its text, and an image loses its alt text.

- **`MdSlugPython`** is the `toc` extension of python-markdown 3.11, which
  builds this site's guides:
  1. Each run of whitespace becomes one space.
  2. Unicode compatibility decomposition (NFKD) is applied, and what is not
     ASCII is dropped, so `Résumé` is `Resume`.
  3. Everything but letters, digits, `_`, `-` and whitespace is removed, the
     ends are trimmed, and the rest is lower-cased.
  4. Each run of `-` and whitespace becomes one `-`.
  5. A repeated id gets `_1`, then `_2`; one that already ends in `_N` counts
     on from N. An empty id is `_1`.
- **`MdSlugGitHub`** is GitHub's, as the github-slugger package reproduces it:
  1. The text is lower-cased.
  2. Everything but letters, combining marks, decimal digits, `_`, `-` and the
     space is removed. `Привет мир` keeps its letters.
  3. Each space becomes a `-`, without collapsing runs: `A & B` is `a--b`.
  4. A repeated id gets `-1`, then `-2`. An empty one is written as no `id`
     attribute.

GitHub's own pages put `user-content-` in front of each id; the rule here
gives the part after it, which is what a `#fragment` link names.

## Walking the inlines

A paragraph's, heading's or table cell's content is a tree of inlines. `markdown/inlines`
returns them for a block, and `markdown/inline-children` returns the inlines
inside one. Both return a fresh `(Vec MdInlineId)`, empty when there are none,
which the caller frees with `vec-free`. `markdown/inline` says what an inline
is:

| `MdInline` | Inline | Holds |
| --- | --- | --- |
| `(MdText)` | text | its text |
| `(MdCode)` | a code span | its contents |
| `(MdHtmlInline)` | raw HTML, such as a tag or a comment | the HTML |
| `(MdEmph)`, `(MdStrong)` | emphasis and strong emphasis | inlines |
| `(MdStrikethrough)` | strikethrough | inlines |
| `(MdLink)` | a link, autolinks and extended autolinks included | inlines, a destination and a title |
| `(MdImage)` | an image | its alt text, as inlines, a destination and a title |
| `(MdSoftBreak)`, `(MdHardBreak)` | a line ending, and one written as a break (`<br />`) | nothing |

Three accessors read an inline's details, each as `(some s)`, a fresh
`String`, or `(none)`:

- `markdown/inline-text` returns what a text, code or raw-HTML inline holds. A
  text inline's escapes and character references are resolved, and a code
  span's line endings are spaces.
- `markdown/link-dest` returns a link's or image's destination as the HTML has
  it: escapes resolved and the URL percent-encoded.
- `markdown/link-title` returns its title, or `(none)` when it has none.

Text that comes from a backslash escape or a character reference is a text
inline of its own, so join adjacent text inlines when you want the words.

This prints the destination of every link in a paragraph, however deeply it is
nested:

```turmeric
(load "stdlib/markdown.tur")

;; Every link destination at or under inline id.
(defn print-links [d : MdDoc id : MdInlineId] : void
  (let [kids   (markdown/inline-children d id)
        ^mut i 0]
    (do
      (match (markdown/inline d id)
        (MdLink) (match (markdown/link-dest d id)
                   (Some u) (do (println (string/to-cstr u)) (string/release u))
                   (None) nil)
        _ nil)
      (while (< i (vec-len kids))
        (do (print-links d (vec-get kids i)) (set! i (+ i 1))))
      (vec-free kids))))

(defn print-block-links [d : MdDoc id : MdBlockId] : void
  (let [ins    (markdown/inlines d id)
        ^mut i 0]
    (do
      (while (< i (vec-len ins))
        (do (print-links d (vec-get ins i)) (set! i (+ i 1))))
      (vec-free ins))))

(defn main [] : int
  (let [src (string/from-cstr "See [the spec](https://spec.commonmark.org/) and *[one](</a b>)*.\n")
        doc (markdown/parse src (markdown/default-opts))
        top (markdown/children doc (markdown/root doc))]
    (do
      (print-block-links doc (vec-get top 0))
      (vec-free top)
      (markdown/doc-free doc)
      (string/release src)
      0)))
```
```sweet-exp
load "stdlib/markdown.tur"

;; Every link destination at or under inline id.
defn print-links [d : MdDoc id : MdInlineId] : void
  let [kids   markdown/inline-children(d id)
       ^mut i 0]
    do
      (match (markdown/inline d id)
        (MdLink) (match (markdown/link-dest d id)
                   (Some u) (do (println (string/to-cstr u)) (string/release u))
                   (None) nil)
        _ nil)
      while {i < vec-len(kids)}
        do print-links(d vec-get(kids i)) set!(i {i + 1})
      vec-free kids

defn print-block-links [d : MdDoc id : MdBlockId] : void
  let [ins    markdown/inlines(d id)
       ^mut i 0]
    do
      while {i < vec-len(ins)}
        do print-links(d vec-get(ins i)) set!(i {i + 1})
      vec-free ins

defn main [] : int
  let [src string/from-cstr("See [the spec](https://spec.commonmark.org/) and *[one](</a b>)*.\n")
       doc markdown/parse(src markdown/default-opts())
       top markdown/children(doc markdown/root(doc))]
    do
      print-block-links doc vec-get(top 0)
      vec-free top
      markdown/doc-free doc
      string/release src
      0
```

```
https://spec.commonmark.org/
/a%20b
```

An image's alt text is its children. The HTML renderer writes them as plain
text: emphasis and code lose their tags, and raw HTML is escaped, as cmark
does it. (commonmark.js writes raw HTML there unescaped, into the attribute.)

## Conformance

`tests/run-markdown-spec.sh` (ctest `tur_markdown_spec`) runs every example of
the CommonMark spec, and the 24 GFM extension examples. It renders each one
compiled and under `--interpret`, and compares the result with the spec's HTML.

- **Where the examples are.** They live in
  [turmeric-lang/commonmark-spec](https://github.com/turmeric-lang/commonmark-spec),
  because they are CC-BY-SA. Clone it next to this checkout to run them; the
  script skips without it.
- **What has to pass.** `tests/markdown-spec/ratchet.txt` lists the examples
  that pass, and the run fails if one of those stops passing. Each phase adds
  to the list.
- **Where it stands.** All 652 CommonMark examples and all 24 GFM examples
  pass on all three engines. All but two are byte for byte the same; the two
  task list examples differ only in attribute order, which the spec's
  normalisation ignores.

`tests/run-markdown-diff.sh` (ctest `tur_markdown_diff`) goes past the spec's
examples. It renders every Markdown file in the repository, plus random
documents, and fails on any difference from the reference implementations:

- Plain CommonMark is compared with commonmark.js 0.31.2, the implementation
  this library was ported from. This needs node and `npm ci` in
  `tests/markdown-spec/oracle`.
- The GFM extensions are compared with cmark-gfm. This needs cmake and a
  cmark-gfm checkout next to this one, at the commit
  `tests/markdown-spec/oracle/cmark-gfm.pin` names.

Each comparison skips without what it needs.

The heading-id rules were checked against their originals when they were
written: `MdSlugPython` against python-markdown 3.11 over every heading in
this repository's Markdown, and both against their reference slug functions
over 20,000 random strings. `tests/fixtures/markdown-headings` keeps a
document whose ids match both originals.

See the [running-tests guide](running-tests-guide.md#testsrun-markdown-specsh)
for the script's options.

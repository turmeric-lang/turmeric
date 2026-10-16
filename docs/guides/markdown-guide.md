---
title: Markdown Guide
category: Data Structures and Libraries
description: Parse CommonMark Markdown into a document tree with `tur/markdown`, render it as HTML, walk its blocks and inlines, and what is implemented so far
---

# Markdown -- `tur/markdown`

`stdlib/markdown.tur` reads Markdown into a document and writes the document
out as HTML. It follows the [CommonMark spec](https://spec.commonmark.org/0.31.2/),
version 0.31.2, and is checked against the spec's own examples.

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
| M3 | The GFM extensions (tables, strikethrough, extended autolinks, task lists) and heading ids | Next |

All of CommonMark is in: every one of the spec's 652 examples passes.

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

The extensions arrive with M3. Until then both settings parse plain CommonMark.

`markdown/render-html` takes an `MdHtmlOpts`, whose `raw-html` field says what
to do with HTML written into the document:

| Value | Raw HTML is |
| --- | --- |
| `(MdRawTagFilter)` | written through, except that the `<` opening `title`, `textarea`, `style`, `xmp`, `iframe`, `noembed`, `noframes`, `script` or `plaintext` is escaped, as GitHub does. The default, `(markdown/default-html-opts)`. |
| `(MdRawPass)` | written through unchanged, as the CommonMark spec does |
| `(MdRawEscape)` | written as text |

`(markdown/escape-html s)` is the escaping the renderer uses for text: `&`,
`<`, `>` and `"` become entities.

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
| `(MdItem)` | a list item |
| `(MdParagraph)` | a paragraph |
| `(MdHeading level)` | a heading, level 1 to 6 |
| `(MdThematicBreak)` | a thematic break |
| `(MdCodeBlock fenced)` | an indented or fenced code block |
| `(MdHtmlBlock)` | an HTML block |

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

## Walking the inlines

A paragraph's or heading's content is a tree of inlines. `markdown/inlines`
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
| `(MdLink)` | a link, autolinks included | inlines, a destination and a title |
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
- **Where it stands.** All 652 CommonMark examples pass on all three engines,
  byte for byte. Three of the GFM examples pass; the rest wait for M3's
  extensions.

`tests/run-markdown-diff.sh` (ctest `tur_markdown_diff`) goes past the spec's
examples. It renders every Markdown file in the repository, and 30,000 random
documents, both here and with commonmark.js 0.31.2, the implementation this
library was ported from, and fails on any difference. It needs node and
`npm ci` in `tests/markdown-spec/oracle`, and skips without them.

See the [running-tests guide](running-tests-guide.md#testsrun-markdown-specsh)
for the script's options.

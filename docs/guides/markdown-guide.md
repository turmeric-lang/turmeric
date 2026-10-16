---
title: Markdown Guide
category: Data Structures and Libraries
description: Parse CommonMark Markdown into a document tree with `tur/markdown`, render it as HTML, walk its blocks, and what is implemented so far
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
| M2 | Inline syntax: emphasis, code spans, links and images, autolinks, raw inline HTML, backslash escapes, entities, hard breaks | Next |
| M3 | The GFM extensions (tables, strikethrough, extended autolinks, task lists) and heading ids | After M2 |

Until M2 lands, the text of a paragraph or heading is written out as escaped
text: `*emphasis*` comes out as `*emphasis*`, and `&amp;` as `&amp;amp;`.
Line breaks inside a paragraph are soft breaks, and a line ending in two or
more spaces is a hard break (`<br />`).

## Parse and render

`markdown/parse` takes the source as a `String` and returns an `MdDoc`.
`markdown/render-html` writes the document as a fresh `String`, and
`markdown/doc-free` releases the document.

```turmeric
(load "stdlib/markdown.tur")

(defn main [] : int
  (let [src  (string/from-cstr "# Notes\n\n- one\n- two\n\nText.\n\n    code\n")
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
  let [src  string/from-cstr("# Notes\n\n- one\n- two\n\nText.\n\n    code\n")
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
<li>two</li>
</ul>
<p>Text.</p>
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
block and every string in the document.

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
  heading's text (trimmed), a code block's contents, or an HTML block's HTML.
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
- **Where it stands.** With M1, every example in the block sections passes
  except those whose expected HTML needs inline syntax (emphasis, links, code
  spans).

See the [running-tests guide](running-tests-guide.md#testsrun-markdown-specsh)
for the script's options.

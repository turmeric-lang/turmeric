# Plan: DOM and SAX Parser Spices for XML and HTML

> Status: implemented (2026-10-10), P0-P7, in one turmeric-spices PR on
> branch `claude/xml-html-parsers-plan-si0hmx`; see "Outcome" below.
> Spices: `tur-xml`, `tur-html` and `tur-dom-core` (new, first-party, in
> `turmeric-spices`)
> Pattern to follow: `spices/json` (vendored C library via `:cmake-deps`,
> typed handles over an `:int` carrier, a self-contained `ownstr` module).
>
> Follow-ups A-C (markup literals with templated values, an editable tree,
> CSS selectors and node paths): planned 2026-10-11, not started. See
> "Follow-ups" at the end.

## Outcome (2026-10-10)

All phases landed together. What was decided, and where the code differs from
the sketch below:

- **P0.** expat **R_2_9_0** (2026-10-05; carries CVE-2026-102633 and
  CVE-2026-77214) and lexbor **v3.0.1**. Both build on Linux under gcc and
  clang; the macOS leg is CI's. expat needs a `cmake-deps/expat` shim (its
  `CMakeLists.txt` lives in `expat/`, which a `:url` dep cannot reach);
  lexbor's shim compiles only core, dom, ns, tag and html (~37k of ~390k
  lines). No throwaway spice: the version functions are the real
  `xml-backend-version` / `html-backend-version`.
- **P4 folded into P3.** `tur-dom-core` is its own spice from the start --
  an arena-owned C tree (`c/dom/turdom.c`, iterative walks only) that both
  parsers build into, with the node API, both serializers and `dom-equal?`.
  `SaxEvent` lives there too, so XML and HTML SAX share it literally.
- **The DOM is built on the SAX layer in C.** expat's callbacks record
  events into a queue; the Turmeric SAX driver drains it into `SaxEvent`s and
  the DOM builder drains the same queue into the tree. Text is coalesced
  across tokens and chunks, which is what makes P2's chunk invariance hold.
- **Constant memory needed regions.** Events and attribute cells are `:heap`
  nodes delivered inside a per-chunk `with-region`: 36 MB of XML streams in a
  4 MB process. Event `String`s are borrowed for the handler call
  (`retain` keeps one), not handed over -- handing them over leaks every
  ignored event.
- **Type-shape changes forced by compiler bugs** (each filed under
  `docs/reported/`, see the 2026-10-10 section of its index):
  `Doctype` carries its ids as `String`s, empty when absent (an
  `(Option String)` payload in a `:heap` sum does not compile);
  `ParseError` is `[kind msg line col]` with `parse-error-pos`, and
  `ParseErrorKind` is a `:heap` sum (a struct holding an opaque beside an
  aggregate does not compile). Every `:heap` constructor call allocates a
  node nothing frees, so the parsers build errors from four shared nodes
  (`(syntax-kind)` ... `(io-kind)`). A failed parse then costs the 32-byte
  `ParseError` box (48 B of heap) and no kind node: 80 B down to 48 B.
  The box itself goes away once `ParseError` can be by value, which needs
  the opaque-sibling fix above. It also needed
  `sum-payload-box-leaks-at-match` (now fixed), since a by-value struct in a
  `Result`'s `Err` was boxed and never freed by a `match`.
- **`SaxParser` is a `:copy` struct** of the C handle and the handler, so no
  Turmeric word is stored in C memory and no region note is needed.
- **HTML** never fails (`html-parse : Document`); caps live in `HtmlOpts`
  (depth 512, flattened Blink-style rather than truncated; 64 MiB on the
  `-opts`/file paths). `html/sax` is tokenizer-level and switches raw-text
  states by tag name itself.
- **lexbor bug.** Its tokenizer mishandles a chunk boundary inside the
  `[CDATA[` and DOCTYPE `PUBLIC`/`SYSTEM` lookaheads; the push path holds back
  the first unclosed `<` tail. Found by the fuzz pass.
- **Testing.** The W3C suite could not be vendored: James Clark's `xmltest`
  licence permits redistributing only the unmodified archive. `tur-xml`'s
  `fixtures/run.sh` fetches the official tarball (SHA-256 pinned) and runs
  every `valid/sa` (canonical XML compared byte for byte) and `not-wf/sa`
  document. html5lib's tree-construction tests moved to WPT in June 2026; the
  slice is vendored (MIT) from the last html5lib-tests commit that had them,
  `9329e646`, ~1700 cases with 5 newer-than-lexbor known failures. Both spices
  have a mutation fuzz pass (one-shot vs push, SAX vs DOM, reparse), clean
  under ASan+UBSan.

## Motivation

Turmeric has `json`, `msgpack`, `csv` and `template`, but no way to read
markup. Feeds (RSS/Atom), SVG, config files, SOAP-era APIs, sitemaps, and
scraping all need an XML or HTML parser, and `regex` is the wrong tool for
every one of them. Two access styles are worth shipping because they answer
different needs:

- **SAX (event) API** -- a pull/push stream of start-element, text, end-element
  events with O(depth) memory. For large documents, early exit, and
  constant-memory transforms.
- **DOM (tree) API** -- an in-memory tree with navigation and query. For small
  and medium documents where random access matters more than memory.

DOM is built on SAX, so the SAX layer is the foundation and ships first.

## Scope decisions

| Question | Decision | Why |
|---|---|---|
| One spice or two? | Two: `tur-xml`, `tur-html` | XML is strict and well-formedness errors are fatal; HTML is forgiving and *never* fails (the WHATWG algorithm defines a tree for every input). Different error models, different dependencies. |
| Share a tree type? | Yes, via a small `tur-dom-core` module re-exported by both (see "Shared DOM") | One `Node` vocabulary means a query helper or serializer written once works for both. |
| Write the parsers in Turmeric? | No -- bind a C library | Same call `json` made with yyjson. A conforming HTML5 tree builder is thousands of lines of edge cases. |
| XML backend | **expat** (MIT, small, streaming SAX, ubiquitous) | Smallest dependency that is a true push parser. libxml2 is far larger and drags in a lot we do not need. |
| HTML backend | **lexbor** (Apache-2.0, C, WHATWG-conformant, ships its own DOM) | Gumbo is archived; lexbor is maintained and builds cleanly under CMake like yyjson does. |
| CSS selectors / XPath | Out of scope for the first cut; a follow-up plan | Walkers and `find-all` by tag/attribute cover most use. Selectors are a big surface; do not couple them to the parser release. *(2026-10-11: CSS selectors are follow-up C. XPath stays out.)* |
| Namespaces | XML: supported (expat namespace mode, `{uri}local` split). HTML: only the three built-in foreign-content namespaces (HTML, SVG, MathML). | Matches what each format actually needs. |

## Public API sketch

Types are real types, never `:int` stand-ins (see CLAUDE.md "No Lazy `:int`
Stand-Ins"). Opaque handles over the C pointer, sum types for events.

```turmeric
;; Shared (tur-dom-core)
(defopaque Document :int)          ;; owns the whole tree
(defopaque Node     :int)          ;; borrowed from a Document
(defdata NodeKind (Element) (Text) (Comment) (Cdata) (ProcessingInstruction))

(defstruct SourcePos [line : int col : int])
(defstruct ParseError [msg : String pos : SourcePos])

;; SAX (tur-xml/sax)
(defdata SaxEvent
  (StartElement String (list<Attr>))   ;; name, attributes in document order
  (EndElement   String)
  (Chars        String)
  (Comment      String)
  (Pi           String String)
  (StartCdata) (EndCdata))

(defn sax-parse-string [src : cstr h : (fn [SaxEvent] SaxControl)]
  : (Result unit ParseError))
(defn sax-parse-file   [path : cstr h : (fn [SaxEvent] SaxControl)]
  : (Result unit ParseError))
(defdata SaxControl (Continue) (Stop))   ;; Stop = early exit, still Ok

;; Push parser for streams / sockets
(defopaque SaxParser :int)
(defn sax-parser-new  [h : (fn [SaxEvent] SaxControl)] : SaxParser)
(defn sax-feed!       [p : SaxParser chunk : cstr] : (Result unit ParseError))
(defn sax-finish!     [p : SaxParser] : (Result unit ParseError))

;; DOM (tur-xml/dom, tur-html/dom)
(defn dom-parse-string [src : cstr] : (Result Document ParseError))   ;; xml
(defn html-parse       [src : cstr] : Document)                       ;; never fails
(defn dom-root     [d : Document] : (Option Node))
(defn node-kind    [n : Node] : NodeKind)
(defn node-name    [n : Node] : (Option cstr))
(defn node-text    [n : Node] : String)                 ;; concatenated descendant text
(defn node-attr    [n : Node name : cstr] : (Option cstr))
(defn node-children [n : Node] : (list<Node>))
(defn node-parent  [n : Node] : (Option Node))
(defn find-all     [n : Node tag : cstr] : (list<Node>))
(defn dom-serialize [n : Node] : String)
(defn dom-free     [d : Document] : unit)
```

Final signatures follow whatever `list<T>` / `String` / callback-type shape the
repo has settled on when P1 starts; the point of the sketch is the *kinds* of
type, not the exact spelling. In particular a handler is a typed
`(fn [SaxEvent] SaxControl)`, never an `:int` function pointer.

**Handler as a typeclass.** Consider a `SaxHandler` class (`on-start`,
`on-end`, `on-chars`, ...) as a second front door for callers who want one
instance per consumer instead of one big `match`. It is additive, so defer it
until P3 and decide from usage. *(2026-10-10: deferred; nothing in the
tests or guides wanted it.)*

## Ownership and lifetime

This is where the C-binding spices have been burned before, so it is spelled
out up front.

- A `Document` owns every `Node` under it. A `Node` is a *borrow*: it is valid
  until `dom-free` (or the owning region ends). Do not return a `Node` from a
  function that consumed its `Document`.
- Strings handed to a SAX callback point into the parser's buffer and die when
  the callback returns. The `SaxEvent` constructors must **copy** into owned
  `String`s -- the same copy-at-the-boundary rule the `json` spice's `ownstr`
  module exists for. Ship a self-contained `<spice>/ownstr` rather than
  depending on a compiled-`String` conversion that has known toolchain
  blockers (see the spice String adoption notes).
- Anything that stores a caller's word into memory that can outlive a
  `with-region` bracket must note it (`TUR_REGION_NOTE(word);` in inline-C,
  per CLAUDE.md "Region Store Hooks"). That covers the handler-closure box
  stored in a `SaxParser` and any `Node` cached in a Turmeric-side container.
- Fallible C constructors return a real `(Result Handle E)` through the typed
  `tur_ok_ptr` / `tur_err_int` builders, not a sentinel integer.

## Security (not optional)

XML parsers are a classic attack surface, so the defaults are the safe ones:

- **External entities and DTD loading are off** (no XXE, no SSRF through
  `SYSTEM` identifiers). Opt in per call with an explicit `ParseOpts` field,
  and never from the default path.
- **Entity expansion is bounded** (billion-laughs / quadratic blowup). Use
  expat's amplification limits (2.4.0+), exposed as `max-expansion-ratio`.
- **Depth and size caps** on both formats (`max-depth`, `max-bytes`) with sane
  defaults, so a hostile document cannot exhaust the stack in a recursive DOM
  walker or the heap in a tree build.
- HTML: parsing is safe by construction, but **serializing is not
  sanitizing**. The docs must say that `html-parse` followed by
  `dom-serialize` does not make untrusted HTML safe to embed; point at a
  sanitizer follow-up instead of implying one.
- **Editing and templating reopen the serializer's assumptions.** The
  serializer writes names, comment bodies, PI data and HTML raw text
  verbatim. That is safe only because a parser produced them. Follow-ups A
  and B add the validation that keeps it safe; see "Validation is a security
  property" under B.

## Phases

| Phase | Deliverable | Gate |
|---|---|---|
| P0 | Decide the open questions below; pin expat and lexbor versions; confirm both build under `:cmake-deps` on macOS (AppleClang 21) and Linux, including `-Wint-conversion` hygiene | A throwaway spice links and calls each library's version function |
| P1 | `tur-xml/sax`: `sax-parse-string`, `sax-parse-file`, `SaxEvent`, `ParseError` with line/col, safe defaults | Fixtures for well-formed, malformed (each error class), entities, CDATA, namespaces, UTF-8 and UTF-16 input, early `Stop` |
| P2 | `tur-xml/sax` push parser (`SaxParser`, `sax-feed!`) | Feed the same document one byte at a time and in random chunk sizes; events are identical to the one-shot parse |
| P3 | `tur-xml/dom` built on the SAX layer; navigation, `find-all`, `node-attr`, `node-text`, `dom-serialize` | Round-trip `parse -> serialize -> parse` is structurally equal; a leak-check fixture (`requires.leak-check`) covers `dom-free` |
| P4 | `tur-dom-core` extracted from P3 so HTML can share it | XML fixtures unchanged after the extraction |
| P5 | `tur-html`: `html-parse` over lexbor, mapped onto the shared `Node` API; fragment parsing (`html-parse-fragment ctx-tag src`) | Tag-soup corpus (unclosed `<p>`, misnested `<b><i>`, tables, `<script>`/`<style>` raw text), plus a slice of the html5lib tree-construction tests |
| P6 | `tur-html/sax`: tokenizer-level events for HTML (no tree) | Same event vocabulary as XML SAX, plus `Doctype` |
| P7 | Guides: one for each spice, plus a "choosing SAX or DOM" section in the XML guide; `;;;` docstrings on every export | `tools/gendocs.py` and `tools/genspices.py` render both without warnings |

The phases are an ordering of the work, not a PR boundary: the whole plan is
expected to land as a single PR in `turmeric-spices`. Each phase's gate still
has to hold before the next phase builds on it.

## Testing

- Fixtures live in the spice (`tests/*.tur`), matching how `json` is tested,
  using the `test` spice.
- A conformance corpus: the W3C XML Conformance Test Suite (valid / not-wf /
  invalid sets) for P1, html5lib-tests for P5. Vendor only a curated slice
  and record the upstream commit so it is reproducible. *(As built: the W3C
  slice is fetched and checksum-pinned instead -- see Outcome.)*
- A fuzz pass (libFuzzer or a simple mutation loop over the corpus) for both
  SAX entry points before the first release. Parsers of untrusted input should
  not ship unfuzzed.
- Leak policy as in CLAUDE.md: the compiler path is leak-checked;
  a leak in *emitted* code is invisible to `run.sh`, so the DOM free path uses
  `tests/run-leak-check.sh` with a `requires.leak-check` marker.

## Open questions

1. **Backend license posture.** expat (MIT) and lexbor (Apache-2.0) are both
   permissive; confirm Apache-2.0's patent/NOTICE terms are acceptable for the
   spices repo's MIT license before vendoring lexbor. *Answered: neither is
   vendored -- both are fetched and built by `tur fetch` -- so no NOTICE file
   is redistributed. The test data is the licence question that mattered (see
   Outcome).*
2. **Where does `Node` text live for HTML?** lexbor stores UTF-8 internally;
   confirm `node-text` can return a borrowed `cstr` for single text nodes and
   only allocates a `String` when concatenating. *Answered: the HTML tree is
   copied into dom-core's arena, so a single node's data is the borrowed
   `node-value : (Option cstr)`, and `node-text` (always a concatenation,
   DOM `textContent`) returns an owned `String` -- for both formats.*
3. **Callback style for SAX.** A closure per call (above) versus a pull
   iterator over `seq`. A pull API is nicer to compose but needs a coroutine
   or a buffered event queue; the closure form is the simplest correct P1.
   Revisit once `seq` over effects is settled. *Answered: closures, as
   sketched. The buffered event queue exists anyway (it is how the C
   callbacks hand events to Turmeric), so a pull iterator is now a small
   addition when `seq` is ready.*
4. **Streaming HTML.** lexbor can tokenize incrementally; whether P6 exposes a
   push parser like P2 depends on whether anyone needs it before P5 ships.
   *Answered: exposed (`html-sax-parser-new` / `html-sax-feed!` /
   `html-sax-finish!`); it cost little once the XML driver existed, and its
   chunk-invariance test is what found the lexbor bug.*
5. **Is a pure-Turmeric tokenizer worth it later?** Only if the C dependency
   becomes a portability problem (WASM, for the web REPL). Not a v1 concern.
   *Unchanged.*

## Non-goals

- XPath, XSLT, XQuery, XML Schema / DTD validation.
- CSS selectors *(now follow-up C)*.
- HTML sanitization (follow-up plan; see Security).
- Incremental or streaming *serialization* beyond `dom-serialize`
  *(B4 lifts this for SAX events only)*.
- Editing the tree in place. P3/P5 are read-only; a mutable DOM is a separate
  design because it changes the ownership story above. *(Now follow-up B.
  The arena turns out to keep the ownership story intact.)*

## Follow-ups (planned 2026-10-11)

Three additions on top of the shipped spices. Each is its own
`turmeric-spices` PR, branched from `main`. PRs are never stacked (see
CLAUDE.md).

| Follow-up | What | Depends on |
|---|---|---|
| **B** -- editable tree | In-place mutation of the arena tree, validated so that edits cannot inject structure; streaming SAX rewrite | nothing |
| **C** -- CSS selectors and node paths | A selector engine in dom-core's C layer, plus `node-css-path` | nothing (independent of B) |
| **A** -- markup literals | `#html{...}` / `#xml{...}` with `${...}` holes, escaped by construction, rendered to a string or a tree | A1-A2 (string mode) need nothing; A3-A4 (tree mode, in-place re-render) need B1-B2 merged |

Suggested order: B, then A, with C whenever. B is the substrate both of the
others benefit from. A's string mode can start in parallel with B, because it
needs only the parsers and the escaper.

### Follow-up A: markup literals with templated values (`#html{...}`, `#xml{...}`)

#### Goal

Write markup as markup, with values spliced in, and get escaping by
construction rather than by remembering to call an escape function:

```turmeric
(import dom/markup :refer [html->string])

(defn item [name : cstr href : cstr done : bool] : Fragment
  #html{<li class="item" data-done=${done}>
          <a href=${href}>${name}</a>
        </li>})

;; items : (Cons Fragment), one (item ...) per row
(println (html->string #html{<ul>${items}</ul>}))
```

Nothing like this exists today. There is no string interpolation at all in
the language. The `template` spice is ERB-style, runtime-parsed, takes an
untyped `:int` env, and does **no HTML escaping**
(`spices/template/src/template/render.tur:222`). `tourist`'s `render-html`
wraps it. There is no safe-HTML type in either repo. A markup literal is the
place to fix that.

#### What the reader can do today (verified)

- A user reader macro is `(reader-macros/define 'html :raw-brace
  '(html* $body))` (`src/compiler/reader_macros.c:228-300`). `html` and
  `xml` are not reserved (`kReserved`, `reader_macros.c:20-34`). A `define`
  inside a module is visible to the module's importers (fixture
  `tests/fixtures/reader-macros-cross-module`), so the spice can ship the
  syntax with `(import dom/markup)`.
- Raw mode hands the template one **string literal** of the verbatim text up
  to the matching `}` (`read_raw_body`, `src/compiler/reader.c:3302-3353`).
  No user code runs at read time.
- Real logic comes from composition: the template expands to a call to a
  `defmacro*`, whose body is ordinary Turmeric run by the sandboxed
  interpreter at elaboration time (fixture
  `tests/fixtures/reader-macros-procedural`). It can walk the string and call
  `read-string` on each `${...}` to get a form back
  (`docs/guides/macros-guide.md:160`). The result goes through the full
  typechecker.
- A macro **cannot** see types. Anything type-directed (a `cstr` becomes text,
  a `Fragment` is spliced, `None` omits an attribute) has to be pushed into
  typeclasses on the emitted code.

Three constraints of the raw scanner the design has to live with:

1. It counts `{`/`}` nesting but does not know about quotes, so
   `title="}"` ends the literal early. Balanced braces (CSS in `<style>`, a
   `${ {a + b} }` hole) are fine; an unbalanced one is written `\}`.
2. `\X` becomes `X`, so a literal backslash is `\\`.
3. Forms produced by `read-string` carry the `<read-string>` pseudo-file
   span, so a type error inside `${...}` does not point at the user's line.

None blocks a first cut. (3) is the one worth a compiler change later: a
`read-string-at` that takes a base span, or promoting `#html{` to a built-in
reader in `reader.c` the way `#json(` is (`reader.c:2010-2340`), which also
fixes (1) by scanning quotes. Do the spice-only version first and promote it
only if the spans hurt in practice.

#### Design: parse once with the real parser, mark the holes

The approach is lit-html's, transplanted onto our parsers. It keeps HTML's
real tree-construction rules -- implied end tags, tables, raw text --
without reimplementing them in a macro.

1. **Compile time (`html*`, a `defmacro*`).** A small context tokenizer
   written in Turmeric walks the body and tracks the HTML tokenizer state it
   would be in: data, tag name, attribute name, attribute value
   (quoted or unquoted), comment, raw text (`script`, `style`, `textarea`,
   `title`), plus CDATA for `#xml`. At each `${...}` it decides the hole's
   **context**:
   - *child* (in data): replaced by a comment marker `<!--tur:N-->`.
     Comments are inserted where they appear even in table context, so a
     marker is never foster-parented out of a `<table>` -- a text or element
     marker would be.
   - *attribute value*: replaced by the marker text `tur:N` inside the value.
     `class="a ${b} c"` is one attribute with three parts.
   - *anywhere else* -- tag name, attribute name, inside a comment, inside raw
     text, inside a doctype: a **compile error** naming the hole. These are the
     places where a spliced value could change the document's structure, and
     no escaping makes them safe.

   It emits the static markup with markers, plus one expression per hole,
   wrapped by context:

   ```turmeric
   (tpl-render (tpl-site "<li class=\"item\" data-done=\"tur:0\"><a href=\"tur:1\"><!--tur:2--></a></li>")
               (tcons-of (to-attr done) (tcons-of (to-attr href) (tcons-of (to-child name) (tnil)))))
   ```

2. **First use (runtime, once per literal).** `tpl-site` parses the marked
   string with lexbor (`html-parse-fragment`, context `body`, or the context
   given by `#html[tr]{...}`-style syntax if needed) or expat. It walks the
   tree once and records, per marker, a path (child indices from the fragment
   root) and the kind of slot. It checks that each marker index appears
   exactly once and in the slot the macro predicted. A disagreement is a bug
   in the macro's tokenizer, so it panics with the template text. The
   compiled `Template` is cached, keyed by the literal's address, for the
   life of the process. It is process-lifetime state behind one mutex, and
   tiny.

3. **Instantiation**, in two modes:
   - **String mode (`html->string`, the server-side rendering path).** At
     cache time the template is also split into static serialized chunks
     between holes. Rendering concatenates the chunks with each hole's value
     escaped for its context, reusing `escape_into` (`turdom.c:504`). No tree
     is built. This is the path `tourist` / `httpd` responses want, and its
     cost is O(output).
   - **Tree mode (`html->dom`).** Clone the template tree into a document
     (`node-clone` / `node-import` from follow-up B), then patch each hole
     through its recorded path. The result is a `Fragment` that can be
     inserted into another document or serialized. Because the hole nodes are
     remembered, a `TemplateInstance` can be **re-rendered in place**:
     `tpl-update!` with new values only touches holes whose value changed.
     That is the "inexpensive update" for the common re-render case, and it
     falls out of B's mutators for free.

#### Typing the holes

Two typeclasses, chosen by the macro from the hole's context, so the
*position* picks the class and the *type* picks the instance:

```turmeric
(defclass ToChild [T] (to-child [x : T] : Child))
(defclass ToAttr  [T] (to-attr  [x : T] : AttrVal))

;; children
(definstance ToChild cstr      ...)   ;; text node -- escaped by construction
(definstance ToChild String    ...)
(definstance ToChild int       ...)   ;; decimal text
(definstance ToChild Fragment  ...)   ;; a nested #html{} -- spliced as nodes
(definstance ToChild (Cons Fragment) ...)
(definstance ToChild Node      ...)   ;; deep-copied in (node-import)
(definstance ToChild (Option T) ...)  ;; None renders nothing
(definstance ToChild RawHtml   ...)   ;; explicit opt-out, see below

;; attribute values
(definstance ToAttr cstr  ...)        ;; escaped as an attribute value
(definstance ToAttr int   ...)
(definstance ToAttr bool  ...)        ;; HTML boolean attribute: present or absent
(definstance ToAttr (Option T) ...)   ;; None drops the whole attribute
```

A `Node` or `Fragment` in attribute position has no instance, so it is a
type error at the call site.

**Unescaped HTML is an explicit, greppable act.** `(raw-html s)` wraps a
`cstr` as `RawHtml`. In child position it is *parsed* with
`html-parse-fragment`, using the hole's parent element as context, and the
resulting nodes are spliced in. So it can never unbalance the surrounding
template. It is still not sanitized: a `<script>` in it survives, and the
docstring says so in the words the Security section uses.

What this does **not** do: decide that a URL is safe. `href=${x}` with
`x = "javascript:..."` is escaped correctly and is still a script URL.
Contextual URL filtering belongs with the sanitizer follow-up. Open question
A-1 below is whether `href`/`src`/`action` holes should take a `Url` type
instead of a `cstr`.

#### `#xml{...}`

`#xml{...}` uses the same pipeline over expat, with a well-formed body. CDATA
sections are a raw context, so no holes are allowed inside them. Namespace
prefixes must be declared inside the literal. An undeclared prefix is an expat
error at first use, and the context tokenizer can catch the simple cases at
compile time.

#### Phases

| Phase | Deliverable | Gate |
|---|---|---|
| A1 | `dom/markup`: `reader-macros/define` for `html` and `xml`, the `html*` / `xml*` `defmacro*` expanders and their context tokenizer | Each forbidden hole context is a compile error with a message naming it; fixtures for `\}` and `\\` |
| A2 | `Template` cache, marker recovery, `ToChild` / `ToAttr` and their instances, `html->string` / `xml->string` | Escaping table: `<`, `&`, `"` and `</script>` in every allowed context. A hostile value never changes the parsed structure: `parse(render(t, v))` has the same shape as `parse(render(t, "x"))` for every corpus string `v` |
| A3 | `html->dom` / `Fragment`, nested templates, lists, `raw-html` (needs B1/B2) | Tree mode and string mode agree: `dom-serialize(html->dom t v) == html->string t v` |
| A4 | `TemplateInstance` and `tpl-update!` | Updating with the same values touches nothing (pinned by a counter); updating one hole rewrites one node |
| A5 (optional, compiler) | Real spans for `${...}` holes (`read-string-at`, or a built-in `#html{` reader) | A type error in a hole points at the user's line and column |

#### Open questions

- **A-1.** Should URL-bearing attributes (`href`, `src`, `action`,
  `formaction`, `xlink:href`) require a `Url` type rather than accepting a
  `cstr`? That is safer, but it adds friction to every link. A middle road is
  to accept `cstr` and reject a `javascript:` scheme at render time.
- **A-2.** Should the fragment context be selectable (`#html[tr]{<td>..}`),
  or should templates that start with `<td>`/`<tr>`/`<option>` be detected
  and given the right context automatically? lit-html does the latter.
- **A-3.** Should `tourist`'s `render-html` move onto `#html{}` once A2 lands?
  That would retire the unescaped ERB path for HTML responses.

### Follow-up B: an editable tree

#### What the tree already gives us

The arena tree in `c/dom/turdom.c` is closer to editable than the "read-only"
label suggests:

- Every node already carries `parent`, `first`, `last`, `next`, `prev` and
  `nchildren` (`turdom.c:34-54`), so unlinking a node or inserting before a
  sibling is O(1) pointer surgery. Nothing about the representation is
  append-only.
- The builder entry points (`turdom_append_element`, `turdom_add_attr`,
  `turdom_append_chars`, ...) already exist and are what both parsers call.
  They are C-only today, re-declared `extern` in `turxml.c`/`turhtml.c`.
- The arena already abandons memory: adjacent-text coalescing allocates the
  joined buffer and drops the old one (`turdom.c:246-260`). Garbage inside a
  document is not new.
- Every walk is iterative, so a mutation cannot create a stack hazard for any
  reader, however deep the result.

What makes it read-only is the API and the ownership prose, not the data
structure.

#### Options weighed

| Mechanic | Cost per edit | Fit | Verdict |
|---|---|---|---|
| **In-place mutation of the arena tree** | O(1) link/unlink; O(attrs) for set-attr; O(len) to copy a new string into the arena | Same handles, same serializer, same selectors; arena keeps every old borrow valid | **Chosen.** |
| Persistent tree with path copying (zipper, structural sharing) | O(depth) node copies per edit, plus a refcount or GC for shared subtrees | The arena frees in one pass and has no per-node lifetime, so sharing across versions means either never freeing or adding refcounts to every node; `Node` stops being a plain pointer | Rejected. The cheapness is illusory once lifetime is accounted for. |
| Rebuild from a transformed SAX stream | O(n) per pass, O(depth) memory | Excellent for "rename/drop/insert as it streams by" on big documents; useless for "change one attribute in a tree I already hold" | **Also ship**, as the streaming counterpart (B4). |
| Rebuild the whole DOM from a Turmeric-side value (hiccup tree) per edit | O(n) per edit | That is follow-up A's builder, not an update mechanic | Covered by A. |

#### Ownership after mutation -- the arena makes it simple

The rule that every borrowed `cstr` lives until `dom-free` survives
mutation unchanged, *because nothing is freed before `dom-free`*:

- `set-attr!` copies the new value into the arena and repoints the attribute;
  the old value's bytes stay where they were. A `cstr` a caller got from
  `node-attr` a moment earlier is still valid -- it now reads the old value.
- `node-remove!` unlinks a subtree; the nodes stay allocated. A `Node` handle
  to a removed node is a **detached** node, not a dangling pointer: it can be
  read, re-inserted, or ignored. There is no use-after-free in the API at all.
- The price is garbage. Add `dom-garbage-bytes` (bytes abandoned by edits:
  replaced strings, coalesced text, attributes removed) next to
  `dom-live-bytes`, and `dom-compact : Document -> Document`, which copies the
  reachable tree into a fresh arena and returns a new document. Compaction is
  the one operation that invalidates handles, and it says so by returning a
  new `Document` the caller must `dom-free` separately. A long-running editor
  calls it when garbage passes some ratio; a script that edits then
  serializes never needs it.
- `dom-node-count` keeps meaning "nodes this document's arena holds", which
  now includes detached nodes. Say so in its docstring rather than paying an
  O(subtree) walk on every detach.

No Turmeric word is ever stored into C memory: every mutator copies its
`cstr`/`String` argument into the arena at the boundary, exactly as the
parsers do. So no `TUR_REGION_NOTE` is needed, and the plan should keep it
that way -- a "user data" slot on a node that holds a Turmeric value is out of
scope, because it would put the tree on the region-hook list.

#### API sketch (`dom/edit`, a new module in `tur-dom-core`)

```turmeric
;; creating (detached, owned by d's arena until inserted)
(defn dom-new          [flavor : DomFlavor] : Document        ;; empty doc
(defn create-element   [d : Document name : cstr] : (Result Node EditError)
(defn create-element-ns [d : Document uri : cstr qname : cstr] : (Result Node EditError)
(defn create-text      [d : Document s : cstr] : Node
(defn create-comment   [d : Document s : cstr] : (Result Node EditError)
(defn node-import      [d : Document n : Node] : Node          ;; deep copy across arenas
(defn node-clone       [n : Node] : Node                       ;; deep copy, same arena

;; tree surgery -- all O(1) except the cycle check, O(depth)
(defn node-append!        [parent : Node child : Node] : (Result unit EditError)
(defn node-prepend!       [parent : Node child : Node] : (Result unit EditError)
(defn node-insert-before! [ref : Node child : Node] : (Result unit EditError)
(defn node-insert-after!  [ref : Node child : Node] : (Result unit EditError)
(defn node-replace!       [old : Node new : Node] : (Result unit EditError)
(defn node-remove!        [n : Node] : unit                    ;; detach; no-op if detached
(defn node-unwrap!        [n : Node] : unit                    ;; children take n's place

;; content
(defn set-attr!     [el : Node name : cstr value : cstr] : (Result unit EditError)
(defn remove-attr!  [el : Node name : cstr] : bool             ;; true when it existed
(defn set-text!     [n : Node s : cstr] : (Result unit EditError)  ;; textContent=
(defn set-value!    [n : Node s : cstr] : (Result unit EditError)  ;; text/comment/PI data
(defn rename!       [el : Node name : cstr] : (Result unit EditError)
(defn set-inner-html! [el : Node src : cstr] : unit            ;; tur-html only: fragment parse with el as context

;; housekeeping
(defn dom-garbage-bytes [d : Document] : int
(defn dom-compact       [d : Document] : Document
(defn node-attached?    [n : Node] : bool

(defdata EditError :copy
  (WrongDocument)     ;; child belongs to another arena: use node-import
  (WouldCycle)        ;; inserting an ancestor under its descendant
  (NotAContainer)     ;; children under a text/comment/PI/doctype node
  (BadName)           ;; not an XML Name / not a valid HTML tag or attr name
  (BadContent))       ;; "--" in a comment, "?>" in a PI, "</script" in raw text
```

`DomFlavor` (`XmlFlavor` / `HtmlFlavor`) becomes public because `dom-new`
needs it; today the flavour is only set by whichever parser built the tree.

#### Validation is a security property, not pedantry

The serializer escapes text and attribute *values* (`escape_into`,
`turdom.c:504`) but writes everything else verbatim: element and attribute
names, comment bodies, PI data, doctype ids, and the text children of HTML
`<script>`/`<style>` (`turdom.c:560-610`). For a parsed tree that is correct,
because the parser could only have produced well-formed content there. An
editable tree breaks that assumption, so every mutator that can put bytes in
one of those places validates them:

- **Names** (`create-element`, `set-attr!`, `rename!`): XML `Name`
  production for XML documents; for HTML, the tokenizer's acceptance set (no
  ASCII whitespace, `/`, `>`, `=`, quotes, NUL). Otherwise `BadName`.
  `<${tag} onload=...>` through a name is the injection this stops.
- **Comment bodies**: reject `--` (XML) and `-->`/`--!>` (HTML).
- **PI data**: reject `?>`.
- **HTML raw text**: a text node whose parent is `<script>`/`<style>` (at
  insert time *or* when its parent is renamed into one) rejects content
  containing `</script` / `</style` (ASCII case-insensitive). This is the
  classic breakout; the serializer cannot escape there, so the editor has to
  refuse.
- **Doctype ids** containing both quote characters: `BadContent`
  (`put_quoted_id` cannot represent them).

A fuzz property pins it: for any sequence of successful edits,
`parse(serialize(doc))` is `dom-equal?` to `doc`. Any mutator that lets
injected structure through breaks that equality.

#### Live walks and snapshots

`find-all` (and `select`, below) return a `(Cons Node)` snapshot, so editing
while iterating one is safe. The cursor-style C walks (`turdom_next_in(root,
from)`) are not: detaching `from` mid-walk ends the walk early. Either do not
export a cursor walk, or document that a detached cursor ends iteration (it
does -- `from->parent` is NULL -- but that is an accident to be pinned with a
test, not a contract).

#### Streaming edits (B4)

For a document too large to hold, the cheap update is a transform over the
SAX stream: a `SaxWriter` that serializes `SaxEvent`s (escaping as
`turdom_serialize` does, sharing `escape_into`), and `sax-transform`, which
feeds each event to a `(fn [SaxEvent] (Cons SaxEvent))` and writes what comes
back. Rename, drop, wrap and inject at O(depth) memory. This lifts the
"incremental serialization" non-goal for events only; it does not add a
streaming DOM serializer.

#### Phases

| Phase | Deliverable | Gate |
|---|---|---|
| B1 | C mutators in `turdom.c` (detached nodes, link/unlink, insert-before, attr set/remove, value set, rename, clone, import, compact, garbage counter), all iterative | C-level unit test; ASan clean; `dom-live-bytes` returns to 0 after `dom-free` of an edited document |
| B2 | `dom/edit` module and `EditError`, with every validation above | One fixture per `EditError` case, including a `</script>` breakout and a `<x onload=...>` name injection; a fuzz pass of random edit sequences holding `parse(serialize(d)) == d` for both flavours |
| B3 | `set-inner-html!` / `node-outer-html` in `tur-html` via `html-parse-fragment` + `node-import` | html5lib fragment cases reparsed through `set-inner-html!` match |
| B4 | `SaxWriter`, `sax-transform` | Identity transform reproduces `dom-serialize` of the same input byte for byte |

### Follow-up C: CSS selectors and node paths

#### Engine choice

lexbor has a selectors module, but it matches against lexbor's DOM, and
`tur-html` copies lexbor's tree into the dom-core arena and throws lexbor's
away (Open question 2). Using it would mean keeping two trees alive for HTML
and having nothing for XML. So: **a small selector engine in `tur-dom-core`'s
C layer**, over `turdom_node`, shared by both flavours. A tokenizer plus a
compiled form plus a right-to-left matcher is on the order of 1.5-2k lines of
C; the CSS Syntax tokenizer is the fiddly part (escapes, `\:` in names,
quoted strings), not the matcher.

#### Supported grammar (first cut)

Selectors Level 3 plus the cheap, widely used Level 4 pieces:

- type, universal, `#id`, `.class`, `ns|type`, `*|type`, `|type`
- attributes: `[a]`, `=`, `~=`, `|=`, `^=`, `$=`, `*=`, and the `i` / `s`
  flags
- combinators: descendant, `>`, `+`, `~`
- `:root`, `:empty`, `:first-child`, `:last-child`, `:only-child`,
  `:first-of-type`, `:last-of-type`, `:only-of-type`, `:nth-child(An+B)`,
  `:nth-last-child`, `:nth-of-type`, `:nth-last-of-type`,
  `:nth-child(An+B of S)`
- `:not(list)`, `:is(list)`, `:where(list)`
- selector lists (`a, b`) with results in document order, deduplicated

Deferred: `:has()` (it is a subtree walk per candidate -- O(n^2) on the
wrong input; add it with a step budget, behind its own phase). Rejected with
a compile error, never silently false: pseudo-elements, and every
dynamic/UI-state pseudo-class (`:hover`, `:focus`, `:visited`, `:checked`,
...) -- a static tree has no state to match, and a selector that silently
matches nothing is a bug report waiting to happen.

#### Case and namespace rules

- **HTML documents**: type selectors and attribute *names* match ASCII
  case-insensitively on HTML-namespace elements (consistent with
  `turdom_element_matches`); attribute *values* are case-sensitive except
  for the HTML spec's list of case-insensitive attributes (`type`, `lang`,
  `dir`, ...), and the `i`/`s` flags override. `#id` and `.class` are
  case-sensitive (no-quirks behaviour). dom-core does not record quirks mode;
  say so and treat every document as no-quirks rather than plumb a flag
  through for a legacy mode.
- **XML documents**: everything case-sensitive. `ns|type` needs a prefix map,
  passed at compile time (`selector-compile-ns`); prefixes in the document are
  irrelevant, URIs are compared. With no default namespace, an unprefixed type
  selector matches the local name in any namespace (the CSS rule). Note this
  differs from `find-all`, which matches the qualified name as written --
  document the difference rather than change either.
- `atom:feed` is not a type selector (the `:` starts a pseudo-class). Write
  `atom|feed` with a prefix map, or escape it as `atom\:feed` to match the
  qualified name.

#### API sketch (`dom/select`)

```turmeric
(defopaque Selector :ptr<void>)          ;; compiled; malloc'd, not in an arena
(defstruct SelectorError :copy [msg : String offset : int])

(defn selector-compile    [src : cstr] : (Result Selector SelectorError)
(defn selector-compile-ns [src : cstr ns : (Cons (Pair cstr cstr))]
  : (Result Selector SelectorError)
(defn selector-free       [s : Selector] : unit

(defn select        [root : Node s : Selector] : (Cons Node)    ;; document order
(defn select-first  [root : Node s : Selector] : (Option Node)
(defn node-matches-selector? [n : Node s : Selector] : bool     ;; Element.matches
(defn node-closest  [n : Node s : Selector] : (Option Node)     ;; Element.closest

;; one-shot conveniences: compile, run, free; a bad selector is an Err
(defn query     [root : Node src : cstr] : (Result (Cons Node) SelectorError)
(defn query-one [root : Node src : cstr] : (Result (Option Node) SelectorError)

;; CSS paths: a selector that picks exactly this node
(defn node-css-path [n : Node] : String
```

`select` searches `root`'s descendants, like `find-all` and like
`querySelectorAll` (whose `:scope`-less selectors still match against the
whole document's ancestry -- `div p` from a `<p>`'s parent finds the `<p>`
even when the `div` is above the root). Follow the DOM here; it surprises
people either way, and following the spec means the WPT tests apply.

The compiled form follows `regex-compile`'s shape (`Result` on compile, a
handle to free), so a selector in a hot loop is compiled once.

**`node-css-path`** walks up from `n` and emits `tag:nth-child(k)` steps
joined by `>`, stopping early at an ancestor with an `id` that is unique in
the document (checked, not assumed). The property test is the point:
`select-first(doc, node-css-path(n)) == n` for every element of every
corpus document. It is also what makes a selector-based scraper's
"which node was that?" debuggable.

#### Interaction with editing

Selectors read the same links the editor writes, so they need nothing
special; `select` returns a snapshot, so `(for n (select d s) (node-remove!
n))` is safe. The one caveat is `node-css-path`: a path is only meaningful
for the tree it was computed on.

#### Testing

- WPT `css/selectors` has some static-DOM `querySelectorAll` tests worth
  porting; most of WPT selectors is reftests and does not apply. Check the
  licence (3-clause BSD) and record the commit, as for html5lib.
- The `css-what` / `css-select` (BSD-2) and nwsapi test fixtures are
  plain selector-and-expected-match tables; a slice is a cheap conformance
  set. Licence-check before vendoring.
- Differential fuzz: random selectors from a small grammar, run against
  random corpus documents, compared to a deliberately naive reference matcher
  (written in Turmeric over `dom/core`, left-to-right, slow and obvious). The
  naive matcher is the oracle; the C engine must agree.
- The `node-css-path` round-trip property above.

#### Phases

| Phase | Deliverable | Gate |
|---|---|---|
| C1 | Tokenizer and compiler (`selector-compile`, `SelectorError` with a byte offset) | Every supported construct compiles; every rejected construct is an error naming it |
| C2 | Matcher, `select` / `select-first` / `node-matches-selector?` / `node-closest`, `query` | Ported test tables pass on XML and HTML; differential fuzz agrees with the naive oracle |
| C3 | `node-css-path` | Round-trip property over both corpora |
| C4 | `:has()` with a step budget | Its own fixtures, including a pathological input that hits the budget |

# Plan: DOM and SAX Parser Spices for XML and HTML

> Status: planning (2026-10-07).
> Spices: `tur-xml` and `tur-html` (new, first-party, in `turmeric-spices`)
> Pattern to follow: `spices/json` (vendored C library via `:cmake-deps`,
> typed handles over an `:int` carrier, a self-contained `ownstr` module).

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
| CSS selectors / XPath | Out of scope for the first cut; a follow-up plan | Walkers and `find-all` by tag/attribute cover most use. Selectors are a big surface; do not couple them to the parser release. |
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
until P3 and decide from usage.

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

Each phase lands as its own PR off `main`; P1 is useful alone.

## Testing

- Fixtures live in the spice (`tests/*.tur`), matching how `json` is tested,
  using the `test` spice.
- A conformance corpus: the W3C XML Conformance Test Suite (valid / not-wf /
  invalid sets) for P1, html5lib-tests for P5. Vendor only a curated slice
  and record the upstream commit so it is reproducible.
- A fuzz pass (libFuzzer or a simple mutation loop over the corpus) for both
  SAX entry points before the first release. Parsers of untrusted input should
  not ship unfuzzed.
- Leak policy as in CLAUDE.md: the compiler path is leak-checked;
  a leak in *emitted* code is invisible to `run.sh`, so the DOM free path uses
  `tests/run-leak-check.sh` with a `requires.leak-check` marker.

## Open questions

1. **Backend license posture.** expat (MIT) and lexbor (Apache-2.0) are both
   permissive; confirm Apache-2.0's patent/NOTICE terms are acceptable for the
   spices repo's MIT license before vendoring lexbor.
2. **Where does `Node` text live for HTML?** lexbor stores UTF-8 internally;
   confirm `node-text` can return a borrowed `cstr` for single text nodes and
   only allocates a `String` when concatenating.
3. **Callback style for SAX.** A closure per call (above) versus a pull
   iterator over `seq`. A pull API is nicer to compose but needs a coroutine
   or a buffered event queue; the closure form is the simplest correct P1.
   Revisit once `seq` over effects is settled.
4. **Streaming HTML.** lexbor can tokenize incrementally; whether P6 exposes a
   push parser like P2 depends on whether anyone needs it before P5 ships.
5. **Is a pure-Turmeric tokenizer worth it later?** Only if the C dependency
   becomes a portability problem (WASM, for the web REPL). Not a v1 concern.

## Non-goals

- XPath, XSLT, XQuery, XML Schema / DTD validation.
- CSS selectors (follow-up plan).
- HTML sanitization (follow-up plan; see Security).
- Incremental or streaming *serialization* beyond `dom-serialize`.
- Editing the tree in place. P3/P5 are read-only; a mutable DOM is a separate
  design because it changes the ownership story above.

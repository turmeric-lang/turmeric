# Plan: DOM and SAX Parser Spices for XML and HTML

> Status: **done** -- archived 2026-10-25. P0-P7 landed 2026-10-10 as
> `tur-xml`, `tur-html` and `tur-dom-core` 0.1.0 (turmeric-spices #97,
> `d9ab97f`); see "Outcome" below. Follow-up B (an editable tree, B1-B4)
> landed 2026-10-16 as 0.2.0 (turmeric-spices #106, `7465a08`); see
> "Follow-up B" below. The compiler-bug workarounds both left in place were
> dropped in turmeric-spices #110.
> Spices: `tur-xml`, `tur-html` and `tur-dom-core` (new, first-party, in
> `turmeric-spices`)
> Pattern to follow: `spices/json` (vendored C library via `:cmake-deps`,
> typed handles over an `:int` carrier, a self-contained `ownstr` module).
>
> The follow-ups not yet built -- A (markup literals, `#html{...}` /
> `#xml{...}`), C (CSS selectors and node paths) and D (hiccup) -- moved to
> [xml-html-followups-plan](../upcoming/spices/xml-html-followups-plan.md)
> on 2026-10-25, with their decided open questions.

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
| CSS selectors / XPath | Out of scope for the first cut; a follow-up plan | Walkers and `find-all` by tag/attribute cover most use. Selectors are a big surface; do not couple them to the parser release. *(2026-10-11: CSS selectors are follow-up C, now in [xml-html-followups-plan](../upcoming/spices/xml-html-followups-plan.md). XPath stays out.)* |
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
- CSS selectors *(now follow-up C, in [xml-html-followups-plan](../upcoming/spices/xml-html-followups-plan.md))*.
- HTML sanitization (follow-up plan; see Security).
- Incremental or streaming *serialization* beyond `dom-serialize`
  *(B4 lifts this for SAX events only)*.
- Editing the tree in place. P3/P5 are read-only; a mutable DOM is a separate
  design because it changes the ownership story above. *(Now follow-up B.
  The arena turns out to keep the ownership story intact.)*

## Follow-ups

Four follow-ups were planned on top of the shipped spices (2026-10-11; D
added 2026-10-16). B, the editable tree, is done and recorded below. A
(markup literals), C (CSS selectors and node paths) and D (hiccup) are not
started and live in [xml-html-followups-plan](../upcoming/spices/xml-html-followups-plan.md).

## Follow-up B: an editable tree

### Status (2026-10-16; merged as turmeric-spices #106)

B1-B4 landed in turmeric-spices #106 (dom-core, tur-xml and tur-html
0.2.0). Where the code differs from the sketch
below, B1-B2 first:

- **Validation lives in C.** `turdom_check_*` hold the rules and the
  `turdom_edit_*` entry points check then mutate in one call, so nothing can
  skip a check by calling functions in the wrong order; `dom/edit` maps error
  codes to `EditError`. The raw mutators stay unchecked, for the parsers.
- **`EditError` has nine cases**, not five: `BadPlacement` (a second root or
  doctype, top-level text, a doctype in an element or after the root, an
  element under an HTML raw-text element, a child of an HTML void element,
  CDATA or a PI in an HTML document), `Detached` (the reference node has no
  parent), `WrongKind` (e.g. `set-value!` on an element) and `OutOfMemory`
  join the original five.
- **More is refused than the sketch listed**, each found by the fuzz or the
  spec: the `plaintext` element (its end tag is never recognized, so it would
  swallow every later sibling); `<!--` in `<script>` text (the script-data
  escape states can stop `</script>` from closing it); CDATA and PIs in HTML
  documents (its parser produces neither -- `<![CDATA[` inside SVG reads as
  text); `\r` in comments, CDATA, PI data and raw text (it cannot be escaped
  there and comes back as `\n`); `]]>` in CDATA (it comes back as two
  sections); PI data starting with whitespace; and in XML, a prefix bound to
  two URIs on one element.
- **Serializer changes.** The XML serializer declares the namespace of a node
  an edit created or renamed wherever its prefix is unbound, tracking scope
  as it walks; nodes from a parser are never touched, so parsed trees
  serialize exactly as before. The HTML serializer escapes `\r` as `&#13;`
  (parsed HTML never holds one).
- **HTML elements made by `create-element` go in the XHTML namespace**, as
  `createElement` does and as the parser builds them; otherwise an edited
  tree never compares equal to a reparse.
- **API additions:** `set-attr-ns!`, `rename-ns!`, `create-cdata`,
  `create-pi`, `dom-flavor`, `edit-error-message`; `create-text`,
  `node-import` and `node-clone` return `Result` (XML text can be invalid; a
  cross-flavour import validates the subtree); `dom-compact` returns
  `(Option Document)`.
- **The cycle check skips childless nodes**, which cannot be ancestors, so
  building a deep chain one append at a time stays linear rather than
  O(depth) per insert.
- **B2's round-trip property, as tested.** For XML it holds exactly over
  random edit sequences, except that the parser lists namespace declarations
  before other attributes, so the fuzz keeps `xmlns` attributes out of its
  pools. For HTML it cannot hold for every valid edit, because the tree
  builder re-nests some trees on purpose (a `<div>` in a `<p>`, an HTML
  element inside `<svg>`); the fuzz checks it over trees a predicate says the
  parser keeps, and counts the rest (about 6.5% of iterations). 18,000
  iterations per flavour held.

B3 (`html/edit`): `set-inner-html!` parses with the fragment algorithm in the
element's own context, imports the result and places it with the `dom/edit`
checks, so a refused call changes nothing -- `BadContent` for markup that
would close a raw-text element early, `BadPlacement` for a void element,
`WrongKind` for an SVG/MathML element or an XML document. Every html5lib
fragment test runs a second time through it.

B4 (`dom/writer`, `xml/transform`, `html/transform`):

- **The gate holds as written only with namespaces off.** With namespaces
  on, SAX names are Clark names and `xmlns` attributes are not reported, so
  the writer has to choose prefixes and the bytes differ from
  `dom-serialize`. The gates as tested:
  - XML, namespaces off: the identity output IS `dom-serialize` of the same
    document, byte for byte -- unit tests, every xmltest `valid/sa`
    document (`fixtures/conformance.tur`, which CI fetches), and a fuzz
    property;
  - XML, namespaces on: the output parses to the same event stream;
  - HTML: the SAX stream is tokenizer-level, so the gate is too -- the
    output tokenizes to the same events. 1,554 of 1,709 html5lib inputs
    pass; each of the other 155 is checked to be a refusal the writer makes
    on purpose (a `<plaintext>` start tag; a comment holding `<!--`; raw
    text holding `<!--` or a `</script` lookalike -- html5lib's script-data
    tests are most of them). Ordinary pages parse to the same tree.
- **Ownership:** the transform owns the event it is given (returns it or
  releases it); the driver writes every returned event, then releases it.
  `sax-event-retain` (dom/event) is how the driver hands over the input
  event. The bridge to C passes only `cstr`s, so the SAX driver's per-chunk
  region still rewinds the event nodes.
- **The C writer holds each start tag until the next event**, so `xmlns`
  attributes on a tag are in scope for its own Clark names, and a transform
  can choose prefixes by writing one. Otherwise it reuses a binding in scope
  or declares one (a default for an element, `nsN` for an attribute), and a
  plain name under a writer-made default gets `xmlns=""`.
- **XML names are Names, not QNames**, for elements, attributes and PI
  targets: a namespaces-off document may use `a:b:c` (xmltest's `valid/sa/012`
  has an attribute named `:`; the fuzz found a PI target `xp:ml`).
- **More is refused than the B2 checks:** a duplicate attribute, and in XML
  an end tag that does not match, a second root, text outside it, a missing
  root at the end. `WriteError` is `(Rejected EditError)` or `(IoFailed)`;
  transforms fail with `TransformError` (`InputError`, `OutputError`).
- **HTML specifics:** an end tag right after a void element's start (`<br/>`)
  is written self-closed, so it tokenizes alike and parses to one `<br>`; a
  doctype keeps its public and system ids (the tree serializer drops them),
  so the quirks mode survives.
- **Streaming:** `sax-writer-to-file` writes in 64 KiB chunks, and
  `sax-transform-file` streams XML file to file. tur-html has no file SAX
  entry point; `sax-transform-handler` plugs into its push parser instead.
- Two compiler bugs found on the way, both fixed the same day:
  [heap-adt-word-slot-missing-cast](heap-adt-word-slot-missing-cast.md),
  and a second symptom of
  [emit-c-segfault-on-colliding-private-helper](emit-c-segfault-on-colliding-private-helper.md)
  (a program could not import both spices' SAX modules). The spices'
  workarounds for both were dropped in turmeric-spices #110.

### What the tree already gives us

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

### Options weighed

| Mechanic | Cost per edit | Fit | Verdict |
|---|---|---|---|
| **In-place mutation of the arena tree** | O(1) link/unlink; O(attrs) for set-attr; O(len) to copy a new string into the arena | Same handles, same serializer, same selectors; arena keeps every old borrow valid | **Chosen.** |
| Persistent tree with path copying (zipper, structural sharing) | O(depth) node copies per edit, plus a refcount or GC for shared subtrees | The arena frees in one pass and has no per-node lifetime, so sharing across versions means either never freeing or adding refcounts to every node; `Node` stops being a plain pointer | Rejected. The cheapness is illusory once lifetime is accounted for. |
| Rebuild from a transformed SAX stream | O(n) per pass, O(depth) memory | Excellent for "rename/drop/insert as it streams by" on big documents; useless for "change one attribute in a tree I already hold" | **Also ship**, as the streaming counterpart (B4). |
| Rebuild the whole DOM from a Turmeric-side value (hiccup tree) per edit | O(n) per edit | That is a builder, not an update mechanic | Covered by D (hiccup), in [xml-html-followups-plan](../upcoming/spices/xml-html-followups-plan.md). |

### Ownership after mutation -- the arena makes it simple

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

### API sketch (`dom/edit`, a new module in `tur-dom-core`)

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

### Validation is a security property, not pedantry

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

### Live walks and snapshots

`find-all` (and `select`, below) return a `(Cons Node)` snapshot, so editing
while iterating one is safe. The cursor-style C walks (`turdom_next_in(root,
from)`) are not: detaching `from` mid-walk ends the walk early.
*Decided 2026-10-16: the cursor walk is not exported.* Snapshots are the
only public traversal, so there is no edit-while-walking contract to keep.
Revisit only if a caller needs O(1)-memory traversal of a tree it is not
editing.

### Streaming edits (B4)

For a document too large to hold, the cheap update is a transform over the
SAX stream: a `SaxWriter` that serializes `SaxEvent`s (escaping as
`turdom_serialize` does, sharing `escape_into`), and `sax-transform`, which
feeds each event to a `(fn [SaxEvent] (Cons SaxEvent))` and writes what comes
back. Rename, drop, wrap and inject at O(depth) memory. This lifts the
"incremental serialization" non-goal for events only; it does not add a
streaming DOM serializer.

### Phases

| Phase | Deliverable | Gate |
|---|---|---|
| B1 | C mutators in `turdom.c` (detached nodes, link/unlink, insert-before, attr set/remove, value set, rename, clone, import, compact, garbage counter), all iterative | C-level unit test; ASan clean; `dom-live-bytes` returns to 0 after `dom-free` of an edited document |
| B2 | `dom/edit` module and `EditError`, with every validation above | One fixture per `EditError` case, including a `</script>` breakout and a `<x onload=...>` name injection; a fuzz pass of random edit sequences holding `parse(serialize(d)) == d` for both flavours |
| B3 | `set-inner-html!` / `node-outer-html` in `tur-html` via `html-parse-fragment` + `node-import` | html5lib fragment cases reparsed through `set-inner-html!` match |
| B4 | `SaxWriter`, `sax-transform` | Identity transform reproduces `dom-serialize` of the same input byte for byte (namespaces off; with namespaces on and for HTML, the same events back -- see Status) |


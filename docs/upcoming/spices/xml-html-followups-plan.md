# Plan: XML/HTML spice follow-ups -- markup literals, CSS selectors, hiccup

> Status: in progress (2026-10-28). Done: C1-C4 (`dom/select`; turmeric-spices
> branch `claude/ecstatic-cori-ikodwq`), A1-A2 (`dom/holes`, `dom/markupc`,
> tur-html `html/markup`, tur-xml `xml/markup`), A3-A4 (`html->dom`,
> `#html-live{...}` and `tpl-update!`) and D1-D3 (tur-html `html/hiccup`:
> the macro, result-position rewriting, `hiccup->dom` / `hiccup->xml-string`
> / `hiccup->xml-dom`, `node->hiccup`), all on turmeric-spices branch
> `claude/ecstatic-cori-ikodwq-markup`, since D builds on A2's hole classes.
> D2's `RenderHtml` and tourist's `html-response` (A-3) are on the same
> branch; A5 (`read-string-at`) is on turmeric's `claude/ecstatic-cori-ikodwq`.
> Not started: A6.
> What changed against the plan while
> building it is under "Implementation notes (2026-10-28)" at the end. The
> compiler fixes A and D needed are on turmeric's
> `claude/ecstatic-cori-ikodwq`; the spices markup branch's CI builds against
> turmeric `main`, so it goes green once those land.
> Planned 2026-10-11 (A, C) and 2026-10-16 (D) as
> follow-ups to [xml-html-parsers-plan](../../archive/xml-html-parsers-plan.md), which shipped
> `tur-xml`, `tur-html` and `tur-dom-core` and is archived with its follow-up
> B (the editable tree, turmeric-spices #106). Split out 2026-10-25; the
> letters A, C and D are kept so the spices' comments and the filed reports
> still match.
> Open questions A-1 to A-4 decided 2026-10-16, with A6 (escaping for script,
> style and srcdoc holes) planned for later. D's open questions D-1 to D-3
> decided the same day; D-1 was revised after a prototype: phase D1b rewrites
> markup vectors in result positions.
> Spices: `tur-dom-core`, `tur-html`, `tur-xml` (turmeric-spices).
> **Track:** post-v1 -- nothing on the v1 line depends on it.

## Overview

Three additions on top of the shipped spices. Each is its own
`turmeric-spices` PR, branched from `main`. PRs are never stacked (see
CLAUDE.md). "B" below is the archived plan's
[follow-up B](../../archive/xml-html-parsers-plan.md#follow-up-b-an-editable-tree): the
`dom/edit` mutators (`node-clone`, `node-import`, the `EditError` checks),
now merged.

| Follow-up | What | Depends on |
|---|---|---|
| **C** -- CSS selectors and node paths | A selector engine in dom-core's C layer, plus `node-css-path` | nothing |
| **A** -- markup literals | `#html{...}` / `#xml{...}` with `${...}` holes, escaped by construction, rendered to a string or a tree | nothing left: A1-A2 (string mode) never needed anything, and A3-A4 (tree mode, in-place re-render) needed B1-B2, now merged |
| **D** -- hiccup | Markup as a typed Turmeric value (`Html`), written `(hiccup [:li.item #map{:href u} name])`, safe by construction, rendered to a string or a tree; and `node->hiccup` back from a parsed DOM | D1 (string mode) needs nothing but the hole classes it shares with A2; D1b (result-position rewriting) follows D1 -- the macro-time forward-reference fix it wanted is in (resolved 2026-10-17); D2 (tree mode) needed B1-B2, now merged |

Suggested order: A and D, with C whenever. A's and D's string modes share
the escaper and the hole classes, so whichever lands first builds them.

## Follow-up A: markup literals with templated values (`#html{...}`, `#xml{...}`)

### Goal

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

### What the reader can do today (verified)

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

### Design: parse once with the real parser, mark the holes

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
   - *the value of an attribute that holds code or markup* -- any `on*`
     event handler, `style`, `srcdoc`: also a **compile error** (A-4). HTML
     escaping protects only the attribute layer; the browser decodes the
     value and hands it to a JavaScript, CSS or HTML parser. A6 adds the
     escaping that makes some of these positions safe.

   It emits the static markup with markers, plus one expression per hole,
   wrapped by context:

   ```turmeric
   (tpl-render (tpl-site "<li class=\"item\" data-done=\"tur:0\"><a href=\"tur:1\"><!--tur:2--></a></li>")
               (tcons-of (to-attr done) (tcons-of (to-attr href) (tcons-of (to-child name) (tnil)))))
   ```

2. **First use (runtime, once per literal).** `tpl-site` parses the marked
   string with lexbor (`html-parse-fragment` with context `template`, see
   A-2) or expat. It walks the
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

### Typing the holes

Three typeclasses, chosen by the macro from the hole's context, so the
*position* picks the class and the *type* picks the instance:

```turmeric
(defclass ToChild   [T] (to-child    [x : T] : Child))
(defclass ToAttr    [T] (to-attr     [x : T] : AttrVal))
(defclass ToUrlAttr [T] (to-url-attr [x : T] : AttrVal))   ;; URL-bearing attrs, see A-1

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

;; URL-bearing attribute values (href, src, action, ...; list under A-1)
(definstance ToUrlAttr cstr       ...)   ;; scheme-checked, then escaped
(definstance ToUrlAttr String     ...)
(definstance ToUrlAttr TrustedUrl ...)   ;; explicit opt-out: escaped, not checked
(definstance ToUrlAttr (Option T) ...)   ;; None drops the whole attribute
```

A `Node` or `Fragment` in attribute position has no instance, so it is a
type error at the call site.

**Unescaped HTML is an explicit, greppable act.** `(raw-html s)` wraps a
`cstr` as `RawHtml`. In child position it is *parsed* with
`html-parse-fragment`, using the hole's parent element as context, and the
resulting nodes are spliced in. So it can never unbalance the surrounding
template. It is still not sanitized: a `<script>` in it survives, and the
docstring says so in the words the archived plan's
[Security](../../archive/xml-html-parsers-plan.md#security-not-optional) section uses.

**URL attributes are scheme-checked** (decided under A-1). Escaping alone
would leave `href=${x}` with `x = "javascript:..."` a correctly escaped
script URL, so holes in URL-bearing attributes go through `ToUrlAttr`, which
checks the scheme before escaping. `(trusted-url s)` wraps a `cstr` as
`TrustedUrl` to skip the check -- the same explicit, greppable act as
`raw-html`. This is a scheme check, not sanitization: a `https:` URL to a
hostile site passes, as it should.

### `#xml{...}`

`#xml{...}` uses the same pipeline over expat, with a well-formed body. CDATA
sections are a raw context, so no holes are allowed inside them. Namespace
prefixes must be declared inside the literal. An undeclared prefix is an expat
error at first use, and the context tokenizer can catch the simple cases at
compile time. The `on*` and `style` rule (A-4) applies to `#xml{}` too,
because browsers run them in SVG and XHTML served as XML.

### Later: holes in script, style and srcdoc (A6)

Until A6, a hole in an `on*`, `style` or `srcdoc` attribute, or in a
`<script>` / `<style>` body, is a compile error (A-4 and the raw-text rule).
A6 makes the useful positions in them legal by escaping in **layers**: the
inner language's escaper first, then the HTML attribute escaper for an
attribute, or nothing more for an element body (the inner escaper already
keeps `</script` and `</style` out). This is Go `html/template`'s approach.
Ours differs in two ways: the macro picks the context at compile time, and
the value's type picks the escaper through a class, not reflection.

Ground rules for every A6 phase:

- **Tokenize what the browser sees.** The macro runs the inner tokenizer
  over the *entity-decoded* static text, because that is what the
  JavaScript or CSS parser receives. `onclick="f(&quot;${x}&quot;)"` puts
  the hole inside a double-quoted JavaScript string.
- **Quoted attribute values only.** `onclick=${x}` and `style=${x}` stay
  compile errors.
- **When in doubt, reject.** A position the inner tokenizer cannot classify
  is a compile error, never a guess. The standard case is JavaScript's `/`:
  whether it starts a regex literal or is a division depends on the token
  before it, and a hole after an ambiguous `/` is rejected.
- **First use re-checks.** `tpl-site` confirms the attribute name and inner
  context the macro recorded, as it already does for slot kinds.
- **An explicit opt-out per language**, like `raw-html` and `trusted-url`:
  `TrustedJs` and `TrustedCss`, inserted after only the outer layer's
  escaping. Greppable, and documented as unsafe for untrusted input.

**JavaScript (A6a)** -- in an `on*` attribute, or a `<script>` body whose
`type` is absent, a JavaScript MIME type, or `module`:

- *Inside a single- or double-quoted string literal:* a `cstr`, `String`
  or `int` is JS-string-escaped. `\`, both quotes, `` ` ``, `<`, `>`, `&`,
  `=`, every C0 control and the line terminators U+2028 / U+2029 become
  `\uXXXX`. Escaping `<` is what keeps `</script` and `<!--` out of an
  element body.
- *In expression position* (at the start, or after `(`, `,`, `=`, `:`,
  `[`, `?`, `!`, `{`, `;`, `return` or an operator): the value becomes a
  JavaScript *literal* through a `ToJs` class. `int` gives a decimal,
  `bool` gives `true`/`false`, `cstr`/`String` gives a double-quoted string
  escaped as above, `(Option T)` gives `null` or the value, and `TrustedJs`
  is the only instance that writes code. The literal is padded with a space
  on each side, so `a-${x}` with `x = -1` cannot form `--`.
- *Template literals (backticks), comments, regex literals:* compile error.
- `<script type="application/json">` and `application/ld+json` bodies are
  JSON. A hole in value position goes through `ToJs` too: its literals are
  valid JSON, and the same `<` escaping applies. Any other `type` is a
  compile error.

**CSS (A6b)** -- in a `style` attribute or a `<style>` body:

- *Property value position* (after the `:` of a declaration): a value
  through a `ToCss` class. `int` gives a decimal. `cstr`/`String` passes
  only if every character is an ASCII letter or digit, or one of
  `# . - _ % ,` and space; anything else (parens, quotes, `\`, `/`, `;`,
  `{`, `}`, `<`, so `expression(`, `url(` and `/*` too) renders the
  invalid value `tur-unsafe`, which the browser drops. Typed values such as
  `CssColor` or `CssLength` are a natural later addition and would pass
  unfiltered.
- *Inside a quoted CSS string:* every character other than an ASCII letter
  or digit becomes a `\HEX ` escape (`<` gives `\3c `).
- *Inside `url(...)`*, quoted or not: A-1's scheme allowlist, then
  percent-encode parens, quotes, whitespace and `\`.
- *Property names, selectors, at-rules, comments:* compile error.

**srcdoc (A6c)** -- the value is a whole HTML document:

- Only a whole-value hole, `srcdoc="${x}"`. A hole inside static markup
  (`srcdoc="<p>${x}</p>"`) stays a compile error; supporting it means
  running the context tokenizer recursively over the decoded value, which
  can come later if anyone wants it.
- A `Fragment` (a nested `#html{}`) is rendered with `html->string`, then
  attribute-escaped: the iframe gets exactly the fragment's markup.
- A `cstr`/`String` is HTML-text-escaped, then attribute-escaped. It
  becomes text in the iframe, never markup.
- `RawHtml` is attribute-escaped only: its markup is live in the iframe, by
  request, as `raw-html`'s docstring says.

Testing, for all three:

- An escaping table per context against a hostile corpus: both quotes,
  backtick, `\`, `</script>`, `</style>`, `<!--`, `*/`, U+2028, `-->`,
  `expression(`, `javascript:`.
- End to end in a real browser, where a headless Chromium is available
  (turmeric's `web/` already runs one under Playwright): render each corpus
  value into each context, load the page, and assert that a sentinel
  global is never set, and that a JavaScript string hole reads back as
  exactly the value that went in.
- Where the contexts line up, compare with Go `html/template`'s output for
  the same template and value, as a second oracle.

### Phases

| Phase | Deliverable | Gate |
|---|---|---|
| A1 | `dom/markup`: `reader-macros/define` for `html` and `xml`, the `html*` / `xml*` `defmacro*` expanders and their context tokenizer | Each forbidden hole context is a compile error with a message naming it, including holes in `on*` (any case), `style` and `srcdoc` values (A-4); fixtures for `\}` and `\\` |
| A2 | `Template` cache, marker recovery, `ToChild` / `ToAttr` / `ToUrlAttr` and their instances, `trusted-url`, `html->string` / `xml->string` | Escaping table: `<`, `&`, `"` and `</script>` in every allowed context. A hostile value never changes the parsed structure: `parse(render(t, v))` has the same shape as `parse(render(t, "x"))` for every corpus string `v`. URL table: `javascript:`, `JavaScript:`, `java\tscript:`, a leading-space/control-char scheme, `data:`, `vbscript:` all render `about:invalid#tur-unsafe`; relative, `http(s):`, `mailto:`, `tel:` pass. Template-context probes: literals opening with `<tr>`, `<td>`, `<col>`, `<option>`, `<caption>` keep that structure (the vendored html5lib slice has only one `template`-context fragment case, so this needs its own fixtures) |
| A3 | `html->dom` / `Fragment`, nested templates, lists, `raw-html` (needs B1/B2) | Tree mode and string mode agree: `dom-serialize(html->dom t v) == html->string t v` |
| A4 | `TemplateInstance` and `tpl-update!` | Updating with the same values touches nothing (pinned by a counter); updating one hole rewrites one node |
| A5 (optional, compiler) | Real spans for `${...}` holes (`read-string-at`, or a built-in `#html{` reader) | A type error in a hole points at the user's line and column |
| A6a (later) | JavaScript contexts: `on*` attributes and `<script>` bodies (including JSON bodies), the decoding inner tokenizer, `ToJs`, `TrustedJs` | Escaping table and browser check from "Later: holes in script, style and srcdoc"; a hole in a template literal, comment, regex or after an ambiguous `/` is a compile error |
| A6b (later) | CSS contexts: `style` attributes and `<style>` bodies, `ToCss`, `TrustedCss` | Escaping table and browser check; `expression(`, `url(javascript:` and `;}` in a value hole all render `tur-unsafe` |
| A6c (later) | Whole-value `srcdoc` holes for `Fragment`, `cstr`/`String` and `RawHtml` | A `cstr` holding `<script>` appears as text in the iframe; a partial `srcdoc` hole is still a compile error |

### Open questions

All four were decided 2026-10-16.

- **A-1.** Should URL-bearing attributes (`href`, `src`, `action`,
  `formaction`, `xlink:href`) require a `Url` type rather than accepting a
  `cstr`? *Decided: accept `cstr`, check the scheme, offer an explicit
  opt-out.* No `Url` type exists in either repo, and requiring one taxes
  every link. Instead:
  - The macro already knows the attribute name, so it picks `ToUrlAttr`
    for `href`, `src`, `action`, `formaction`, `xlink:href`, `poster`,
    `cite`, and `srcset` (each comma-separated candidate checked). Nothing
    is looked up by name at runtime.
  - The check is an **allowlist**, not a `javascript:` denylist: strip
    leading C0 controls and spaces, drop ASCII tab/LF/CR anywhere (as the
    URL parser does), then a value with no scheme (relative) or a scheme of
    `http`, `https`, `mailto` or `tel` (ASCII case-insensitive) passes.
    Anything else renders `about:invalid#tur-unsafe`. That is the Closure /
    Go `html/template` approach: a denylist misses the next scheme, and
    substituting a dead URL beats panicking inside a request handler.
  - `(trusted-url s) : TrustedUrl` skips the check (still escaped).
- **A-2.** Should the fragment context be selectable (`#html[tr]{<td>..}`),
  or should templates that start with `<td>`/`<tr>`/`<option>` be detected
  and given the right context automatically? *Decided: neither -- always
  parse with `template` as the fragment context.* That is what lit-html
  actually does: the "in template" insertion mode accepts `<tr>`, `<td>`,
  `<col>`, `<option>` and `<caption>` at the top level, so no syntax and no
  sniffing are needed. `html-parse-fragment` already takes the context tag.
  Two edges, both handled in the macro's context tokenizer:
  - a literal whose first tag is `<!doctype` or `<html>` is a whole
    document: parse it with `html-parse`, not as a fragment;
  - a top-level `<head>` or `<body>` would be silently dropped in template
    context, so it is a compile error pointing at the whole-document form.
- **A-3.** Should `tourist`'s `render-html` move onto `#html{}` once A2 lands?
  *Decided: add, don't replace.* `render-html` renders a template *file*
  against an `env : int` at runtime; `#html{}` is a literal in the source,
  so it cannot stand in one for one. When A2 lands, `tourist` gains
  `html-response : Fragment -> Response`, and its guide and examples move to
  it. `render-html` stays for file-based views. Separately, and not waiting
  on A: the `template` spice escapes nothing (`<%= x %>` writes `x`
  verbatim; the only "escape" in `spices/template/src` is the `<%%`
  literal). Fix that in its own small `turmeric-spices` PR -- `<%= %>`
  HTML-escapes by default, `<%== %>` writes raw.
- **A-4 (raised 2026-10-16).** Holes in attributes whose *value* is itself
  code or markup. The context tokenizer as first sketched allowed a hole in
  any attribute value, but entity-escaping only protects the HTML layer;
  the browser decodes the value and then hands it to another parser:
  - `on*` event handlers: `onclick="go('${x}')"` with
    `x = "'); alert(1); ('"` escapes `'` to `&#39;`, which decodes back to
    `'` before the JavaScript runs.
  - `style`: the value is CSS (`url(...)`, `expression(...)` in old IE).
  - `srcdoc`: the value is an HTML document, so `&lt;script&gt;` decodes to
    a live `<script>` in the iframe.
  *Decided: a compile error for now, real escaping later.* A1 rejects a
  hole in any `on*`, `style` or `srcdoc` value, beside the raw-text
  contexts (design step 1, and the A1 gate). Layered JavaScript, CSS and
  srcdoc escaping is planned as A6a-A6c under "Later: holes in script,
  style and srcdoc", and is not needed for A1-A5.

## Follow-up C: CSS selectors and node paths

### Engine choice

lexbor has a selectors module, but it matches against lexbor's DOM, and
`tur-html` copies lexbor's tree into the dom-core arena and throws lexbor's
away (Open question 2). Using it would mean keeping two trees alive for HTML
and having nothing for XML. So: **a small selector engine in `tur-dom-core`'s
C layer**, over `turdom_node`, shared by both flavours. A tokenizer plus a
compiled form plus a right-to-left matcher is on the order of 1.5-2k lines of
C; the CSS Syntax tokenizer is the fiddly part (escapes, `\:` in names,
quoted strings), not the matcher.

### Supported grammar (first cut)

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

### Case and namespace rules

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

### API sketch (`dom/select`)

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

### Interaction with editing

Selectors read the same links the editor writes, so they need nothing
special; `select` returns a snapshot, so `(for n (select d s) (node-remove!
n))` is safe. The one caveat is `node-css-path`: a path is only meaningful
for the tree it was computed on.

### Testing

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

### Phases

| Phase | Deliverable | Gate |
|---|---|---|
| C1 | Tokenizer and compiler (`selector-compile`, `SelectorError` with a byte offset) | Every supported construct compiles; every rejected construct is an error naming it |
| C2 | Matcher, `select` / `select-first` / `node-matches-selector?` / `node-closest`, `query` | Ported test tables pass on XML and HTML; differential fuzz agrees with the naive oracle |
| C3 | `node-css-path` | Round-trip property over both corpora |
| C4 | `:has()` with a step budget | Its own fixtures, including a pathological input that hits the budget |

## Follow-up D: hiccup -- markup as typed data (planned 2026-10-16)

### Goal

Build markup from Turmeric values, Clojure-hiccup style, and render it to a
string or a tree with A's escaping guarantees:

```turmeric
(import dom/hiccup :refer [hiccup hiccup->string])

(defn row->item [r : Row] : Html
  (hiccup [:li.item #map{:data-done (.done r)}
            [:a #map{:href (.href r)} (.name r)]]))

(println (hiccup->string (hiccup [:ul (map row->item rows)])))
```

A writes markup and splices values into it. D goes the other way: the tree
*is* a Turmeric value (`Html`) that ordinary functions build, map, filter
and `match` on before it is rendered. That is what A cannot give, and why
B's [options table](../../archive/xml-html-parsers-plan.md#options-weighed) sends "rebuild from a
Turmeric-side value" here.

### What the reader hands a macro (checked 2026-10-16)

Probed with `read-string` / `syntax->string` on the v0.65.2 release:

- `[...]` is a vector form and `:div` is a keyword, so `[:div ...]` reaches
  a `defmacro*` intact. `:div.card#main` is a single keyword, so the
  CSS-style shorthand can be split at expansion time.
- `#map{...}` arrives as a map form, keys and value expressions in order,
  so it is the attribute syntax (D-3).
- `{...}` is curly-infix in every dialect, so Clojure's bare `{...}` does
  not survive: `{:class "a"}` arrives as the list `(:class "a")`, and
  `{:class "a" :id "b"}` as `($nfx$ :class "a" :id "b")`.
- A colon splits a keyword: `:svg:rect` reads as `:svg :rect`, and
  `:xlink:href` likewise. Names containing a colon are written as strings:
  `["svg:rect" ...]`, `#map{"xlink:href" u}`. HTML rarely needs them;
  `[:svg [:rect ...]]` and SVG 2's plain `href` cover most cases.
- A vector inside an ordinary expression is not hiccup to the outer macro.
  In `[:ul (map (fn [r] [:li r]) rows)]` the inner `[:li r]` is a plain
  vector. See D-1.

### Design

**One typed tree, safe by construction.**

```turmeric
(defopaque Tag      ...)    ;; a validated element name
(defopaque AttrName ...)    ;; a validated attribute name
(defopaque Attr     ...)    ;; a name and an escaped value
(defopaque RawBody  ...)    ;; a raw-text body: a checked literal or a Trusted* value

(defdata Html :heap                      ;; :heap -- see "D-1 investigation"
  (HElem     Tag (Cons Attr) (Cons Html))
  (HRawText  Tag (Cons Attr) RawBody)    ;; script, style and the other raw-text elements
  (HText     String)                     ;; escaped at render
  (HRaw      RawHtml)                    ;; A's raw-html opt-out
  (HList     (Cons Html))                ;; siblings with no wrapper
  (HFragment Fragment))                  ;; a #html{} value spliced in (D2)
```

`Html`'s constructors are public, so callers can `match` on and rebuild
trees. Every field that could carry unsafe bytes is an opaque type with a
checking constructor, so any `Html` value is safe to render, whoever built
it:

- `tag` and `attr-name` take a `cstr` and return an `Option`. Both follow
  the XML `Name` production for both flavours. That is stricter than HTML
  needs, and it rules out the `<x onload=...>` name injection that B's
  `BadName` exists for.
- `tag` refuses the raw-text elements (`script`, `style`, `xmp`, `iframe`,
  `noembed`, `noframes`, `plaintext`). They are reachable only through
  `HRawText`, whose `RawBody` comes from a literal the macro checked (no
  `</script` or `</style`), or after A6 from a `TrustedJs` / `TrustedCss`.
- `attr-name` refuses A-4's code-bearing names (`on*`, `style`, `srcdoc`).
  The macro can still write them with a literal value it checked; a runtime
  value for one waits for A6.
- `(attr n v)` builds an `Attr` through `ToAttr`, and runs A-1's scheme
  allowlist when `n` is URL-bearing, so trees built at runtime and literals
  written with the macro apply the same rule.

**The `hiccup` macro** is a `defmacro*` beside A's `html*`. It lowers a
literal to constructor calls:

- `[name attrs? child...]` becomes `HElem`. `name` must be a keyword or a
  string literal; a computed tag is a compile error, and a caller who needs
  one uses `tag` and the constructors. The macro validates every literal
  name at expansion time and emits unchecked constructors for them, so a
  literal tree pays nothing at runtime for validation.
- `:tag#id.c1.c2` shorthand, merged with an explicit `:class` (joined by a
  space). A second `id` is a compile error.
- The attribute map is a `#map{...}` literal in second position, with
  keyword or string keys. The macro consumes the form, so its values need
  not share a type the way a real `Map[K V]`'s do. A value is a literal or a
  hole typed through A's classes: `ToAttr`, or `ToUrlAttr` for the
  URL-bearing names. A dynamic value under an `on*`, `style` or `srcdoc` key
  is a compile error (A-4). Attributes built at runtime splice in under the
  reserved key `:attrs*` (`#map{:class "a" :attrs* extra}`, with
  `extra : (Cons Attr)`). `*` is not an XML name character, so the key can
  never collide with a real attribute.
- A bare `{...}` in attribute position is a compile error that points at
  `#map{...}`. The macro recognizes it by curly-infix's output shape (a
  list headed by a keyword, or `$nfx$`); if that shape ever changes, only
  the message gets worse, since nothing is accepted on the strength of it.
- A child that is a string or number literal becomes `HText`. A nested
  vector headed by a keyword or a string recurses. Anything else is a hole
  typed through `ToChild`, with the same instances as A plus
  `ToChild Html` and `ToChild (Cons Html)`. In D1 a vector inside such
  an expression is not rewritten, and markup there is written
  `(hiccup ...)` again. From D1b, vectors in that expression's result
  positions are rewritten too (D-1; the positions are listed under
  "D-1 investigation"). Markup anywhere else still takes `(hiccup ...)`,
  and the macro says so when it finds a vector headed by a known element
  name outside a result position.
- Nesting the HTML parser would undo is a compile error where the literal
  shows it (D-2): block content directly in `p`, `a` inside `a`, `form`
  inside `form`, interactive content inside `a` or `button`, and anything
  but the table-structure elements directly in `table`, `thead`, `tbody`,
  `tfoot` or `tr`.
- A void element (`br`, `img`, `input`, ...) with children is a compile
  error.
- `[:script "..."]` and `[:style "..."]` take one string literal, which is
  checked. A hole there is a compile error until A6.

**Rendering.** `hiccup->string` follows the HTML serialization rules (void
elements, boolean attributes, raw-text bodies) and shares its text and
attribute escaping with dom-core's `escape_into` and A2's string mode.
`hiccup->xml-string` writes `<x/>` for an empty element, has no void list,
and treats the raw-text elements as ordinary elements whose text is
escaped. `hiccup->dom` (D2) builds nodes in a `Document` through B's
mutators. The names are already valid, so B's checks cannot fail on them.

**Composition with A, both ways.** `ToChild Html` lets a `#html{}` hole take
hiccup, and `HFragment` lets hiccup take a `#html{}` value. A and D share
the hole classes and the escapers: whichever of A2 and D1 lands first puts
them in a small `dom/holes` module, and the other imports it. `tourist`'s
`html-response` (A-3) takes either, through a `RenderHtml` class with
`Fragment` and `Html` instances.

**DOM to hiccup (D3).** `node->hiccup : Node -> Html` converts a parsed tree.
This is the hickory direction: parse, transform as data with ordinary
functions and `match`, then render. A parser can only produce valid names
and raw-text bodies, so the conversion uses the trusted constructors.

### Phases

| Phase | Deliverable | Gate |
|---|---|---|
| D1 | `dom/hiccup`: `Html`, `Tag`, `AttrName`, `Attr`, `RawBody` and their checking constructors; the `hiccup` macro (keyword and string names, `#id.class` shorthand, `#map{}` attributes, `:attrs*`, holes through `ToChild` / `ToAttr` / `ToUrlAttr`); `hiccup->string` | A compile-error table: an invalid literal name, a computed tag, children under a void element, a dynamic value under `on*` / `style` / `srcdoc`, a dynamic `<script>` / `<style>` body, a second `id`, a bare `{...}` attribute map, and each D-2 nesting case. The checking constructors refuse bad names, the code-bearing names and the raw-text tags. A2's escaping and URL tables, applied to hiccup. Once A2 exists, `hiccup` and `#html{}` render byte-identical output for a shared corpus |
| D1b | Result-position rewriting (D-1): markup vectors in the result positions of `if`, `when`, `unless`, `cond`, `do`, `let`, `match`, `for`, and an inline one-parameter `fn` passed to `list-map`, become `Html`; missing `else` branches become an empty `Html`; an inline `list-map` becomes a `list-foldl` straight into `Html`; a vector headed by a known element name outside a result position is a compile error saying to wrap it in `(hiccup ...)`. Best after `macro-time-forward-reference-typed-int` is fixed, so the walker can be ordinary mutually recursive functions; otherwise it ships as the single self-recursive walker the prototype used | One fixture per form in the result-position table, with markup in every branch and a missing `else` where the form allows one. Keyword-headed vectors that are type syntax or option lists (`(fn [:int] :int)`, `:only [:encode]`) inside a hiccup argument are neither rewritten nor reported. A vector in a `let` binding's value or an unknown call's argument gives the "wrap in `(hiccup ...)`" error. A type error inside a rewritten branch points at the user's line |
| D2 | `hiccup->dom` (needs B1-B2), `hiccup->xml-string`, `ToChild Html` and `HFragment`, `RenderHtml` for `html-response` (done: dom/holes' `RenderHtml`, tourist/markup's `html-response`) | For parser-stable trees (D-2), `html-parse-fragment` with context `template` of `hiccup->string h` is `dom-equal?` to `hiccup->dom h`; the same through expat for the XML flavour |
| D3 | `node->hiccup` | `hiccup->dom (node->hiccup n)` is `dom-equal?` to `n` for every element of both corpora; the guide shows a transform (add `rel="noopener"` to every external `<a>`) |

### Open questions

All three were decided 2026-10-16.

- **D-1. Hiccup inside an ordinary expression.** In
  `[:ul (map (fn [r] [:li r]) rows)]` the inner vector is out of the
  macro's reach, so it reads as a plain vector and fails to typecheck.
  Either require an explicit `(hiccup [:li r])` there, or have the macro
  rewrite every keyword-headed vector anywhere inside its argument. A
  binding vector starts with a symbol, so `[r rows]` is left alone, and the
  rewrite is what makes hiccup feel like hiccup. Its cost is that a real
  vector of keywords inside a hiccup body is misread. *Decided: explicit
  `(hiccup ...)` in D1, then revised the same day after a prototype (see
  "D-1 investigation"): phase D1b, before D2, rewrites markup vectors in
  result positions only.* Rewriting every keyword-headed vector is
  rejected, because in-tree those are type syntax and option lists. Markup
  outside a result position keeps `(hiccup ...)`, and the macro reports a
  vector it would otherwise have missed.
- **D-2. Trees the HTML parser will not reproduce.** `[:p [:div "x"]]`,
  `[:a [:a "y"]]`, and text directly inside `[:table]` are valid `Html`
  values, but a browser re-nests their serialized form, so string mode and
  `hiccup->dom` disagree. This is not a security issue, since the structure
  is the author's, but it is a correctness surprise. *Decided: a compile
  error wherever the literal shows it* (the list under "The `hiccup`
  macro"). Trees assembled at runtime are documented, not checked, and D2's
  gate is stated over parser-stable trees.
- **D-3. Bare `{...}` attribute maps.** Accept them by reading curly-infix's
  output (`(:k v)` and `($nfx$ ...)`), or require `#map{...}`? *Decided:
  `#map{...}` only.* It is the documented map literal, and the attribute
  syntax then does not depend on how curly-infix rewrites braces. A bare
  `{...}` is a compile error pointing at `#map{...}`.

### D-1 investigation: rewriting vectors in result positions (2026-10-16)

What the rewrite takes, found by building it; adopted as D1b. A prototype
walker of about 70 lines, in a `:for-macros` module, compiled and ran
correctly on the v0.65.2 release and on `main` (`d60c40f6`), with the
escaping checked. It handled element children, `if` with and without
`else`, `when`, `do`, `let`, and `list-map` over an inline `fn` whose body
was itself an `if` with markup in both branches. A production version,
with `unless`, `cond`, `match`, `for` and the diagnostics below, is
estimated at about 150 lines.

**Rewrite result positions, not every vector.** A vector is rewritten only
where its value becomes a child:

| Form | Result positions |
|---|---|
| hiccup element | each child |
| `if` | both branches; a missing `else` becomes an empty `Html` |
| `when` / `unless` | the last body form; the missing branch becomes an empty `Html` |
| `cond` | each result; a missing `:else` gets an empty `Html` |
| `do`, `let` | the last body form |
| `match` | each arm's body |
| `for` | the last body form |
| `list-map` with an inline one-parameter `fn` | the `fn`'s last body form; the call becomes a `list-foldl` straight into `Html`, so no `(List Html)` is built |

Any other leaf in a result position becomes `(to-child x)`, so every branch
is already `Html` and the author never has to make branch types agree.

**Why not every keyword-headed vector.** Across the roughly 5,000 `.tur`
files in `stdlib/`, `tests/fixtures/` and the spices, keyword-headed
vectors appear 31 times. Outside comments, every one is type syntax
(`(fn [:int] :int)`, `(definstance Show [:int])`) or a macro option list
(`derive-json ... :only [:encode]`). A walk over every vector would
rewrite those; a result-position walk never reaches them.

**Reach.** Markup outside a result position still needs `(hiccup ...)`:
a `let` binding's value, an argument to a function the walker does not
know, or the body of a named helper. A missed vector surfaces as a
`vec-of` error (`function 'vec-push!' arg 2: expected :Sym, got cstr`),
which does not say what to do. The macro can also scan non-result
positions for vectors headed by a known element name and report "wrap
this in `(hiccup ...)`" instead.

**Spans are good.** Subforms the walker does not touch keep their source
spans, and a rebuilt list takes its first element's span, so a type error
inside rewritten code points at the user's line. A's `${...}` holes
cannot do that (they go through `read-string`).

**What got in the way:**

- **Mutual recursion at macro time.** A call to a function defined later
  is typed `int`, both in a `letrec` and across a `:for-macros` module's
  `defn`s ([macro-time-forward-reference-typed-int](../../archive/macro-time-forward-reference-typed-int.md),
  resolved 2026-10-17).
  The walker is therefore one self-recursive function that dispatches on a
  mode argument. It works, but it reads worse than three functions would.
- **The `syntax-*` functions exist only at macro time.** The walker cannot
  be an ordinary module; it has to be imported `:for-macros`. This is
  documented, and fine.
- **`syntax-vec` has a fixed arity, and nothing turns a list into a
  vector.** Building the fold's parameter vector
  (`[acc : Html r : cstr]`) works only because the shape is fixed (a
  one-parameter `fn`; a typed parameter is two elements, the symbol and a
  `type-ann`). Rewriting a binding vector of arbitrary length would need a
  `syntax-list->vec`.
- **`Html` must be `:heap`.** A by-value `defdata` as a `(List A)`
  element segfaults in `list-map` and fails in cc in `list-of`
  ([typed-list-byvalue-adt-element-unboxed](../../archive/typed-list-byvalue-adt-element-unboxed.md),
  resolved 2026-10-17). With
  `:heap`, both `(list-map rows item)` through a `ToChild (List Html)`
  instance and the fold rewrite work. The cost is the one noted in the
  archived plan's [Outcome](../../archive/xml-html-parsers-plan.md#outcome-2026-10-10): every `:heap`
  constructor call allocates a node nothing frees, so rendering an `Html` tree per request needs a `with-region`
  bracket around it, or a string mode that does not materialize the tree.

## Implementation notes (2026-10-28)

What building C, A1-A2 and D1 changed against the text above.

**C (selectors).**

- `select` is a reserved form (channel `select`), so the query is
  `select-all`; the rest of the API is as sketched. `query` and `query-one`
  are as planned.
- `:has()` shipped with C1-C3 rather than as a later phase: its budget is a
  million candidate tests per query. Past it the `:has()` answers false,
  `selector-gave-up?` reports it, and `query` / `query-one` return an Err
  rather than a short answer.
- The selector source follows CSS Syntax at end of input: an unclosed
  string, attribute selector or function is closed there (`[a="b` is valid),
  as browsers do.
- Conformance: the WPT `dom/nodes/selectors.js` table (BSD-3, vendored with
  its commit) runs in the document, detached-element and in-document-element
  contexts over WPT's HTML and XHTML documents; the rows that use a rejected
  pseudo-class (`:link`, `:target`, the UI states, `:lang`, pseudo-elements)
  must be compile errors. The differential fuzz builds random selector ASTs,
  renders them for the engine and evaluates them with a naive Turmeric
  matcher over random `dom/edit` trees. css-select / nwsapi tables were not
  needed.

**A (markup literals).**

- String mode builds no tree: the macro emits one template string (static
  markup with an inline marker per hole) and the hole values; rendering is
  concatenation. The first render of each literal parses it once with the
  real parser and checks every hole landed in the slot the macro predicted
  (cached per literal, keyed by the template's address).
- `Fragment` holds its rendered markup (malloc'd); `fragment-free` releases
  it. `html->string` borrows it.
- Two holes the plan's list missed are compile errors too: a hole right
  after a `<` (`<${x}>` -- the static `<` and the escaped value would still
  print a tag), and a hole inside `<textarea>` / `<title>` / `<noscript>`
  (raw text in the tokenizer's terms).
- A `raw-html` value that the serializer cannot write back as parsed (a
  `<plaintext>`, a comment or raw text that would end early) panics rather
  than unbalance the literal; dom/edit's `turdom_check_subtree` decides.
- The literal syntax is only available in a file read after `html/markup`
  is loaded; a file can use it itself through
  `#use-reader-macros "<path>/html/markup.tur"`, or call `html*` directly.
- A macro-time-only module (`dom/markupc`, `html/hiccupc`) cannot be
  `tur check`ed on its own -- it calls the `syntax-*` natives -- so it
  carries a `;; tur-check: macro-time` header line and the spices CI's check
  stage skips it; it is compiled through every module that imports it.
- Four compiler problems had to be fixed first (turmeric branch
  `claude/ecstatic-cori-ikodwq`):
  [macro-env-force-loads-driver-used-modules](../../archive/macro-env-force-loads-driver-used-modules.md)
  (no `defmacro*` compiled in turmeric-spices at all); re-registering an
  identical reader macro (`#use-reader-macros` plus the import) was an
  error; `tur fmt` printed a user reader macro's expansion instead of the
  invocation and deleted `#use-reader-macros` lines -- which, with spices CI
  running `tur fmt --check`, would have rewritten every literal; and
  `syntax-len` / `syntax-nth` saw no items in a `#map{...}` literal, which
  hiccup's attribute maps need.

- **Tree mode is split from string mode (decided 2026-10-28).** A
  `Fragment` is its rendered markup, so it cannot be cloned-and-patched the
  way step 3 above describes. A3's `html->dom` parses the markup instead (in
  the same `template` context the literal was checked in), and its gate is
  restated: the tree serializes to `html->string`'s markup except where the
  HTML parser normalizes (a table's `<tbody>`, `checked=""`). A4 is a
  separate literal, `#html-live{...}` / `html-live*`: each evaluation is a
  `LiveHtml` snapshot of the hole values; `live-mount` builds the tree once,
  replacing each content hole's marker with an empty text anchor and
  grouping attribute holes by element and attribute; `tpl-update!` patches
  only the holes whose values changed and returns the number of patches.
  Only code that asks for a live literal pays for a tree.
- An expansion may only name helpers in the macro's own module (`mk-*` in
  `html/markup`, `xml/markup`, `html/hiccup`): a module-qualified name
  resolves only for a module the using file imports itself.

**D (hiccup).**

- D1 lives in tur-html (`html/hiccup`, macro-time `html/hiccupc`), not
  `dom/hiccup`: rendering a `raw-html` hole needs lexbor, and so does
  `ToChild Html`. The expansion calls wrapper functions (`mk-elem`, ...)
  rather than the qualified constructors, which mistype as arguments
  ([qualified-adt-constructor-mistyped-as-argument](../../reported/qualified-adt-constructor-mistyped-as-argument.md)).
- `ToChild` gained one constrained instance for `(Cons T)` (and `Child` a
  `ChildList` case) in place of per-type list instances: an instance for
  `(Cons Html)` in `html/hiccup` would be an orphan. D2's `hiccup->xml-string` will need the types moved down
  to dom-core with the renderer taking the raw fixer as an argument (as
  `render-template` already does for A).
- `HText` holds a `cstr`, not a `String`.
- Literal attribute values are written as given (not scheme-checked),
  matching `#html{...}`'s static text; values in holes go through
  `to-attr` / `to-url-attr` and `srcset-check`.
- **D1b-D3 (2026-10-28).** The opaque types are `HTag`, `HAttr`,
  `HAttrName`, `HRawBody` (and D2's `HCommentBody`, `HCdataBody`,
  `HPiBody`): a type name is global, and tur-dom-core's SAX `Attr` and ecs's
  `Tag` were taken
  ([module-type-names-collide-across-modules](../../reported/module-type-names-collide-across-modules.md)).
- Content holes go through a new `ToHtml` class rather than `ToChild`, so a
  nested `Html` stays a tree (through `ToChild` it would be flattened to
  markup); `ToHtml` has `ToChild`'s instances plus `Html`, `Node` and
  `Child`, so any `ToChild` type goes in as `(to-child x)`.
- The D1b walker is ordinary mutually recursive functions (the forward
  reference fix holds). `for` is expanded by the walker into nested
  `match`es over `Option`, not through the `for` macro: its `.pure` looks
  for an instance on the body's type when that is concrete, and takes the
  monad only from a `defn`'s return type
  ([for-comprehension-pure-picks-the-payload-type](../../reported/for-comprehension-pure-picks-the-payload-type.md)).
  An inline `fn` given to `list-map` needs a typed parameter. tur-html
  cannot load `stdlib/list-typed.tur` today (its `String` collides with
  dom/ownstr's), so the test supplies `list-foldl`.
- `HComment`, `HCdata` and `HPi` were added so `node->hiccup` loses
  nothing; HTML has no CDATA or PIs, so `hiccup->string` writes CDATA as
  text and leaves a PI out. `hiccup->xml-string` writes a `raw-html` child
  or a spliced `#html{...}` by parsing it with lexbor in its parent and
  converting with `node->hiccup` -- no expat needed in tur-html.
- `hiccup->dom` follows the HTML tree builder's namespace rules (`svg`,
  `math`, the integration points, foreign attributes such as `xlink:href`).
  `node->hiccup` pins each element to the namespace it was in (a marker in
  the tag; an XML element carries its URI, an attribute its namespace), so
  an element converted on its own rebuilds in place. `hiccup->xml-dom`
  otherwise resolves prefixes through the tree's `xmlns` attributes and puts
  declarations first, as expat does.
- `RenderHtml` lives in dom-core's `dom/holes` (its method gives owned
  markup), with instances for `Fragment` there and for `Html` and `LiveHtml`
  in tur-html, so tourist needs only tur-dom-core for the class.
  `tourist/markup`'s `html-response` sits behind optional tur-dom-core and
  tur-html dependencies, as `tourist/template` does behind tur-template. Its
  test reads the Response directly: tourist's HTTP client loads
  `json/ownstr`, whose `String` collides with `dom/ownstr`'s.
- **A5.** `read-string-at` (a macro-time native beside `read-string`):
  `(read-string-at text lit k)` reads text that starts at byte `k` of the
  string literal `lit`'s value and moves every form's span to that text's
  place in the user's file, replaying the literal's escapes (a raw reader
  macro's `\x`, a `"..."` literal's simple escapes); where it cannot (the
  source is not registered, a `\x..` escape comes first), every form takes
  the literal's span. `dom/markupc` reads holes with it, so a type error in
  a hole of `#html{...}`, `#xml{...}` or a direct `html*` / `xml*` call
  points at the hole. Pinned by `tests/fixtures/errors/read-string-at-hole-position`
  and the spices' error tables.
- D3's gate holds for every element of the 54 html5lib files except those
  holding what dom/edit refuses to rebuild (a `<plaintext>`, raw text
  holding its end tag or `<!--`, a comment holding `<!--` or `-->`):
  about 470 of 8300, ancestors included. `hiccup->dom` panics on those.
  The XML corpus is ten documents in the test (namespaces, redeclared
  prefixes, `xml:` attributes, entities, CDATA, PIs); tur-html takes
  tur-xml as an optional, test-only dependency for it and for D2's expat
  gate.

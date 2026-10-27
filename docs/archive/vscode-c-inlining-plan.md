# VSCode Extension: C-Inlining Syntax Highlighting Plan

## Goal

Add syntax highlighting for inline-C blocks (` ```c ... ``` `) to the VSCode
extension, and verify or improve support when Turmeric code is embedded inside
Markdown documents.

---

## Current State

### What exists

- **Main grammar**: `vscode-syntax-ext/syntaxes/turmeric.tmLanguage.json`
  TextMate grammar covering keywords, strings, numbers, s-expressions, comments,
  operators. No rule for inline-C blocks.
- **Markdown injection**: `vscode-syntax-ext/syntaxes/turmeric.markdown-injection.json`
  Injects `source.turmeric` into fenced code blocks tagged `turmeric` or `tur`.
  Once injected, whatever rules are in the Turmeric grammar apply, so fixing the
  main grammar automatically benefits markdown embedding too.

### What is missing

The Turmeric grammar has no pattern for ` ```c ... ``` ` blocks, so they render
as unrecognized text (plain or illegal). Full C syntax highlighting (types,
keywords, string literals, preprocessor directives, comments) is absent entirely.

---

## C-Inlining Syntax Overview

A C-inlining block looks like:

```turmeric
(defn file-size [f] : int
  ```c
  FILE* file = (FILE*)f;
  fseek(file, 0, SEEK_END);
  return (int)ftell(file);
  ```)
```

Key structural facts:

- The opening delimiter is ` ```c ` (optional `c` tag) at the end of a Turmeric
  expression position.
- The closing delimiter is ` ``` ` immediately followed by `)` (closing the
  enclosing s-expression), all on the same line.
- The block spans multiple lines.
- The content is verbatim C99 code pasted into the generated function body.
- TextMate grammars can embed a foreign grammar scope via `"include": "source.c"`.

---

## Grammar Changes Required

### 1. Add `c-inline` repository rule

Add a new rule to `turmeric.tmLanguage.json`'s `repository` object:

```json
"c-inline": {
  "name": "meta.embedded.c.turmeric",
  "begin": "```c?\\s*$",
  "end": "```",
  "beginCaptures": {
    "0": { "name": "punctuation.section.c-inline.begin.turmeric" }
  },
  "endCaptures": {
    "0": { "name": "punctuation.section.c-inline.end.turmeric" }
  },
  "patterns": [
    { "include": "source.c" }
  ]
}
```

Notes:
- `begin` matches ` ```c ` or ` ``` ` (with optional `c`) at end-of-line. The
  trailing `\s*$` keeps it from firing mid-line.
- `end` matches the bare ` ``` `. It intentionally does NOT require `$` so the
  pattern closes correctly even when `)` follows on the same line (` ```) `).
- `"include": "source.c"` delegates to VS Code's built-in C grammar. This
  requires that VS Code has C language support installed, which is standard (C/C++
  extension or the built-in `source.c` grammar from `microsoft.vscode-cpptools`
  or `ms-vscode.cpptools`). If the scope is absent at runtime, the block simply
  renders without C highlighting -- a graceful no-op.

### 2. Wire it into the top-level and sexp patterns

Add `{"include": "#c-inline"}` as the **first** entry in both:

- `patterns` (top-level array) -- covers blocks at the module level.
- `sexp.patterns` -- covers blocks inside s-expressions (the common case).

It must come before `#string`, `#comment`, etc., because the backtick fence
characters are not currently claimed by any rule and might accidentally match
parts of other rules.

### 3. Add a scope for the fence punctuation (optional polish)

VS Code theme authors use scopes to color delimiters distinctly from content.
Naming the begin/end captures `punctuation.section.c-inline.begin.turmeric` and
`.end.turmeric` lets themes dim or highlight the fence markers without touching
the C content scope.

---

## Markdown Embedding Analysis

### Injected Turmeric grammar in Markdown

The markdown injection grammar (`turmeric.markdown-injection.json`) wraps a
Turmeric code block:

```
begin: (^|\G)(\s*)(```+|~~~+)\s*(turmeric|tur)\s*$
end:   (^|\G)(\2)(\3)\s*$
```

The `end` pattern requires the SAME leading whitespace and fence characters,
followed only by optional spaces and end-of-line (`\s*$`). This means a line
like ` ```) ` (closing backticks with an immediately trailing paren) does NOT
match the end rule. The markdown injection scope stays open correctly through the
C block.

Adding the `c-inline` rule to the Turmeric grammar is sufficient -- the injection
automatically benefits from it with no changes to the markdown injection file.

### Rendering in GitHub / rendered Markdown

For documentation files (`.md`) that show Turmeric examples:

CommonMark specifies that a fenced code block closing delimiter must consist
solely of the fence character sequence (plus optional trailing spaces). A line
like ` ```) ` has a non-space character after the backticks and therefore does
NOT close the outer markdown fence. GitHub's renderer correctly treats it as
literal content inside the block.

**This is safe as long as the C-block closing backticks are immediately followed
by `)` on the same line**, which is the standard convention. An alternative
style where `)` goes on its own line:

```turmeric
(defn foo [x] : int
  ```c
  return (int)x * 2;
```
)
```

...would cause the closing ` ``` ` to terminate the outer Markdown fence early.
The plan section below addresses this.

---

## Should the Triple-Backtick Syntax Be Reconsidered?

### The collision surface

Three-backtick C blocks collide with Markdown fences in the following ways:

| Context | Risk level | Notes |
|---------|------------|-------|
| VSCode TextMate grammar | Low | TextMate end rules respect the `)` suffix; this plan's grammar change handles it correctly. |
| GitHub-rendered Markdown | Low (current convention) | Safe when `)` is on the same line as the closing ` ``` `. Fragile if style deviates. |
| Pandoc / other Markdown renderers | Medium | Behaviour varies by renderer and CommonMark strictness. |
| Documentation authors copy-pasting | Medium | Authors may not know the `)` suffix rule; examples could accidentally break rendered output. |
| Syntax highlighting in docs | Low | Grammar injection handles it correctly once this plan is implemented. |

### Alternative syntaxes worth considering

If a future language revision touches the parser, these alternatives eliminate
the collision entirely:

**Option A -- Tilde fences: `~~~c ... ~~~`**

```turmeric
(defn foo [x] :int
  ~~~c
  return (int)x * 2;
  ~~~)
```

- Tildes are not used by backtick-fenced Markdown blocks, so there is zero
  ambiguity.
- The markdown injection grammar already accepts `~~~+` for the outer Turmeric
  block, so only the inner C-block delimiter needs to change.
- Downside: tildes look unusual inside Lisp syntax.

**Option B -- Sigil block: `@c{ ... }`**

```turmeric
(defn foo [x] : int
  @c{
    return (int)x * 2;
  })
```

- Braces are balanced, so TextMate `begin`/`end` patterns are trivial.
- The `@` sigil is already visually distinct in Lisp contexts.
- Eliminates Markdown collision entirely.
- Requires a parser change.

**Option C -- `(c! ...)` special form with a raw-string argument**

```turmeric
(defn foo [x] : int
  (c! #|
    return (int)x * 2;
  |#))
```

- Reuses the existing block-comment delimiters as a raw-string carrier.
- No new delimiter characters.
- Looks awkward; block-comment nesting rules may interfere.

### Recommendation

Keep the triple-backtick syntax for now -- the current convention (`) on the
same line as closing backticks) is safe in practice and the grammar change in
this plan handles VSCode correctly. Document the style rule explicitly in
`docs/guides/c-integration-guide.md` and `CLAUDE.md`.

If the language parser is revisited, **Option A (tilde fences)** is the lowest-
friction migration: one character changes, no grammar restructuring, zero
Markdown collision. It would require updating the TextMate grammar, the
c-integration guide, all fixture files, and the stdlib.

---

## Implementation Steps

### Step 1 -- Update `turmeric.tmLanguage.json`

1. Add the `"c-inline"` rule to the `"repository"` object (see grammar section above).
2. Prepend `{"include": "#c-inline"}` to the top-level `"patterns"` array.
3. Prepend `{"include": "#c-inline"}` to `"repository" > "sexp" > "patterns"`.

### Step 2 -- Rebuild the `.vsix`

```sh
cd vscode-syntax-ext
npx vsce package
```

The output `turmeric-syntax-0.1.x.vsix` replaces the existing file.

### Step 3 -- Add a test fixture

Add a file `vscode-syntax-ext/test/test-c-inline.tur` that exercises:

- A C-block at module level (unusual but legal).
- A C-block inside a `defn` (standard).
- A C-block using `union` and pointer casts (real-world complexity from
  `tests/tip/arrow_tests.tur`).
- An `extern-c` declaration adjacent to a C-block (verifies no scope bleed).

Manual verification steps:
- Open the file in VS Code with the updated extension installed.
- Confirm C keywords (`return`, `int64_t`, `if`) are highlighted using the C
  theme colours.
- Confirm the fence delimiters (` ```c `, ` ``` `) receive the punctuation scope
  colour, not the C content colour.
- Confirm no highlighting bleeds outside the block into surrounding Turmeric
  code.

### Step 4 -- Test Markdown embedding

Open or create a Markdown file with a fenced Turmeric block that contains an
inline-C block:

````markdown
```turmeric
(defn sq [x] :int
  ```c
  int64_t v = x;
  return v * v;
  ```)
```
````

Verify:
- The outer fence triggers the `markdown.embedding.turmeric` injection.
- The inner `source.turmeric` grammar activates.
- The inner `c-inline` rule fires correctly.
- No scope leaks; `)` on the closing line is not highlighted as C.

### Step 5 -- Document the style rule

Add a note to `docs/guides/c-integration-guide.md` under the inline-C section:

> **Style rule**: always place the closing ` ``` ` and the enclosing `)` on the
> same line (` ```) `). Putting ` ``` ` on its own line causes Markdown renderers
> to interpret it as the end of any surrounding code fence, breaking rendered
> documentation.

Add the same note to `CLAUDE.md` under the fixture/ASCII section.

### Step 6 -- Update extension version

Bump `vscode-syntax-ext/package.json` `"version"` from `"0.1.0"` to `"0.2.0"`.
Update `README.md` changelog section.

---

## Files Affected

| File | Change |
|------|--------|
| `vscode-syntax-ext/syntaxes/turmeric.tmLanguage.json` | Add `c-inline` rule; wire into patterns and sexp |
| `vscode-syntax-ext/syntaxes/turmeric.markdown-injection.json` | No change needed |
| `vscode-syntax-ext/test/test-c-inline.tur` | New fixture file |
| `vscode-syntax-ext/package.json` | Version bump to 0.2.0 |
| `vscode-syntax-ext/README.md` | Changelog entry |
| `vscode-syntax-ext/turmeric-syntax-0.1.0.vsix` | Rebuild as 0.2.0 |
| `docs/guides/c-integration-guide.md` | Add closing-paren style rule |
| `CLAUDE.md` | Add closing-paren style note alongside ASCII rule |

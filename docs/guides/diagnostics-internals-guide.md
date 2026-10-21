---
title: Diagnostics Internals
category: Compiler Internals
description: How the compiler reports errors and warnings -- the DiagCode enum and its four tables, the diag_emit family, spans through sweet-exp, macros and loaded files, the terminal/JSON/LSP/embed sinks, tur explain, the errors/ fixture harness, and a checklist for adding a code
---

# Diagnostics (internals)

This guide is for contributors who add, change or test a compiler error or
warning. It covers how a diagnostic is coded, emitted, located, rendered and
tested, and what has to change when you add a new code.

It is not about *using* diagnostics. For reading an error as a user, see the
[debugging guide](debugging-guide.md#reading-a-compile-error). For when the
compiler itself misbehaves (a crash, a hang, invalid C), see
[diagnosing the compiler](diagnosing-the-compiler-guide.md). Editor setup is in
the [LSP guide](lsp-guide.md).

> **Trust the code over the comments.** Where a comment and this guide
> disagree, read the code. If the code disagrees with this guide, fix the
> guide. The last batch of stale comments in this area was fixed in
> [stale-comments-diagnostics](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/stale-comments-diagnostics.md).

---

## The short version

1. **A code is an enum member.** `DiagCode` in `src/compiler/diag.h` has 146
   members. Each one also needs a row in up to three hand-written tables in
   `src/compiler/diag.c`: code to string, string to code, and the long-form
   explanation. Nothing checks that the four agree.
2. **Most diagnostics carry no code.** About 1200 call sites use plain
   `diag_emit` and about 320 use `diag_emit_with_code`. Some codes exist only
   as text inside the message, like `(TUR-E0270)`.
3. **Every emission goes through one gate.** `diag_intercept` swallows errors
   during speculative elaboration. Otherwise the emitter sets the error flag,
   then routes the message to exactly one sink: LSP collection, an embedder's
   sink, JSON, or the terminal.
4. **Spans index the text the reader saw.** For a sweet-exp file that is the
   transformed text. Every reported position is translated back
   (`diag_user_pos`), and the snippet maps the offsets itself. Macro
   templates keep the defmacro's spans, so the elaborator adds a note, and
   the LSP re-anchors the error on the macro call.
5. **`diag_had_error()` is the stage gate.** Each driver stage checks it and
   stops.
6. **Tests are substring checks.** An `errors/` fixture must make
   `tur emit-c` fail, and every line of `expected.diag` must appear somewhere
   in stderr.

---

## Source map

| File | Role |
|---|---|
| `src/compiler/diag.h` | `DiagCode`, `DiagLevel`, `SourceFile`, `SweetMap`, `DiagNote`, `DiagSuggestion`, the whole public API |
| `src/compiler/diag.c` | The file registry, capture frames, the code tables, `diag_explanations_[]`, snippet rendering, the four sinks |
| `src/main.c:cmd_explain` | `tur explain` and `tur --explain` |
| `src/main.c` (global flag loop) | `--json-diagnostics`, `--json`, `--no-color`, `--Werror=deprecated` |
| `src/compiler/elab_call.c` (macro arm) | The "in expansion of macro" note and `diag_set_expansion_site` |
| `src/compiler/elab_toplevel.c`, `elab_module.c` | `diag_set_file_origin` for `load` and `import` |
| `src/compiler/elab_toplevel.c:elab_form_impl` (`F_SYM` arm) | Unbound symbol, the did-you-mean search, the `see:` URL |
| `src/lsp/lsp.c` | Publishes diagnostics; `relocate_foreign_diag` |
| `src/lsp/mcp.c` | `tur mcp` check tool; uses the LSP buffer without relocation |
| `src/turi/repl.c` | REPL `:explain` |
| `src/turi/eval.c:turi_eval_with_sink` | The embed sink for a `TuriEnv` |
| `src/web/wasm_glue.c`, `wasm_lsp.c` | The browser playground's explain and check |
| `tests/run.sh:run_negative` | Compiled `errors/` fixtures |
| `tests/run-turi.sh:run_turi_error_fixture` | Interpreted `errors/` fixtures |
| `tests/run-flags.sh` | The only `tur explain` tests |

Two files look relevant but are not diagnostic sinks. `src/lsp/lsp_sink.c`
is the JSON-RPC message transport. `src/cli/lsp_lite.c` is a completion and
docstring helper that never compiles, so it reports no diagnostics.

---

## Codes

### The enum and its four tables

A coded diagnostic needs four things to agree:

| Where | What | Used by |
|---|---|---|
| `diag.h` `enum DiagCode` | `TUR_E0716_TAILCALL_NOT_TAIL` | every emit site |
| `diag.c:diag_code_to_string` | `case ...: return "TUR-E0716";` | the `[TUR-E0716]` in rendered output, JSON `code`, LSP `code` |
| `diag.c:diag_code_from_string` | `if (strcmp(s, "TUR-E0716") == 0) ...` | `tur explain`, REPL `:explain`, the playground |
| `diag.c:diag_explanations_[]` | `{ TUR_E0716_..., "TUR-E0716: ...\n..." }` | `diag_explain` |

They are four hand-maintained lists, and `tests/check-diag-code-tables.py`
(ctest `tur_diag_code_tables`) holds them together from the source text. It
requires that:

- every enum member has a `diag_code_to_string` case, and the string is the
  one its name spells (`TUR_E0716_...` is `"TUR-E0716"`);
- `diag_code_to_string` has no `default:` arm, so `-Wswitch` flags a member
  added without a case;
- every enum member has a `diag_code_from_string` row that parses that string
  back to it;
- every enum member has a `diag_explanations_[]` entry, or is listed in
  `tests/diag-explain-missing.txt`.

That list holds only codes that are never emitted today (`TUR-E0700` and
`E0701` are retired; `E0370`, `E0376`, `E0400`, `E0401` and `E0704` are
unused). It can only shrink: a code that gains text, or leaves the enum,
fails the check until its line is deleted. A new code gets its text, not a
line there.

### Prefixes and bands

The enum holds 115 `E` (error) codes, 26 `W` (warning), 4 `D`
(deprecation) and 1 `I` (`TUR-I0379`, retired).

**The prefix does not set the severity.** The call site picks the
`DiagLevel`. `--strict-refine` turns `TUR-W0372` into an error. In the `D`
band, `TUR-D0002`, `D0003` and `D0004`, and the uncoded "'x' is deprecated"
warning, are emitted at `elab_deprecation_level()` (`elab_internal.h`): a
warning, or an error under `--Werror=deprecated`. `TUR-D0001` is always an
error (`elab_types.c:reject_fn_type_colon`). A new promotable deprecation
calls the same helper.

**Numbers are unique per prefix, not across prefixes.** `TUR-E0706` and
`TUR-W0706` are unrelated, and so are `TUR-E0624` and `TUR-W0624`.

The numbers are grouped by feature. The groups are a habit, not an
allocation table:

| Range | Area |
|---|---|
| E0001-E0026, E0042 | Core typing, scope, typeclasses, effects |
| W0030-W0044 | Effect-row lints and general lints |
| W0050, W0060-W0064, W0070-W0071 | CLI flags, experiments, `#lang`, JIT fallback (printed to stderr directly) |
| E0100-E0108, E0150-E0151 | Linear, borrow, substructural |
| E0200-E0260 | Uniqueness, sessions, handler typing, sized types |
| E0270-E0291 | Reader literals (`#json`, `#map`, `#rat`, `#cx`) and `#row` fields, text-only |
| E0292-E0303 | Construction, union, opaque casts |
| E0304-E0312 | HKT and poly checks (text-only), experiments, engine, Saffron |
| E0330-E0332 | `#lang` line and sweet-exp `$` |
| E0350-E0393 | Intersection, refinement, frames, superclasses |
| E0500-E0502 | Multi-shot capture |
| E0600-E0624, W0600-W0624 | Dynamic vars, manifest (`build.tur`) |
| E0700-E0716, W0706 | Control flow and codegen refusals |
| D0001-D0004 | Deprecations |

### Codes outside the enum

About two dozen codes are never `DiagCode` values. They come in two shapes.

1. **Text in a `diag_emit` message.** The reader's literal errors end their
   message with the code, so it renders as
   `error: #json: ... (TUR-E0270)`, with no `[...]` after `error`. The
   effect checker writes `"[TUR-E0008]: ..."`, which renders as
   `error: [TUR-E0008]: ...`. The
   [effect-rows internals guide](effect-rows-internals-guide.md) covers
   E0008.
2. **A direct `fprintf(stderr, ...)`.** `TUR-W0050` (a retired `-X` flag),
   `TUR-W0060`/`W0061` (`src/runtime/experiments.c:experiment_warn_if_used`),
   `TUR-W0062`-`W0064`, `TUR-W0070`/`W0071` (JIT fallback), and
   `TUR-E0330`/`E0331` (bad `#lang` line, in `main.c` and `turi/eval.c`).
   These bypass every sink below. They never reach JSON, the LSP or an
   embedder.

`TUR-W0060` and `TUR-W0061` are in the enum anyway, so that `tur explain`
can describe them. The other codes in this section have no explanation.

### Retired codes

A retired code keeps its enum member and its string rows, with `RETIRED` in
the comment, so old output and old docs still resolve. Examples: `TUR-E0700`
and `TUR-E0701` (call/cc gating), `TUR-I0379` (the Z3 oracle). Do not reuse a
retired number. `TUR-E0309` was retired before it reached the enum, and is
now only mentioned in comments.

---

## Emitting

### The API

| Function | Carries a code | Extra parts | Call sites (approx.) |
|---|---|---|---|
| `diag_emit(level, span, fmt, ...)` | no | -- | 1215 |
| `diag_emit_with_code(level, span, code, fmt, ...)` | yes | -- | 318 |
| `diag_emit_with_suggestion(level, span, msg, &sug)` | no | `help:` text, `try:` replacement, `see:` URL | 2 |
| `diag_emit_with_suggestion_code(level, span, code, msg, &sug)` | yes | the same | 2 |
| `diag_emit_with_notes(level, span, msg, notes, n)` | no | notes with their own spans | 1 |
| `diag_emit_multi_span(...)` | no | primary label, secondary spans | 0 |
| `diag_emitv` | no | the `va_list` form of `diag_emit` | 0 outside `diag.c` |
| `diag_emit_json(level, span, code, msg)` | yes | the JSON sink itself, not an entry point | 0 outside `diag.c` |

`DiagLevel` has four values: `DIAG_ERROR`, `DIAG_WARNING`, `DIAG_NOTE` and
`DIAG_HELP`. Only `DIAG_ERROR` sets the error flag.

All the variadic entry points carry `TUR_PRINTF_FMT`, so `-Wformat=2` checks
each call's arguments and rejects a non-literal format string. To print a
message you built yourself, use `diag_emit(level, span, "%s", msg)`.

A formatted message is cut off silently at 4096 bytes on the terminal, 1024
in JSON, and 512 for the LSP and the embed sink.

### Which one to use

- **A new error or warning:** `diag_emit_with_code`. A code is what makes a
  diagnostic greppable, explainable and matchable from an `expected.diag`.
- **A follow-up line** ("argument has this type", "in expansion of macro"):
  a separate `diag_emit(DIAG_NOTE, ...)` or `diag_emit(DIAG_HELP, ...)` right
  after the coded one. This is the house idiom, with about 50 `DIAG_NOTE`
  calls written this way. A note is its own record in every sink, at its own
  span.
- **A coded diagnostic with a suggestion:** `diag_emit_with_suggestion_code`.
  The plain `diag_emit_with_suggestion` and `diag_emit_with_notes` take no
  `DiagCode`, so with them the code is lost in all four sinks.

### Suggestions, did-you-mean and `see:`

There are four suggestion sites. Two are in `elab_call.c` (the unbound
call-head check). They print "unknown function or operator", plus a
`help:` line, and sometimes a `try: (load "...")` line naming the stdlib
file that defines the function. The other two are the unbound-symbol check.

The one did-you-mean in the compiler is in the `F_SYM` arm of
`elab_toplevel.c:elab_form_impl`. It walks the scope chain for the binding
with the smallest `sym_levenshtein_distance` below 3. When it finds one, it
calls `diag_emit_with_suggestion_code` with TUR-E0003 and a `doc_url`. When
it finds none, it calls `diag_emit_with_code`. Both carry the code:

```
didyoumean.tur:3:5: error [TUR-E0003]: unbound symbol 'coumt'
1 | (defn main [] : int
2 |   (let [count 1]
3 |     coumt)
  |     ^^^^^
4 |   0)
didyoumean.tur:3:5: help: Did you mean 'count'?
didyoumean.tur:3:5: help: see: https://turmeric-lang.dev/docs/errors/TUR-E0003

unbound.tur:3:5: error [TUR-E0003]: unbound symbol 'zzzqqq'
```

Until 2026-10-17 the suggested case lost `[TUR-E0003]`, and its JSON and LSP
records had an empty `code`
([did-you-mean-unbound-symbol-drops-its-code](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/did-you-mean-unbound-symbol-drops-its-code.md)).

The `see:` line is printed only when `DiagSuggestion.doc_url` is set. Only
the two TUR-E0003 sites set it, and both hard-code the URL. **No page in
this repository backs that URL.** There is no `docs/errors/`, and neither the
site worker (`web/worker.js`) nor the guide generator produces one. The
long-form text lives only in `diag_explanations_[]`. The JSON and LSP sinks
drop `replacement` and `doc_url` and keep only the `help` text.

### Speculative elaboration: capture frames

Some elaboration is a trial run: a body is elaborated at a guessed result
kind and, if it fails, deferred rather than reported. Nine call sites in the
elaborator and `turi/eval.c` bracket such a run with
`diag_push_capture()`/`diag_pop_capture()`.

Inside a frame, `diag_intercept` drops every error, note and help, and
counts the errors. Warnings pass through. `diag_pop_capture` restores the
error flag to its value at the push and returns the count. To see the real
diagnostics, the caller elaborates again with no frame.

`diag_error_serial()` counts errors that were *shown*. The macro expander
reads it before and after an expansion to decide whether to add its note.

---

## Spans and provenance

A `Span` (defined in `src/compiler/forms.h`) carries a `file_id`, a 1-based
`line`, `col_start` and exclusive `col_end`, and byte offsets
`off_start`/`off_end` into that file's `SourceFile.src`. `SPAN_UNKNOWN` has
line 0.

### The file registry

`diag_register_file` stores a `SourceFile *` at `files_[file_id]`. The
registry holds `DIAG_MAX_FILES` (512) entries, and a full registry aborts
with `tur: too many source files`. Ids are handed out in four bands:

| Ids | Who | Where |
|---|---|---|
| 0 | The entry file | the driver |
| 1 .. N-1 | The auto-loaded stdlib | `src/compiler/stdlib_autoload.c:tur_stdlib_prepend_forms`; the end is recorded by `diag_note_autoload_file_ids` |
| N and up | `import` and `load` | `elab_core.c` (`next_import_file_id` starts at `diag_autoload_file_ids_end()`) |
| 511 down | A pass reading a file on its own (R7RS `include`) | `diag_alloc_file_id` |

A span whose file id is not registered renders as `<unknown>:L:C` with no
snippet. This is the usual sign of a `SourceFile` registered on the stack
and then gone, or a registry cleared by `diag_reset()`.

`diag_reset()` clears the error flag and the whole registry. The
interpreter's incremental path keeps its arenas, so it saves and restores
the table around each turn with `diag_files_save`/`diag_files_restore`. A
macro-time evaluator uses `diag_files_replace`, and then
`diag_force_had_error` to put back an error flag the nested reset cleared.

### Sweet-exp and `#lang` offsets

The sweet-exp preprocessor hands the reader transformed s-expression text,
so every span on the tree indexes that text. The `SourceFile` keeps the
user's text in `orig_src`, and a `SweetMap` of verbatim runs in `xform_map`.
`head_offset` counts the bytes of a stripped `#lang` line. The transform
keeps line numbers, but not columns.

There are three translators:

- `diag.c:render_snippet_ex` maps the span through the `SweetMap` and draws
  the excerpt from `orig_src`. The caret is right.
- `diag.c:diag_translate_span` does the same and adds `head_offset`. The LSP
  symbol and scope tables use it (`lsp_scope.c`).
- `diag.c:diag_user_pos` keeps the span's offsets and takes only the line and
  columns `diag_translate_span` computes. Every position that leaves
  `diag.c` goes through it: the `path:line:col` header of each `diag_emit*`
  entry point (with its notes, suggestions and secondary spans), the LSP
  entry (`lsp_append`), the embed-sink record and the `--json-diagnostics`
  record. A span is never translated at the entry point itself, because the
  snippet renderer would then map its offsets a second time.

So the header and the caret agree:

```
sweet.tur:3:14: error [TUR-E0001]: '+' arg 2: type mismatch - expected int, got cstr
1 |
2 | defn main [] : int
3 |   let [x +(1 "x")]
  |              ^^^
```

Until 2026-10-21 only the snippet translated, and the header, JSON and LSP
all said column 15
([sweet-exp-diagnostic-columns-not-translated](../archive/sweet-exp-diagnostic-columns-not-translated.md);
pinned by `errors/sweet-diagnostic-column`).

### Macro expansion

Template forms keep the spans they have in the `defmacro` body. An error
inside an expansion therefore points into the macro's file. Three things
compensate, all in the macro arm of `elab_call.c`:

1. **A note at the call.** The expander reads `diag_error_serial()` before
   and after expanding and elaborating. If an error was shown in between, it
   adds one `DIAG_NOTE` at the call site. It does this on the outermost
   expansion only, so nested macros produce one note, not one per layer.
2. **The expansion site, for the LSP.** On the outermost expansion,
   `diag_set_expansion_site(call->span, name, &prev)` records the call. It is
   saved and restored, not cleared, because a macro-time evaluation runs a
   nested elaborator. `lsp_append` copies the site into each entry raised
   meanwhile.
3. **`definstance` re-spanning.** A macro-generated top-level `definstance`
   gets the call's span, so the orphan check (TUR-E0013) credits the caller's
   file. Its subforms keep the macro's spans.

A macro in a loaded file, used wrongly, prints this in the terminal:

```
mlib.tur:2:10: error [TUR-E0001]: '+' arg 2: type mismatch - expected int, got cstr
2 |   `(+ ~x "oops"))
  |          ^^^^^^
mlib.tur:2:10: note: argument has this type
macro.tur:4:3: note: in expansion of macro 'add-bad' -- the diagnostics above are inside code this call generated
4 |   (add-bad 1))
  |   ^^^^^^^^^^^
```

(Context lines are omitted here.)

The same program over `tur lsp`, abridged:

```json
{"severity":1,
 "range":{"start":{"line":3,"character":2},"end":{"line":3,"character":13}},
 "message":"in mlib.tur:2:10 (expanding add-bad): '+' arg 2: type mismatch - expected int, got cstr",
 "code":"TUR-E0001","source":"turmeric","file":"mlib.tur",
 "relatedInformation":[{"location":{"uri":"file:///.../mlib.tur",
   "range":{"start":{"line":1,"character":9},"end":{"line":1,"character":15}}},
   "message":"'+' arg 2: type mismatch - expected int, got cstr"}]}
```

### Loaded and imported files

`elab_toplevel.c` (for `load`) and `elab_module.c` (for `import`) call
`diag_set_file_origin(file_id, span)` with the span of the form that pulled
the file in. `lsp_append` follows these links to the outermost file, at most
64 steps (a guard against cycles). This lets a diagnostic from a file loaded
by a loaded file land on the document's own `load` line, with `(via mid.tur)`
in the message.

An error in the auto-loaded stdlib that nothing in the document leads to is
placed on the document's first line. Its message says the error is in the
standard library, not in this file.

### R7RS names

The Scheme lowering renames local binders (`n` becomes `n__v12`).
`diag_scheme_names` strips that suffix in messages about `#lang r7rs` files.
`diag_emitv` and `diag_emit_with_code` call it. The suggestion, notes and
multi-span entry points do not.

---

## Sinks

Every entry point runs the same sequence:

1. `diag_intercept`: inside a capture frame, return. Otherwise count shown
   errors.
2. If the level is `DIAG_ERROR`, set the error flag.
3. Use the first sink that is active, in this order: **LSP collection**,
   **embed sink**, **JSON**, **terminal**.

The flag is set before the sink is chosen, so it does not depend on the
sink. The sinks differ only in where the text goes.

### Terminal

```
<path>:<line>:<col>: <level>[ [<CODE>]]: <message>
<snippet>
```

`render_snippet_ex` prints the error line with `DIAG_CONTEXT_LINES` (2) lines
after it and one more than that (3) before it, each with a line-number gutter. It then prints a `^` run
from `col_start` to `col_end`, at least one character wide. A note from
`diag_emit_with_notes` or `diag_emit_multi_span` is underlined with `~`. A
`DIAG_NOTE` from plain `diag_emit` gets `^`.

Only the error line is underlined, so a span over several lines shows its
first line. Color is on when stderr is a TTY and neither `--no-color` nor a
JSON flag was given (`main.c`, `diag_init`).

### JSON (`--json-diagnostics`, `--json`)

`diag_emit_json` writes one pretty-printed object per record to **stderr**:

```json
{
  "severity": "error",
  "code": "TUR-E0003",
  "message": "unbound symbol 'zzzqqq'",
  "file": "unbound.tur",
  "line": 3,
  "col": 5,
  "endLine": 3,
  "endCol": 11
}
```

The objects are concatenated: the output is not an array, and not one
object per line. `endLine` is always equal to `line`. Notes and help are
records of their own. `code` is `""` when there is none. `--json` implies
`--json-diagnostics`.

### LSP collection

`diag_lsp_begin()` makes every emission append a `DiagLspEntry`. Each entry
holds 0-based coordinates, copies of the file path and message, the origin
chain and the expansion site. Paths are copied at append time, because the
`SourceFile` may be on the stack. `diag_lsp_end()` frees the entries.

`tur lsp` flushes with `diag_lsp_flush_array_for(buf, doc_path, relocate,
ctx)`, passing `lsp.c:relocate_foreign_diag`, which moves entries from other
files onto the document. `tur mcp` uses `diag_lsp_flush_array`, which keeps
every entry's own coordinates. `diag_lsp_flush` wraps the array in
`{"diagnostics":[...]}`.

When an entry comes from another file, `lsp_build_array_ex` decides where to
draw it:

1. At the macro call, if the expansion site is in the document. The message
   gets `(expanding <macro>)`.
2. Otherwise at the `load`/`import` that the relocate callback finds. Then
   through the origin chain (`(via ...)`), and then at the recorded origin
   span.

The real location goes in `relatedInformation`, and the message is prefixed
with `in <file>:<line>:<col>`. A zero-width range is widened to one
character.

`tur lsp` compiles a temp copy of the buffer, so it calls
`diag_lsp_remap_path(tmp, doc)` afterwards to put the document's path back.

### Embed sink

`diag_set_sink(fn, ud)` routes each record to a callback as
`(level, code, file, line, col_start, col_end, message, ud)`. This is how a
libturi embedder (the Godot binding) gets per-script errors.
`turi_eval_with_sink` installs it around each evaluation of a `TuriEnv`, and
restores the previous one afterwards. A record that a capture frame swallows
is not delivered. LSP collection takes precedence over the sink.

### Error-flag gating

`diag_had_error()` is a single process-wide flag. The driver checks it after
each stage and returns 1. For example, after elaboration in
`main.c:run_core_pass`: `if (!ctx->prog || diag_had_error()) return 1;`.
Warnings never stop a build.

Because the flag is global, a driver that compiles several files resets it
between them with `diag_reset()`. Otherwise one file's error fails the next
file's gate. The comments around the `diag_reset()` calls in `main.c` say
which reset protects which gate.

---

## `tur explain`

`tur explain <arg>` and `tur --explain <arg>` both reach `main.c:cmd_explain`:

1. If `diag_looks_like_code(arg)` (that is, `TUR-` then `E`, `W`, `D` or
   `I`, then digits), it calls `diag_code_from_string`, then `diag_explain`.
   It prints the text, or `tur: no explanation for '<arg>'` and exits 1.
2. Otherwise it treats `arg` as **source code**: it reads and elaborates it
   as `<explain>`, prints any diagnostics, and exits 0 if there are none.

A new prefix has to be added to `diag_looks_like_code` as well as to the
table; one it does not accept is elaborated as source, and comes back as
"unbound symbol 'TUR-X0001'".

The REPL's `:explain [code]` (`turi/repl.c`) and the playground
(`web/wasm_glue.c`) use the same three functions. Bare `:explain` explains
the last code the REPL saw.

**Where the text lives.** Only in `diag_explanations_[]`, as C string
literals: a header line `TUR-XNNNN: Title`, a blank line, prose, usually an
`Example:` block, and often a `See docs/...` pointer. There is no generated
page and no other copy.

**What keeps it in sync.** `tests/check-diag-code-tables.py` (ctest
`tur_diag_code_tables`) compares the enum with the three tables, as described
under [The enum and its four tables](#the-enum-and-its-four-tables).
`tests/run-flags.sh` (ctest `tur_flags_tests`) checks the user-visible half:
that a sample of codes, including the `I` prefix, are found and printed by
`tur explain`, that `TUR-E9999` exits non-zero, and that the snippet mode
works. `tests/check-docs-lint.sh` covers `docs/reported/` and the SRFI table,
not codes.

---

## Testing a diagnostic

### The `errors/` layout

```
tests/fixtures/errors/<name>/
  input.tur            required
  expected.diag        substrings that must appear in stderr, one per line
  unexpected.diag      substrings that must NOT appear (one fixture uses it)
  flags                global flags placed before `emit-c`
  expected.stderr      run-turi.sh's fallback needles when there is no expected.diag
  requires.*           skip markers (see below)
  expected.timeout     seconds, default 10
```

There are 745 fixture directories today. 737 have an `expected.diag`. Seven
`fh-*`/borrow fixtures use only `expected.stderr`, and one is owned by a
dedicated runner.

### Compiled path: `tests/run.sh:run_negative`

The harness runs `$TUR $flags emit-c input.tur`, with stdout discarded and
stderr written to `actual.stderr` in the fixture directory. Then:

1. A timeout fails the fixture, and so does **exit status 0**. The program
   must be rejected.
2. If there is an `expected.diag`, each non-empty line must occur in stderr
   (`grep -F`). This is a fixed-string match anywhere in the output. Order,
   line position and count are not checked.
3. If there is an `unexpected.diag`, none of its lines may occur.

There is no exact-match mode. The stdout-style golden file of the happy
path has no equivalent here. Without an `expected.diag`, any failure passes.

Markers honoured here: `requires.dedicated-runner`, `requires.spices`,
`requires.posix-apis` and `requires.interp-only` skip the fixture.
`requires.compiled` is accepted and does nothing here.

Writing good needles:

- Put the code on a line of its own (`TUR-E0716`), then a distinctive phrase
  from the message on another line. The code alone also matches the
  diagnostic you did not mean. The phrase alone survives a code change that
  should have failed.
- Do not match a path, line or column unless that is the point of the test.
- End the file with a newline. Both harnesses read an unterminated last line
  now, but a needle loop written as a bare `while read -r` drops it, and a
  one-line file then asserts nothing
  ([needle-files-without-trailing-newline-assert-nothing](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/needle-files-without-trailing-newline-assert-nothing.md)).
- Editing a needle re-runs the fixture: `stamp_key` hashes every file in the
  fixture directory except the outputs the harness writes there
  ([run-sh-stamp-cache-ignores-expectation-files](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/run-sh-stamp-cache-ignores-expectation-files.md)).

A real fixture, `errors/tailcall-argument-position/expected.diag`:

```
TUR-E0716
`^tailcall` call is not in tail position
the call sits in an argument position
```

### A warning

A warning does not fail the build, so it cannot be an `errors/` fixture. Pin
it in a happy-path fixture instead. `run.sh` writes `tur build`'s stderr to
the same file the program's stderr is appended to, so `expected.stderr`
needles see compile-time warnings. `unexpected.stderr` pins their absence.
TUR-W0044 is tested this way: `await-below-unthreaded-frame-refused` has
the warning in `expected.stderr`, and five pending-future fixtures list it in
`unexpected.stderr`.

### Interpreter path: `tests/run-turi.sh:run_turi_error_fixture`

The same directories run under `tur $flags --interpret input.tur`. The needles
come from `expected.diag`, or from `expected.stderr` when there is none. A
fixture with neither fails as "asserts nothing". Differences from `run.sh`:

- `unexpected.diag` is not read. The exit status is checked: an interpreter
  that prints the diagnostic and then runs the program to exit 0 fails the
  fixture, as it does in `run.sh`
  ([turi-ignores-effect-row-errors](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-ignores-effect-row-errors.md)).
- `requires.compiled` skips the fixture. Use it when only the compiled path
  diagnoses the program, as all the `tailcall-*` fixtures do.
- `TURI_ERRORS_DENY` lists fixtures whose interpreter diagnostics diverge.
  It is empty today. An entry naming a missing fixture stops the run.

### Interpreter-only diagnostics

A diagnostic that only `--interpret` produces, such as a runtime check the
compiled runtime cannot make, gets `requires.interp-only`. `run.sh` then
skips it and `run-turi.sh` owns it. The marker's contents explain why.
For example, `errors/session-deadlock-no-peer-turi` receives on a session
that nothing will ever send to. The interpreter's cooperative scheduler sees
that nothing is runnable; the compiled runtime just hangs. Its
`expected.diag` is:

```
session recv deadlocked
no sender
on Session
```

Its marker says it "asserts the diagnostic and the nonzero exit", and
`run-turi.sh` checks both.

Elaboration errors look the same on both engines, since the interpreter
elaborates through the same `diag_emit*` calls. Runtime errors from the
interpreter (`turi_errorf`) do not go through `diag.c`.

---

## Adding a new code: checklist

This is the set of files the TUR-W0044 commit (`8cb6c2a51`) and the
TUR-E0716 commit (`72c37c1c5`) touched, generalized:

1. **Pick the number.** Use the next free number in the feature's band, in
   the right prefix. Search for it in `src/` and `docs/` first: some codes
   exist only as message text and are not in the enum. Never reuse a retired
   number.
2. **`diag.h`.** Add the enum member with a comment. Say what it rejects and
   why, and name the report or plan it comes from.
3. **`diag.c:diag_code_to_string`.** Add the `case`. Without it the output
   reads `error:` with no code, and `expected.diag` cannot match it.
4. **`diag.c:diag_code_from_string`.** Add the `strcmp` row. Without it
   `tur explain` and `:explain` cannot find the code.
5. **`diag.c:diag_explanations_[]`.** Add the long form, in the format
   above, with an example of the mistake and the fix. Steps 3 to 5 are
   checked: `python3 tests/check-diag-code-tables.py` fails on a code
   missing from any of them.
6. **The emit site.** Use `diag_emit_with_code`, and add follow-up
   `DIAG_NOTE`/`DIAG_HELP` lines with plain `diag_emit`. Pick the level at
   the site. If a flag escalates it, read the flag there (see the
   `g_werror_deprecated` uses).
7. **A fixture.**
   - An error: `tests/fixtures/errors/<name>/` with `input.tur` and an
     `expected.diag` that ends in a newline. Add `requires.compiled` if only
     the compiled path diagnoses it, or `requires.interp-only` if only the
     interpreter does.
   - A warning: `expected.stderr` in a happy-path fixture, and
     `unexpected.stderr` on the near misses it must not fire on.
   - Run the fixture with `TUR_FORCE=1` and a filter (see the
     [running-tests guide](running-tests-guide.md)).
8. **Optional:** a `tur explain` check in `tests/run-flags.sh`; a mention in
   the feature's user guide (W0044 updated `async-await-guide.md`, E0716
   `performance-guide.md`); a `turi/docstrings.c` entry for new syntax (E0716
   added `^tailcall`).
9. **Not needed:** there is no other registry and no `docs/errors/` page, and
   `tests/check-docs-lint.sh` does not check codes.

If the diagnostic belongs to an experiment, that is the experiment's own
`TUR-W0060`/`W0061`, which `experiment_warn_if_used` prints. It does not need
a new code. See the [experimental flags guide](experimental-flags-guide.md).

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

None open. [sweet-exp-diagnostic-columns-not-translated](../archive/sweet-exp-diagnostic-columns-not-translated.md)
was fixed 2026-10-21.

## Maintenance

Update this guide in the same PR when you:

- add a `diag_emit*` entry point, a sink, or a `DiagLevel`;
- change the sink order, the JSON shape or the LSP relocation rules;
- change the file-id bands or `DIAG_MAX_FILES`;
- change how `run.sh` or `run-turi.sh` judge an `errors/` fixture;
- add a check that keeps the code tables in sync. Then rewrite the
  "What keeps it in sync" paragraph and the checklist.

The counts here (enum members, call sites, fixtures) are from `grep`. Treat
them as orders of magnitude. The example outputs were produced with
`./build/tur check`, `--json-diagnostics` and a `tur lsp` session. Refresh
them when the format they show changes. Cite functions, not line numbers.

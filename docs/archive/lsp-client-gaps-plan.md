# LSP Client Gaps Plan

> **Status:** Executed -- everything scheduled in §6 has landed
> **Last Updated:** 2026-07-27
> **Type:** Tooling / `tur lsp`

---

## Overview

Every item here was found by writing a real LSP client against `tur lsp` —
[Trowel](https://github.com/turmeric-lang/trowel), the native Turmeric editor —
and hitting something that had to be worked around in the client rather than
fixed in the server. That is the value of the list: these are not speculative
gaps, they are places where a shipping consumer already carries compensating
code, with the workaround pinned to a file and line.

Paths marked **(trowel)** are in the Trowel repository; unmarked paths are in
this one.

A first tranche has already landed on the `lsp-phase2` branch (commit
`36572dda8`). Trowel does not benefit from it yet: it bundles a pinned
prebuilt toolchain, so every fix here needs a Turmeric release plus a bump of
`TROWEL_TURMERIC_VERSION` and its three per-arch SHA-256s before a user sees
it.

---

## 0. Status after execution

Everything in §6's ordered list is implemented, plus all of §4. What is left
is exactly what §6 declined to schedule.

| Gap | State |
|---|---|
| §2.1 Completion needs a successful compile | Done -- last-good symbol index retained |
| §2.2 Nothing at offset 0 | Done -- prefix now scans left only |
| §2.3 Bare `CompletionItem[]` | Done -- emits a `CompletionList` |
| §2.4 Unaffordable space trigger | Done -- triggers are `["("]` |
| §2.5 Cap truncates local symbols | Done -- document-local emitted first, `isIncomplete` set |
| §3.1 No `$/cancelRequest` | Done -- honoured across the analysis flush |
| §3.2 No `textDocument/formatting` | Done -- in-process, shared with `tur fmt` |
| §3.3 No `textDocument/signatureHelp` | Done |
| §3.4 No semantic tokens | Not scheduled (unchanged) |
| §3.5 `workspace/symbol` is open-documents-only | Not scheduled (unchanged) |
| §4 Analysis is path-based | Not a server gap -- see the note in §4 |
| §4 Zero-width diagnostic ranges | Done -- widened in `diag_lsp_flush_array` |
| §4 Hover ignores `contentFormat` | Done |
| §4 Only `contentChanges[0]` read | Done -- last change wins |
| §4 Hardcoded `/tmp` | Done -- `tur_temp_dir()` |
| §5 OSC 133 markers | Done -- `TUR_SHELL_INTEGRATION=1`, plus C/D markers |
| §5 `getcwd()` resolves symlinks | Done -- `pwd -L` semantics, inode-checked (see the verification note below) |
| §5 No region/string eval | Done -- `:load-string "<src>"` |
| §5 `TUR_STDLIB_DIR` leak | Partly -- invalid values rejected; a stale-but-valid one still wins |

Where the work landed:

- `src/lsp/lsp.c` -- handlers, capabilities, cancel queue, symbol retention
- `src/lsp/lsp_util.c` -- `lsp_offset_at_pos`, `lsp_prefix_at_pos`,
  `lsp_enclosing_call`
- `src/lsp/lsp_docs.{c,h}` -- `LspDoc.symbols_stale`
- `src/compiler/fmt.c` -- `fmt_format_buffer`, the shared format pipeline
  (moved out of `main.c` so `tur_core`, which carries the LSP, can link it)
- `src/compiler/diag.c` -- zero-width range widening
- `tests/lsp/mcp_lsp_test.py` -- `test_lsp_client_gaps`,
  `test_lsp_unsaved_buffer`
- `src/turi/repl.c` -- OSC 133 C/D markers, logical cwd, `:load-string` (§5)
- `src/main.c` -- `TUR_STDLIB_DIR` validation (§5)
- `tests/turi/repl-host-integration.sh` -- §5 coverage (ctest target
  `tur_repl_host_integration`)

The release-and-version-bump caveat above still applies: Trowel sees none of
this until a Turmeric release ships and `TROWEL_TURMERIC_VERSION` moves.

---

## 1. Done — landed on `lsp-phase2`, awaiting a release

| Gap | Client workaround it retires |
|---|---|
| Full recompile ran inline on every `didChange` | 250ms debounce in `lsp_manager.cpp:21` **(trowel)** |
| `character` was a byte offset but nothing said so | `kPositionEncoding = Utf8` in `lsp_position.h:16` **(trowel)** |
| `\uXXXX` was passed through with the backslash dropped | *none possible* — see below |
| A request with missing `params` got no response at all | *none possible* — the client simply hung |

The `\uXXXX` bug deserves a note because it had no client-side mitigation and
was invisible until measured. `unescape_json` turned `é` into the five
literal characters `u00e9`, which both corrupted the text and shifted every
byte offset after it. For `(def a "é") (def later 2)`, `later` was reported at
character 21 instead of 18 — and every diagnostic, hover, and definition later
in the file drifted by the same amount. Any file containing a single non-ASCII
character in a string literal had silently wrong positions from that point on.

---

## 2. Completion is the weakest surface

Ordered by how much they hurt in practice.

### 2.1 Completion requires a *successful* compile — the big one

`on_completion` serves symbols gathered by `run_doc_analysis`, which collects
them from a full compile. A buffer that does not parse yields no symbols, so
completion returns nothing.

The problem is that *not parsing is the normal state while typing*. The moment
a user types `(`, or `(foo`, the buffer is unbalanced and completion goes
silent — precisely when it is wanted. Verified directly: appending `\n(smoke`
to an otherwise valid file drops completion from 200 items to zero.

No client can work around this. Proposals, cheapest first:

1. **Retain the last good symbol set** per document and serve completions from
   it when the current parse fails. A few stale entries beat an empty list, and
   the symbol table changes far more slowly than the text does.
2. **Error-tolerant parsing** for the symbol-collection pass — recover at the
   next top-level form rather than abandoning the file.

(1) is a small change with most of the benefit and should land first.

**Landed: (1).** `run_doc_analysis` collects into a scratch buffer and adopts
it only when the compile succeeded or produced at least one symbol; otherwise
the previous index stays in place and `LspDoc.symbols_stale` is set. The
"succeeded" half of that condition matters — without it, deleting every
definition from a file would leave its symbols visible forever, because an
empty result is indistinguishable from a failed one by count alone.

The verification case from above now holds: appending `\n(smoke` to a valid
file leaves completion at 200 items instead of dropping it to zero.

(2) is still open, and is the better long-term answer. Retention goes stale
across a rename or a large paste; error-tolerant parsing would not.

**Follow-up landed: retention now primes.** A consumer review found that
retention only helped a document that had *already* parsed once -- a file
opened with a syntax error already in it had nothing to retain and completion
stayed empty, which is when it is wanted most. `LspDoc.ever_analyzed` fixes
the priming, and a lazily-built stdlib symbol cache backs the never-parsed
case. Details and measurements in
[docs/archive/history/lsp-symbol-retention-never-primes.md](history/lsp-symbol-retention-never-primes.md);
coverage in `test_lsp_unprimed_completion`.

### 2.2 Returns nothing at offset 0

Completion at `{"line": 0, "character": 0}` returns an empty array while the
same document at the end of the buffer returns 200 items. A client that
requests completions immediately after opening a file — the obvious thing to
do — gets nothing and cannot tell that from "no matches".

**Landed.** The cause turned out to be the prefix extraction, not the position
handling. `on_completion` derived its prefix from `lsp_word_at_pos`, which is
built for hover and *steps right* when the cursor is not on an identifier
character. At offset 0 of `(defn foo ...)` that yielded the prefix `"defn"`,
which filtered every candidate away — an empty list that looked like "no
symbols" but was really "no symbol starts with defn". The same bug fired
anywhere the cursor sat just before a word.

Completion now uses `lsp_prefix_at_pos`, which only ever scans left. At offset
0 that is the empty string, meaning "offer everything".

### 2.3 Returns a bare `CompletionItem[]`, not a `CompletionList`

Legal per the specification, but the asymmetry means every client needs the
both-shapes branch that Trowel carries at `lsp_manager.cpp:347` **(trowel)**.
Worth emitting a `CompletionList` for uniformity.

**Landed.** `{"isIncomplete": bool, "items": [...]}`. Uniformity was the stated
reason, but the list shape also carries `isIncomplete`, which is the only way
to tell a client that the cap truncated the result and it should re-query as
the prefix narrows — see §2.5.

### 2.4 Advertises a space trigger character that is too expensive to honor

`completionProvider.triggerCharacters` is `["(", " "]`. In a lisp, space fires
on nearly every keystroke, and each trigger forced a `didChange` plus a full
compile. Trowel honors `(` only and binds the rest to an explicit key
(`editor_view.cpp:57` **(trowel)**). Now that analysis is debounced this is
less costly, but the advertised trigger set should still match what is actually
affordable.

**Landed.** `triggerCharacters` is `["("]`. A space is still a perfectly good
*retrigger* for signature help, where the work is a symbol lookup rather than a
compile, so `signatureHelpProvider` advertises it there.

### 2.5 Capped at 200 items, in document order

Clients that present a sorted list must sort and dedupe themselves
(`editor_view.cpp:368` **(trowel)**). Prefix-filtering server-side would be
more useful than a raw cap, which currently truncates the buffer's *own*
symbols behind 200 stdlib entries.

**Landed.** Prefix filtering was already there; what it lacked was a working
prefix (§2.2), so in practice the cap was doing all the work. With that fixed,
items are emitted in two passes — document-local symbols, then everything else
— so the buffer's own definitions are never the ones the cap drops. When it
does truncate, `isIncomplete` says so.

A full server-side sort is still not done: two passes give the ranking that
matters at a fraction of the cost, and a client that wants strict alphabetical
order can still sort a bounded 200-item list cheaply.

---

## 3. Missing protocol surface

### 3.1 No `$/cancelRequest`

The server is single-threaded and synchronous, so a slow analysis blocks every
request behind it with no way to abandon one. Trowel compensates with 1.5s
timeouts, a single-in-flight cap, and a per-document generation counter that
drops replies which arrive after the buffer moved on (`lsp_client.h:57`
**(trowel)**). Debouncing reduced the exposure but did not remove it.

**Landed**, without restructuring the request loop — which is what made this
the largest item on the list. The observation that avoids the restructure:
being single-threaded is what leaves no window for a cancel, and the one step
long enough to matter is the analysis flush. So after flushing, and *before*
committing to the per-request work, the server drains whatever the client sent
meanwhile off the fd into a queue. Cancels found there apply immediately;
everything else stays queued in order and is served on the following
iterations. A cancelled request is answered `-32800`; a cancel for a request
already on the wire is a no-op, exactly as the spec asks.

This is narrower than true cancellation — a request that is slow for some
reason *other* than the flush still cannot be abandoned — but it covers the
case the timeouts were built for.

### 3.2 No `textDocument/formatting`

`tur format` exists and works; it is simply not reachable over LSP. Both known
clients therefore shell out to it:

- VS Code, via `execSync` in `vscode-syntax-ext/extension.js:11-34`.
- Trowel, via a **blocking** `QProcess` in `main_window.cpp:936-945`
  **(trowel)** that can freeze the UI for up to ten seconds.

Wiring the existing formatter to a `textDocument/formatting` handler removes a
real UI stall and deletes duplicated client code.

**Landed.** One wrinkle worth recording: the format pipeline lived in
`main.c`, and the LSP is compiled into the `tur_core` object library, which
`main.c` is not part of — a handler calling into it would not link (and every
unit test that links `tur_core` would have needed another stub). So
`fmt_format_source`'s body moved to `src/compiler/fmt.c` as
`fmt_format_buffer`, and `main.c` now delegates to it. One implementation,
reachable from both, and `tur fmt` and the LSP cannot drift apart.

The handler returns a single full-document `TextEdit` and deliberately ignores
`FormattingOptions`; an unparseable buffer returns `null` ("no edits") rather
than an error, since a save-time format on a half-typed buffer should not
raise a popup.

### 3.3 No `textDocument/signatureHelp`

No argument hints while typing a call. The calltip logic already exists,
unwired, in `src/cli/lsp_lite.c` (`calltip` method) — this is mostly a matter
of exposing what is already written.

**Landed**, though not by reusing `lsp_lite.c`. That path answers "first line
of the docstring for this name", which is a calltip but not signature help: it
carries no parameter list and no notion of which argument the cursor is in,
and it reads a different symbol source than the LSP's own index.

What the handler needs instead is the enclosing call, which
`lsp_enclosing_call` computes by scanning forward from the start of the buffer
— string- and comment-aware — keeping a stack of open `(` forms. Forward is
what makes quoting work: a `(` inside a string or a `;` comment must not open
a frame, and a backward scan cannot know that without re-reading the file
anyway. Parameter labels come from the bracketed run in the symbol's rendered
type (`(fn [int int] : int)`); `LspSymbol` does not carry parameter *names*,
so the labels are types. `activeParameter` accounts for whether the cursor sits
at the end of the argument being typed or after the whitespace that follows it,
which is the difference between `(f 1|` (argument 0) and `(f 1 |` (argument 1).

### 3.4 No semantic tokens

Every editor must hand-write and maintain a Turmeric lexer. Trowel's is 447
lines (`scanner_turmeric.cpp` **(trowel)**) and will drift from the language as
it evolves.

### 3.5 `workspace/symbol` covers open documents only

`on_workspace_symbol` iterates the open-document store, so there is no
project-wide symbol search and no cross-file go-to-definition for a file the
user has not already opened. This also shapes client architecture: Trowel runs
one shared server for the whole application rather than one per window,
because a per-window `rootUri` currently buys nothing.

---

## 4. Smaller papercuts

- **Analysis is path-based.** `run_doc_analysis` writes the buffer to a temp
  file and remaps diagnostics back, so a document needs a real filesystem path.
  Trowel therefore gives unsaved buffers no language support at all
  (`lsp_manager.cpp:76` **(trowel)**) — a visible gap for a new file.

  *Resolved as a non-gap.* Measured directly: an `untitled:` URI with no
  filesystem path gets symbols, diagnostics, completion, and formatting like
  any other document, because the temp file is what the compiler actually
  reads — the document's own path is only ever used to remap results back.
  `doc->path` for an untitled URI is a harmless non-path string. The
  restriction is client-side, and `test_lsp_unsaved_buffer` pins the server
  behaviour so it stays that way. What genuinely does not work for an unsaved
  buffer is a relative `(import ./sibling)`, since the temp file has no
  sibling — a separate problem, and one that affects saved files opened
  outside their project too.
- **Zero-width diagnostic ranges.** Some diagnostics report `start == end`,
  which paints nothing; clients must widen them to stay visible.

  *Resolved* in `lsp_build_array` (`src/compiler/diag.c`): an end column not
  greater than the start column is bumped by one. Widening once at the source
  beats every client doing it.
- **Hover ignores `hover.contentFormat`.** The client capability is never read
  anywhere in `src/lsp/`; markdown with ``` fences is always returned. A client
  whose tooltip surface is plain text — Scintilla call tips, for instance —
  must strip them (`editor_view.cpp:382` **(trowel)**).

  *Resolved.* `on_initialize` reads
  `capabilities.textDocument.hover.contentFormat` and records whether markdown
  is acceptable; `on_hover` emits the fences and the matching `kind` only when
  it is. An absent capability keeps the 3.17 default of markdown.
- **Full-document sync only** (`textDocumentSync: 1`). `on_did_change` reads
  only `contentChanges[0].text` and would silently mishandle ranges if a client
  sent them. Lower priority now that analysis is debounced.

  *Partly resolved.* `on_did_change` now walks to the **last** change object
  rather than the first, so a batched notification is no longer silently
  truncated to its first element. Sync is still Full-only; a client that
  ignores the negotiated kind and sends ranged edits is still wrong, but it is
  now wrong about the newest edit rather than the oldest.
- **Analysis writes to hardcoded `/tmp`.** Fine on Unix, wrong on Windows, and
  it means one temp file per analysis.

  *Resolved.* The directory now comes from `tur_temp_dir()`
  (`src/platform_fs.h`), which already existed for exactly this reason: on
  Windows a leading `/` means "root of the current drive", so `/tmp/...`
  resolved to a `C:\tmp` that does not exist and every analysis silently
  produced nothing. Still one temp file per analysis — that part is unchanged.

---

## 5. Not LSP, but same origin

Found the same way — a real client hitting real behavior.

All four are now **done**. Every one reproduced exactly as described before
being changed; the repros are pinned in
`tests/turi/repl-host-integration.sh` (ctest target
`tur_repl_host_integration`).

- **OSC 133;A prompt markers** are absent from older `tur` and when not on a
  tty, leaving a client unable to tell busy from idle. Trowel latches "busy"
  forever as the safe default (`repl_session.h:63` **(trowel)**).

  *Done.* Two separate problems were tangled here. The first is reach:
  `TUR_SHELL_INTEGRATION=1` now forces markers on when stdout is not a tty,
  which is the GUI-host case — driving the REPL over pipes rather than a pty
  was the one configuration that could never get them.
  `TUR_NO_SHELL_INTEGRATION` still wins if both are set, and the default over
  a pipe stays off so a redirect nobody asked for is not corrupted with
  escapes.

  The second is that `A` alone is not enough to latch on. `A` marks the idle
  edge but nothing marked the busy one, so a host could see that a prompt had
  been written but not when work started or ended. `133;C` (evaluation
  starting) and `133;D;<status>` (finished, `0`/`1`) now bracket the eval, so
  the cycle is A → idle, C → busy, D → done, with the status distinguishing a
  failed form without parsing stderr.

  `133;B` is deliberately not emitted; the reasoning is in the guide and in a
  comment at the emit site. Placing it correctly means writing it inside
  editline's own output, behind `\1..\2` guards libedit does not reliably
  honor — a visibly corrupted prompt is a worse outcome than an absent marker
  that only serves command extraction.
- **The REPL reports `getcwd()`**, which resolves symlinks (`/private/var/…` on
  macOS) while the client's own path does not, so naive comparison never
  matches. Trowel compares canonically (`repl_session.cpp:52` **(trowel)**, on
  its unmerged `repl-cwd-indicator` branch).

  *Done*, with `pwd -L` semantics: `$PWD` is reported when it still names the
  current directory, `getcwd()` otherwise. The safety check is a
  `(device, inode)` comparison against `.`, so a stale or hostile `$PWD` can
  never make the REPL claim a path that is not this directory — verified with
  a `$PWD` pointing somewhere that does not exist. `:cd` maintains `$PWD`
  itself, normalizing `.`/`..` textually, and drops it when the normalized
  result fails the identity check (a `..` that crossed a symlink), which
  falls the report back to `getcwd()`.

  One divergence worth knowing: `:cd` still moves *physically*, so `:cd ..`
  from a symlinked directory lands in the real parent where a shell's logical
  `cd` would go to the symlink's parent. That was out of scope here — the
  report was about what gets reported, and reporting is now truthful in both
  cases. Documented in the guide rather than silently changed.

  **Verification basis.** Exercised on Linux by
  `tests/turi/repl-host-integration.sh`, including the `..`-across-a-symlink
  case, a `$PWD` pointing at a directory that does not exist, and all three
  `TMPDIR` shapes (unset, with and without a trailing slash). The mechanism is
  platform-independent — `$PWD` versus `getcwd()`, arbitrated by a
  `(device, inode)` comparison — but it has **not been run on macOS**, which is
  the platform whose `/var` → `/private/var` symlink motivated the item. A
  consumer report against `8f341d32e` saw these cases red on macOS; that was
  the harness's trailing-slash `TMPDIR` handling, fixed in `45962aec` (the
  failing set matched the harness signature, not the product one — a product
  genuinely reporting physical paths fails a *different* set, including the
  `check_absent` leak case that report showed passing). Re-confirmation on real
  macOS hardware is still wanted.
- **No way to evaluate a region or a string.** To run a selection, Trowel
  writes a scratch file and sends `(load "…")` (`run_buffer.cpp:45`
  **(trowel)**). A `:load-string` style meta-command would remove the
  round-trip through disk.

  *Done.* `:load-string "<src>"` takes one double-quoted literal with C-style
  escapes, so an arbitrary multi-line region collapses onto the single line
  the prompt reads. Cheap to implement because `turi_eval_file` is already
  just read-file plus `turi_eval` — the file was never doing any work.

  It routes through the same `repl_do_eval` path an interactively typed form
  takes rather than calling `turi_eval` directly, so Show-instance rendering,
  the `_` binding, and error reporting are identical instead of being a second
  approximation that drifts.
- **`TUR_STDLIB_DIR` leaks from the ambient environment** and can pair a new
  `tur` with an old stdlib. Both of Trowel's subprocess launchers defensively
  pin it to the `stdlib/` sitting next to the resolved binary.

  *Done, partially — read the limit.* `resolve_stdlib_root` now checks that
  `$TUR_STDLIB_DIR/macros.tur` is readable (the same anchor the walk-up probe
  uses) before honoring it. A directory that fails is reported in one line
  naming the variable, then unset so every downstream reader agrees with the
  resolved value, and the walk-up finds the stdlib beside the binary.

  That fixes the loud half: previously a stale value was taken verbatim and
  the first sign of trouble was a wall of `load: cannot open .../macros.tur`
  with nothing pointing at the cause.

  It does **not** fix the quiet half. A `TUR_STDLIB_DIR` pointing at an older
  but *intact* stdlib still passes the check and is still honored — correctly,
  since an explicit override has to remain an override. Detecting that would
  need a version marker in the stdlib tree to compare against the binary,
  which does not exist today. Until it does, a host that cares must keep
  pinning the variable itself, exactly as Trowel does.

---

## 6. Suggested order — all executed

1. **Retain last-good symbols** (§2.1) — small, and fixes the most damaging
   day-to-day behavior.
2. **`textDocument/formatting`** (§3.2) — the formatter exists; this removes a
   UI freeze and duplicated client code.
3. **Completion at offset 0 and prefix filtering** (§2.2, §2.5).
4. **`signatureHelp`** (§3.3) — logic already written in `lsp_lite.c`.
5. **`$/cancelRequest`** (§3.1) — needs the request loop restructured; largest
   change here.
6. Everything in §4 as opportunistic cleanup.

Semantic tokens (§3.4) and workspace-wide symbols (§3.5) are larger features
rather than gap-filling, and are deliberately left unscheduled.

All six are done. What remains open, and deliberately so:

- **§3.4 semantic tokens** and **§3.5 workspace-wide symbols** — unscheduled
  as stated above.
- **§2.1 proposal (2), error-tolerant parsing** — retention covers the
  day-to-day case; recovery at the next top-level form is the better answer
  when someone wants to spend the time.
- **§5's stale-but-valid `TUR_STDLIB_DIR`** — needs a version marker in the
  stdlib tree to detect; see the note in §5. Everything else in §5 is done.
- **Logical `:cd`** — `:cd` moves physically while reporting truthfully. Making
  the move itself logical (shell `cd` semantics) is a behavior change nobody
  has asked for; noted in the REPL guide.

Coverage lives in `tests/lsp/mcp_lsp_test.py`
(`test_lsp_client_gaps`, `test_lsp_unsaved_buffer`), run by
`tests/lsp/run-mcp-lsp.sh`, and in `tests/turi/repl-host-integration.sh`
(ctest target `tur_repl_host_integration`) for §5.

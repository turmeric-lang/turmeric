# `tur lsp` advertised `(` as a signature-help trigger, which a lisp can never answer

**Severity:** low-medium.
**Status:** RESOLVED (2026-10-03). The trigger is now space, and a signature
request no longer forces an analysis unless the index lacks the callee. See
[Resolution](#resolution).
**Found:** 2026-10-03, against v0.60.1.

*Rewritten after investigation. The measurements in the original filing were
right; its suggested fix -- "answer at the `(`, the callee is already known
there" -- was not, and the cost argument needed one more fact. Both are
corrected below.*

## What happens

`initialize` declared:

```json
"signatureHelpProvider": { "triggerCharacters": ["("], "retriggerCharacters": [" "] }
```

Driving `tur lsp` over stdio on `(defn nav-use [] : int (nav-double nav-total))`:

| cursor | result |
|---|---|
| col 24 -- just after the `(` | `null` |
| col 34 -- just after the callee's name | `null` |
| col 35 -- after the name **and a space** | the `nav-double` signature, `activeParameter` 0 |
| col 44 -- at the end of the argument list | the same signature |

So a client that honors the advertised trigger gets nothing back, and pays for
a request every time `(` is typed.

## Why `(` cannot be answered

`lsp_enclosing_call` (`src/lsp/lsp_util.c:112`) scans the text **up to the
cursor** to find the innermost open call and its head. In the measurement
above the callee happened to be written already, *after* the cursor. But in
the case a trigger exists for -- someone typing -- nothing follows a `(` that
was typed a moment ago. In a lisp the callee comes after the paren, so at the
moment `(` is typed there is no name to look up. A `(` trigger is a C-family
convention (`f(`), where the name comes first. Answering "at the `(`" would
mean guessing.

The column-34 `null` is the same rule: the head is still being typed, so
there is no call yet.

## Why space looked unaffordable, and was not

The first position with an answer is the space after the head. The original
filing argued that a space trigger is too expensive, because each request
forced a pending `didChange` to be analyzed: `textDocument/signatureHelp` was
in the dispatcher's flush group (`src/lsp/lsp.c`, the hover/definition/...
list), which runs `lsp_flush_dirty` before answering.

But signature help does not need the analysis. `didChange` updates `doc->text`
immediately (analysis is what gets deferred), and the call position comes
from the text alone. From the symbol index it needs only the callee's type,
which an index one edit old almost always still has. The flush was a cost the
handler inherited from its neighbors, not one it needed.

## Resolution

- `textDocument/signatureHelp` left the flush group. `on_signature_help` finds
  the call position from the current text, looks the callee up in the index
  as it stands, and runs the pending analysis only when the index does not
  know the callee: a function defined since the last analysis, or a document
  never analyzed. The common space-trigger request is not in argument position
  at all, and returns `null` before it touches the index.
- The capability now advertises `"triggerCharacters":[" "]` (retrigger
  unchanged), with the reasoning in a comment above it so `(` is not re-added.
- `tests/lsp/mcp_lsp_test.py` pins the trigger set, and checks a callee that
  exists only in an unanalyzed edit (the lazy-flush path).
- The LSP guide documents the trigger and its cost.

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo -- the editor this was found from.)*

Trowel offers signature help only on an explicit command (Run > Show
Signature Help), with the original measurement recorded in the `charAdded`
hook in `src/editor/editor_view.cpp`. It can now auto-trigger on space as the
server advertises; the note in `charAdded` should be updated either way, since
the cost it cites is gone.

# `tur lsp` advertises `(` as a signature-help trigger but answers null there

**Severity:** low-medium.
**Found:** 2026-10-03, against v0.60.1.
**Impact:** a client that honours the advertised trigger gets nothing, and pays
a didChange plus a compile per `(` to get it.

## What happens

`initialize` declares:

```json
"signatureHelpProvider": { "triggerCharacters": ["("], "retriggerCharacters": [" "] }
```

So a client fires `textDocument/signatureHelp` when the user types `(`. The
server returns `null` there. Measured on `tests/smoke/fixtures/symbols.tur`
line 16, `(defn nav-use [] : int (nav-double nav-total))`, driving `tur lsp`
over stdio:

| cursor | result |
|---|---|
| col 24 — just after the `(` | `null` |
| col 34 — just after the callee's name | `null` |
| col 35 — after the name **and a space** | `{"signatures":[{"label":"(nav-double : (fn [int] : int))", "parameters":[{"label":"int"}], "activeParameter":0}], ...}` |
| col 44 — at the end of the argument list | same signature |

The feature works. It just does not work at either of the two positions a
client reaches by honouring `triggerCharacters`.

## Why it matters

The trigger character is a contract about *when asking is worthwhile*. In a
lisp the `(` is the one keystroke a client can cheaply hook, and it is exactly
the one that returns nothing. The position that does answer is "after a space",
which is most keystrokes in s-expression source — and each request forces the
pending didChange to flush and the document to be re-analysed on the server's
single thread. That is the same cost that made space unaffordable as a
*completion* trigger, and the server's own capability block says so:

> Space was advertised as a completion trigger, which in a lisp fires on nearly
> every keystroke and forced a full analysis behind each one.

So the advertised trigger cannot be honoured usefully, and the position that
works cannot be hooked affordably.

## Suggested fix

Answer at the `(` — the callee is already known at that point, which is what
makes the label computable, and `activeParameter` would simply be 0. Failing
that, stop advertising `(` and advertise the retrigger set that actually
produces answers, so a client is not misled into a request per open paren.

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo — the editor this was found from. Included so the report
names a real consumer and so the workaround can be deleted alongside a
fix.)*

Trowel does **not** auto-trigger signature help. It is explicit only, on
Run > Show Signature Help (Ctrl+Shift+P), with the measurement recorded in the
`charAdded` hook in `src/editor/editor_view.cpp` so nobody re-adds the trigger
on the strength of the capability block. The smoke test
`test_signature_help_shows_the_callees_parameter_list` deliberately places the
caret in argument position for the same reason.

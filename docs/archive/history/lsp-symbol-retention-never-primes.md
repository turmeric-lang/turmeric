# LSP: last-good symbol retention never primes for a file that has not yet parsed

**Severity:** medium (completion silently dead in a real, common scenario --
opening a file that already has a syntax error).

Reported from the consumer side by the agent working on
[Trowel](https://github.com/turmeric-lang/trowel), the native Turmeric editor,
against `claude/busy-clarke-6zj9jl` (HEAD `8f341d32e`). Verified here.

## Status (2026-07-27): FIXED

Both halves. `LspDoc.ever_analyzed` replaces the `!doc->symbols` test so the
retention path primes correctly, and a process-wide stdlib symbol cache backs
the case where a document has never parsed at all.

Measured on the three cases from this report, before and after:

| Case | Before | After |
|---|---|---|
| opened good, edited broken before any analysis | 0 items | 200 |
| opened already broken (never parsed) | 0 items | 200 |
| opened good, request forced an analysis, then broken | 200 | 200 |

Regression coverage: `test_lsp_unprimed_completion` in
`tests/lsp/mcp_lsp_test.py`, which was confirmed to fail (3 assertions)
against the pre-fix binary. See "How the fix works" at the end.

## Summary

The §2.1 fix in [docs/archive/lsp-client-gaps-plan.md](../lsp-client-gaps-plan.md)
retains the previous symbol index when a compile yields nothing, so completion
survives the unbalanced-paren state that is normal while typing. It works --
**but only once a successful analysis has run at least once for that document.**

If the buffer has never parsed, there is no last-good set to fall back on and
completion returns an empty list, exactly as it did before the fix. The
realistic version is a user opening a file that already contains a syntax
error: completion stays dead until they fix the error unaided, which is when
they most want it.

## Repro

Drive `tur lsp` over stdio. The two runs differ in exactly one thing --
whether any request forced an analysis before the buffer was broken.

```
GOOD   = '(defn zork [a :int] :int a)\n'
BROKEN = GOOD + '\n(smoke\n'

A)  didOpen(GOOD) -> documentSymbol -> didChange(BROKEN) -> completion  => 200 items
B)  didOpen(GOOD) ->                   didChange(BROKEN) -> completion  =>   0 items
```

And the case that motivated the report -- a file that is broken on disk:

```
C)  didOpen('(defn zork [a :int] :int a)\n(smoke\n') -> completion       =>   0 items
```

(B) matters because analysis is *debounced*: `didOpen` only marks the document
dirty, so a client that opens a file and edits it before the first flush lands
in the unprimed state without doing anything unusual.

## Root cause

`src/lsp/lsp.c`, `run_doc_analysis`, the adoption condition:

```c
if (rc == 0 || fresh_count > 0 || !doc->symbols) {
    /* adopt `fresh` */
} else {
    free(fresh);
    doc->symbols_stale = 1;
}
```

The `!doc->symbols` disjunct is what breaks priming. On the first analysis
`doc->symbols` is NULL, so the empty result is adopted unconditionally --
including when the compile failed. From then on `doc->symbols` is non-NULL but
*empty*, and every later failure takes the retain branch and faithfully retains
the empty set.

That disjunct is not gratuitous: it exists so a file legitimately emptied of
definitions reports empty rather than staying stale forever. The bug is that it
cannot distinguish "successfully analyzed, genuinely has no symbols" from
"never successfully analyzed".

## Why the existing test did not catch it

`test_lsp_client_gaps` in `tests/lsp/mcp_lsp_test.py` issues a
`textDocument/completion` between the `didOpen` and the broken `didChange`.
That request forces `lsp_flush_dirty`, which primes the index -- so the test
exercises path (A) and passes for a reason that hides (B) and (C). Any fix
should add an unprimed case rather than trusting the current one.

## Not to be confused with

The retention fix itself, which works. The consumer explicitly confirmed
`(smoke-` prefix completion in a broken buffer returns correct filtered results
once primed, and called it the most valuable item on the list. This report is
only about the unprimed edge.

## How the fix works

**(B), the priming half.** `LspDoc.ever_analyzed` records that *some* revision
of the text produced a real index, which is the thing the old `!doc->symbols`
test was standing in for and getting wrong. `run_doc_analysis` now branches
three ways instead of two:

- the result is *usable* (`rc == 0`, or a late failure that still collected
  symbols -- a type error after a clean parse): adopt it, set `ever_analyzed`;
- not usable, and nothing has ever been analyzed: adopt the empty result but
  leave `ever_analyzed` clear, so the fallback below applies;
- not usable, but an earlier revision worked: retain it and set
  `symbols_stale`.

`lsp_doc_free_symbols` deliberately does **not** clear `ever_analyzed` -- it is
called to swap one index for another, and "an analysis once succeeded" is a
property of the document, not of the array being replaced.

**(C), the never-parsed half.** A process-wide cache of the stdlib surface,
served by `doc_symbol_view()` to any document with `ever_analyzed == 0`.

Two things about it are worth keeping:

*It is not harvested from another document's analysis.* That was the first
attempt and it does not work: the case that most needs the fallback is a
server whose only open document is the broken one, where no successful
analysis ever happens to harvest from. All three repro cases still returned 0
items. Instead `stdlib_cache_prime()` analyzes an **empty buffer** -- the
stdlib is auto-loaded, so an empty file alone yields the whole surface. Lazy,
once per process, and only on the first request that actually needs it: a
session where every document parses never pays it. Measured cost is ~8ms on
that one request.

*Membership is a path-prefix test against `TUR_STDLIB_DIR`*, not "any symbol
not from this document". The looser test would sweep in the first document's
*imports* and then offer them to an unrelated file that never imported them.
`resolve_stdlib_root()` in `main.c` sets that variable before any subcommand
runs, so it is reliably available; when it is not, the cache stays empty
rather than guessing.

The fallback is principled rather than a shot in the dark: Turmeric auto-loads
the stdlib, so every symbol it offers genuinely *is* in scope for the broken
file. What cannot be known while the file does not parse is what the *document*
declares -- and that is exactly what the fallback omits.

Verified alongside the headline numbers: the fallback carries no
document-local symbols, prefix filtering still applies to it, hover over a
stdlib name works in a never-parsed buffer, the document's own definitions
return and rank first the moment it parses, retention still applies to a later
break, and the synthetic compile does not leak diagnostics into the ones
published for the user's document.

## Follow-up left open

`find_symbol` now consults the fallback too, so hover, go-to-definition, and
signature help over a stdlib name keep working in a file the user is still
getting to compile. Go-to-definition into stdlib source was already possible
before this change and is unaffected.

Not addressed: a document whose *own* prior index was retained still cannot
offer stdlib symbols that its retained index happened to lack. In practice the
retained index already contains the stdlib surface, so this is theoretical.

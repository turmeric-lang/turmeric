# The two `--lang` flags take different vocabularies

**Severity:** low.
**Found:** 2026-10-03, against v0.60.1.
**Impact:** minor, but it is a trap for every editor integration: the correct
argument depends on which subcommand you are calling.

## What happens

`tur repl --lang` takes a **base** that `tur dialects` lists, and rejects reader
spellings:

```
tur repl --lang saffron          ok
tur repl --lang saffron/sweet    ok
tur repl --lang turmeric/sweet   ok
tur repl --lang r7rs/sweet       ok
tur repl --lang sweet-exp        unknown --lang 'sweet-exp' (expected a base
                                 `tur dialects` lists, e.g. "turmeric",
                                 "saffron" or "r7rs")
tur repl --lang scheme           unknown --lang 'scheme'
```

`tur fmt --lang` takes a **reader**, and rejects the language half:

```
tur fmt --stdin --lang turmeric     ok
tur fmt --stdin --lang sweet        ok
tur fmt --stdin --lang sweet-exp    ok
tur fmt --stdin --lang tursweet     ok
tur fmt --stdin --lang curly-infix  ok
tur fmt --stdin --lang neoteric     ok
tur fmt --stdin --lang r7rs         ok
tur fmt --stdin --lang r7rs/sweet   ok
tur fmt --stdin --lang saffron      tur fmt: unknown dialect 'saffron'
tur fmt --stdin --lang saffron/sweet  tur fmt: unknown dialect 'saffron/sweet'
tur fmt --stdin --lang scheme       tur fmt: unknown dialect 'scheme'
```

So no spelling is accepted by both for the Saffron bases, and `sweet-exp` /
`scheme` are accepted by exactly the one that rejects the other's vocabulary.

## Is it a bug?

Arguably not: formatting is a reader concern, and Saffron's reader is
Turmeric's, so `--lang turmeric` is the honest answer for a Saffron buffer. But
the flags are spelled identically on the same binary, `tur dialects` is
advertised as *the* registry of what a `#lang` can name, and the `tur repl`
error message points at it. A caller that reads `tur dialects --json` and feeds
a base to `tur fmt` gets "unknown dialect" for four of the ten rows.

## Suggested fix

Cheapest: have `tur fmt --lang` also accept a base, resolving it to its reader —
`saffron` → Turmeric reader, `saffron/sweet` → sweet. Then `tur dialects` output
is usable with every `--lang` on the binary. Failing that, the help text for
each flag could say which vocabulary it wants; `tur fmt --help` currently says
only `--lang <dialect>`, which is the word `tur dialects` uses for the base.

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo — the editor this was found from. Included so the report
names a real consumer and so the workaround can be deleted alongside a
fix.)*

Trowel keeps two accessors over one table, `DialectBaseToken` (for `#lang`
lines and `tur repl --lang`) and `DialectFmtLangFlag` (for `tur fmt --lang`),
with a comment explaining why they differ.

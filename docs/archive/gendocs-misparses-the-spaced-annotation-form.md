---
title: "gendocs parses `[v : int]` as two parameters named `v` and `int`, affecting 53% of stdlib definitions"
category: Reported
description: "tools/gendocs.py splits a parameter vector on whitespace, so the FUSED form `[a :int]` parses correctly and the SPACED form `[a : int]` -- which 612 stdlib defns and CLAUDE.md's own style guide use -- yields a phantom parameter and a return type of ':'. Latent: docs/api/ is not checked in, so it would surface on the next `tur run docs`."
---

> **RESOLVED 2026-09-09** via fix direction 2, and the split site was one
> place after all: `_parse_params` in `tools/gendocs.py`. A standalone `:`
> token now attaches the NEXT token as the previous parameter's type, spelled
> fused (`':int'`) so both spellings take the same downstream path; the return
> position shares one helper (`_return_type_after_bracket`) that also skips an
> `#fx{...}` effect row. Two adjacent holes closed with it: the whitespace
> split broke compound types in EITHER spelling (`(Option int)` became a
> phantom parameter `int)`), so the tokenizer keeps balanced `()`/`[]`/`<>`
> groups together, and the `[^\]]*` bracket regex ended the vector at the
> first `]` inside `(fn [int] int)`, so the vector is found by a nesting-aware
> scan. Re-measured over `stdlib/*.tur`: 2002 definitions, **0** parameters
> typed `':'`, **0** return types `':'` (was 984 / 1064). Test:
> `tests/tools/check_gendocs_parse.py` (ctest `tur_gendocs_parse_lint`), fused,
> spaced and untyped forms plus the compound, variadic and `#fx` cases; it
> stubs `markdown` so the parser is testable without the renderer's dependency.

# gendocs misparses the spaced annotation form

**Severity: medium.** Latent rather than shipped -- `docs/api/` is empty in the
repo, so nothing wrong is currently published. It would corrupt the generated
API reference the moment `tur run docs` is run, and it affects **the majority
of the stdlib**.

Found while checking whether `tools/gendocs.py` needed work for Saffron
(saffron-lang-plan S8). It does not; this is what turned up instead, and it has
nothing to do with the dialect.

## Repro

Three defns, one parser:

```turmeric
(defn g [a : int b : int] : int (+ a b))    ;; FUSED   -- correct
(defn g [a : int b : int] : int (+ a b))    ;; SPACED  -- wrong
(defn g [a b] (+ a b))                      ;; untyped -- correct
```

```
fused     params=[('a', ':int'), ('b', ':int')]                  return=':int'
spaced    params=[('a', ':'), ('int', None), ('b', ':'), ('int', None)]  return=':'
untyped   params=[('a', None), ('b', None)]                      return=None
```

In the spaced form the parameter `a` is given the type `':'`, a **phantom
parameter named `int`** appears with no type, and the return type is `':'`
rather than `int`.

Real stdlib code, not a synthetic case -- `stdlib/arc.tur`:

```turmeric
(defn arc-new [v : int] : Arc ...)
```
```
arc-new   params=[('v', ':'), ('int', None)]   return=':'
```

## Scope

Measured across `stdlib/*.tur` by running `parse_tur_file` on each:

```
stdlib definitions parsed : 1999
  params with type ':'     : 984
  return_type == ':'       : 1064   (53%)
  files affected           : 114
```

So more than half of the API reference would render with a wrong signature.

The spaced form is not an obscure spelling: 612 `defn`s in stdlib use it, and
it is what **CLAUDE.md's own indentation style guide** writes throughout
(`(defn greet [name : cstr] : void`).

## Root cause (partial)

The parameter vector is split on whitespace and each token treated as a name,
with a following `:`-prefixed token taken as its type. That handles `a :int`
(one token carrying the colon) and bare `a`, but in `a : int` the colon is its
own token, so it is consumed as `a`'s type and `int` becomes the next
parameter.

I have not pinned the exact split site -- the fix wants the tokenizer to treat
a standalone `:` as an infix marker joining the previous name to the next
token, which is one place but I have not confirmed there is only one.

## Fix directions

1. **Normalise before splitting**: rewrite ` : ` to ` :` in the parameter
   vector, so the spaced form becomes the fused one the parser already handles.
   A two-line change, and it makes both spellings take the same tested path.
   Blunt, but the parser is a doc tool rather than a compiler front end.
2. **Handle a standalone `:` token in the split loop**: when a token is exactly
   `:`, attach the NEXT token as the previous parameter's type rather than
   starting a new parameter. More faithful, and it also fixes the return
   position (`] : int`), which direction 1 has to handle separately.

Direction 2 is the honest one; direction 1 would want the return type handled
anyway, so the saving is smaller than it looks.

Either way this wants a test over both spellings AND the untyped form -- the
last so that a "fix" cannot regress Saffron modules, which parse correctly
today.

## Not this bug

Saffron modules are **fine**. A `#lang saffron` file's untyped parameters come
back as `('a', None)`, which is correct, and the `#lang` line does not disturb
the parser. That was the S8 question and the answer is that gendocs needs no
Saffron work.

Note also that `tools/gendocs.py` cannot run at all without the `markdown`
package (it imports `genguides`), so this was measured by loading
`parse_tur_file` directly with a stub. The rendered HTML was not inspected --
`docs/api/` is empty -- so the exact on-page appearance of the corruption is
inferred from the parse, not observed.

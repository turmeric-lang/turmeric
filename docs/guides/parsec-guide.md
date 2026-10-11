---
title: Parsec Reference Guide
category: Data Structures and Libraries
description: Reference for the stdlib parser-combinator library tur/parsec -- core types, primitive parsers, sequencing, choice, repetition, optional values, mapping, runners, and common patterns
---

# Parsec Reference Guide

`tur/parsec` is the standard library's parser-combinator library. This page
is the reference: one section per concept, with each function's type. For a
guided introduction, start with the [parsec tutorial](parsec-tutorial.md).

Every snippet in "Common patterns" is in
`tests/fixtures/parsec-guide-patterns/`, and the tutorial's programs are in
`tests/fixtures/parsec-guide-step*/`, so they are checked on every CI run.

---

## Loading the library

`tur/parsec` ships with the compiler -- there is nothing to add to
`build.tur` -- but it is not auto-loaded:

```turmeric
(load "stdlib/parsec.tur")
```

Companion modules:

| Module | Provides | Loaded |
|---|---|---|
| `stdlib/parsec.tur` | `Parser`, the parsers, combinators and runners below | by you |
| `stdlib/char.tur` | `Char`, `char->int`, `int->char`, `char->digit`, `digit?`, `alpha?`, `alnum?`, `space?`, `upper?`, `lower?`, `punct?`, `char=?` | always (auto-loaded) |
| `stdlib/option.tur` | `Option A`, `some`, `none`, `some?`, `none?`, `unwrap`, `unwrap-or` | always (auto-loaded) |
| `stdlib/list-typed.tur` | `List A`, `list-nil`, `list-cons`, `list-first`, `list-rest`, `list-empty?`, `list-count`, `list-foldl`, `list-map`, `list-reverse`, `list->string` | by `parsec.tur` |
| `stdlib/string.tur` | `String`, `string/to-cstr`, `char->string`, `show-line` (via `Show`) | by `parsec.tur` |

---

## Core types

**`(Parser A)`** -- a parser that, on success, produces an `A`. It is an
ordinary value: store it, pass it, return it from a `defn`. Under the hood
it is a closure from an input position to the list of `(value, position)`
pairs it can reach -- the list monad -- which is what makes choice fully
backtracking.

**`Char`** -- one character. Character literals are written `#\A`,
`#\space`, `#\newline`, `#\tab`, `#\u41`, and have type `Char`, which is
**not** `int`: cross explicitly with `(char->int c)` / `(int->char n)`, and
compare with `char=?` or `eq?`. See the character literal section of
[reader-forms-guide.md](reader-forms-guide.md).

**`(Option A)`** -- `(some v)` or `(none)`. Produced by `optional` and
`run-parser-full`.

**`(List A)`** -- a cons list, produced by `many`, `many1`, `sep-by` and
`sep-by1`. `(list->string cs)` turns a `(List Char)` into a `String`.

**`String`** -- an owned string, produced by `pstring` and `list->string`.
`(string/to-cstr s)` borrows it as a `cstr`.

**`(ParseResult A)`** -- one parse found by `run-parser`: its value
(`parse-result-value`) and the unread input (`parse-result-rest`).

---

## Primitive parsers

| Function | Type | Reads |
|---|---|---|
| `(item)` | `(Parser Char)` | any one character; fails at the end of input |
| `(pchar c)` | `Char -> (Parser Char)` | exactly the character `c` |
| `(satisfy pred)` | `(fn [Char] bool) -> (Parser Char)` | one character for which `pred` holds |
| `(pstring s)` | `cstr -> (Parser String)` | exactly the text `s`; produces a fresh `String` |
| `(pfail)` | `(Parser A)` | nothing -- always fails |
| `(pure v)` | `A -> (Parser A)` | nothing -- always succeeds with `v` |

`(pchar 65)` is a type error: a character is a `Char`. Write `(pchar #\A)`,
or `(pchar (int->char 65))` when you hold a code. The old integer form is
available, deprecated, as `(pchar-int 65)` for one release.

`pure` is the `Applicative` method, so it finds its instance from the type
the context expects: give the `defn` that builds the parser its result type
(`: (Parser int)`), or ascribe it, `(:: (pure 42) (Parser int))`.

---

## Sequencing and binding

`do-m` runs parsers one after another, binding each value:

```turmeric
(do-m
  a parser-a
  b parser-b
  (pure (combine a b)))
```

Each name gets the **value** its parser produced, typed from that parser --
in `(do-m c (item) ...)`, `c` is a `Char`. Bind a value you do not need to
`_`. If any step fails, the whole `do-m` fails. A step can be any parser
expression -- a call, an `alt-or`, a `many` -- but write a nested `do-m` as
its own named `defn` with a result type; inline, it loses its type (see
[What a parser can produce](#what-a-parser-can-produce)).

`(bind p k)` is what `do-m` expands to, if you want it directly:

```turmeric
(defn digit-flag [] : (Parser bool)
  (bind (item) (fn [c] (pure (digit? c)))))
```

`(then-parser p q)` runs `p` then `q` and keeps only `q`'s value:
`(then-parser (pchar #\() (item))` reads `(x` and produces `#\x`.

`bind-parser` and the other `*-raw` / `*-impl` functions are the carrier
layer the typed surface is built on; user code should not need them.

---

## Choice

`(alt-or p q)` -- the `Alternative` method -- tries both `p` and `q` from the
same position and keeps the results of **both**. Parsing is fully
backtracking: if `p` succeeds but leads nowhere later, `q`'s parse is still
there to be used.

```turmeric
(alt-or (pchar #\Y) (pchar #\N))
```

The runners pick the first full parse in order, so when two branches can
both succeed, put the preferred one first. `or-parser` is the same
combinator under its worker name; prefer `alt-or`.

For a choice among several parsers, nest `alt-or`:
`(alt-or p (alt-or q r))`. `(pfail)` is the choice with no options.

---

## Repetition

| Function | Type | Fails when |
|---|---|---|
| `(many p)` | `(Parser A) -> (Parser (List A))` | never -- no match is the empty list |
| `(many1 p)` | `(Parser A) -> (Parser (List A))` | `p` does not match at least once |
| `(sep-by p sep)` | `(Parser A) (Parser S) -> (Parser (List A))` | never -- no element is the empty list |
| `(sep-by1 p sep)` | `(Parser A) (Parser S) -> (Parser (List A))` | there is not at least one element |

`many`, `many1` and `sep-by` are **greedy**: they take every repetition they
can and offer no shorter alternatives. An empty result is an empty list --
test it with `(list-empty? xs)` or `(list-count xs)`, never with `0`.

---

## Optional values

```turmeric
(optional p)   ; (Parser A) -> (Parser (Option A))
```

`optional` always succeeds: `(some v)` when `p` matched, `(none)` --
consuming nothing -- when it did not. Read the result with `some?`,
`none?`, `unwrap`, `unwrap-or`, or `match`:

```turmeric
(do-m
  sign (optional (pchar #\-))
  n    (nat-p)
  (pure (if (some? sign) (- 0 n) n)))
```

---

## Mapping and transforming

`(fmap p f)` -- the `Functor` method -- applies `f` to the value `p`
produces, without changing what `p` reads:

```turmeric
(fmap (satisfy digit?) char->digit)          ; (Parser int)
(fmap (many1 (satisfy alpha?)) list->string)  ; (Parser String)
```

Transform inside the parser rather than after running it: `nat-p` as a
`(Parser int)` composes into larger parsers; a `(Parser (List Char))` you
convert afterwards does not.

An un-annotated lambda takes its parameter type from the parser:
`(fmap (many1 (satisfy digit?)) (fn [ds] ...))` gives `ds : (List Char)`.

---

## Running parsers

| Function | Returns | Use when |
|---|---|---|
| `(parse-first p s)` | `A` | the input must parse -- `main`, tests, examples |
| `(run-parser-full p s)` | `(Option A)` | the input may not parse |
| `(run-parser p s)` | `(List (ParseResult A))` | debugging: every parse of a prefix of `s` |

`run-parser-full` returns `(some v)` for the first parse, in backtracking
order, that reads **all** of `s`, and `(none)` when every parse leaves input
over or none succeeds.

`parse-first` returns that value directly. When there is none it panics,
naming where the longest partial parse stopped:

```turmeric
(parse-first (pchar #\A) "AB")
; panic: parse failed at: "B" (after "A")
```

Use it only where unparseable input is a programming error; anywhere else,
handle the `(none)` from `run-parser-full`.

`run-parser` returns every partial parse; read each with
`(parse-result-value r)` and `(parse-result-rest r)` (the unread suffix, as
a `String`).

---

## What a parser can produce

A parser's value travels through the library as one machine word. These
result types are exact: `int`, `bool`, `Char`, `String`, `(List A)`, an
`(Option A)` built by `optional`, and any `:heap` struct:

```turmeric
(defstruct Setting :heap [key : String value : String])

(defn setting-p [] : (Parser Setting)
  (do-m k (key-p) _ (pchar #\=) v (value-p) (pure (Setting k v))))
```

A **by-value** aggregate handed to `pure` -- `(pure (pair a b))`, a plain
`defstruct`, an `Option` you built yourself -- does not survive the trip
today: it is passed as the address of a temporary that is gone by the time
the parser runs. Build a `:heap` record instead. The defect is tracked in
[docs/reported/byvalue-aggregate-erased-to-dangling-stack-address.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/byvalue-aggregate-erased-to-dangling-stack-address.md).

Two more limits of today's compiler, both with easy workarounds:

- Inside a **generic** `defn` (one with type parameters, like a
  combinator of your own), `do-m` binds its names as the untyped `int`
  carrier rather than as `A`. Ascribe where you need the type --
  `(:: x A)` -- or, better, use the library's combinators (`token`,
  `between`, `sep-by`), which handle it for you.
- A `do-m` written inline -- as an argument, `(many (do-m ...))`, or as a
  step of another `do-m` -- has no declared type to build its `pure` from or
  to give its value. Give it a name: a `defn` with a result type.

---

## Common patterns

**A natural number:**

```turmeric
(defn nat-p [] : (Parser int)
  (fmap (many1 (satisfy digit?))
        (fn [ds] (list-foldl ds 0 (fn [acc c] {{acc * 10} + (char->digit c)})))))
```

**An integer with an optional minus:**

```turmeric
(defn int-p [] : (Parser int)
  (do-m
    sign (optional (pchar #\-))
    n    (nat-p)
    (pure (if (some? sign) (- 0 n) n))))
```

**Whitespace and tokens.** `(spaces)` reads any run of whitespace (as a
`(List Char)`); `(token p)` runs `p` and then skips the whitespace after it,
so a grammar built from tokens never mentions spaces:

```turmeric
(token (pchar #\x))   ; reads "x   ", produces #\x
```

**Bracketed:**

```turmeric
(between (pchar #\() (pchar #\)) (nat-p))   ; reads "(7)", produces 7
```

**Separated lists**, and everything together -- a bracketed, comma-separated
list of integers with whitespace allowed after every token:

```turmeric
(defn int-list-p [] : (Parser (List int))
  (between (token (pchar #\[))
           (pchar #\])
           (sep-by (token (int-p)) (token (pchar #\,)))))

(parse-first (int-list-p) "[1, -2, 30]")   ; a (List int) of 1, -2, 30
```

**A keyword from a fixed set:**

```turmeric
(defn bool-p [] : (Parser bool)
  (alt-or (fmap (pstring "true") (fn [_s] true))
          (fmap (pstring "false") (fn [_s] false))))
```

---

## Migrating from the integer API

Before the typed surface, `tur/parsec` passed characters and results around
as raw integers. The old spellings and their replacements:

| Old | New |
|---|---|
| `(pchar 65)` | `(pchar #\A)` (or `pchar-int`, deprecated) |
| `(item)` producing a character code | `(item)` producing a `Char`; `(char->int c)` for the code |
| `(optional p)` producing `0` for absent | `(Parser (Option A))`: `some?` / `unwrap` |
| `(many p)` producing a raw cell pointer | `(Parser (List A))`: `list-count`, `list-first`, `list-foldl` |
| `(pstring "hi")` producing a raw pointer | `(Parser String)` |
| `(parse-value (run-parser p s))` | `(parse-first p s)`, or `parse-result-value` on a `run-parser` element |
| `(run-parser-full p s)` returning a cell or `0` | `(Option A)` |
| `(parse-result-count (run-parser p s))` | `(list-count (run-parser p s))` |
| `print-char-list`, `char-list-length` | `(show-line (list->string cs))`, `(list-count cs)` |
| `bind-parser` / `or-parser` in user code | `do-m` / `bind`, `alt-or` |

---

## See also

- [parsec-tutorial.md](parsec-tutorial.md) -- the step-by-step introduction.
- [parser-combinators-tutorial.md](parser-combinators-tutorial.md) -- build
  a combinator library from scratch.
- [hkt-guide.md](hkt-guide.md) -- the `Functor` / `Applicative` / `Monad` /
  `Alternative` typeclasses behind `fmap`, `pure`, `do-m` and `alt-or`.
- [backtracking-guide.md](backtracking-guide.md) -- the list monad that
  makes choice fully backtracking.
- [`stdlib/parsec.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/parsec.tur)
  -- the source, with a docstring on every function.

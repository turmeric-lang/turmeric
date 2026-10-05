---
title: Regular Expressions Guide
category: Data Structures and Libraries
description: Pure-Turmeric POSIX ERE regex engine -- compile, match, find, and replace
---

# Regular Expressions Guide

Turmeric's `tur/re` module (`stdlib/re.tur`) is a regular expression engine
written entirely in Turmeric. Because it is pure Turmeric with no inline C,
it runs on every backend: the AOT-compiled path, the `tur --interpret`
tree-walker, and the browser/WASM REPL.

## Overview

A compiled pattern is a `Regex` value -- the parsed AST, not a C `regex_t`.
`re/compile` returns it directly, so it flows unchanged between compiled and
interpreted code. There is no `malloc`'d C buffer to manage: `re/free` and
the `*-free` shims are no-ops kept for source compatibility.

The engine reports leftmost-longest per start position (it collects every
reachable end and takes the maximum), which coincides with POSIX
leftmost-longest for the patterns in the test suite. Group captures are not
yet threaded -- `re/match` returns the whole match only.

## Supported Syntax

The engine implements a POSIX ERE subset:

| Feature | Syntax | Example |
|---|---|---|
| Literals | any non-metacharacter | `abc` |
| Any character | `.` | `a.c` |
| Character class | `[...]` | `[a-z]` |
| Negated class | `[^...]` | `[^0-9]` |
| POSIX class | `[:alpha:]` etc. | `[[:digit:]]` |
| Star | `*` | `a*` |
| Plus | `+` | `a+` |
| Optional | `?` | `a?` |
| Bounded repeat | `{n}`, `{n,}`, `{n,m}` | `a{2,4}` |
| Alternation | `\|` | `cat\|dog` |
| Grouping | `(...)` | `(ab)+` |
| Begin anchor | `^` | `^abc` |
| End anchor | `$` | `abc$` |
| Escape | `\` + metacharacter | `\.` |

Supported POSIX class names: `[:alpha:]`, `[:digit:]`, `[:alnum:]`,
`[:upper:]`, `[:lower:]`, `[:space:]`, `[:blank:]`, `[:xdigit:]`.

**Not supported:** backreferences, Perl shorthands (`\d`, `\w`, `\b`),
lookaround, non-greedy quantifiers.

## Quick Start

```turmeric
;; Compile once, match many times
(def rx (re/compile "[0-9]+"))
(println (re/match? rx "abc123"))     ; => true
(println (re/match rx "abc123"))      ; => (some "123")
(println (re/find-all rx "a1b22c"))   ; => ("1" "22")
(println (re/replace rx "a1b2" "X"))  ; => "aXbX"
```

```sweet-exp
#lang sweet-exp

;; Compile once, match many times
def rx re/compile("[0-9]+")
println(re/match?(rx "abc123"))      ; => true
println(re/match(rx "abc123"))       ; => (some "123")
println(re/find-all(rx "a1b22c"))    ; => ("1" "22")
println(re/replace(rx "a1b2" "X"))   ; => "aXbX"
```

## API Reference

### re/compile

Compile a pattern string into a `Regex` value.

```turmeric
(def rx (re/compile "([A-Za-z]+)[0-9]*"))
```

```sweet-exp
#lang sweet-exp

def rx re/compile("([A-Za-z]+)[0-9]*")
```

The `Regex` is a GC-managed heap value. `re/free` is a no-op shim kept for
source compatibility -- there is nothing to free.

### re/match?

Test whether a pattern matches anywhere in the input. Returns `bool`.

```turmeric
(re/match? (re/compile "[0-9]+") "abc123")  ; => true
(re/match? (re/compile "[0-9]+") "abc")     ; => false
```

```sweet-exp
#lang sweet-exp

re/match?(re/compile("[0-9]+") "abc123")  ; => true
re/match?(re/compile("[0-9]+") "abc")     ; => false
```

### re/match

Return the leftmost-longest whole match as an `Option cstr`. Returns
`(some "match")` if the pattern matches, `None` if it does not.

```turmeric
(re/match (re/compile "[0-9]+") "a12b")   ; => (some "12")
(re/match (re/compile "[0-9]+") "abc")    ; => None
```

```sweet-exp
#lang sweet-exp

re/match(re/compile("[0-9]+") "a12b")   ; => (some "12")
re/match(re/compile("[0-9]+") "abc")    ; => None
```

### re/find-all

Return all non-overlapping matches, left to right, as an `RxStrs` list.
Each element is a `cstr` substring of the input.

```turmeric
(re/find-all (re/compile "[0-9]+") "a1b22c333")
; => ("1" "22" "333")
```

```sweet-exp
#lang sweet-exp

re/find-all(re/compile("[0-9]+") "a1b22c333")
; => ("1" "22" "333")
```

### re/replace

Replace the first match with a literal replacement string. The replacement
is a literal `cstr` -- no backreferences. Returns a fresh `cstr`; if there
is no match, returns a copy of the input.

```turmeric
(re/replace (re/compile "world") "hello world" "WORLD")
; => "hello WORLD"
```

```sweet-exp
#lang sweet-exp

re/replace(re/compile("world") "hello world" "WORLD")
; => "hello WORLD"
```

### re/replace-all

Replace all non-overlapping matches with a literal replacement string.

```turmeric
(re/replace-all (re/compile "[0-9]") "a1b2c3" "X")
; => "aXbXcX"
```

```sweet-exp
#lang sweet-exp

re/replace-all(re/compile("[0-9]") "a1b2c3" "X")
; => "aXbXcX"
```

### re/compile-union

Compile an alternation of multiple patterns into a single `Regex`. Takes a
cons list of `cstr` patterns and joins them with `|`, wrapping each in a
parenthesised branch. Returns `RNever` (a regex that matches nothing) if
the list is empty or contains a null element.

```turmeric
(re/compile-union (cons "[A-Za-z]+" (cons "[0-9]+" 0)))
; matches letters or digits
```

```sweet-exp
#lang sweet-exp

re/compile-union(cons("[A-Za-z]+" cons("[0-9]+" 0)))
; matches letters or digits
```

## Pattern Examples

### Email-like extraction

```turmeric
(def rx (re/compile "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"))
(println (re/find-all rx "contact a@b.com or c@d.io"))
; => ("a@b.com" "c@d.io")
```

```sweet-exp
#lang sweet-exp

def rx re/compile("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}")
println(re/find-all(rx "contact a@b.com or c@d.io"))
; => ("a@b.com" "c@d.io")
```

### Anchored matching

```turmeric
(def rx (re/compile "^[0-9]+$"))
(println (re/match? rx "12345"))   ; => true
(println (re/match? rx "12a45"))   ; => false
```

```sweet-exp
#lang sweet-exp

def rx re/compile("^[0-9]+$")
println(re/match?(rx "12345"))    ; => true
println(re/match?(rx "12a45"))    ; => false
```

### Character classes with POSIX names

```turmeric
(def rx (re/compile "[[:alpha:]][[:alnum:]]*"))
(println (re/find-all rx "foo 123 bar45 _baz"))
; => ("foo" "bar45" "baz")
```

```sweet-exp
#lang sweet-exp

def rx re/compile("[[:alpha:]][[:alnum:]]*")
println(re/find-all(rx "foo 123 bar45 _baz"))
; => ("foo" "bar45" "baz")
```

### Negated classes

```turmeric
(def rx (re/compile "[^0-9]+"))
(println (re/find-all rx "abc123def456"))
; => ("abc" "def")
```

```sweet-exp
#lang sweet-exp

def rx re/compile("[^0-9]+")
println(re/find-all(rx "abc123def456"))
; => ("abc" "def")
```

## Implementation Notes

The engine is a backtracking matcher written in pure Turmeric. The matcher
(`re-run-k`) walks the parsed `Regex` AST against the input `cstr`,
tracking the set of reachable positions (`RxPos`) at each step. For
`RStar`, it iterates until the position set stops growing (fixpoint with
deduplication), which prevents infinite loops on patterns like `(a*)*`.

The parser (`re-parse-go`) is a single recursive function with six modes
(alternation, concatenation, repeat, atom) that descends through the
pattern string. Bounded repeats (`{n}`, `{n,}`, `{n,m}`) desugar to
concatenations of the atom and optional/star wrappers.

Results are ordinary GC/heap values. The `re/free`, `re/match-free`, and
`re/find-all-free` functions are no-op shims retained for source
compatibility with the old libc `regcomp`/`regexec` wrapper -- there is no
`regex_t` and no `malloc`'d C buffer to manage.

## What This Guide Does Not Cover

- **String types** -- `cstr` vs `str` vs `String` and when to use each is
  covered in [strings-guide.md](strings-guide.md).
- **Parser combinators** -- for building your own parsers on top of the
  backtracking monad, see [parser-combinators-tutorial.md](parser-combinators-tutorial.md).
- **JSON** -- for structured data parsing, see
  [json-guide.md](json-guide.md).

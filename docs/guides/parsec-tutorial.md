---
title: Parsec Tutorial
category: Tutorials
description: A step-by-step introduction to the stdlib parser-combinator library tur/parsec -- characters, sequencing with do-m, choice, repetition, optional pieces, and mapping results into real values
---

# Parsec Tutorial

This tutorial teaches `tur/parsec`, the parser-combinator library in the
standard library, one small runnable program at a time. By the end you will
parse `"width=800,height=600"` into a list of records.

Every step is a complete program, and each one is also a test fixture --
`tests/fixtures/parsec-guide-step2-pchar/` through
`tests/fixtures/parsec-guide-step8-key-value/` -- so the code on this page is
the code CI runs. The output each line prints is in the `;` comment beside it.

Once you know the shape of the library, the
[parsec reference guide](parsec-guide.md) lists every function and its type.
If you would rather see how a combinator library is *built*, the
[parser combinators tutorial](parser-combinators-tutorial.md) writes one from
scratch.

---

## Step 1: What is a parser?

A parser reads text and produces a structured value: `"42"` becomes the
integer `42`, `"width=800"` becomes a record with a key and a value. In
`tur/parsec` a parser is an ordinary value of type `(Parser A)`, where `A` is
what it produces:

| Parser | Type | Produces |
|---|---|---|
| `(pchar #\=)` | `(Parser Char)` | the character `=` |
| `(many1 (satisfy digit?))` | `(Parser (List Char))` | the digits it read |
| `(pstring "true")` | `(Parser String)` | the text `true` |
| your `nat-p` | `(Parser int)` | a number |

You build big parsers from small ones with ordinary function calls. That is
the difference from a regular expression: a regex is one opaque string,
while a parser for a key=value pair is literally the key parser, then `=`,
then the value parser -- and each piece can be named, tested and reused.

The library is in the standard library but not auto-loaded. Bring it in
with:

```turmeric
(load "stdlib/parsec.tur")
```

It also loads the `String` type (`stdlib/string.tur`) and the typed list
`(List A)` (`stdlib/list-typed.tur`). `Char` and `Option` are always
available.

---

## Step 2: Your first character parser

```turmeric
(load "stdlib/parsec.tur")

(defn main [] : int
  ;; pchar matches one specific character; parse-first runs a parser over
  ;; the whole input and hands back its value.
  (show-line (parse-first (pchar #\H) "H"))                ; #\H
  ;; That value is a Char, not an int.
  (println (char->int (parse-first (pchar #\H) "H")))      ; 72
  ;; "Hello" has input left over after the H, so there is no full parse...
  (println (some? (run-parser-full (pchar #\H) "Hello")))  ; false
  ;; ...but run-parser shows the partial one, and where it stopped.
  (let [rs (run-parser (pchar #\H) "Hello")]
    (println (list-count rs))                              ; 1
    (show-line (parse-result-value (list-first rs)))       ; #\H
    (show-line (parse-result-rest (list-first rs))))       ; ello
  0)
```

- `#\H` is a **character literal**. Its type is `Char`, not `int`, so
  `(pchar 72)` is a type error -- you cannot hand a parser a number where it
  wants a character. Named characters are spelled `#\space`, `#\newline`,
  `#\tab`; `(char->int c)` and `(int->char n)` cross to and from codes.
- `show-line` prints any value with a `Show` instance; a `Char` shows as the
  literal that spells it.
- There are three ways to run a parser:
  - `(parse-first p s)` -- the value, when `p` reads **all** of `s`. If it
    cannot, the program stops with `parse failed at: ...`. Use it where bad
    input is a bug: `main`, tests, examples.
  - `(run-parser-full p s)` -- the same question answered safely: `(some v)`
    or `(none)`.
  - `(run-parser p s)` -- every way `p` can read a *prefix* of `s`, each with
    the input it left over. This is the debugging view.

---

## Step 3: Sequencing with `do-m`

```turmeric
(load "stdlib/parsec.tur")

;; Match "Hi" and return both characters as a list.  c1 and c2 are Chars:
;; do-m binds each name to the value its parser produced.
(defn hi-p [] : (Parser (List Char))
  (do-m
    c1 (pchar #\H)
    c2 (pchar #\i)
    (pure (list-cons c1 (list-cons c2 (list-nil))))))

(defn main [] : int
  (let [cs (parse-first (hi-p) "Hi")]
    (println (list-count cs))                        ; 2
    (show-line (list-first cs))                      ; #\H
    (show-line (list->string cs)))                   ; Hi
  ;; The second step fails on "Ho", so the whole parser does.
  (println (some? (run-parser-full (hi-p) "Ho")))    ; false
  0)
```

`do-m` is monadic do-notation: each `name parser` pair runs the parser on the
input the previous one left and binds `name` to its value; the last form
builds the result, usually with `pure` (which succeeds without reading
anything). If any step fails, the whole `do-m` fails.

The names are typed from their parsers: `c1` is a `Char` because
`(pchar #\H)` is a `(Parser Char)`, with no annotation. There is no result
cell to unpack and no integer to decode.

Declaring the function's result type -- `: (Parser (List Char))` -- is what
lets `pure` know which kind of value to build, so give every parser-building
`defn` its type.

---

## Step 4: Alternatives with `alt-or`

```turmeric
(load "stdlib/parsec.tur")

(defn yes-or-no [] : (Parser Char)
  (alt-or (pchar #\Y) (pchar #\N)))

(defn main [] : int
  (show-line (parse-first (yes-or-no) "Y"))               ; #\Y
  (show-line (parse-first (yes-or-no) "N"))               ; #\N
  (println (some? (run-parser-full (yes-or-no) "Q")))     ; false
  ;; Full backtracking: both branches read the same input, and every
  ;; success is kept -- here both (pchar #\a) and (item) match "a".
  (println (list-count (run-parser (alt-or (pchar #\a) (item)) "a")))  ; 2
  0)
```

`alt-or` is the `Alternative` typeclass's choice, so the same name works for
`Option` and the other alternatives in the stdlib. For parsers it is **full
backtracking**: both branches start from the same position, and the results
of both are kept. A runner then takes the first one that reads the whole
input, so on ambiguous grammars put the branch you prefer first.

---

## Step 5: Repetition with `many` and `many1`

```turmeric
(load "stdlib/parsec.tur")

(defn digits [] : (Parser (List Char))
  (many (satisfy digit?)))

(defn some-digits [] : (Parser (List Char))
  (many1 (satisfy digit?)))

(defn main [] : int
  (show-line (list->string (parse-first (digits) "123")))     ; 123
  (println (list-count (parse-first (digits) "123")))         ; 3
  ;; many always succeeds: no digits is the empty list.
  (println (list-empty? (parse-first (digits) "")))           ; true
  ;; many1 wants at least one.
  (show-line (list->string (parse-first (some-digits) "42"))) ; 42
  (println (some? (run-parser-full (some-digits) "")))        ; false
  ;; many is greedy: on "123abc" it takes 1, 2 and 3 and stops at the a.
  (let [r (list-first (run-parser (digits) "123abc"))]
    (show-line (list->string (parse-result-value r)))         ; 123
    (show-line (parse-result-rest r)))                        ; abc
  0)
```

- `(satisfy pred)` reads one character for which `pred` is true. The
  predicates in `stdlib/char.tur` drop straight in: `digit?`, `alpha?`,
  `alnum?`, `space?`, `upper?`, `lower?`, `punct?`.
- `many` and `many1` return a real `(List A)`: ask it `list-count`,
  `list-empty?`, `list-first`, `list-rest`, fold it with `list-foldl`, or turn
  a `(List Char)` into a `String` with `list->string`.
- Both are **greedy**: they take every match they can and offer no shorter
  alternative.

---

## Step 6: `optional` and `Option`

```turmeric
(load "stdlib/parsec.tur")

;; An optional leading minus, then digits; produce the sign.
(defn sign-of-number [] : (Parser (Option Char))
  (do-m
    sign (optional (pchar #\-))
    _    (many1 (satisfy digit?))
    (pure sign)))

(defn describe [s : cstr] : cstr
  (if (some? (parse-first (sign-of-number) s)) "has minus" "no minus"))

(defn main [] : int
  (println (describe "-42"))                                      ; has minus
  (println (describe "42"))                                       ; no minus
  (show-line (unwrap (parse-first (sign-of-number) "-42")))        ; #\-
  (show-line (unwrap-or (parse-first (sign-of-number) "42") #\+))  ; #\+
  0)
```

`(optional p)` always succeeds. When `p` matches it produces `(some v)`;
when it does not, `(none)`, and nothing is consumed. Inspect the result with
the ordinary `Option` functions -- `some?`, `none?`, `unwrap`, `unwrap-or`, or
`match` on `Some` / `None` -- rather than comparing it with zero.

A step whose value you do not need is bound to `_`, as the digits are here.

---

## Step 7: Mapping with `fmap`

```turmeric
(load "stdlib/parsec.tur")

;; One digit, as its numeric value.
(defn digit-value [] : (Parser int)
  (fmap (satisfy digit?) char->digit))

;; A natural number: one or more digits, folded into an int.
(defn nat-p [] : (Parser int)
  (fmap (many1 (satisfy digit?))
        (fn [ds] (list-foldl ds 0 (fn [acc c] {{acc * 10} + (char->digit c)})))))

(defn main [] : int
  (println (parse-first (digit-value) "7"))        ; 7
  (println (parse-first (nat-p) "42"))             ; 42
  (println (+ 1 (parse-first (nat-p) "2026")))     ; 2027
  0)
```

`fmap` (the `Functor` method) changes what a parser produces without
changing what it reads. Here it turns a `(Parser (List Char))` into a
`(Parser int)`.

Note the lambdas need no annotations: `ds` is a `(List Char)` because that
is what `(many1 (satisfy digit?))` produces, and `c` is a `Char` because it
is an element of `ds`. `char->digit` gives a digit character's value (`7`
for `#\7`), not its character code.

---

## Step 8: Putting it together -- a key=value parser

```turmeric
(load "stdlib/parsec.tur")

;; The parsed result.  A `:heap` record rides the parser's one-word carrier;
;; see the guide for why a by-value Pair does not (yet).
(defstruct Setting :heap [key : String value : String])

(defn key-p [] : (Parser String)
  (fmap (many1 (satisfy alpha?)) list->string))

(defn value-p [] : (Parser String)
  (fmap (many1 (satisfy alnum?)) list->string))

(defn setting-p [] : (Parser Setting)
  (do-m
    k (key-p)
    _ (pchar #\=)
    v (value-p)
    (pure (Setting k v))))

;; Several settings, comma-separated.
(defn settings-p [] : (Parser (List Setting))
  (sep-by (setting-p) (pchar #\,)))

(defn main [] : int
  (let [r (run-parser-full (setting-p) "width=800")]
    (if (some? r)
      (do (show-line (.key (unwrap r)))                          ; width
          (show-line (.value (unwrap r))))                       ; 800
      (println "not a setting")))
  (println (some? (run-parser-full (setting-p) "width:800")))    ; false
  (let [all (parse-first (settings-p) "width=800,height=600")]
    (println (list-count all))                                   ; 2
    (show-line (.key (list-first (list-rest all)))))             ; height
  0)
```

Each piece is a named parser with a precise type, and the grammar reads
straight off the code: a setting is a key, an `=`, and a value; a settings
line is settings separated by commas. `(sep-by p sep)` is one of the
library's ready-made combinators, alongside `sep-by1`, `between`, `token`
and `spaces`.

The result is a `:heap` record. A parser's value travels through the
library as a single machine word, so build results that are one word wide --
a `:heap` struct, a `String`, a `(List A)`, a `Char`, an `int`. A by-value
`(Pair A B)` or plain `defstruct` returned through `pure` does not survive
that trip today; the [reference guide](parsec-guide.md#what-a-parser-can-produce)
has the details.

---

## Where next

- The [parsec reference guide](parsec-guide.md) -- every parser, combinator
  and runner with its type, plus the common patterns: integers, tokens and
  whitespace, bracketed and separated lists, keywords.
- [hkt-guide.md](hkt-guide.md) -- the `Functor` / `Applicative` / `Monad` /
  `Alternative` typeclasses that `fmap`, `pure`, `do-m` and `alt-or` come from.
- [parser-combinators-tutorial.md](parser-combinators-tutorial.md) -- build a
  combinator library yourself, with a GADT expression AST.

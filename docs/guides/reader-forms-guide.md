---
title: Reader Forms Reference
category: Language Basics
description: Complete reference for every syntactic form the Turmeric reader recognises -- comments, literals, collections, prefix macros, and reader extensions
---

# Reader Forms Reference

The Turmeric reader converts source text into a form tree before the compiler
sees it. This guide documents every construct the reader understands, organised
by category.

---

## Comments

Comments are consumed by the reader and produce no form.

### Line comment -- `;`

Ignores everything from `;` to the end of the line.

```turmeric
(println "hello")  ; this is ignored
```
```sweet-exp
println("hello")
; this is ignored
```

### Block comment -- `#| ... |#`

Ignores a delimited region of text. Block comments nest.

```turmeric
#|
  This whole region is ignored.
  (println "never runs")
  #| nested block comments work too |#
|#
(println "ok")
```
```sweet-exp
#|
This
whole
region
is
ignored.
println("never runs")
#|
nested
block
comments
work
too
|#
|#
println("ok")
```

### Datum comment -- `#;`

Reads one complete datum and discards it. The surrounding code sees nothing.
Unlike `#| |#`, `#;` understands structure -- it always discards exactly one
form, no matter how many lines it spans.

```turmeric no-check
(println #;99 "ok")              ; => ok
(println #;"ignored" "ok")       ; => ok
(println #;(this is discarded) "ok")  ; => ok
(println (+ 1 #;999 2))          ; => 3
```
```sweet-exp
println(#;99 "ok")              ; => ok
println(#;"ignored" "ok")       ; => ok
println(#;(this is discarded) "ok")  ; => ok
println(+(1 #;999 2))          ; => 3
```

Datum comments compose: `#;#;1 2 3` discards `2` (the result that inner `#;1`
would have produced) and leaves `3`.

```turmeric no-check
(println #;#;1 2 3)   ; => 3
```
```sweet-exp
println(#;#;1 2 3)   ; => 3
```

`#;` at end of file, or immediately before a closing delimiter, is an error.

---

## Literals

### Integer

Decimal, hexadecimal (`0x`/`0X`), and binary (`0b`/`0B`) are supported.
An optional type suffix pins the width.

```turmeric
42
-7
0xff
0b1010
255u8
1000i32
```
```sweet-exp
42
-7
0xff
0b1010
255u8
1000i32
```

| Suffix | Type |
|--------|------|
| `i8` | signed 8-bit |
| `i16` | signed 16-bit |
| `i32` | signed 32-bit |
| `i64` | signed 64-bit |
| `u8` | unsigned 8-bit |
| `u16` | unsigned 16-bit |
| `u32` | unsigned 32-bit |
| `u64` | unsigned 64-bit |

### Float

A `.` or exponent (`e`/`E`) makes a literal a float. The suffix `f32` or `f64`
pins the width; unsuffixed floats are `f64`.

```turmeric
3.14
1.0e10
2.5f32
1f64
```
```sweet-exp
3.14
1.0e10
2.5f32
1f64
```

### String -- `"..."`

Double-quoted UTF-8 text. Standard C escape sequences apply (`\n`, `\t`, `\\`,
`\"`, etc.).

```turmeric
"hello, world"
"line one\nline two"
```
```sweet-exp
"hello, world"
"line one\nline two"
```

### Character -- `#\`

`#\` followed by one character, a name, or a `u` hex escape is a character
literal of type `Char` (`stdlib/char.tur`, auto-loaded). A `Char` is not an
`:int`: compare with `char=?`, classify with `digit?` / `alpha?` / `space?`,
and cross explicitly with `char->int` / `int->char`.

| Spelling | Character |
|---|---|
| `#\A`, `#\+`, `#\(` | that character (printable ASCII) |
| `#\space` `#\newline` `#\tab` `#\return` `#\null` `#\backspace` `#\delete` `#\escape` `#\alarm` | the named control or space character |
| `#\u41` | code point 0x41 (1-4 hex digits, at most `#\uFF`) |

The character must be followed by a delimiter: `#\a2` is an error, not
`#\a` then `2`.

```turmeric
(char->int #\A)   ; => 65
(digit? #\7)      ; => true
```
```sweet-exp
char->int(#\A)   ; => 65
digit?(#\7)      ; => true
```

### Keyword -- `:`

A colon followed immediately by a name produces a keyword (an interned
compile-time constant).

```turmeric
:ok
:error
:tur
```
```sweet-exp
:ok
:error
:tur
```

### Symbol / identifier

Any name that starts with a letter or one of `+ - * / = < > ! ? _ $ & . ^ |`
or a UTF-8 code-point in the symbol-start set (includes `λ`, `∀`, `∃`).

```turmeric no-check
foo
my-var
+
->
|>
λ
```
```sweet-exp
foo
my-var
+
->
|>
λ
```

---

## Collections

### List -- `(...)`

An ordered sequence of forms. The primary syntactic unit in Turmeric.

```turmeric
(println "hello")
(+ 1 2 3)
(defn square [x : int] : int (* x x))
```
```sweet-exp
println("hello")
+(1 2 3)
defn square [x :int] :int
  *(x x)
```

### Vector -- `[...]`

A vector literal.

```turmeric
[1 2 3]
[:a :b :c]
```
```sweet-exp
[1 2 3]
[:a :b :c]
```

### Map literal -- `#map{...}`

A map (hash-array-mapped trie) literal. Key-value pairs are written
interleaved.

```turmeric no-check
#map{:name "Alice" :age 30}
```
```sweet-exp
#map{:name "Alice" :age 30}
```

(The bare `#{...}` form historically doubled as a map literal *and* an
effect-row annotation; it is now deprecated -- see [Deprecated Forms](#deprecated-forms)
below.)

### Set literal -- `#set{...}`

A set literal.

```turmeric no-check
#set{1 2 3}
#set{:red :green :blue}
```
```sweet-exp
#set{1 2 3}
#set{:red :green :blue}
```

### Owned-String literal -- `#s"..."`

`#s"text"` reads as `(string/from-cstr "text")` -- a fresh owned `String`,
where a bare `"text"` stays a borrowed `cstr`. It is dispatched by the
delimiter, so it does not collide with the `#s(...)` set literal above.

Like every other form in this guide it is **always on** -- no `#lang` token,
no `#use-reader-macros`:

```turmeric no-check
(load "stdlib/string.tur")
#s"hello"            ; => (string/from-cstr "hello"), an owned String
```

That `(load)` is the one thing still required, and the reason is worth
knowing: the dispatch is read-time, and the code it expands into is not. The
[strings guide](strings-guide.md) has the read-time / eval-time split.

`#s"..."` used to be opt-in, through a `stringed` `#lang` layer or a
`#use-reader-macros "stdlib/string-reader.tur"` directive. Both still parse
for one minor line -- the token warns (`TUR-W0064`) and the file is a
comment-only no-op -- and both can simply be deleted.

### Contract type -- `#refine{ var : T | pred }`

A refinement type annotation. Introduces a binding `var` of type `T` with
predicate `pred`. Bare `{...}` is curly-infix in every dialect (see
[Curly-infix](#curly-infix----a--b-) below), so contract types use the
`#refine{...}` data-literal form alongside `#map{...}`, `#set{...}`,
`#row{...}`, and `#r{...}`.

```turmeric no-check
#refine{ x : :int | (> x 0) }
```
```sweet-exp
#refine{ x : :int | (> x 0) }
```

### Curly-infix -- `{a + b}`

SRFI-105 curly-infix is enabled in every dialect (plain s-expression
Turmeric, `#lang sweet-exp`, `--lang curly-infix`, `--lang neoteric`). A
bare `{...}` form with one repeated operator at the odd positions lowers
to the equivalent prefix call; anything else lowers to `($nfx$ ...)` for a
user-defined precedence macro to handle.

```turmeric
{1 + 2}              ; => (+ 1 2)
{a * b}              ; => (* a b)
{ {a + b} * c }      ; => (* (+ a b) c)
{a + b * c}          ; => ($nfx$ a + b * c)  -- mixed operators
```
```sweet-exp
{1 + 2}              ; => (+ 1 2)
{a * b}              ; => (* a b)
{ {a + b} * c }      ; => (* (+ a b) c)
{a + b * c}          ; => ($nfx$ a + b * c)  -- mixed operators
```

---

## Prefix Reader Macros

Each of these reads one following datum and rewrites it into a longer form.

### Quote -- `'expr`

Expands to `(quote expr)`. Prevents evaluation.

```turmeric
'foo          ; => (quote foo)
'(1 2 3)      ; => (quote (1 2 3))
```
```sweet-exp
'foo
; => (quote foo)
'
1(2 3)
; => (quote (1 2 3))
```

### Quasiquote -- `` `expr ``

Expands to `(quasiquote expr)`. Like quote, but `~` and `~@` splice in values.

```turmeric
`(1 2 3)             ; => (quasiquote (1 2 3))
`(1 ~x 3)            ; unquotes x
`(1 ~@xs 3)          ; splices list xs
```
```sweet-exp
`(1 2 3)
; => (quasiquote (1 2 3))
`(1 ~x 3)
; unquotes x
`(1 ~@xs 3)
; splices list xs
```

### Unquote -- `~expr`

Valid only inside a quasiquote. Expands to `(unquote expr)`.

```turmeric
`(a ~b c)   ; b is evaluated and inserted
```
```sweet-exp
`(a ~b c)
; b is evaluated and inserted
```

### Unquote-splicing -- `~@expr`

Valid only inside a quasiquote. Expands to `(unquote-splicing expr)`. The
value must be a list; its elements are spliced into the surrounding list.

```turmeric
(let [xs '(2 3)]
  `(1 ~@xs 4))   ; => (1 2 3 4)
```
```sweet-exp
let [xs '(2 3)]
  `(1 ~@xs 4)
; => (1 2 3 4)
```

### Deref -- `@expr`

Expands to `(deref expr)`.

```turmeric
@my-ref   ; => (deref my-ref)
```
```sweet-exp
@my-ref
; => (deref my-ref)
```

### Effect-row annotation -- `@{...}` (deprecated)

A `@` immediately followed by `{` reads a brace-delimited effect-row map
rather than a deref. **Deprecated** in favor of [`#fx{...}`](#hash-dispatch-forms);
see [Deprecated Forms](#deprecated-forms). Bare `@x` deref sugar is unaffected.

```turmeric
@{IO}             ; TUR-D0003 -- prefer #fx{IO}
@{IO Exn}         ; TUR-D0003 -- prefer #fx{IO Exn}
```

### Borrow -- `&expr`

Expands to `(& expr)`. A bare `&` followed by whitespace or a close delimiter
is left as the symbol `&` (preserving the explicit `(& x)` call form).

```turmeric
&x       ; => (& x)
(& x)    ; identical -- explicit form still works
```
```sweet-exp
&x
; => (& x)
(& x)
; identical -- explicit form still works
```

### Mutable borrow -- `&mut expr`

Expands to `(&mut expr)`.

```turmeric
&mut x   ; => (&mut x)
```
```sweet-exp
&mut
x
; => (&mut x)
```

---

## Hash-Dispatch Forms

Forms beginning with `#` followed by a specific character.

### Attribute -- `#[...]`

Attaches a compile-time attribute to the next definition.

```turmeric no-check
#[no-unwind]
(defn risky [] :int ...)
```
```sweet-exp
#[no-unwind]
defn risky [] :int
  ...
```

### Reader conditional -- `#?(...)`

Selects a branch based on the current reader target. Use `:tur` for compiled
output and `:turi` for the interpreter.

```turmeric no-check
(println #?(:tur "compiled" :turi "interpreted"))
```
```sweet-exp
println(#?(:tur "compiled" :turi "interpreted"))
```

### Effect row -- `#fx{Effect1 Effect2 ...}`

Annotates a function or handler signature with the effects it may perform.

```turmeric no-check
(defn read-line [] #fx{Unsafe IO} : cstr ...)
(defn pure-helper [] #fx{} : int 0)
```
```sweet-exp
defn read-line [] #fx{Unsafe IO} : cstr
  ...
```

---

## Deprecated Forms

These spellings are still accepted during the grace window but produce a
deprecation warning at every use. The migration script
`tools/migrate-fx-rows.py` rewrites a tree mechanically. The plan that
drives the rename lives at
[`docs/archive/history/fx-row-syntax-rename-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/fx-row-syntax-rename-plan.md).

### `#{...}` effect row -- TUR-D0002

The bare `#{Unsafe IO}` form is being retired in favor of the self-describing
`#fx{Unsafe IO}` form so the family of `#tag{...}` reader literals
(`#map{...}`, `#set{...}`, `#row{...}`, ...) is uniform and the `#{...}` slot
is freed for a future reader form. Run `tur explain TUR-D0002` for details.

### `@{...}` effect row -- TUR-D0003

The `@`-prefixed alternate sugar for an effect row is being retired alongside
the bare `#{...}` form -- two spellings for the same thing was the original
wart this rename fixes. Bare `@x` deref sugar is unaffected; only the
`@{...}` effect-row form is deprecated. Run `tur explain TUR-D0003`.

---

## Inline C Block -- ` ```c ... ``` `

A triple-backtick block embeds raw C code inside a `defn` body. The closing
` ``` ` must be on the same line as its enclosing `)`.

```turmeric no-check
(defn file-size [f] :int
  ```c
  FILE* file = (FILE*)f;
  return (int)ftell(file);
  ```)
```
```sweet-exp
defn file-size [f] :int
  ```c
  FILE* file = (FILE*)f;
  return (int)ftell(file);
  ```
```

---

## Sweet-Expression Extensions

The following forms are active when sweet-expression mode is enabled
(`#lang sweet-exp` or the `--sweet` flag).

### Neoteric application -- `f(...)`

A symbol (or atom) followed immediately by `(` (no space) is read as a
function call.

```turmeric
(square 9)     ; => (square 9)
```
```sweet-exp
square(9)
; => (square 9)
```

### Curly-infix -- `{a op b}`

Braces in sweet-exp mode treat the middle element as an infix operator.

```turmeric no-check
{1 + 2}       ; => (+ 1 2)
{x > 0}       ; => (> x 0)
```
```sweet-exp
{1 + 2}
; => (+ 1 2)
{x > 0}
; => (> x 0)
```

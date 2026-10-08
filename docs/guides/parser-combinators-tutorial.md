---
title: Parser Combinators Tutorial
category: Tutorials
description: Build a small parser in pure Turmeric using algebraic data types, GADTs, higher-order combinators, and pattern matching
---

# Parser Combinators Tutorial

This tutorial builds a working parser-combinator library in pure
Turmeric. By the end you will:

- Model a parse result as a parametric `defdata` sum type and walk it
  with exhaustive `match`.
- Assemble the classic combinators -- `psat`, `pchar`, `or-*`,
  `map-*` -- and use them to compose the grammar.
- Parse `"1+2*(3+4)"` into a **typed GADT AST** and evaluate it via
  `match`, so the whole thing round-trips to `15`.

The focus is pedagogy, not micro-optimisation. Every line is idiomatic
Turmeric with `defdata`, `defgadt`, `match`, and no inline C.

The runnable end-to-end version of every snippet here lives in
[`tests/fixtures/parsec-tutorial/input.tur`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/parsec-tutorial/input.tur).

> **Note.** The snippets below use the `#\<char>` character-literal
> syntax (`#\+` reads as `43`, `#\0` as `48`, `#\space` as `32`).
> That syntax is a v1 legibility slice; see
> [`docs/archive/legible-char-literals-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/legible-char-literals-plan.md).
> A `#\<char>` literal is just an `:int` -- the reader emits the byte
> code, so it composes with `=` and arithmetic exactly like the raw
> integer it replaces. The runnable fixture uses `#\` throughout.

---

## Why combinators?

A parser is a function that takes input and either succeeds -- consuming
some prefix and returning a value plus the leftover input -- or fails.

Hand-written recursive-descent parsers mutate a shared cursor, so trying
one branch, failing, and backing up requires bookkeeping. Parser
combinators replace that ceremony with values: each combinator is a
function *from parsers to a new parser*. The result reads like the
grammar.

Turmeric's ADTs and GADTs let us build the same story with the
type-checker on our side. A `(PRes A)` value can only mean "failure" or
"success carrying an `A`"; a valid `Expr` node can only be one of the
constructors we declared; `match` refuses to compile if we forget a
case. Every combinator gets stronger static guarantees than the
equivalent Haskell tutorial would have handed us.

---

## Modelling the input

For a **byte-oriented** parser we want to walk the source one character
at a time. The natural Turmeric type is a `:cstr` plus `cstr-nth` from
`stdlib/cstr` (not autoloaded, so it takes a
`(import cstr :refer [cstr-len cstr-nth])`). For the tutorial we keep
the input as a **list of character codes** instead -- a plain
`(cons int (cons int ...))` built with `stdlib/list`'s `list-head` and
`list-tail` -- because it lets every combinator work on a single value
type (`int`) without threading an index alongside the source. The
string `"1+2*(3+4)"` becomes:

```turmeric
(list #\1 #\+ #\2 #\* #\( #\3 #\+ #\4 #\))
```

Two helpers make the rest of the code read cleanly. End-of-input is a
`0` list carrier (nil):

```turmeric
(defn at-end? [xs : int] : bool (= xs 0))

(defn is-digit? [c : int] : bool
  (if (< c #\0) false (if (> c #\9) false true)))
```

`list-head xs` returns the current byte and `list-tail xs` advances the
cursor.

---

## The result type

A parse either fails or succeeds and returns leftover input. That's a
two-armed sum, parameterised over the success payload:

```turmeric
(defdata PRes [a]
  (PFail)
  (POK a int))
```

- `(PFail)` -- no parse.
- `(POK v rest)` -- parsed `v`, leftover input list is `rest` (an
  `int` list carrier -- `0` for end-of-input).

Every parser in the tutorial has the shape

```
Parser<A> = (fn [xs : int] : (PRes A))
```

where `xs` is the current input list.

---

## The primitive: `psat`

The one *primitive* parser -- the atom every other combinator is built
on -- consumes a single character if it matches a predicate:

```turmeric
(defn psat [pred : (fn [int] bool)] : (fn [int] (PRes int))
  (fn [xs : int] : (PRes int)
    (if (at-end? xs)
      (PFail)
      (let [c (list-head xs)]
        (if (pred c) (POK c (list-tail xs)) (PFail))))))
```

`psat` returns a *closure* -- a function value that captures `pred`.
Every combinator below follows the same shape: take some parsers (and
maybe a helper), return a new parser (a `(fn [int] (PRes A))`
closure). Once you have `psat`, `pchar` and `digit` are one-liners:

```turmeric
(defn eq-char [target : int] : (fn [int] bool)
  (fn [c : int] : bool (= c target)))

(defn pchar [target : int] : (fn [int] (PRes int))
  (psat (eq-char target)))

(defn digit [xs : int] : (PRes int)
  ((psat is-digit?) xs))
```

`pchar` currying: `eq-char` builds a predicate closure and `psat`
lifts it into a parser. `digit` invokes `(psat is-digit?)` eagerly and
eta-expands the result so it can be used as a plain top-level parser.

---

## Combinators

The combinators are the recurring shapes -- alternation, mapping,
sequencing -- lifted out of ad-hoc grammar code so we can compose them
freely. The tutorial spells each one *monomorphically* (one instance
per element type: `or-int` / `or-expr`, `map-int-to-expr`) as a
pedagogical choice -- the shapes are easier to read when the element
type is spelled out. The polymorphic spelling `(or-parser [A] p q)`
also works: the compiler infers `A` through the returned closure at
the application site.

### Alternation -- `or-*`

"Try `p`; if it fails, try `q`":

```turmeric
(defn or-int [p : (fn [int] (PRes int)) q : (fn [int] (PRes int))]
    : (fn [int] (PRes int))
  (fn [xs : int] : (PRes int)
    (match (p xs)
      (POK v rest) (POK v rest)
      (PFail)      (q xs))))

(defn or-expr [p : (fn [int] (PRes Expr)) q : (fn [int] (PRes Expr))]
    : (fn [int] (PRes Expr))
  (fn [xs : int] : (PRes Expr)
    (match (p xs)
      (POK v rest) (POK v rest)
      (PFail)      (q xs))))
```

The two are line-for-line identical except for the element type. They
collapse to a single polymorphic `defn`:

```turmeric
(defn or-parser [A] [p : (fn [int] (PRes A)) q : (fn [int] (PRes A))]
    : (fn [int] (PRes A))
  (fn [xs : int] : (PRes A)
    (match (p xs)
      (POK v rest) (POK v rest)
      (PFail)      (q xs))))
```

`A` is a type parameter declared right after the name; it appears in
both the argument closures and the returned closure, and the compiler
grounds it at each application site (e.g. `(or-parser paren-expr
number-as-expr)` grounds `A = Expr`). The tutorial keeps the
monomorphic pair below for readability, but any call to `or-int` or
`or-expr` can be replaced with `or-parser` verbatim.

### Mapping -- `map-*-to-*`

"Run `p`; on success, transform the payload":

```turmeric
(defn map-int-to-expr [f : (fn [int] Expr) p : (fn [int] (PRes int))]
    : (fn [int] (PRes Expr))
  (fn [xs : int] : (PRes Expr)
    (match (p xs)
      (PFail)      (PFail)
      (POK v rest) (POK (f v) rest))))
```

Note the bare `(PFail)` on the failure arm: the enclosing
`fn`'s declared return `(PRes Expr)` propagates into the arm body, so
no `(:: (PFail) (PRes Expr))` ascription is needed. Same for the
`(PFail)` inside `or-*`.

The polymorphic spelling factors the same way:

```turmeric
(defn map-parser [A B] [f : (fn [A] B) p : (fn [int] (PRes A))]
    : (fn [int] (PRes B))
  (fn [xs : int] : (PRes B)
    (match (p xs)
      (PFail)      (PFail)
      (POK v rest) (POK (f v) rest))))
```

Two type parameters this time -- the input element `A` and the mapped
output `B` -- because `map` is where the element type actually changes
shape. `(map-parser to-enum number)` grounds `A = int`, `B = Expr` and
gives back exactly `number-as-expr`.

---

## The AST as a GADT

Every constructor pins the `[a]` parameter to `int`. In a richer
calculator you could add `(EEq (Expr int) (Expr int) : (Expr bool))`
and the type checker would then refuse `EAdd (EEq ...) (ENum 1)` at
compile time. That's the GADT payoff -- illegal ASTs stop being
representable.

```turmeric
(defgadt Expr [a]
  (ENum int                     : (Expr int))
  (EAdd (Expr int) (Expr int)   : (Expr int))
  (ESub (Expr int) (Expr int)   : (Expr int))
  (EMul (Expr int) (Expr int)   : (Expr int))
  (EDiv (Expr int) (Expr int)   : (Expr int)))

(defn apply-op [op : int lhs : Expr rhs : Expr] : Expr
  (if (= op #\+) (EAdd lhs rhs)
    (if (= op #\-) (ESub lhs rhs)
      (if (= op #\*) (EMul lhs rhs)
        (if (= op #\/) (EDiv lhs rhs)
          (ENum 0))))))
```

`apply-op` dispatches on the operator byte. It falls through to
`(ENum 0)` on an unknown byte, which never happens if the grammar is
correct -- and if it does, `eval-expr` will still produce a defined
value.

---

## The grammar

```
expr   := term (('+' | '-') term)*
term   := factor (('*' | '/') factor)*
factor := number | '(' expr ')'
number := digit+
```

The two-level `expr` / `term` split is what buys precedence: `*` and
`/` are one level deeper than `+` and `-`, so they bind tighter.

### `number` -- one-or-more digits, folded

```turmeric
(defn digits-int-loop [xs : int acc : int] : (PRes int)
  (if (at-end? xs)
    (POK acc xs)
    (let [c (list-head xs)]
      (if (is-digit? c)
        (digits-int-loop (list-tail xs) (+ (* acc 10) (- c #\0)))
        (POK acc xs)))))

(defn number [xs : int] : (PRes int)
  (match (digit xs)
    (PFail)      (PFail)
    (POK d rest) (digits-int-loop rest (- d #\0))))
```

`digit` (from earlier) is the "at-least-one" gate; `digits-int-loop`
is the tail-recursive `many` that greedily accumulates the rest into
an int. The recursion is self-tail-call, so the compiler turns it into
iteration -- no stack growth, no O(n) intermediate allocations.

### `factor` -- alternation via `or-expr`

```turmeric
(defn to-enum [n : int] : Expr (ENum n))

(defn number-as-expr [xs : int] : (PRes Expr)
  ((map-int-to-expr to-enum number) xs))

(defn paren-expr [xs : int] : (PRes Expr)
  (if (at-end? xs)
    (PFail)
    (let [c (list-head xs)]
      (if (= c #\()
        (match (expr-parse (list-tail xs))
          (PFail)          (PFail)
          (POK inner rest)
          (if (at-end? rest)
            (PFail)
            (if (= (list-head rest) #\))
              (:: (POK inner (list-tail rest)) (PRes Expr))
              (PFail))))
        (PFail)))))

(defn factor [xs : int] : (PRes Expr)
  ((or-expr paren-expr number-as-expr) xs))
```

`factor` is now a one-liner over `or-expr`: try `paren-expr` first,
fall back to `number-as-expr`. `number-as-expr` shows the `map` shape
in action -- `to-enum` wraps `ENum` as a plain function value (a
GADT constructor can't yet be passed directly as a first-class value)
and `map-int-to-expr` lifts it into a `Parser<Expr>`.

`paren-expr` still needs to reach into `match` for the closing `)`
handling; not every one-off pattern collapses into a combinator.

One `(:: (POK inner (list-tail rest)) (PRes Expr))` ascription appears
in `paren-expr`. It is optional -- the forward-decl pass records
`expr-parse`'s compound parametric return type even though `expr-parse`
is defined *later* in the file (mutual recursion), so the match binder
infers correctly without it -- but the tutorial fixture carries it
verbatim.

### Left-associative chains -- `term` and `expr`

`term := factor (('*'|'/') factor)*` is a `factor` followed by an
inlined `many` over `(op, factor)` pairs, folded left-to-right:

```turmeric
(defn term-tail [xs : int lhs : Expr] : (PRes Expr)
  (if (at-end? xs)
    (POK lhs xs)
    (let [c (list-head xs)]
      (if (if (= c #\*) true (= c #\/))
        (match (factor (list-tail xs))
          (PFail)          (PFail)
          (POK rhs rest)   (term-tail rest (apply-op c lhs rhs)))
        (POK lhs xs)))))

(defn term [xs : int] : (PRes Expr)
  (match (factor xs)
    (PFail)          (PFail)
    (POK lhs rest)   (term-tail rest lhs)))
```

`term-tail` is where left-associativity comes from: we apply the op
*before* recursing, so `2*3*4` folds into `EMul (EMul 2 3) 4`, not
`EMul 2 (EMul 3 4)`. `expr` and `expr-tail` are the same shape with
`+`/`-` and `term` swapped in.

```turmeric
(defn expr-tail [xs : int lhs : Expr] : (PRes Expr)
  (if (at-end? xs)
    (POK lhs xs)
    (let [c (list-head xs)]
      (if (if (= c #\+) true (= c #\-))
        (match (term (list-tail xs))
          (PFail)          (PFail)
          (POK rhs rest)   (expr-tail rest (apply-op c lhs rhs)))
        (POK lhs xs)))))

(defn expr-parse [xs : int] : (PRes Expr)
  (match (term xs)
    (PFail)          (PFail)
    (POK lhs rest)   (expr-tail rest lhs)))
```

The chain-fold pattern -- match the head, keep folding tails, wrap
each application through `apply-op` -- is itself a candidate for a
`chainl1` combinator. Today it reads clearly enough as its own `defn`.

---

## Evaluation

Evaluation is a five-line walk of the GADT. No tag comparisons, no
default arm, no fall-through -- `match` proves every constructor is
handled:

```turmeric
(defn eval-expr [e : Expr] : int
  (match e
    (ENum n)   n
    (EAdd l r) (+ (eval-expr l) (eval-expr r))
    (ESub l r) (- (eval-expr l) (eval-expr r))
    (EMul l r) (* (eval-expr l) (eval-expr r))
    (EDiv l r) (/ (eval-expr l) (eval-expr r))))
```

### The round-trip

```turmeric
(defn main [] : int
  (let [input (list #\1 #\+ #\2 #\* #\( #\3 #\+ #\4 #\))]   ;; "1+2*(3+4)"
    (match (expr-parse input)
      (PFail)          (do (println 0) 1)
      (POK ast rest)   (do (println (eval-expr ast)) 0))))
```

Running the fixture prints `15`. Swap `eval-expr` for a pretty-printer
and you have a normaliser without touching the parser code -- that's
the separation of syntax and semantics the ADT/GADT split buys you.

---

## Where the types earned their keep

Looking back at what the type system gave us:

- **`match` on `PRes`** is exhaustiveness-checked. We physically cannot
  forget the failure branch -- the compiler would refuse the definition.
- **`(PRes Expr)`** carries the success payload's type. `number`
  returns `(PRes int)`; `factor` returns `(PRes Expr)`. A cross-wire
  is a type error, not a runtime crash.
- **The `Expr` GADT** rules out impossible AST shapes. There is no
  "unknown tag" case to guard against because there is no way to
  construct one.
- **`is-digit?`** returns `:bool`, not `:int`. The rule against `:int`
  stand-ins (`CLAUDE.md`) is directly why.

None of these are aesthetic wins. Each rules out an entire class of
bugs at compile time. That is the reason to reach for GADTs and ADTs
even for a tutorial-sized parser.

---

## Where to go next

- **Exercise:** write a JSON parser. Strings-with-escapes and
  numbers-with-exponents are the hard cases and need only the patterns
  here. See `tests/fixtures/parsec-json-subset/` for a starting
  point.
- **Production library:** [`stdlib/parsec.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/parsec.tur)
  has performance-tuned versions of every combinator plus `pstring`,
  `parse-value`, and friends -- built on top of inline-C for the tight
  loops.
- **Real strings:** add `(import cstr :refer [cstr-len cstr-nth])` and
  the input list of ASCII ints goes away -- the parser takes a `:cstr`
  directly.
- **Polymorphic combinators:** the `or-int` / `or-expr` (and every
  other monomorphic pair in the tutorial) can be collapsed to a single
  `(or-parser [A] p q)`; keeping the monomorphic spelling in the
  tutorial is a pedagogical choice, not a compiler limit.
- **Error positions and recovery:** the current library returns "no
  result" on failure. A production parser threads the furthest position
  reached, so error messages can say *where* parsing died.
- **Packrat memoisation:** caching `parser` x `input-pos` results
  turns exponential grammars into linear ones. It fits neatly on top
  of the interface here.

## See also

- [gadts-guide.md](gadts-guide.md) -- `defgadt`, `match`, type refinement
- [backtracking-guide.md](backtracking-guide.md) -- the list monad and
  nondeterminism (the direction a full-fat combinator library would go)
- [`stdlib/parsec.tur`](https://github.com/turmeric-lang/turmeric/tree/main/stdlib/parsec.tur)
  -- the production parser-combinator library
- [`tests/fixtures/parsec-tutorial/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/parsec-tutorial/)
  -- the runnable fixture for this tutorial

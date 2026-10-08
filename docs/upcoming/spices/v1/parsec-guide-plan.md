# Plan: Updated Parser Combinators Tutorial and Guide (v1)

> Status: planning. **Revised 2026-10-07** against turmeric v0.63.5; the
> premise (stdlib `parsec` still has the raw-`int` API) is unchanged, but
> several prerequisites in the original were written for a stdlib that no
> longer exists. See "Revision 2026-10-07" below before reading P1-P9.
> Tracks: `stdlib/parsec.tur` typeclass migration
> Replaces: no existing guide for `stdlib/parsec.tur` (this is the first one;
> [parser-combinators-tutorial.md](../../../guides/parser-combinators-tutorial.md)
> now exists, but it builds a small combinator library from scratch rather than
> documenting `stdlib/parsec`)
> Location note: this is a turmeric-repo stdlib plan, not a spice plan. It sits
> under `docs/upcoming/spices/v1/` for historical reasons; the `vendor/tur`,
> `scripts/install-tur.sh` and `spices/...` paths in it are from a spices-repo
> vantage and do not apply (see P9).

## Revision 2026-10-07

What changed in turmeric since this plan was written, and what it does to the
phases:

| Original assumption | Reality on v0.63.5 | Effect |
|---|---|---|
| `#\A` reader syntax does not exist (P2) | Landed (`docs/archive/legible-char-literals-plan.md`), but a `#\A` literal **is an `:int`**, not a `Char` | P2 is done, with different semantics. A `Char` newtype (P1) would reject `#\A` unless the reader changes, so P1 needs a decision first (below) |
| `maybe.tur` is net-new (P4) | `stdlib/option.tur` ships `(defdata Option :copy [A] (None) (Some A))` | Do **not** add `Maybe`. Use `(Option A)` / `option<A>` everywhere the plan says `Maybe`; `just`/`nothing` become `Some`/`None` |
| `(List A)` must be added to `list.tur` (P5) | `stdlib/list-typed.tur` already defines `(defopaque List [A] :int)` with `list-of`; it is **opt-in** (`load`) because `List` is a common user type name | P5 is "re-type `many`/`many1` against `list-typed`", not "add a type". Decide whether parsec should force that load |
| `string.tur` may need creating (P6) | `stdlib/string.tur` ships an owned, refcounted `String` (plus `StringBuilder`), and `cstr` is the borrowed form | No new module. `pstring` should take `cstr` (a borrowed literal) and return `String`, not take `String` |
| `Monad [Parser]` is at `parsec.tur:874` (P8) | The `Functor`/`Applicative`/`Monad`/`Alternative` instances exist at about lines 877-911, `do-m` is in `stdlib/macros.tur`, and `alt-or` is the `Alternative` method | P8 reduces to auditing the instances against the new result types |
| The repo has `char.tur`, `maybe.tur`, `string.tur` as gaps | Only `char.tur` is still absent | Of P1/P4/P5/P6 only P1 is net-new code |
| `parsec` tests live in a spice (`spices/parsec-examples`) | They are compiled fixtures: `tests/fixtures/parsec-{basic,full,json-subset,many,or,sequence,tutorial}` | P9's test matrix goes there, and `bash tests/run.sh` is the gate, not a spices CI job |

**Still true, and now the strongest argument for the plan:**
`stdlib/parsec.tur` is still the "magic int" API (`item`, `pchar`, `pstring`,
`many`, `many1` and `optional` all return `(Parser int)`; `run-parser` and
`run-parser-full` return `int`). That is the typed-handle defect the repo's
"no lazy `:int` stand-ins" rule exists to prevent, so the typed surface is
worth doing independently of the guide.

**Decision needed before P1.** `#\A` is an `:int`, so there are two coherent
shapes and the plan must pick one:

1. **Keep characters as `int`** and give `pchar` / `satisfy` an `int` argument.
   Cheapest, matches the reader, and `Char` is skipped. Rejects the plan's
   `(Parser Char)` goal.
2. **Add `Char` and make `#\A` read as a `Char`.** Honors the plan's intent
   but changes the reader and every existing `#\` use (including the
   `parsec-tutorial` fixture and the tutorial doc) from `:int` to `Char`.

Recommendation: option 2 only if the reader change is wanted for its own sake;
otherwise ship the `Option` / `List` / `String` result typing now (P4-P7) and
leave the character type for a separate plan. Nothing in P4-P7 depends on P1.

## Motivation

The current `parsec.tur` API exposes several "magic int" rough edges that
the typeclass consolidation (`stdlib-hkt-consolidation T2`) was meant to
remove at the library layer but that never surfaced in user-facing docs or
examples:

| Current API | Problem | Target API |
|---|---|---|
| `(pchar 65)` | ASCII code as int literal | `(pchar #\A)` |
| `(item)` returns `(Parser int)` | value is raw ASCII code | `(Parser Char)` |
| `(optional p)` uses `0` = absent | sentinel int ≠ absent | `(Parser (Maybe A))` |
| `(many p)` returns `(Parser int)` | erases `A` to raw Cell ptr | `(Parser (List A))` |
| `(pstring "hi")` returns `(Parser int)` | raw cstr pointer | `(Parser String)` |
| `(bind-parser p f)` takes `f : fn` | loses `A -> (Parser B)` type | `(do-m ...)` / `(bind ...)` |
| `(or-parser p q)` | bespoke combinator | `(alt-or p q)` via `Alternative` |

The tutorial should be written as if the new `Char`, `Maybe`, `List`, and
`String` wrappers already exist (they are the implementation work this plan
also tracks -- see §Prerequisite changes below). The guide author writes the
target API; the implementation team delivers it.

---

## Prerequisite changes to `stdlib/parsec.tur`

These changes must land before the guide examples can be tested. They are
ordered by dependency: each task assumes the previous ones are in place.

Current state inventory (turmeric `stdlib/`, revised 2026-10-07; the original
read `vendor/tur/stdlib/` from a spices checkout): `parsec.tur` is about 910
lines with the legacy raw-`int` API; `list-typed.tur` provides an opt-in
`(List A)`; `option.tur` provides `(Option A)`; `string.tur` provides an owned
`String`; `char.tur` does **not** exist. The reader accepts `#\A`, as an `:int`.
The phase text below is the original plan, with a **[REVISED]** note where
the inventory above changes it.

### P1 — `Char` newtype and module (`stdlib/char.tur`, net-new)

Wraps an ASCII/Unicode code point in an opaque type so `Char` cannot be
mixed with raw `int` at call sites.

**Tasks**
- Create `stdlib/char.tur` with `(defopaque Char :int)`.
- Export constructors / destructors: `(char-lit i)` (int → Char, internal use
  by reader), `(int->char i)`, `(char->int c)`, `(char->str c)`.
- Export predicates: `digit?`, `alpha?`, `alnum?`, `space?`, `upper?`,
  `lower?`, `punct?`. All have signature `Char -> Bool`. Bodies are
  straight ASCII range checks on `(char->int c)`.
- Export `(char-eq? a b) : Bool` and a `definstance Eq [Char]` so
  predicates composed with `satisfy` work.
- Unit tests in `spices/char/test/char_test.tur` (new spice or co-located
  with parsec tests — pick one and note here).

**Acceptance**
- `(int->char 65)` round-trips through `(char->int ...)` → `65`.
- `(digit? (int->char 48))` → `true`; `(digit? (int->char 47))` → `false`.
- Building a parser with `(satisfy digit?)` typechecks.

### P2 — `#\A` reader syntax (compiler-side, tracked here for sequencing)

**[REVISED 2026-10-07] DONE, with different semantics.** The reader accepts
`#\A`, `#\space`, `#\newline` and the rest, but a char literal reads as an
`:int` byte code, not `(char-lit 65)` / `Char`. The tasks below about filing a
tracking issue and bumping `TUR_VERSION` are obsolete. Whether to go further and
read `#\A` as a `Char` is the open decision at the top of this plan.

The Scheme-style char literal `#\A` must desugar to `(char-lit 65)` so the
guide can write `(pchar #\A)` instead of `(pchar (int->char 65))`.

**Tasks**
- File a tracking issue against `turmeric-lang/turmeric` referencing this plan.
  This repo cannot implement it; it lands in a tagged `tur` release.
- Required escapes: `#\space`, `#\newline`, `#\tab`, `#\\`, `#\)`.
- Required range: at least printable ASCII (32–126).
- Once landed, bump `TUR_VERSION` in `scripts/install-tur.sh` to the
  release tag that includes the reader change.

**Acceptance**
- `tur check` on a file containing `(pchar #\A)` succeeds against the new
  pinned `tur` binary.
- `(char->int #\A)` evaluates to `65` at runtime.

**Blocks**: P3 examples and all tutorial steps that use `#\X` literals. If
this slips, the guide can ship using `(int->char 65)` placeholders and a
follow-up doc PR swaps them in.

### P3 — `satisfy` and typed `pchar` / `item` (`stdlib/parsec.tur`)

Replace the raw-int character primitives with `Char`-typed versions.

**Tasks**
- Change `(item)` return type from `(Parser int)` to `(Parser Char)`. The
  underlying `item-impl` still pulls an `int` from input; the parser wrapper
  applies `int->char` before returning.
- Change `(pchar c)` signature to `[c : Char] : (Parser Char)`. Internally
  call `char->int` to compare against the input byte.
- Add `(satisfy pred : (-> Char Bool)) : (Parser Char)` — implemented as
  `(bind-parser (item) (fn [c] (if (pred c) (pure c) (pfail))))` but exposed
  as a primitive so users never write `bind-parser` themselves.
- Keep the old `int`-typed `pchar` available as `pchar-int` for one release
  for migration; mark it `@deprecated` in the docstring.

**Acceptance**
- `(pchar #\A)` typechecks and parses `"Abc"` → `#\A`.
- `(satisfy digit?)` parses `"7x"` → `#\7`, fails on `"x"`.
- The full `parsec.tur` test suite passes after the type change.

### P4 — `Maybe A` module and typed `optional` (`stdlib/maybe.tur`, net-new)

**[REVISED 2026-10-07]** Do not create `maybe.tur`. `stdlib/option.tur` already
has `(Option A)` with `Some` / `None`, `some?` and `none?`; use it. `optional`
becomes `(Parser (Option A))`, and every `just` / `nothing` below reads `Some` /
`None`. The Functor/Applicative/Monad instances for `Option` are the stdlib's,
not new work.

**Tasks**
- Create `stdlib/maybe.tur` with `(defopaque (Maybe A) :ptr<void>)` backed
  by a tagged cell (tag 0 = nothing, tag 1 = just + value pointer).
- Export `(just v) : (Maybe A)`, `(nothing) : (Maybe A)`,
  `(just? m) : Bool`, `(nothing? m) : Bool`, `(just-value m) : A`,
  `(maybe-or m default) : A`, `(fmap-maybe m f) : (Maybe B)`.
- `definstance Functor [Maybe]`, `Applicative [Maybe]`, `Monad [Maybe]` so
  it composes with the rest of the typeclass surface.
- Change `(optional p)` in `parsec.tur` to return `(Parser (Maybe A))`.
  Implementation: try `p`; on success wrap in `just`, on failure return
  `(nothing)` without consuming input.

**Acceptance**
- `(run-parser-full (optional (pchar #\-)) "-42")` → `(just (just #\-))`
  (outer `just` from `run-parser-full`, inner from `optional`).
- `(run-parser-full (optional (pchar #\-)) "42")` → `(just (nothing))`.
- No call site needs to check `(= r 0)` for absence.

### P5 — Typed `List A` surface for `many` / `many1` (`stdlib/list.tur` + `parsec.tur`)

**[REVISED 2026-10-07]** `(List A)` exists: `stdlib/list-typed.tur`
(`(defopaque List [A] :int)`, `list-of`). It is opt-in because `List` is a
common user type name, so the real task is re-typing `many` / `many1` to
`(Parser (List A))` and deciding whether `parsec.tur` loads `list-typed.tur`
itself (which puts `List` in every parsec user's namespace). Skip the
"add `defopaque List`" task.

`list.tur` already implements the cons-list operations but the parser
combinators return a raw `int` cell pointer. Re-expose the existing cells
behind the `(List A)` newtype and re-type the parser combinators.

**Tasks**
- Add `(defopaque (List A) :ptr<void>)` in `stdlib/list.tur` if not already
  present; ensure `nil`, `cons`, `null?`, `head`, `tail`, `list-length`,
  `list-foldl`, `list-map` all have `(List A)`-typed signatures.
- Export `(list->str : (List Char) -> String)`. Implementation walks the
  list and writes each `(char->int c)` into a freshly-allocated buffer.
  Location decision (Open Question 2) recommendation: put it in
  `tur/list` since it is a list consumer; re-export from `tur/string` for
  discoverability.
- Change `(many p)` return type from `(Parser int)` to `(Parser (List A))`.
- Change `(many1 p)` return type similarly.
- Verify `bt-cons` / `bt-nil` internals still work — the change is only at
  the type-surface layer; the underlying cell representation is unchanged.

**Acceptance**
- `(run-parser-full (many (satisfy digit?)) "123abc")` returns a `(List Char)`
  with `list-length` = 3.
- `(list->str (parse-first (many (satisfy digit?)) "123"))` → `"123"`.
- `(many p)` still succeeds on empty input and returns a list with
  `(null? result)` true.

### P6 — `String` module and typed `pstring` (`stdlib/string.tur`, net-new or extend)

**[REVISED 2026-10-07]** `stdlib/string.tur` exists: an owned, refcounted
`String` (`:ptr<void>`, non-null) distinct from the borrowed `cstr`. Do not
define `(defopaque String :cstr)`. `pstring` keeps its `cstr` argument and
returns `(Parser String)`, which owns the matched bytes.

**Tasks**
- Create `stdlib/string.tur` (or extend if a stub exists) with
  `(defopaque String :cstr)` and the operations the guide uses:
  `(string-length s)`, `(string-eq? a b)`, `(string-append a b)`,
  `(cstr->string s)`, `(string->cstr s)`.
- Change `(pstring s)` in `parsec.tur` to take `String` and return
  `(Parser String)`. The current implementation takes `cstr` — at the
  reader level a `"hi"` literal already produces something convertible to
  `String`; add the wrap.
- Update `pstring-c-impl` to return the matched `String` rather than an
  `int` cell.

**Acceptance**
- `(run-parser-full (pstring "hi") "hi there")` → `(just "hi")`.
- `(string-eq? (parse-first (pstring "ok") "ok") "ok")` → `true`.

### P7 — Typed `run-parser` / `run-parser-full` / `parse-first`

**Tasks**
- Change `(run-parser p s) : (Parser A) cstr -> (List (Pair A String))`.
  The underlying `bt-cons` chain is already a list of pairs; this is a
  type-surface change plus wrapping the remaining `cstr` as `String`.
- Add `(Pair A B)` opaque if not present, or reuse the existing
  `pair-new` / `pair-first` / `pair-second` with a typed wrapper.
- Change `(run-parser-full p s) : (Parser A) String -> (Maybe A)`. Returns
  `(just v)` iff exactly one parse consumed all input; `(nothing)`
  otherwise. (If multiple full parses exist, return the first — document
  in the guide.)
- Add `(parse-first p s) : (Parser A) String -> A`. Calls `run-parser-full`;
  on `(nothing)` panic with `"parse failed at: <remaining input>"`
  (Open Question 3 recommendation).
- Remove or rename the legacy `parse-value` / `run-parser-full-c` helpers
  that return raw ints.

**Acceptance**
- `(parse-first (pchar #\A) "Abc")` → `#\A`.
- `(run-parser-full (pchar #\A) "Bbc")` → `(nothing)`.
- `(run-parser (many (pchar #\a)) "aab")` returns a `(List (Pair (List Char) String))`
  with at least one entry whose remaining string is `"b"`.

### P8 — Typeclass instances and `do-m`

The guide opens with `do-m`. Confirm the existing `Monad [Parser]` instance
(`parsec.tur:874`) is wired to whatever macro provides `do-m` in the
current stdlib.

**Tasks**
- Audit `definstance Functor [Parser]` / `Applicative [Parser]` /
  `Monad [Parser]` / `Alternative [Parser]` to confirm methods type-check
  against the new `(Parser Char)` / `(Parser (List A))` / `(Parser (Maybe A))`
  signatures.
- Verify `do-m` resolves to `bind` on the `Monad` instance for `Parser` (the
  tutorial Step 3 example must run end-to-end without import gymnastics).
- Ensure `alt-or` is exported as the user-facing name for `Alternative`'s
  `<|>` method (Step 4 uses it directly).

**Acceptance**
- Tutorial Steps 3, 4, 7 from §Tutorial outline compile and execute against
  the rebuilt `parsec.tur`.

### P9 — Test matrix and migration

**[REVISED 2026-10-07]** Wrong repo vantage: parsec is turmeric stdlib, so
there is no `spices/parsec-examples`, `scripts/install-tur.sh` or spices CI
step. Put one fixture per tutorial step beside the existing
`tests/fixtures/parsec-*` fixtures and gate on `bash tests/run.sh`. The `rg`
audits should run over `stdlib/`, `tests/fixtures/` and the spices checkout
(`regex`, `c-dsl`, `scscm` are likeliest to call parsec).

**Tasks**
- Add `spices/parsec-examples/` (or extend the existing parsec test spice)
  with one file per tutorial step (1–8). Each is a `tur check`-clean
  runnable example used as the doc's source of truth.
- Wire those examples into `.github/workflows/ci.yml` so doc drift breaks
  the build.
- Search the repo for callers of the old API and migrate them in the same
  PR as P3/P4/P5/P6:
  - `rg "\(pchar [0-9]" spices/` → replace integer-form `pchar`.
  - `rg "\(optional " spices/` → audit for `(= result 0)` patterns.
  - `rg "\(many " spices/` → audit for raw-cell traversal.

**Acceptance**
- `./scripts/install-tur.sh && tur check spices/parsec-examples/...` is
  green on macOS-arm64 and linux-x86_64.
- CI passes on a branch that contains all of P1–P8 plus this migration.

### Dependency graph

```
P1 (Char) ──┬─► P3 (satisfy/pchar/item) ──┐
P2 (#\A reader, compiler) ──────────────────┘
P4 (Maybe) ─────────────────► (optional) ──┐
P5 (List)  ─────────────────► (many/many1) ─┼─► P7 (runners) ─► P8 (typeclasses) ─► P9 (tests + migration)
P6 (String) ────────────────► (pstring)  ──┘
```

P1, P4, P5, P6 are independent and parallelizable. P3 blocks on P1+P2.
P7 blocks on P4+P5+P6. P8 blocks on P3+P7. P9 blocks on everything.

---

## Tutorial outline (`parsec-tutorial.md`)

A step-by-step beginner introduction, each step runnable on its own.

### Step 1: What is a parser?

- One paragraph: a parser is a function from text to a structured value
- Contrast with regex: parsers compose, regex does not
- Box: "Parser in Turmeric" -- `(Parser A)` is a value; you build big parsers
  from small ones using ordinary function calls

### Step 2: Your first character parser

```turmeric
(import tur/parsec :refer [pchar run-parser parse-first])

;; Match the letter 'H'
(let [p (pchar #\H)]
  (println (parse-first p "Hello")))  ;; => #\H
```

- Explain `pchar` takes a `Char` literal
- Explain `run-parser` vs `parse-first` (the latter signals failure; the
  former returns all partial parses -- use `parse-first` for simple cases)

### Step 3: Sequencing with `do-m`

```turmeric
(import tur/parsec :refer [pchar do-m parse-first])

;; Match "Hi" and return the pair
(let [p (do-m
          c1 (pchar #\H)
          c2 (pchar #\i)
          (pure (cons c1 (cons c2 0))))]
  (println (parse-first p "Hi there")))  ;; => (#\H #\i)
```

- `do-m` is the monadic `do`-notation: each binding runs the parser and
  binds the result; the final expression is returned via `pure`
- No `bind-parser` / no int threading

### Step 4: Alternatives with `alt-or`

```turmeric
(import tur/parsec :refer [pchar alt-or parse-first])

(let [yes-or-no (alt-or (pchar #\Y) (pchar #\N))]
  (println (parse-first yes-or-no "Yes"))   ;; => #\Y
  (println (parse-first yes-or-no "Nope"))) ;; => #\N
```

- `alt-or` is the `Alternative` method -- no `or-parser` by name
- Full backtracking: if the left branch fails at any point, the right
  branch gets the same input

### Step 5: Repetition with `many` and `many1`

```turmeric
(import tur/parsec :refer [satisfy many many1 parse-first])
(import tur/char   :refer [digit? alpha?])

;; Collect digits
(let [digits (many (satisfy digit?))]
  (println (parse-first digits "123abc")))  ;; => (\1 \2 \3)

;; At-least-one digit (many1 fails on empty)
(let [nat (many1 (satisfy digit?))]
  (println (parse-first nat "42")))  ;; => (\4 \2)
```

- `many` returns `(Parser (List Char))` -- the caller gets a real list, not
  an integer pointer
- `satisfy` takes a predicate on `Char`

### Step 6: `optional` and `Maybe`

```turmeric
(import tur/parsec :refer [pchar optional do-m pure parse-first])
(import tur/maybe  :refer [just nothing just-value just?])

;; Optional leading minus
(let [sign (optional (pchar #\-))]
  (let [r (parse-first sign "-42")]
    (if (just? r)
      (println "has minus")
      (println "no minus"))))
```

- `optional` returns `(Parser (Maybe Char))` -- pattern-match with `just?`
  and `just-value`, not `(= result 0)`

### Step 7: Mapping with `fmap`

```turmeric
(import tur/parsec :refer [many1 satisfy fmap parse-first])
(import tur/char   :refer [digit? char->int])
(import tur/list   :refer [list-foldl])

;; Parse a natural number directly into an int
(let [nat-p (fmap (many1 (satisfy digit?))
                  (fn [chars]
                    (list-foldl chars 0
                      (fn [acc c] (+ (* acc 10) (char->int c))))))]
  (println (parse-first nat-p "42rest")))  ;; => 42
```

- `fmap` is the `Functor` method -- transform parsed values without re-running
  the parser

### Step 8: Putting it together -- a key=value parser

```turmeric
(import tur/parsec :refer [pchar pstring satisfy many1 do-m alt-or
                            run-parser-full])
(import tur/char   :refer [alpha? digit? alnum?])

(defn key-p [] : (Parser String)
  (fmap (many1 (satisfy alpha?)) list->str))

(defn value-p [] : (Parser String)
  (fmap (many1 (satisfy alnum?)) list->str))

(defn pair-p [] : (Parser (Pair String String))
  (do-m
    k (key-p)
    _ (pchar #\=)
    v (value-p)
    (pure (cons k v))))

(defn main [] :int
  (let [r (run-parser-full (pair-p) "width=800")]
    (println (cons-first r))   ;; => "width"
    (println (cons-second r))) ;; => "800"
  0)
```

---

## Guide outline (`parsec-guide.md`)

The reference guide: one section per concept, suitable for readers who
already know what parser combinators are and want to know how this library's
API is shaped.

### §0 Installing / importing

`tur/parsec` is stdlib -- no `build.tur` entry needed. Companion stdlib
modules used in this guide:

| Module | Provides |
|---|---|
| `tur/parsec` | core parsers, combinators, runners |
| `tur/char` | `Char` type, `digit?`, `alpha?`, `char->int`, `char->str` |
| `tur/maybe` | `Maybe A`, `just`, `nothing`, `just?`, `just-value` |
| `tur/list` | `List A`, `list->str`, `list-foldl`, `list-map` |

### §1 Core types

**`(Parser A)`** -- an opaque fat-closure `Input -> List (Pair A Input)`.
All functions in this section return `(Parser A)` for some `A`. The type
parameter tracks what value a successful parse produces.

**`Char`** -- an opaque wrapper around an ASCII/Unicode code point. Character
literals use Scheme-style `#\A` syntax and desugar to `(char-lit 65)`.
Never pass a raw integer where `Char` is expected.

**`Maybe A`** -- `(just v)` for a present value, `(nothing)` for absent.
Produced by `optional`.

**`List A`** -- a cons-list. Produced by `many` and `many1`. Use
`list->str` to convert `(List Char)` to `String`.

### §2 Primitive parsers

| Function | Type | Description |
|---|---|---|
| `(item)` | `(Parser Char)` | Consume one character or fail at EOF |
| `(pchar #\X)` | `(Parser Char)` | Consume character `#\X` exactly |
| `(pstring "hi")` | `(Parser String)` | Consume literal string `"hi"` |
| `(satisfy pred)` | `(Parser Char)` | Consume one char where `(pred c)` is true |
| `(pfail)` | `(Parser A)` | Always fail, no input consumed |

The old `(pchar 65)` integer form is gone. Use `(pchar #\A)` or `(satisfy (= #\A))`.

### §3 Sequencing and binding

Use `do-m` for sequencing. Every bound name gets the *value* the parser
produced, not an `int` pointer:

```turmeric
(do-m
  a <parser-A>
  b <parser-B>
  (pure <expr using a and b>))
```

For "run p then q but keep only q's result", use `then-parser` or the
`*>` operator if the compiler supports it.

`bind-parser` and `then-parser-raw` still exist as implementation details
but should not appear in user code.

### §4 Choice

`(alt-or p q)` -- try `p`; if it fails without consuming input, try `q`.
Full backtracking: both branches always see the same input position.

`(or-parser p q)` is the internal name for the same thing; prefer `alt-or`.

For a list of alternatives: `(foldl alt-or (pfail) parsers)`.

### §5 Repetition

| Function | Returns | Fails when |
|---|---|---|
| `(many p)` | `(Parser (List A))` | never (0 results = empty list) |
| `(many1 p)` | `(Parser (List A))` | `p` does not match at least once |

`many` always succeeds. Do not use the `0`-check pattern from the old API
(`(if (= result 0) ...)`); the new API returns an empty list, which can be
inspected with `(null? results)` or `(list-length results)`.

### §6 Optional values

```turmeric
(optional p) : (Parser (Maybe A))
```

Use `just?` to test, `just-value` to extract. Never check for `0`.

```turmeric
;; Old (do not write):
(let [r (optional (pchar #\-))]
  (if (= r 0) ...))

;; New:
(let [r (optional (pchar #\-))]
  (if (just? r) (just-value r) default-value))
```

### §7 Mapping and transforming

`fmap` is the primary tool for turning parsed text into structured values:

```turmeric
(fmap parser (fn [v] ...transformed value...))
```

Avoid applying a transformation after `parse-first` when it could be applied
inside the parser itself -- keeping transformation inside the parser makes
the `(Parser A)` type more precise and makes the combinator reusable.

### §8 Running parsers

| Function | Returns | Use when |
|---|---|---|
| `(run-parser p s)` | `(List (Pair A String))` | debugging, want all results |
| `(run-parser-full p s)` | `(Maybe A)` | want first full parse, no leftover |
| `(parse-first p s)` | `A` or runtime error | sure the parse will succeed |

`run-parser-full` returns `(nothing)` if no parse consumes all input.
`parse-first` is a convenience that calls `run-parser-full` and crashes with
a descriptive message if it fails -- only use it in `main` or tests where an
unparseable input is a programming error.

### §9 Common patterns

**Parse a natural number:**
```turmeric
(defn nat-p [] : (Parser int)
  (fmap (many1 (satisfy digit?))
        (fn [cs] (list-foldl cs 0 (fn [acc c] (+ (* acc 10) (char->int c)))))))
```

**Skip whitespace:**
```turmeric
(defn spaces [] : (Parser (List Char))
  (many (satisfy space?)))
```

**Token parser (skip trailing whitespace):**
```turmeric
(defn token [p] : (Parser A)
  (do-m v p _ (spaces) (pure v)))
```

**Bracketed parser:**
```turmeric
(defn between [open close p] : (Parser A)
  (do-m _ open v p _ close (pure v)))
```

**Comma-separated list:**
```turmeric
(defn sep-by [p sep] : (Parser (List A))
  (alt-or
    (do-m
      v  p
      vs (many (do-m _ sep x p (pure x)))
      (pure (cons v vs)))
    (pure 0)))  ;; empty list
```

---

## Files to produce

| File | Purpose |
|---|---|
| `docs/guides/parsec-tutorial.md` | Step-by-step beginner walkthrough (§ Steps 1-8 above) |
| `docs/guides/parsec-guide.md` | Full reference guide (§ 0-9 above) |

The tutorial leads with motivation and worked examples; the guide is the
reference a user tabs to when they know *what* they want but need the exact
signature or idiom.

---

## What stays the same

- The backtracking semantics are unchanged -- `alt-or` still tries both
  branches and returns all successes.
- `run-parser` still returns all partial parses (not just the first full
  parse); the return type changes to `List (Pair A String)` but the
  backtracking behavior is identical.
- Internal workers (`mzero`, `mreturn`, `mplus`, `mbind`, `apply-parser`,
  `pair-new`, etc.) remain available for advanced use but do not appear in
  the user-facing guide.

---

## Open questions

1. **Char literal syntax** -- settled as `#\A` and implemented, reading as an
   `:int`. Whether it should read as a `Char` is the open decision at the top.

2. **`list->str` location** -- stdlib `tur/list`, `tur/char`, or `tur/parsec`?
   The guide imports it from `tur/list` for now; relocate if needed.

3. **`parse-first` error message** -- should it include the failed input text?
   Recommended: yes (`"parse failed at: <input>"`) so test output is actionable.

4. **`sep-by` return on empty** -- the snippet above returns `(pure 0)` (raw
   null list) to avoid a circular dep on `tur/list` in the parsec guide;
   once `List` is a proper stdlib type the empty case should use `(pure (nil))`
   or whatever the empty-list constructor is.

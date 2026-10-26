---
title: Strings Guide
category: Language Basics
description: The three string tiers -- borrowed `cstr`, borrowed `str` view, and owned refcounted `String` -- which to reach for, `#s"..."` literals, zero-copy `StringSlice`, the lifetime rules, and the dangling-pointer pitfalls
tags:
  - strings
  - ownership
---

# Strings in Turmeric -- `cstr` vs `str` vs `String`

Turmeric has three string-shaped types. They are not interchangeable, and the
difference that matters is **ownership**: who is responsible for the bytes, and
how long they stay valid.

## The three tiers

| Type | Owns bytes? | Has length? | Typeclasses | Reach for it when ... |
|---|---|---|---|---|
| `cstr` | no (borrowed `const char*`) | no (NUL-terminated) | `Eq`, `Show` | string literals, FFI boundaries, static text |
| `str` | no (borrowed pointer+len view) | yes | `Eq` | a zero-copy sub-view over a buffer you already own |
| `String` | **yes** (owned, immutable, refcounted) | yes | `Eq`, `Ord`, `Show`, `Hash`, `Clone`, `MapKey` | a string that must **outlive its source** |

### `cstr` -- borrowed literal / FFI

`cstr` is a bare `const char*`: no stored length, no ownership. It is the type
of every `"..."` literal and the right type at an FFI boundary. `Eq[cstr]` is a
content `strcmp` (and `Show[cstr]` copies the bytes into a fresh owned
`String`), so at the scalar level a `cstr` already behaves like a value -- but
it **borrows**.

Note that the `=` **operator** has no `cstr` overload: `(= a b)` on two `cstr`
values is a `TUR-E0006` operator-lookup error, not a pointer compare. Content
equality is `(eq? a b)` through the auto-loaded `Eq[cstr]` instance, or
`(cstr-eq? a b)` from `stdlib/cstr.tur` for the direct call. The moment you store a
`cstr` somewhere that outlives its buffer, or use a *computed* `cstr` as a
Map/Set key, you have a latent dangling pointer.

### `str` -- borrowed view

`str` (`stdlib/str.tur`) is a borrowed pointer+length view: a zero-copy window
onto bytes some other buffer owns. Useful for substring-without-copy over a
buffer whose lifetime you already control. Still borrowed, still not a safe
owning key.

### `String` -- owned, immutable, refcounted

`String` (`stdlib/string.tur`) **owns** its bytes. It is a refcounted immutable
heap payload (`{ rc; len; bytes[len+1] }`, NUL-terminated), so:

- `Clone[String]` is an O(1) retain (refcount++), and structural sharing in a
  persistent Map/Set is free.
- It is immutable, so there is no torn-read hazard and every transform returns a
  fresh `String`.
- As a `MapKey`, `mk-owned? = 1`: the map **copies and owns** the key bytes, so a
  `String` key never dangles the way a computed `cstr` key does.

## The decision, in one line

> Use `cstr` for a literal or an FFI call. Use `String` the moment the string
> must outlive the thing that produced it -- a returned value, a stored field, or
> a collection key. Use `str` only for a genuine zero-copy sub-view.

Literals stay `cstr` (that is exactly right for a literal). Do **not** do a
big-bang `cstr -> String` sweep: most `cstr` sites are borrowed literals where
`cstr` is correct and `String` would just add a copy.

## Why `String` keys don't dangle

```turmeric
(load "stdlib/string.tur")

(defn make-key [] : String                 ;; bytes on the heap, not a literal
  (let [b (builder/new)]
    (do (builder/push-cstr! b "al")
        (builder/push-cstr! b "pha")
        (builder/finish b))))

(defn demo [] : int
  (let [k (make-key)
        m (map-assoc (:: (map-new) (Map String int)) k 42)]
    (do
      (string/release k)                    ;; drop the SOURCE key
      (let [probe (string/from-cstr "alpha")] ;; independent, equal key
        (do
          (println (map-get m probe))       ;; => 42  (map owns its own copy)
          (string/release probe)
          (map-free m)                      ;; frees the owned key box once
          0)))))
```

The same program with a *computed* `cstr` key would read freed memory after the
source was dropped. `MapKey[String]` boxes the bytes into a key the map owns and
frees exactly once (the WKC2 owned-key path), which is the whole point of the
type.

## API summary

Construct / convert:

- `string/from-cstr` -- copy a `cstr` into a fresh owned `String`.
- `string/to-cstr` -- borrow the NUL-terminated payload (O(1); valid only while
  the `String` is retained).

Refcount:

- `string/retain` / `string/release` -- share / drop an owning handle.

Query:

- `string/len`, `string/empty?`, `string/byte-at`, `string/eq?`,
  `string/compare`, `string/hash`, `string/starts-with?`, `string/ends-with?`,
  `string/contains?`.
- `string/byte-is?` and `string/char-at` -- bounds-checked byte access for a
  scanner. `(string/byte-is? s i #\,)` is false, never an error, when `i` is out
  of range, so it replaces a length test plus a magic number such as `44`;
  `string/char-at` gives `(some c)` or `(none)`. Both read one byte, so compare
  them with ASCII characters. There are matching `slice/` versions, and
  `cstr-byte-is?` / `cstr-char-at` in `stdlib/cstr.tur`. A `cstr` stores no
  length, so those two measure the string on every call; to scan a long text
  byte by byte, make it a `String` first. `=` does not take a `Char`; compare
  with `(eq? c #\,)`.

Search (none of these allocates; a position is an `(Option int)`, `(none)` when
there is no match, never `-1`):

- `string/index-of`, `string/index-of-from`, `string/last-index-of` -- where a
  `String` needle occurs. The empty needle is found at the start offset (at
  `(string/len s)` for `last-index-of`).
- `string/index-of-char`, `string/index-of-char-from` -- where an ASCII
  character occurs.
- `string/starts-with-at? s prefix at` -- whether `prefix` occurs at offset
  `at`; false, not an error, when `at` is out of range or the prefix would run
  past the end.
- `string/span-while s start pred` -- the index where a run of bytes passing
  `pred` ends, or `(string/len s)`. The `stdlib/char.tur` predicates plug in:
  `(string/span-while line 0 space?)` is the indentation width,
  `(string/span-while line i alnum?)` the end of a name.
- `string/run-length s start c` -- how many `c` in a row start at `start`: a
  fence's backticks, an ATX heading's `#`s.

Each has a `slice/` twin that searches a `StringSlice` for a `String` needle,
within the slice's own bounds. `stdlib/cstr.tur` has `cstr-starts-with?`,
`cstr-ends-with?`, `cstr-index-of`, `cstr-index-of-char` and `cstr-span-while`,
so a literal or a line from `fs/read-lines` can be scanned without a copy.

Split, join and lines:

- `string/split s sep`, `string/split-char s c` -- the parts between
  separators, empty ones included, each a fresh owned `String` in a fresh
  `(Vec String)`.
- `string/lines s` -- the lines, without their endings. A line ends at LF,
  CRLF or CR, and a final ending does not start another line, so `"a\nb\n"`
  is two lines.
- `string/join parts sep` -- borrows the parts and returns a fresh `String`;
  it undoes `string/split`.
- `string/release-all v` -- releases every `String` in `v`, then `v`. A `Vec`
  does not own its elements, so `vec-free` alone would leak them.
- Zero-copy: `string/line-slices s` and `slice/split-char sl c` return
  `(Vec StringSlice)` views instead of copies (each retains its parent), freed
  with `slice/release-all`. A parser can hold one `String` for the whole
  document and one O(1) slice per line.
- `fs/read-string path` reads a whole file into an owned `String`, keeping
  every byte, NUL included. It is in `stdlib/fs-string.tur`, which loads
  `fs.tur` and `string.tur`; `fs.tur` itself stays free of `String`, so a
  program that loads it and defines its own `show` is unaffected.

Transform (each returns a fresh immutable `String` and borrows its arguments):

- `string/concat`, `string/substring`, `string/to-upper`, `string/to-lower`.
- `string/trim`, `string/trim-start`, `string/trim-end` -- strip ASCII
  whitespace (space, tab, LF, VT, FF, CR: `char.tur`'s `space?`) from both
  ends, the front, or the back.
- `string/replace-all s from to` -- a literal replace, left to right, never
  overlapping and never re-scanning `to`. An empty `from` matches nothing, so
  `s` comes back as a copy. For a pattern, use `re/replace-all`.
- `string/repeat s n` -- `s` `n` times; `n` of 0 or less is the empty string.
- `string/pad-start s width c`, `string/pad-end s width c` -- pad with `c` to
  `width` bytes. A longer `s` comes back unchanged, never cut.
- `string/parse-int s` -- `(some n)` when the whole of `s` is a decimal
  integer (an optional sign, then digits), else `(none)`, including when it
  does not fit in 64 bits. Unlike `str->int-checked`, it takes a `String`,
  skips no spaces and never clamps.
- Zero-copy: `slice/trim`, `slice/trim-start` and `slice/trim-end` return a
  sub-slice over the same parent, and `slice/parse-int` parses a slice
  without copying it, such as an ordered list item's start number.

Incremental construction:

- `StringBuilder` (`builder/new`, `builder/push-cstr!`, `builder/push-string!`,
  `builder/push-byte!`, `builder/push-char!`, `builder/len`, `builder/finish`)
  -- accumulate bytes in linear time, then `builder/finish` freezes them into an
  immutable `String`. This is the mutable escape hatch; `String` itself stays
  immutable.
- `builder/push-int! b n` writes `n`'s decimal digits, `builder/push-repeat! b c
  n` writes `n` copies of the character `c`, and `builder/push-slice! b sl`
  (`stdlib/string-slice.tur`) copies a slice's bytes. None of them makes a
  temporary `String`, so there is nothing to release.
- `builder/push! b x` takes any value whose type has a `BuilderPush` instance:
  `cstr`, `String`, `StringSlice` and `Char` append themselves, an `int` its
  digits, a `bool` `true` or `false`. There is no float instance yet.
- `string/of` builds a whole `String` from its arguments in one expression. It
  expands to one builder, a `builder/push!` per argument, left to right, and
  `builder/finish`, so the `String` it returns is the only thing to release:

  ```turmeric
  (let [line (string/of path "\t" depth "\t" (vec-len headings))]
    (do (println (string/to-cstr line))
        (string/release line)))
  ```

  It borrows its arguments. A call that returns a fresh `String`, such as
  `(int->string n)`, would leak there, so pass `n` itself.

Typeclasses: `Eq`, `Ord` (lexicographic), `Show` (the bytes), `Hash` (content),
`Clone` (retain), `MapKey` (owned key). Every op has an interpreter native
override, so `String` behaves identically under `--interpret` / the REPL and
when compiled.

## Owned-String literals -- `#s"..."`

Bare `"..."` stays a `cstr` (borrowed) -- the default literal typing is
deliberately unchanged. When you want an owned `String` literal, write
`#s"..."`. It is **always available**: no `#lang` token, no
`#use-reader-macros`, nothing to declare.

```turmeric
(load "stdlib/string.tur")     ;; the String code (eval-time)

... #s"hello" ...              ;; => (string/from-cstr "hello"), an owned String
```

**That `(load)` is still required, and the reason is fundamental rather than
an oversight.** The two things live in different phases. `#s"..."` is expanded
while the file is being *read*, into a call to `string/from-cstr`; the function
that call names does not exist until `(load ...)` runs at *eval* time. So the
load cannot enable the syntax -- the syntax is already gone by then -- and the
syntax cannot supply the code. You need the one line, and only the one line:
the reader installs the dispatch for every file.

`#s"..."` works identically compiled and under `--interpret`, and an
owned-String literal is safe as a `Map`/`Set` key. See
`tests/fixtures/string-reader-macro`.

The dispatch is delimiter-keyed, so `#s"..."` and the `#s(...)` set literal
are different forms and can appear in the same file.

Two directives used to turn this on, and both are now unnecessary:
`#lang turmeric stringed` (the layer axis, decommissioned -- the token warns
with `TUR-W0064` for one minor line, then becomes an error) and
`#use-reader-macros "stdlib/string-reader.tur"` (that file is now a
comment-only no-op, deleted at age-out). Delete either one where you find it;
what they enabled is now unconditional.

## Indent-aware multi-line literals -- `#dedent"""..."""`

For text -- usually code -- that should be indented to match the surrounding
source without that indent landing in the string. Always available, no `load`
and no `#use-reader-macros`; it reads as an ordinary string literal, so the
result is a static `cstr`.

```turmeric no-check
(println #dedent"""
    int main(void) {
        printf("hi\n");
        return 0;
    }
    """)
```

The body is **verbatim**: no escapes, so `\n`, `{`, `}` and single `"` need
nothing. Rules (Swift's):

- The text starts on the line after the opening `"""`; leading blank lines are
  dropped.
- If the closing `"""` is alone on its line, **its** indentation is stripped
  from every line, and the line break before it is not part of the string.
  Put a blank line before it for a final `\n`. A closer indented less than
  the text leaves the extra indent in the string.
- If the closer follows text on the same line, the first non-blank line sets
  the indent instead.
- A non-blank line indented less than that is an error. Whitespace-only lines
  become empty, `\r` before `\n` is dropped, and indentation is matched byte
  for byte (do not mix tabs and spaces).
- To put `"""` inside the body, add `#`s to both ends: `#dedent#"""..."""#`,
  `#dedent##"""..."""##`, as many as it takes.

`tests/fixtures/dedent-reader` covers each rule and
`tests/fixtures/errors/dedent-*` the diagnostics. It works under
`#lang sweet-exp` too (`tests/fixtures/dedent-reader-sweet`).

## Zero-copy slicing -- `StringSlice` (opt-in)

`string/substring` copies. When you want ranged access *without* copying -- walk
a range, compare sub-ranges, split, tokenize -- use `StringSlice`
(`stdlib/string-slice.tur`): a bounds-checked, refcounted **view** `{ parent
String; offset; len }`.

It is safe in a way a raw pointer+len view over a `cstr` is not: a `StringSlice`
**retains its parent String**, and `String` is immutable, so the viewed bytes
can neither be freed nor mutated underneath the slice.

```turmeric
(load "stdlib/string-slice.tur")

(let [s (string/from-cstr "hello world")
      w (string/slice s 6 5)]        ;; "world" -- no copy
  (string/release s)                 ;; parent kept alive by the slice
  (slice/byte-at w 0)                ;; 119 ('w'); bounds-checked, -1 out of range
  (slice/sub w 0 3)                  ;; "wor" -- O(1), views the same parent
  (slice/to-string w))              ;; materialize an owned String only when needed
```

Surface: `string/slice` / `string/slice-cstr` (construct), `slice/sub` (O(1)
sub-view), `slice/len` / `slice/empty?` / `slice/byte-at` / `slice/byte-is?` /
`slice/char-at` / `slice/compare` / `slice/eq?` / `slice/hash` (query), the
search functions (`slice/index-of`, `slice/span-while` and the rest, as
above), `slice/split-char`, `slice/trim` / `slice/trim-start` /
`slice/trim-end` (O(1) sub-views), `slice/parse-int`, `slice/to-string` /
`slice/to-cstr`
(materialize), `slice/retain` / `slice/release` (share/drop). Typeclasses `Eq`,
`Ord`, `Show`, `Hash`. Compiled and `--interpret` behave identically.

- **cstr with an owned copy:** `string/slice-cstr` copies the `cstr` into a
  String the slice solely owns, giving safe ranged access to `cstr` data without
  tracking the original's lifetime.
- A slice is not NUL-terminated at its end, so there is deliberately no
  *borrowed* cstr view; `slice/to-cstr` always returns a fresh (owned) cstr.
- This supersedes the inert `str` view for the safe/owned case. `str`
  (`stdlib/str.tur`) remains for zero-ownership interop over a caller-managed
  buffer.

## Unicode -- `stdlib/unicode.tur` (opt-in)

A `String` is bytes, and everything above treats it that way: `len` counts
bytes, `byte-at` reads one, and the `char.tur` predicates (`space?`,
`punct?`, `alpha?`) know only ASCII. When text past ASCII matters,
`(load "stdlib/unicode.tur")` adds the Unicode side. A `Char` there holds a
whole code point, not a byte.

- `utf8/decode-at s i` -- `(some (pair c next))`, the code point starting at
  byte `i` and the index just past it, or `(none)` when `i` is out of range.
- `utf8/decode-before s i` -- `(some (pair c start))`, the code point ending
  just before `i` and the index of its first byte: the way to look left, at
  the character before a delimiter, say.
- `utf8/push-char! b c` -- appends the UTF-8 encoding of `c`, one to four
  bytes; a surrogate or a value past U+10FFFF appends U+FFFD instead.
- Invalid UTF-8 decodes as U+FFFD rather than failing. An overlong form, a
  surrogate, a stray continuation byte or a sequence cut short each gives one
  replacement character, covering the bytes up to where the sequence went
  wrong, so a scan that follows the returned index always moves forward.
- `unicode/space? c` and `unicode/punct? c` -- CommonMark's definitions:
  - whitespace is the Zs category plus tab, LF, FF and CR;
  - punctuation is the P and S categories, which take in every ASCII
    punctuation character as well as quotation marks, dashes, currency
    signs and arrows.
- `unicode/fold-case s` -- full Unicode case folding, as a fresh `String`.
  The result can be longer than `s`: `"STRASSE"` and `"Stra\xc3\x9fe"` both
  fold to `"strasse"`. Folding both sides and comparing the bytes is a
  case-insensitive match. The fold stops at a NUL byte.

For Markdown's `&name;` references, `stdlib/markdown/entities.tur` adds
`markdown/entity-lookup`: the name without `&` and `;` (`"copy"`) gives
`(some s)`, a fresh `String` of the character or characters it stands for.
The table is the HTML Standard's 2125 names that end in `;`, generated by
`tools/gen-html-entities.py` and pinned by a digest.

The tables are the ones `#lang r7rs`'s `(scheme char)` uses, generated by
`tools/gen-r7rs-unicode.py` into `stdlib/r7rs/unicode.tur` from the Unicode
Character Database that `tools/fetch-ucd.sh` pins (Unicode 16.0.0). The
interpreter registers natives over the same generated C, so compiled code,
the JIT and `--interpret` agree.

## Lifetimes -- the rules

- A freshly constructed `String` (`from-cstr`, `concat`, `substring`, `to-*`,
  `trim`, `replace-all`, `builder/finish`) has refcount 1 and is owned by its creator. Release
  it with `string/release`, or hand ownership to a container.
- `string/retain` hands out a second owning handle; balance every `retain` with a
  `release`.
- `string/to-cstr` returns a **borrow** into the payload -- do not use it after
  the `String` is released.
- A `String` used as a Map/Set key is copied into a key box the collection owns;
  the collection frees it on `map-free` / `set-free`. Releasing your own handle
  afterward is independent and safe.

## Memory management & common pitfalls

The one question that resolves most string bugs: **who owns these bytes, and who
frees them?** Each type answers it differently.

| Value | Who owns the bytes | Your obligation |
|---|---|---|
| `"literal"` (`cstr`) | static / the compiler | none -- never free it |
| a `cstr` returned by a stdlib fn ("caller frees the result": `str-concat`, `int->str`, `cstr-sub`, `path/*`, `digest/*-hex`, `json/encode`, `slice/to-cstr`, ...) | **you** (fresh heap buffer) | free it, or hand it to `string/adopt-cstr` |
| a `cstr` from an accessor (`httpd-req-*`, `json/get-string`, `sym->str`, `string/to-cstr`) | the underlying structure | **borrow only** -- don't free, don't outlive the owner |
| `String` | refcount | balance each `from-*`/`concat`/`retain` with a `string/release` (or hand it to a container) |
| `StringSlice` | refcount + retained parent | balance each `string/slice`/`slice/sub`/`slice/retain` with a `slice/release` |

### Copy vs consume vs borrow -- the three cstr->String bridges

- `string/from-cstr c` -- **copies** `c` into a new String; does **not** free `c`.
  Safe for any cstr (literal, borrow, or heap).
- `string/adopt-cstr c` -- **consumes** `c`: copies its bytes AND `free`s `c`.
  Use it *only* on a freshly heap-allocated cstr you own (typically the result of
  a "caller frees" fn): `(string/adopt-cstr (path/join a b))`.
- `string/to-cstr s` / `slice/to-cstr sl` -- go the other way. `string/to-cstr`
  **borrows** (O(1), valid only while `s` is retained). `slice/to-cstr`
  **copies** into a fresh heap cstr (you free it) -- a slice is not
  NUL-terminated at its end, so it can't lend a borrow.

### Owned builders: wrap vs build

`stdlib/str-build-string` ships owned-String siblings for the two foundational
cstr builders -- `str-concat-string` (over `str-concat`) and `cstr-sub-string`
(over `cstr-sub`) -- each a one-line `string/adopt-cstr` wrapper. There is a
real fork in how to reach for them:

- **Single join / one substring -- wrap.** `(str-concat-string a b)`,
  `(cstr-sub-string s i j)`. Minimal, obviously correct, one allocation. This is
  the common case.
- **Multi-join accumulation -- build, don't fold.** A formatter that
  concatenates more than twice (a csv row, a json object, `re/union`,
  `re/replace-all`) must **not** be written by nesting `str-concat-string`.
  `str-concat` allocates a fresh throwaway cstr per join, so folding the wrapper
  inherits an O(n^2) allocation profile -- adopting the result only launders the
  leak, it doesn't remove the intermediate churn. Build into a `StringBuilder`
  instead (`builder/new` -> `builder/push-cstr!` / `builder/push-string!` ->
  `builder/finish`): linear time, a single allocation of the final buffer. The
  `String` stays immutable; the builder is the sanctioned mutable escape hatch
  for accumulation.

### Mixed fresh/static returns -- choose per branch

Some accessors/formatters return a **fresh malloc on one branch and a static
string literal on another** -- e.g. `httpd-req-cookie` (malloc on a hit, static
`""` when the cookie is absent) or `bound->str` (malloc for Inclusive/Exclusive,
static `"unbounded"` for Unbounded). A blind `string/adopt-cstr` over the whole
return is **undefined behavior** the moment the static branch fires: `adopt`
frees its argument, and freeing a string literal is UB. The rule:

- **`adopt` the malloc'd branches, `from-cstr` the literal branches** -- never
  blind-`adopt` the whole thing. `bound->str-string` (in
  `stdlib/range-bound-string`) does exactly this: `adopt` the fresh
  Inclusive/Exclusive renders, `string/from-cstr "unbounded"` (copy, no free)
  for Unbounded.
- **Pick the return type by whether absence is meaningful.** When every branch
  has a valid rendering (a *formatter*), return a total `String` -- that is
  `bound->str-string`. When a branch means "not there" (an *accessor*), return
  `option<String>` -- that is `httpd-req-cookie-opt` / `httpd-req-form-opt` (in
  `stdlib/httpd-string`), which answer `none` on every miss and `some` owned
  bytes on a hit. `option` also fixes a sentinel ambiguity the `cstr` form
  can't: "present but empty" is `some ""`, distinct from "absent" (`none`),
  where the old `""` return conflated the two.

### The pitfalls

- **`adopt-cstr` on a literal or a borrow -> undefined behavior.** It frees its
  argument. `(string/adopt-cstr "hi")` frees static memory;
  `(string/adopt-cstr (httpd-req-path c))` frees a pointer the connection still
  owns. When unsure, use `string/from-cstr` (copy, no free).
- **Use-after-release of a `to-cstr` borrow.** `(let [c (string/to-cstr s)] ...
  (string/release s) ... c ...)` reads freed memory. Finish with the borrow
  before releasing the String, or copy it.
- **Leaking a "caller frees" cstr.** `(println (str-concat a b))` leaks the
  joined buffer every call. Adopt it (`(string/adopt-cstr (str-concat a b))` ->
  owned String) or free it. (This is the latent hazard the stdlib adoption audit
  tracks: `docs/archive/string-adoption-stdlib-plan.md`.)
- **A computed `cstr` as a Map/Set key dangles.** `MapKey[cstr]` borrows the
  pointer (`mk-owned? = 0`); if the key was computed or is later freed, the map
  holds a dangling pointer. Use a `String` key -- `MapKey[String]` copies the
  bytes into a box the map owns (`mk-owned? = 1`). See "Why String keys don't
  dangle" above.
- **Double-release / forgotten release.** Releasing a String or slice twice frees
  it twice; never releasing it leaks. One `release` per owning handle
  (`from-*`/`concat`/`slice`/`retain`), no more, no fewer.
- **A `StringSlice` keeps its whole parent alive.** A 3-byte slice of a 10 MB
  String pins all 10 MB until the slice is released. For a small long-lived
  substring of a large transient String, `slice/to-string` (copy out) and release
  the slice + parent.
- **StringBuilder is single-use.** `builder/finish` frees the builder; don't
  touch it afterward.

### The short rules

- Every `string/from-cstr`, `string/concat`, `string/substring`, `string/to-*`,
  `string/trim*`, `string/replace-all`, `string/repeat`, `string/pad-*`,
  `string/of`, `builder/finish`, `slice/to-string` returns an **owned** String
  (rc 1) -- release it or give it away.
- Every `string/slice`, `string/slice-cstr`, `slice/sub`, `slice/trim*`,
  `slice/retain` returns an owned slice -- release it.
- Anything named `*-cstr` that *returns* a cstr gives you a **fresh** buffer to
  free/adopt; anything named `to-cstr` that borrows is valid only for its
  source's lifetime.
- Prefer `String` (copy on store) over a stored/keyed `cstr`. Prefer
  `string/from-cstr` (copy) over `string/adopt-cstr` (consume) unless you
  specifically own a fresh heap cstr.

## See also

- `stdlib/string.tur` -- the module.
- `stdlib/string-slice.tur` -- `StringSlice`, the zero-copy view.
- `stdlib/str-build-string.tur` -- owned-String builders (`str-concat-string`,
  `cstr-sub-string`); the wrap-vs-build fork above.
- `stdlib/httpd-string.tur` -- optional owned-String accessors
  (`httpd-req-cookie-opt`, `httpd-req-form-opt`).
- `stdlib/range-bound-string.tur` -- `bound->str-string`, the per-branch
  adopt/from-cstr formatter.
- `src/runtime/tur_string.c` -- the refcounted payload + operations.
- `docs/archive/owned-string-type-plan.md` -- the design plan.
- `docs/archive/string-adoption-stdlib-plan.md` -- the borrowed-`cstr`
  migration audit.
- `docs/archive/string-owned-builders-and-optional-accessors-plan.md` --
  the owned-builders / optional-accessors design (this section).

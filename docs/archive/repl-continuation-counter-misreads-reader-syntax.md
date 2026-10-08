# The REPL's multi-line counter misreads inline-C fences (and other reader syntax)

**RESOLVED 2026-09-30**, the day after it was filed. Pinned by
`tests/turi/repl-multiline-input.sh` (ctest `tur_repl_multiline_input`):
24 checks, every repro below plus the blank-line and end-of-input cases.
Against the pre-fix binary, 16 of the 24 fail.

## Resolution

The REPL no longer counts brackets line by line. After each line it calls
`reader_open_depth` (`src/compiler/reader.c`, declared in `reader.h`) on the
**whole** accumulated input. That function lexes the input the way the
reader does:

- strings across lines, with escapes;
- ```` ```c ```` fences, opaque up to the next ```` ``` ````, as
  `read_cblock` reads them;
- nested `#| |#` comments;
- `;` comments, and `#;` datum-comment prefixes in every dialect;
- `#\c` literals, and Scheme `|symbols|`.

The last two go through `sweet_lexeme_end`, the helper the sweet-exp
preprocessor already uses for the same lexemes, so the two scanners share
their rules rather than diverging again.

Two behaviours came with it, both from the same investigation:

- **A blank line inside a string, fence or block comment is content.** A
  blank line between two C statements used to print `(cancelled)`, and the
  rest of the body was then read as Lisp. `reader_open_depth` reports "input
  ends inside a lexeme" separately (`*in_lexeme`). The REPL then keeps the
  blank line, in s-expression and sweet-exp continuation alike. A blank line
  with only brackets open still cancels, as before.
- **End of input in the middle of a form prints `(cancelled)`.** With piped
  input, an unfinished form used to vanish without a word.

Why not the report's preferred direction, asking the reader itself: the
reader has 38 separate end-of-input ("unterminated ...") error sites and no
shared end-of-input signal. Classifying its failures as "incomplete" versus
"malformed" would have been a second approximation of its own. The scanner
lives beside the sweet preprocessor's and reuses its helpers, which was the
point of that direction.

Still not modelled: brackets inside a user reader macro
(`#use-reader-macros`) count as brackets.

## The report as filed

**Severity: medium.** A valid multi-line form either never reaches the
evaluator or is cut off partway through, depending on what its lines contain.
With piped input nothing is reported and the process exits 0. Interactively
the `..` prompt keeps absorbing lines until a blank line prints `(cancelled)`,
so a form containing, for example, a C `for` loop cannot be entered at the
prompt at all. Every form below compiles and runs from a file.

Filed 2026-09-29 while investigating
[aot-compiled-repl-plan](../upcoming/aot-compiled-repl-plan.md), whose
headline feature (inline-C at the prompt) depends on this. Measured on `main`
at `c6ba4162`, Release `-DTUR_JIT=ON`, `TUR_NO_AUTO_SPICE=1 tur repl`.

## Repro

Under-count: the form never evaluates, and the lines after it are absorbed.

````text
(defn c-mix [a : int b : int] : int
  ```c
  int64_t t = a;
  for (int i = 0; i < 3; i++) t = t * 31 + b;
  return t;
  ```)
(c-mix 3 4)
(+ 2 3)
````

Piped into `tur repl`, this prints nothing after the banner and exits 0. Not
even `(+ 2 3)` is evaluated. Driven through a pty, every line from the second
onward gets a `..` prompt, `(c-mix 3 4)` is absorbed into the pending form,
and the first blank line prints `(cancelled)`. `tur jit` on the same `defn` in
a file prints `93345`.

Over-count: the form is evaluated early, partway through the fence.

````text
(defn is-close [c : int] : bool
  ```c
  if (c == ')') return 1;
  return 0;
  ```)
(is-close 41)
````

The REPL evaluates after the `if` line, then treats the remaining lines as
separate forms. The result is four bogus diagnostics: `unterminated C code
block (missing ```)`, `unbound symbol 'return'`, `expected 'c' for C code
block`, and `unknown function or operator 'is-close'`. From a file, `tur jit`
prints `true`.

The same root cause also swallows all later input on these valid forms:

| Input | From a file |
| --- | --- |
| `(println "a` / `(b")` (a string spanning lines) | prints `a` / `(b` |
| `(+ 1 #\| ( \|# 2)` (block comment) | prints `3` |
| `#lang r7rs`, then `(display #\()` (char literal) | prints `(` |

## Root cause

`paren_balance` (`src/turi/repl.c:208`) decides whether the accumulated input
is complete. It is a per-line approximation of the reader that knows two
things: `"` strings and `;` line comments. Its result is summed across lines
(`balance += paren_balance(line)`, `src/turi/repl.c:1732`), and the form is
evaluated when the sum reaches `<= 0` (`src/turi/repl.c:1739`). It does not
know about:

- **```` ```c ```` fences.** The fence body is C. Its `(`, `)`, `{`, `}` are
  counted as Lisp brackets, and a C statement's `;` ends the scan of the line.
  So `for (int i = 0;` counts `+1` and never sees the `)`. In the other
  direction, `')'` counts a close paren with no matching open.
- **Strings that span lines.** `in_str` is not carried from one line to the
  next, so the second line is scanned as if it started outside a string.
- **`#| ... |#` block comments and `#\(` / `#\)` char literals.** Their
  brackets are counted.

The real reader (`src/compiler/reader.c`, which is where "unterminated C code
block" is reported at `:2873`) handles all of these. The counter is a second
implementation of the same question, and it has drifted from the first.

## Fix directions

- **Preferred: ask the reader.** Add a reader entry point that says whether
  a buffer is *incomplete*, i.e. it ends inside an open list, string, fence,
  or block comment, as opposed to *malformed*. It should run with
  diagnostics suppressed and respect the session's current `reader_type`
  (sweet, r7rs, curly-infix). The REPL loop would call it on the accumulated
  `multi` buffer instead of summing `paren_balance`. Only incomplete input
  continues; malformed input is evaluated so the real diagnostic prints.
  This removes the second implementation, so it cannot drift again.
- **Smaller: make the counter stateful across lines.** Carry `in_str`,
  `in_fence`, and a block-comment depth between lines. Treat everything
  between an opening ```` ```c ```` line and its closing ```` ``` ```` as
  opaque. Skip the character after `#\`. This is cheaper but keeps two
  readers.
- Either way, add `tests/turi/repl-multiline-input.sh` with every repro above,
  piped. Assert on the evaluated output, not just the exit code, because
  the failure exits 0.

Sweet-exp continuation (`in_sweet_form`, `src/turi/repl.c:1720`) ends on a
blank line and bypasses the counter. It is not affected by this bug, but
a reader-based check should keep that blank-line rule.

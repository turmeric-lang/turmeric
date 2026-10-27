# Datum Comments (`#;`) -- Implementation Plan (DC0–DC3)

> **Status:** Not started.
>
> **Prerequisites:** None. The reader is self-contained; no type-system or
> elaborator changes are required.
>
> **Last updated:** 2026-05-22

---

## Motivation

Turmeric supports two comment styles today:

- `;` line comments -- ignore text to end of line
- `#| ... |#` block comments -- ignore delimited text (nestable)

Neither lets you comment out a single syntactic form without selecting exactly
the right span of characters. Datum comments, borrowed from Racket and
standardised in SRFI-110, fill that gap:

```turmeric
; Before:
; (println (+ 1 2))   ← must manually select the whole form

; After:
#;(println (+ 1 2))   ← one token comments out the whole expression
```

This is particularly useful during development for toggling a single argument,
a branch, or a debug print without disturbing surrounding forms.

---

## Semantics

`#;` is a **reader-level** construct. It reads one complete datum (the next
form in the input stream) and discards it. The surrounding expression sees
nothing -- no AST node is produced.

| Input | Visible to compiler | Notes |
|-------|---------------------|-------|
| `#;1` | _(nothing)_ | scalar datum |
| `#;(foo bar baz)` | _(nothing)_ | whole list |
| `#;[1 2 3]` | _(nothing)_ | whole vector |
| `1 #;2 3` | `1`, `3` | middle element elided |
| `#;#;1 2 3` | `3` | outer `#;` discards the result of inner `#;1 2`, which is `2` |

### Nested `#;` semantics

`#;#;1 2 3` is equivalent to "comment out two consecutive forms":

1. Outer `#;` reads one datum. The next thing is `#;1 2`, which the reader
   processes as: discard `1`, return `2`. So the outer `#;` discards `2`.
2. `3` remains.

This matches Racket's behaviour and SRFI-110 section 2.1.

### `#;` at end of input

`#;` with no following datum is a reader error:

```
error: datum comment #; requires a following form, got end of input
```

### `#;` followed by a close delimiter

`(foo #;)` is also a reader error (the `)` is not a datum):

```
error: datum comment #; requires a following form, got ')'
```

---

## Architecture

The entire change is confined to `src/compiler/reader.c`. No other files need
modification for DC0–DC2.

```
src/compiler/reader.c   -- add #; dispatch in read_form (DC1)
tests/fixtures/         -- datum-comment-* and errors/datum-comment-* (DC0)
docs/upcoming/          -- this file
```

The formatter (`src/compiler/fmt.c`) is addressed in DC3.

---

## Phase DC0 -- Specification and fixtures

**Goal:** Write runnable fixtures that document the expected behaviour before
touching the reader. All fixtures in this phase are expected to fail until DC1.

### Fixtures (expected to fail until DC1)

- [ ] `tests/fixtures/datum-comment-basic/input.tur` -- `#;` on a scalar,
  a list, and a vector; surrounding forms still execute correctly.

  ```turmeric
  (println #;99 "ok")
  (println #;"ignored" "ok")
  (println #;(this is discarded) "ok")
  ```

  `expected.stdout`:
  ```
  ok
  ok
  ok
  ```

- [ ] `tests/fixtures/datum-comment-inline/input.tur` -- datum comment
  elides one element inside a list.

  ```turmeric
  (println (+ 1 #;999 2))
  ```

  `expected.stdout`:
  ```
  3
  ```

- [ ] `tests/fixtures/datum-comment-nested/input.tur` -- `#;#;1 2 3`
  leaves only `3`.

  ```turmeric
  (println #;#;1 2 3)
  ```

  `expected.stdout`:
  ```
  3
  ```

- [ ] `tests/fixtures/datum-comment-multiline/input.tur` -- `#;` spanning
  a multi-line form.

  ```turmeric
  (println #;(this
              spans
              lines) "ok")
  ```

  `expected.stdout`:
  ```
  ok
  ```

### Error fixtures (expected to fail until DC1)

- [ ] `tests/fixtures/errors/datum-comment-eof/input.tur` -- `#;` at end
  of file.

  ```turmeric
  (println "before")
  #;
  ```

  `expected.diag`:
  ```
  datum comment #; requires a following form, got end of input
  ```

- [ ] `tests/fixtures/errors/datum-comment-close/input.tur` -- `#;`
  immediately before a close delimiter.

  ```turmeric
  (println #;)
  ```

  `expected.diag`:
  ```
  datum comment #; requires a following form, got ')'
  ```

**Exit criterion:** Fixtures exist; all currently fail (reader produces an
"unexpected character" error on `#;` or proceeds incorrectly).

---

## Phase DC1 -- Reader implementation

**Goal:** Add `#;` dispatch to `read_form` in `src/compiler/reader.c`.

### Where to insert

In `read_form` (line ~1248), the existing `#`-dispatch block is:

```c
if (c == '#' && peek2(r) == '{') { return read_map(r); }
if (c == '#' && peek2(r) == 's' && peek3(r) == '(') { return read_set(r); }
if (c == '#' && peek2(r) == '[') { return read_attribute(r); }
if (c == '#' && peek2(r) == '?') { return read_reader_cond(r); }
```

Add before these (or after; `#;` is unambiguous since no other `#;`-prefixed
form exists):

```c
if (c == '#' && peek2(r) == ';') {
    Span s = span_point(r);
    advance(r); advance(r);          /* consume '#' and ';' */
    Form *discarded = read_form(r);  /* read one datum and throw it away */
    if (r->error) return NULL;
    if (!discarded) {
        diag_emit(DIAG_ERROR, s,
                  "datum comment #; requires a following form, "
                  "got end of input");
        r->error = true;
        return NULL;
    }
    return read_form(r);             /* return the next real datum */
}
```

### How the close-delimiter error is handled

When `read_form` is called to read the discarded datum and encounters `)`,
`]`, or `}`, it already emits an "unexpected `<char>`" error and sets
`r->error = true`. The subsequent `if (r->error) return NULL` check propagates
this correctly, so no extra handling is needed for DC1.

If a more specific message is desired for the close-delimiter case, it can be
added in DC2.

### Tasks

- [ ] Insert the `#;` check in `read_form`.
- [ ] `datum-comment-basic`, `datum-comment-inline`, `datum-comment-nested`,
  and `datum-comment-multiline` fixtures all pass.
- [ ] `datum-comment-eof` error fixture passes.
- [ ] All existing tests continue to pass.

**Exit criterion:** All DC0 fixtures pass (except the close-delimiter error
fixture, which may emit the generic "unexpected ')'" message until DC2).

---

## Phase DC2 -- Diagnostics polish

**Goal:** Improve the error message for `#;` followed by a close delimiter,
and add any remaining edge-case fixtures.

### Improved close-delimiter message

After `advance(r); advance(r);` (consuming `#;`), check whether the very next
non-whitespace character is a close delimiter before calling `read_form`:

```c
skip_ws_and_comments(r);
int next = peek(r);
if (next == ')' || next == ']' || next == '}') {
    diag_emit(DIAG_ERROR, s,
              "datum comment #; requires a following form, got '%c'",
              (char)next);
    r->error = true;
    return NULL;
}
```

This must come **after** `skip_ws_and_comments` so `#; )` (with whitespace)
gives the same clear message as `#;)`.

### Additional edge-case fixtures

- [ ] `tests/fixtures/datum-comment-top-level/input.tur` -- `#;` commenting
  out an entire top-level `defn`.

  ```turmeric
  #;(defn unused [] : int 42)
  (println "ok")
  ```

  `expected.stdout`:
  ```
  ok
  ```

- [ ] `tests/fixtures/errors/datum-comment-close/input.tur` -- update
  `expected.diag` to match the improved message from DC2.

### Tasks

- [ ] Add pre-call close-delimiter check with specific message.
- [ ] `datum-comment-close` error fixture passes with the precise message.
- [ ] `datum-comment-top-level` fixture passes.

**Exit criterion:** All DC0–DC2 fixtures pass; no regressions.

---

## Phase DC3 -- Formatter preservation (optional)

**Goal:** Teach `fmt.c` to round-trip `#;datum` sequences rather than
silently dropping them.

### Background

The formatter re-scans source text in the gap between consecutive form spans
to extract and re-emit `;` and `#| |#` comments
(`emit_comments_in_gap`, `fmt.c` line ~775). Because `read_form` discards the
datum at read time, a `#;datum` sequence ends up in a gap between the
preceding form and the next real form.

`emit_comments_in_gap` currently breaks on any `#` not followed by `|`
(line ~812: "anything else is part of a form -- stop"). A `#;datum` in a gap
is silently dropped.

### Approach

Extend `emit_comments_in_gap` to recognise and re-emit `#;datum` sequences:

1. Detect `#` followed by `;`.
2. Scan forward to find the end of the datum. Since the gap scanner does not
   have access to the full reader, a lightweight bracket-counter scan suffices
   for the common cases:
   - Atom (no brackets): scan to the next whitespace or close delimiter.
   - List/vector/map (open bracket): advance past the matching close, counting
     nesting depth.
   - String: scan past the closing `"`, handling `\"` escapes.
3. Re-emit the `#;datum` text verbatim.

This is a best-effort scan -- it does not need to handle every edge case
(e.g., `#;` inside a string is already handled because string scanning
skips `\;`). If the scan fails or the gap contains a `#;` adjacent to a
raw `#;` datum, fall back to dropping it and emitting a plain `; datum
comment stripped by formatter` note.

### Tasks

- [ ] Add `#;datum` branch to `emit_comments_in_gap`.
- [ ] Fixture: `tests/fixtures/fmt-datum-comment/input.tur` -- formatted
  output preserves `#;datum` in expected positions.
- [ ] Verify that the formatter does not duplicate the datum in the output.

**Exit criterion:** `fmt -` round-trips files containing `#;datum` sequences
without dropping them.

---

## Open Questions

1. **`#;` inside quasiquote** -- `\`(foo #;bar baz)` should expand to
   `(foo baz)`. Since `#;` operates at read time (before quasiquote
   expansion), this works automatically. No special handling needed.

2. **`#;` in the formatter's measure pass** -- `fmt_measure` and
   `fmt_form_flat` operate on the form tree, so they never see discarded
   datums. No interaction.

3. **`#;` in the REPL** -- the web REPL uses the same reader, so `#;`
   will work there automatically once DC1 is complete.

4. **`#;` in docstrings** -- `;;;` doc blocks are parsed before the reader
   runs (they are `;` comments). A `#;` immediately before a `defn` will
   not appear as a docstring; the `;;;` scanning is not affected.

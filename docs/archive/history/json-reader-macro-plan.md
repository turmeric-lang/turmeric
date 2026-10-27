# `#json{...}` Reader Macro -- Plan (JR0--JR5)

> **Status:** Scoped down -- superseded for the mixed-value case by data
> literals ([data-literals-plan.md](data-literals-plan.md), landed through DL3).
> `#map{...}` / `#set{...}` / `[...]` already construct collections whose slot
> values are arbitrary Turmeric expressions, which covers the "literal shape,
> computed values" need a JSON-only reader cannot. This plan is now narrowed to
> the **JSON-source-paste only** use case: validating and embedding a verbatim
> JSON blob copy-pasted from an external source. If that narrower need does not
> materialize, the feature can be dropped in favor of writing the data literal
> directly.
>
> **Phase status (2026-05-31):**
> - **JR0** -- Done (commit `be5ae9ba`, PR #145). Reader sub-parser in
>   `src/compiler/reader.c` dispatches `#json(...)`; emits `tur/json`
>   tagged-node constructors rather than the originally planned
>   `hamt-of` / `vec-of` (see the "Implemented design" note below).
> - **JR1** -- Obsolete. The `hamt-of` / `vec-of` lowering was abandoned
>   during JR0; no separate builtins were added. Skip.
> - **JR2** -- Done. `#json<Type>(...)` type-hint syntax is parsed by the
>   reader (`reader.c:1347`); the hint is currently stored but ignored
>   during elaboration, as planned.
> - **JR3** -- Done. All five fixtures exist under
>   `tests/fixtures/json-reader-{object,array,nested,null,escape}/`.
> - **JR4** -- Done. `docs/guides/json-guide.md` covers the reader macro
>   alongside the runtime `tur/json` API.
> - **JR5** -- Done as part of PR #145 (merged green; snapshots regenerated
>   in the same change).
>
> **Flag:** `-Xjson-reader` (opt-in; no effect on programs that don't use the
> macro). All phases gated behind this flag so the feature can land
> incrementally.
>
> **Last updated:** 2026-05-31

---

## Motivation

Pasting JSON into Turmeric today requires either a runtime `json-parse` call on
a string literal (no compile-time validation, verbose) or hand-translating the
JSON into nested map/vec constructor calls (tedious, error-prone). A
`#json{...}` reader macro solves both problems: the JSON is validated at compile
time, the result is a first-class Turmeric value (a HAMT map, a vec, or a
primitive), and the source is copy-pasteable from any JSON source.

Primary use cases:

- Embedding fixture data and test payloads directly in source.
- Defining default configs that happen to be JSON-shaped.
- Calling APIs that accept `ptr<void>` map arguments without constructing the
  map manually.

---

## Design

### Delimiter choice

`#json{...}` uses `{` / `}` as the outer fence. Because JSON objects also use
`{}`, a bare object becomes `#json{{...}}` -- the outer pair is the macro fence,
the inner pair is the JSON object. This is unambiguous but visually noisy.

Two alternative outer delimiters avoid the double-brace:

| Syntax | Top-level object | Top-level array |
|---|---|---|
| `#json{...}` | `#json{{"a": 1}}` | `#json{[1, 2]}` |
| `#json[...]` | `#json[{"a": 1}]` | `#json[[1, 2]]` |
| `#json(...)` | `#json({"a": 1})` | `#json([1, 2])` |

**Recommendation: `#json(...)`.** Round parens never appear as structural
characters in JSON, so `#json({"a": 1})` reads cleanly and `#json([1, 2])` has
no doubled delimiter. The reader already dispatches on `#` + next-char, so
adding `#json(` is a single new dispatch branch.

### What the reader produces

> **Implemented design (supersedes the table below).** The original plan
> lowered objects/arrays to `hamt-of` / `vec-of`. That was abandoned during
> implementation: `hamt-of` / `vec-of` are **monomorphic `:int` collections**
> (one carrier type for all elements -- see the `vec-of` docstring), so any
> heterogeneous JSON -- a string value, a bool, a float, a `null`, or mixed
> element types -- fails to typecheck. `null` only "worked" because a bare `0`
> slips through as an int, which conflated null with zero.
>
> Instead the reader emits **`tur/json` tagged-node constructors**, the same
> node tree `json/decode` builds at runtime:
>
> | JSON | Emitted |
> |---|---|
> | `{"a": 1}` | `(json/object-put (json/object-new) "a" (json/int 1))` |
> | `[1, 2, 3]` | `(json/array-push (json/array-push (json/array-new) (json/int 1)) ...)` |
> | `"hello"` | `(json/string "hello")` |
> | `42` | `(json/int 42)` |
> | `3.14` | `(json/float 3.14)` |
> | `true` / `false` | `(json/bool 1)` / `(json/bool 0)` |
> | `null` | `(json/null)` -- a real null node (`json/type` == 0) |
>
> Every node is a uniform `:int` handle, so heterogeneous and nested JSON
> compose; the type variation lives in each node's runtime tag (`json/type` +
> `json/get-*`). `json.tur` is auto-loaded only under `-Xjson-reader`. (Fixing
> `json.tur` to compile required removing the `static` keyword from the nested
> helper functions in `json/encode` / `json/decode` / `json/free` -- a latent
> bug, as no fixture had ever compiled that module.)

The original (abandoned) S-expression table:

| JSON | Turmeric equivalent |
|---|---|
| `{"a": 1, "b": true}` | `(hamt-of "a" 1 "b" true)` |
| `[1, 2, 3]` | `(vec-of 1 2 3)` |
| `"hello"` | `"hello"` (`:cstr` literal) |
| `42` | `42` (`:int` literal) |
| `3.14` | `3.14` (`:float` literal) |
| `true` / `false` | `1` / `0` (`:bool` literal) |
| `null` | `(nil-value)` |

The emitted S-expression is elaborated by the normal typechecker, so types are
checked and values participate in type inference as usual.

### Brace / string tracking in the reader

The reader sub-parser for `#json(...)` must handle:

1. **Paren depth**: track `(` / `)` depth; exit at depth 0.
2. **String mode**: on `"`, consume until the matching unescaped `"`,
   treating `\"` as an escape. Depth counting is suppressed inside strings.
3. No other context is needed -- `[`, `]`, `{`, `}` inside JSON are passed
   through as-is since the JSON sub-parser handles them.

This is ~60 lines of C added to `src/compiler/reader.c`.

---

## Phases

### JR0 -- Reader sub-parser

Implement the JSON-to-S-expression reader in `src/compiler/reader.c`:

- Dispatch `#json(` in the `#`-dispatch table.
- Recursive descent: `json_read_value`, `json_read_object`, `json_read_array`,
  `json_read_string`, `json_read_number`.
- Emit `hamt-of`, `vec-of`, string/int/float/bool literals, `nil-value`.
- Parse errors (malformed JSON, unexpected EOF) become `TUR-E0270` reader
  errors with line/col pointing into the `#json(` block.

### JR1 -- `hamt-of` and `vec-of` builtins (or macro expansion)

`hamt-of` and `vec-of` need to exist as stdlib macros or compiler builtins that
construct a HAMT map / growable vec from a flat key-value / element list.

Check whether `stdlib/hamt.tur` already exposes a multi-pair constructor; if
not, add:

```turmeric
;;; hamt-of -- construct a HAMT map from alternating key/value pairs.
(defmacro hamt-of [& pairs] ...)

;;; vec-of -- construct a Vec from a sequence of elements.
(defmacro vec-of [& elems] ...)
```

### JR2 -- Type annotation on the macro

Allow an optional type hint on the outer form for future use with typed
decoding (see Future Work below):

```turmeric
#json<MyStruct>({"name": "alice", "age": 42})
```

In this phase the hint is parsed and stored but ignored during elaboration
(the result is still a `ptr<void>` HAMT). This reserves the syntax.

### JR3 -- Fixture: `tests/fixtures/json-reader-*`

Add test fixtures:

- `json-reader-object` -- `#json({"a": 1, "b": "hello"})` round-trips correctly
- `json-reader-array` -- `#json([1, 2, 3])` produces a vec
- `json-reader-nested` -- nested objects and arrays
- `json-reader-null` -- `null` → `nil-value`
- `json-reader-escape` -- `"he said \"hi\""` string escapes

### JR4 -- Docstring and guide

- `;;;` docstring on the `#json(...)` form description in `stdlib/json.tur`
  (or a new file `stdlib/json-reader.tur`).
- `docs/guides/json-guide.md` covering the reader macro, runtime `json-parse`,
  and the interplay with `tur/hamt`.

### JR5 -- `just docs` + snapshot audit

Regenerate all codegen snapshots affected by `hamt-of` / `vec-of` macro
expansion, run `bash tests/run.sh`, confirm zero `FAIL` lines.

---

## Error codes

| Code | Condition |
|---|---|
| `TUR-E0270` | Malformed JSON inside a `#json(...)` block |
| `TUR-E0271` | Unexpected EOF inside a `#json(...)` block |

---

## Non-goals for JR0--JR5

- **Runtime JSON parsing** -- a `json-parse` stdlib function that parses a
  `cstr` at runtime is a separate feature. This plan is compile-time only.
- **Serialization** -- writing Turmeric values back to JSON strings is out of
  scope.
- **WASM/streaming** -- JSON is validated once at compile time; no streaming
  parser is needed.

---

## Future work: typed JSON decoding (option #3)

The `#json<Type>(...)` syntax reserved in JR2 is the entry point for a more
ambitious extension: instead of producing a `ptr<void>` HAMT, the reader
macro validates the JSON structure against a named `defstruct` (or a
schema from a `tur/schema` library) and emits a typed struct constructor call.

For example:

```turmeric
(defstruct Point [x : float y : float])

(def p #json<Point>({"x": 1.5, "y": 2.0}))
;; elaborates to: (Point 1.5 2.0)
;; type: Point
;; compile-time error if field is missing or wrong type
```

The open questions for this extension:

1. **Field-name mapping** -- JSON keys are strings; struct fields are symbols.
   Needs a convention (exact match, snake_case ↔ camelCase, or explicit
   `#[json-name "camelField"]` annotation).
2. **Optional and nullable fields** -- `null` must map to an `Option` type or
   a sentinel value; the struct definition needs to declare which fields are
   optional.
3. **Arrays to typed vecs** -- `[1, 2, 3]` inside a typed context should
   produce `Vec<int>` rather than `Vec<any>`.
4. **Interplay with `tur/schema`** -- if a schema library is added (see
   [IDEAS.md](../upcoming/IDEAS.md)), typed decoding could delegate validation
   to the schema instead of reading field types directly from `defstruct`.
   The `#json<MySchema>(...)` form would then call `schema-decode` at compile
   time.

This is non-trivial but achievable. It requires the elaborator to walk the JSON
S-expression tree against the struct's field type list, which is a small
bidirectional type-checking pass. Estimate: ~500 lines in `elab_call.c` or a
new `elab_json.c`, after JR0--JR5 are complete.

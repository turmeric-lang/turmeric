# Defmacro Type-Annotated Params Plan

## Scope and non-scope

In scope:
- A decision on what `(defmacro name [param :Type] body)` should
  do. Today it is a hard parse error
  (`src/compiler/elab_macros.c:1011`), and the diagnostic
  ("defmacro: parameter must be a symbol, got keyword") doesn't tell
  the user how to fix it.
- Whichever direction we pick, fix `stdlib/future.tur:132` --
  currently the only site in the whole codebase that hits this
  (`(defmacro promise-pair [cell :ptr<void>] ...)`) -- so the file
  loads cleanly again.

Out of scope:
- General macro hygiene improvements (gensyms, capture rules).
  Orthogonal.
- A typed-macro system in the Scala-3 / Racket sense, where the
  macro's expansion is checked against a contract. Way bigger than
  the papercut this plan is responding to.
- Reader-level keyword handling. The lexer is fine; this is purely
  about how `elab_defmacro` interprets the parameter list.

## Current state

`elab_defmacro` (`src/compiler/elab_macros.c:1003`) walks the param
list looking for either:

- a bare `F_SYM` -- the parameter name, or
- the `&` symbol marker, after which exactly one more `F_SYM` is the
  rest parameter.

Anything else is rejected at parse time:

```
defmacro: parameter must be a symbol, got keyword
```

This contrasts with `defn`, which accepts (and requires, for
type-checked params) `[name :Type]` pairs. Users coming from `defn`
muscle memory can reach for the same syntax in a `defmacro` body and
get a terse error.

Survey: across `stdlib/` and `tests/` there is exactly **one** site
using type-annotated macro params -- `stdlib/future.tur:132`:

```turmeric
(defmacro promise-pair [cell : ptr<void>]
  (list (promise-of-cell cell) (future-of-cell cell)))
```

This is almost certainly a copy-paste from a nearby `defn`. The
annotation is meaningless at the macro layer (the parameter binds a
`Form`, not a `:ptr<void>` runtime value) and was never enforced; the
file simply fails to load today.

No spice, no test, no example depends on this file loading -- it's
dead library code -- but the error message is bad and the inconsistency
between `defn` and `defmacro` is real.

## Decision

**Settled: Option A.** Keep the strict parser, fix the one offending
site in `stdlib/future.tur`, and improve the diagnostic to point users
at the right fix. The other options (silently discard the annotation,
or grow a Form-tag check pass) are documented below for posterity but
are not pursued by this plan.

## Design options (considered, not adopted except A)

### Option A -- fix the one site; improve the diagnostic (CHOSEN)

Keep the strict parser. Update the error to point users at the right
fix:

```
defmacro: parameter '%s' has a type annotation, but macro parameters
bind Forms (not runtime values) and cannot be typed.  Use
'(defmacro name [%s] body)' (remove the annotation), or '(defn name
[%s :T] body)' if you meant a runtime function.
```

Then remove the annotation from `stdlib/future.tur:132`.

Pros:
- Tiny diff. No grammar change. Honest about the semantics
  (macro params bind syntax, not values).
- Forces the writer to think about whether they wanted a macro at
  all -- the annotation is a signal they may have wanted `defn`.

Cons:
- The friction returns the next time someone copy-pastes from `defn`.
  (Survey says: zero times in the existing codebase, so this risk is
  low.)

### Option B -- silently accept and discard the annotation

Treat `[name :Type]` in a `defmacro` param list as equivalent to
`[name]`. The keyword is parsed and dropped.

Pros:
- One-line change in `elab_defmacro`. No new error path.
- Aligns the surface of `defmacro` and `defn` so muscle memory works.

Cons:
- The annotation becomes a syntactic lie -- it appears to say
  something, but the compiler ignores it. A reader sees
  `(defmacro foo [x :int] ...)` and reasonably expects `x` to be an
  `int`-valued runtime parameter, which it isn't.
- Encourages exactly the confusion (`defmacro` vs `defn`) that the
  current strict parser surfaces.

### Option C -- give annotations a meaning (Form-tag hint)

Allow `[name :Form]`, `[name :Symbol]`, `[name :Int]`, etc., where the
keyword names a Form tag (`F_SYM`, `F_NUM`, `F_LIST`, ...) the
expansion site must satisfy. The check fires at macro-call time with
a real error if the argument's tag doesn't match.

Pros:
- The annotation becomes meaningful. Authors of complex macros get
  better error messages at call sites.
- Brings macros closer to the rest of the language without faking
  runtime types.

Cons:
- Substantial scope: need a keyword-to-Form-tag table, a new check
  pass at macro expansion, a way to express "any Form", and error
  messages that don't make readers more confused. (`:Form` vs the
  unrelated runtime `Form` struct, etc.)
- Cost is wildly out of proportion to the one site that triggers
  the current papercut. Worth a plan of its own if real demand
  shows up.

## Why Option A (rationale recap)

The papercut is a one-line stdlib fix and a slightly nicer error
message. The codebase has exactly one offending site and zero tests
that exercise the case. Options B and C both pay a real complexity
cost (a silent annotation lie, or a whole new Form-tag-check
machinery) for a problem that nobody outside `stdlib/future.tur` has
hit.

If, after Option A lands, the new error fires repeatedly in real
user code (spices, examples, anywhere with `defmacro`), reopen this
plan with a tally of where the friction lives and reconsider
Option B.

## Implementation steps (Option A)

### Prerequisites -- unblock `stdlib/future.tur`

The whole file currently fails to load on the `promise-pair`
defmacro. These steps land first, on their own, so the rest of the
plan can land behind a green tree.

P1. **Confirm the failure surface.** Run
    `./build/tur check stdlib/future.tur` and capture the error.
    Expected: a single `defmacro: parameter must be a symbol, got
    keyword` error at `stdlib/future.tur:132`. If any *other* errors
    appear, stop and triage -- they are pre-existing rot per the
    Risks section and need their own treatment before this plan can
    claim "future.tur loads."

P2. **Strip the annotation from `promise-pair`.** Edit
    `stdlib/future.tur:132` from
    `(defmacro promise-pair [cell :ptr<void>]`
    to
    `(defmacro promise-pair [cell]`.
    The body (`(list (promise-of-cell cell) (future-of-cell cell))`)
    is unchanged: it already forwards `cell` to two `defn`s typed as
    `:ptr<void>`, so the runtime contract still holds at expansion
    sites.

P3. **Audit the surrounding docstring (lines 126--131) and the API
    block-comment (line 29).** Neither currently asserts a typed
    parameter contract -- both describe `cell` as "FutureCell
    pointer from `promise-new`", which is a description of the
    *expected runtime value at the call site*, not a macro-param
    type. Leave them as-is. (Listed here so a future reader doesn't
    re-open the question.)

P4. **Sanity-load `stdlib/future.tur`.** Run
    `./build/tur check stdlib/future.tur` again and confirm it
    parses cleanly. If new errors appear that were masked by the
    earlier parse failure, log them as out-of-scope follow-ups (per
    the Risks section) -- do not expand this plan to chase them.

P5. **Grep for `defmacro` + keyword param across the tree** to
    confirm `promise-pair` really is the only site:
    `rg -n 'defmacro\s+\S+\s+\[[^]]*:' stdlib/ tests/ examples/`.
    Expected: zero matches after P2. If matches show up, fix them
    the same way (drop the annotation) before moving on -- the
    diagnostic change in step 1 below will start hard-erroring on
    them as soon as it lands.

### Main work (after prerequisites are green)

1. **Improve the diagnostic in `elab_defmacro`** at
   `src/compiler/elab_macros.c:1014`. When the offending form is an
   `F_KEYWORD`, mention the `defn`-vs-`defmacro` distinction and
   show the corrected param list verbatim. For other tags
   (`F_NUM`, `F_STR`, etc.) keep the generic message. Keep the
   leading `"defmacro: parameter"` substring so any existing
   fixture matching on it continues to match.
2. **Add a negative fixture**
   `tests/fixtures/errors/defmacro-typed-param/` that asserts the
   improved diagnostic substring (something stable like
   `"macro parameters bind Forms"`).
3. **Run the full test suite** (`just test`) to confirm no other
   fixture's expected-error substring regressed on the message
   change.

## Risks and open questions

- **future.tur has more rot.** The file is dead-shipped and may have
  other syntactic stale spots that no test catches. The plan's
  success criterion is just that step 4 stops failing on
  `promise-pair`. Anything else found there belongs in a separate
  cleanup.
- **Error-string churn.** Test fixtures match diagnostic substrings.
  Step 1 should keep enough of the existing wording
  ("defmacro: parameter") that any future fixture asserting on this
  message keeps working; the negative-fixture in step 3 should pin
  the new wording.
- **Repeated copy-paste.** If users routinely copy `[x :T]` from
  `defn` to `defmacro`, Option A alone is friction. The recommendation
  is to ship Option A and watch; revisit with data if it bites.

## Success criteria

Prerequisite gate (must be true before the main work lands):

- `./build/tur check stdlib/future.tur` no longer fails on
  `promise-pair` (any remaining errors are pre-existing and
  documented as out-of-scope follow-ups).
- The P5 grep returns zero hits -- no other `defmacro` in `stdlib/`,
  `tests/`, or `examples/` uses a typed parameter.

Main work:

- `(defmacro foo [x :int] ...)` emits an error that names the
  problem and points the reader at the fix (`defn` vs no
  annotation), while keeping the `"defmacro: parameter"` lead-in
  for fixture compatibility.
- `tests/fixtures/errors/defmacro-typed-param/` passes.
- `just test` is green; no pre-existing fixture's expected-error
  substring regressed on the new wording.
- Every other `defmacro` in `stdlib/` continues to compile
  unchanged.

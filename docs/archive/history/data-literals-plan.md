# Map / Vec / Set Data Literals -- Plan (DL0--DL6)

> **Status:** DL0--DL6 complete (landed). Reader dispatch + `#map{...}` /
> `#set{...}` forms and error codes (DL0); elaboration lowering for all three
> literals plus the `hamt-of` / `set-of` stdlib macros (DL1); sweet-exp
> interaction verified (DL2); happy-path + error fixtures (DL3); docstrings,
> guide, and CLAUDE.md / JSON-reader cross-references (DL4); set-eq? regression
> guard (DL5); regenerated codegen snapshots + docstring table, full suite
> green at 1142 passed / 0 failed (DL6). The DL5 bulk single-shot set
> constructor is deferred -- no profiling justification yet; set-of mirrors
> vec-of/hamt-of's per-element threading.
>
> **Implementation note (DL1):** `#map{...}` keys are normalized in the
> elaborator -- int keys pass through, keyword/string keys lower to
> `(hamt/hash-str "name")` so equal keys hash identically. `#set{...}` uses
> the typed `Set[A]` identity-hash convention (each element is its own hash,
> via the `set-add1` helper); the `(hash x)` typeclass-method injection the
> original design sketched is not used because the `Hash[A]` method does not
> monomorphize in compiled codegen. Consequently the `data-literal-set-no-hash`
> guard fixture from DL3 is omitted (no Hash constraint is enforced).
>
> **Flag:** `-Xdata-literals` (opt-in; no effect on programs that don't use the
> syntax). All phases gated behind this flag so the feature can land
> incrementally.
>
> **Last updated:** 2026-05-31

---

## Motivation

Constructing maps and vecs in Turmeric today is verbose:

```turmeric
(hamt-of "name" name "age" age "active" 1)
(vec-of x y z)
```

For literal-shaped data with mixed literal/computed values, that boilerplate
adds up fast -- fixtures, default configs, test payloads, builder calls all
read worse than they need to. Adjacent languages (Clojure, EDN, JS, Python,
even Ruby) all have literal syntax for their core collections; Turmeric does
not.

A previous plan ([json-reader-macro-plan.md](json-reader-macro-plan.md))
targeted the narrower "paste a JSON blob" use case. But JSON-only fixes
nothing for the more common case: a map/vec with literal *shape* but
some computed *values*. The deeper gap is the lack of evaluating
collection literals.

This plan adds reader syntax that constructs HAMT maps, vecs, and sets
whose slot values are normal Turmeric expressions, evaluated at runtime
in the surrounding scope.

```turmeric
;; Literal shape, computed values -- no quasiquote needed.
(def payload #map{:name name :age (+ age 1) :active 1})
(def points  [(make-point 0 0) (make-point 1 1) origin])
(def tags    #set{:alpha :beta current-tag})
```

If this lands well, the `#json(...)` reader macro becomes a thin
specialization (literal-only slots) of the same machinery, or can be dropped
entirely in favor of writing the map literal directly.

---

## Design

### The three surface forms

| Syntax | Produces |
|---|---|
| `[e1 e2 e3 ...]` *(expression position)* | Vec: `(vec-of e1 e2 e3 ...)` |
| `#map{k1 v1 k2 v2 ...}` | HAMT map: `(hamt-of k1 v1 k2 v2 ...)` |
| `#set{e1 e2 e3 ...}` | Set: `(set-of e1 e2 e3 ...)` |

The vec form reuses the existing `[...]` reader -- `F_VEC` -- and adds
context-sensitive elaboration (see below). The map and set forms are
brand-new `#`-dispatch entries that avoid every existing collision.

### Why `[...]` for vecs (Clojure-style overloading)

Turmeric's reader already parses `[...]` uniformly as `F_VEC` regardless
of context (`reader.c:1982`). Parameter lists in `defn`, binding vectors
in `let`/`loop`, and any other `[...]` produce the same `F_VEC` form
tag. The surrounding special form decides how to interpret it.

This is exactly how Clojure resolves the apparent "param list vs vector
literal" collision: there *is* no collision because `[x y]` is always
the same vector data structure. `defn` consumes its second argument as
a binding spec; `let` consumes its first argument as a binding spec;
everywhere else, `[x y]` is just a vector.

The change here is purely **elaboration**: when an `F_VEC` appears in
expression position (i.e. it wasn't grabbed by a binding-form macro
first), lower it to `(vec-of ...)`. No reader change required.

### Why `#map{...}` for maps and `#set{...}` for sets

The `{...}` slot in expression position is already claimed by
**curly-infix** arithmetic (`{a + b}` -> `(+ a b)`), and `#{...}`
already produces `F_MAP` for effect rows. Both are load-bearing and
should not be touched.

`#map{...}` and `#set{...}` are fresh dispatches with no current
meaning -- the reader sees `#`, reads the tag (`map` or `set`), then
`{`, and reads a delimited sequence. The named-tag spelling pairs the
two literals symmetrically: both collection literals follow the same
`#<tag>{...}` shape, so a reader sees `#map{` and `#set{` and
immediately knows which collection they are getting -- there is no
shorthand-vs-named asymmetry to remember.

The conventional Lisp/Clojure spelling for sets is `#{...}`, but that
slot is already taken by effect rows in Turmeric (`#{Unsafe}`,
`#{IO File}`, etc.). Effect rows were deliberately chosen to *look*
like a set because an effect row *is* a set of effects, so the
original meaning earns its spelling. Rather than migrate effect rows
off `#{...}` (a wide fixture-surface refactor for marginal payoff),
this plan keeps `#{...}` for effect rows and uses `#set{...}` for
set literals.

Alternatives considered and rejected:

- **`##{...}`** -- type-agnostic and one char shorter, but pairs
  awkwardly with `#set{...}` (the only other reserved collection
  literal). Symmetry between the two outweighs the saved character.
- **`#m{...}` / `#hamt{...}` / `#dict{...}`** -- single-letter `#m`
  is cryptic; the longer forms commit to an implementation detail
  (HAMT) or a non-Turmeric vocabulary word (dict). `#map` matches
  the conceptual type name and is the same length as `#set`.
- **Bare `{...}` with parity disambiguation** (even count -> map,
  odd count -> infix) -- clean in the common case, but the 2-element
  edge case (`{- x}` unary infix vs `{:k v}` map) requires a
  first-form check that adds parsing subtlety for marginal payoff.
- **Reuse `#{...}`** -- already `F_MAP` (effect rows). Sharing the
  surface form would require context-sensitive elaboration over a
  type-system-load-bearing construct. Too risky.

### What the reader produces

| Source | Elaborates to |
|---|---|
| `[1 2 3]` *(expr position)* | `(vec-of 1 2 3)` |
| `[]` *(expr position)* | `(vec-of)` |
| `#map{:a 1 :b x}` | `(hamt-of :a 1 :b x)` |
| `#map{}` | `(hamt-of)` |
| `[#map{:k v} #map{:k w}]` | `(vec-of (hamt-of :k v) (hamt-of :k w))` |
| `#set{:a :b :c}` | `(set-of :a :b :c)` |
| `#set{}` | `(set-of)` |
| `#set{x (compute) :literal}` | `(set-of x (compute) :literal)` |

Slots are arbitrary expressions -- variable references, calls, nested
literals, etc. The normal typechecker handles them; the literal is
just sugar over `hamt-of` / `vec-of` / `set-of`.

### "Expression position" for `[...]`

A `[...]` `F_VEC` form is consumed as a **binding spec** (not
elaborated to `vec-of`) when it appears as:

- The parameter list of `defn`, `defmacro`, `fn`, `defmethod`, etc.
- The binding vector of `let`, `loop`, `letfn`, `for`, `doseq`, `when-let`, etc.
- A destructuring pattern inside one of the above.
- The argument list slot of any future macro that opts in via a known
  marker (the elaborator maintains a small allow-list).

In every other position -- function arguments, top-level values,
right-hand sides of `def`, etc. -- `F_VEC` elaborates to `(vec-of ...)`.

The allow-list lives in the elaborator and is the single source of
truth for "what is a binding form." Adding a new binding-form macro
means appending one entry; the rest of the language remains uniform.

### Key forms in `#map{...}`

Keys must be one of:

- A keyword: `:name`, `:age` -- the typical case.
- A string literal: `"name"` -- when interop with `cstr`-keyed maps is needed.
- An int literal: `42` -- for int-keyed maps.

Other forms (variables, calls) as keys are **rejected** in DL0 with a
clear error. Computed keys are rare in practice and allowing them would
make the literal harder to read at a glance ("is `foo` here a key or
a value?"). If a real use case emerges, add a separate form for it
(see Future work) rather than overloading the basic literal.

### Element forms in `#set{...}`

Unlike `#map{...}`, set literals do not restrict element forms -- any
expression that yields a value of the element type is allowed. That
is the same rule as `#map{}` *values* (not keys), since a set element
is conceptually a value, not a label.

The expansion uses the `Hash`/`Eq` typeclasses already required by
`Set[A]` (`stdlib/set.tur`, Phase TC2-C). The `set-of` macro inserts a
`(hash e)` call per element so the surface literal stays free of
hashing boilerplate:

```turmeric
;; Source
#set{x y z}

;; Expansion (conceptual -- see set-of below)
(let [__s (set-new)]
  (set-add __s (hash x) x)
  (set-add __s (hash y) y)
  (set-add __s (hash z) z)
  __s)
```

Because `set-add` returns a *new* set (the underlying HAMT is
persistent), the real expansion threads the result, not the
side-effect form sketched above. See `set-of` in DL1.

The element type is inferred from the first element (same rule as
`vec-of`). Mixed-type sets are rejected by the typechecker.

### Vec element types

`[...]` produces a `vec-of` call with no type annotation; the element
type is inferred from the first element (same as the existing `vec-of`
macro). Mixed-type vecs are rejected by the typechecker, not the reader.

For an explicit element type, write `(vec-of :int 1 2 3)` (current
behavior) or wrap the literal: `(the (Vec :int) [1 2 3])` once that
form exists.

### Reader changes

Three pieces of work:

1. **`#<tag>{...}` dispatch table** in `src/compiler/reader.c`: after
   the initial `#`, read the tag identifier, then require `{`, then
   read a delimited sequence. Implemented as a small table mapping
   tag string -> form constructor so additional tags
   (`#bag{`, `#ordmap{`, ...) drop in without further branches.
2. **Two new `Form` tags** in `src/compiler/forms.h`:
   - `F_MAP_LITERAL` -- payload identical to `F_LIST`, distinguishable
     from `F_MAP` (effect rows).
   - `F_SET_LITERAL` -- payload identical to `F_LIST`.
3. **No vec reader change** -- existing `F_VEC` is reused. All work
   for `[...]` happens in the elaborator.

---

## Phases

### DL0 -- Reader dispatch table + `#map{...}` / `#set{...}`

Wire up the `#<tag>{...}` dispatch table in `src/compiler/reader.c`.
Land both tags in the same phase so the table proves itself as a table
(rather than a hard-coded `map` branch retrofitted later). Introduce
`F_MAP_LITERAL` and `F_SET_LITERAL` in `src/compiler/forms.h`.

- Parse errors (odd `#map{...}` slot count, missing `}` on either
  literal) become `TUR-E0280` / `TUR-E0281` reader errors with line/col
  pointing into the literal.
- Key-form validation for `#map{...}`: keyword / string / int only;
  anything else is `TUR-E0282`.
- `#set{...}` has no key-form rule -- any expression is permitted.
- Unknown `#<tag>{` tag (e.g. `#bag{`) is `TUR-E0283`.

### DL1 -- Elaboration for all three forms

In the elaborator:

- Lower `F_MAP_LITERAL` to a `hamt-of` call.
- Lower `F_SET_LITERAL` to a `set-of` call.
- Lower `F_VEC` to a `vec-of` call **when in expression position**
  (i.e. not consumed by a binding-form macro). Maintain the
  binding-form allow-list described above.

Audit existing macros that take an `F_VEC` argument to confirm none
break. Likely-affected: `defn`, `defmacro`, `defmethod`, `fn`, `let`,
`loop`, `letfn`, `for`, `doseq`, `when-let`, `if-let`, `def-struct`
field lists, `defstruct` field lists, attribute argument lists,
pattern-match clause lists. Each of these grabs the `F_VEC` slot
explicitly today; the elaborator change just needs to skip
expression-position lowering for those slots.

Check whether `hamt-of`, `vec-of`, and `set-of` already accept the
shapes we need:

- `vec-of` exists today (`stdlib/vec.tur:185`) and is variadic; reuse.
- `hamt-of` does **not** yet exist; add it under `stdlib/map.tur` as
  a variadic `defmacro` that threads `(map-set __m k v)` calls (or the
  HAMT-native equivalent) over an `__m` started from `(map-new)`.
- `set-of` does **not** yet exist; add it under `stdlib/set.tur` as a
  variadic `defmacro` that threads `(set-add __s (hash x) x)` over an
  `__s` started from `(set-new)`. Mirrors `vec-of`'s `__v`/`vec-push-each__`
  shape, plus an injected `(hash ...)` per element to satisfy the
  `Hash[A]` constraint the runtime already requires (see
  `stdlib/set.tur:46` `set-add` signature).

Because `set-add` is persistent (returns a new set), the macro must
either:

(a) thread the result through `let`-rebindings, or
(b) introduce a `set-add-each__` helper analogous to `vec-push-each__`
    that mutates the wrapper's internal HAMT pointer in place.

Option (b) parallels the existing `vec-of` shape and is preferred.

### DL2 -- Sweet-exp interaction

Verify the literals work inside `.tur.sweet` files alongside indentation,
neoteric, and `$` rest-of-line. The reader changes happen below the
sweet-exp layer, so this should work transparently -- but add a fixture
to lock in the behavior:

```turmeric
#lang sweet-exp

defn build-payload [name : cstr age : int] : ptr<void>
  #map{:name name :age age :active 1}

defn three-points [] : ptr<void>
  [make-point(0 0) make-point(1 1) origin]

defn watched-tags [extra : Keyword] : ptr<void>
  #set{:audit :auth extra}
```

### DL3 -- Fixtures

Add test fixtures under `tests/fixtures/`:

- `data-literal-map-basic` -- `#map{:a 1 :b 2}` round-trips
- `data-literal-vec-basic` -- `[1 2 3]` in expression position round-trips
- `data-literal-set-basic` -- `#set{1 2 3}` round-trips through
  `set-count` / `set-member?`
- `data-literal-vec-in-defn` -- confirms `[x y]` in `defn`/`let` still
  works as a binding spec (regression guard)
- `data-literal-computed-values` -- `#map{:k (+ 1 2)}` evaluates slot
- `data-literal-set-computed` -- `#set{x (compute) y}` evaluates each slot
- `data-literal-set-dedupe` -- `#set{1 1 2}` collapses to two elements
  (drives the HAMT-merge behavior the literal inherits from `set-add`)
- `data-literal-nested` -- `[#map{:k 1} #map{:k 2}]` and
  `#map{:tags #set{:a :b}}`
- `data-literal-empty` -- `#map{}`, `#set{}`, and `[]` (expression position)
- `data-literal-bad-key` -- `#map{x 1}` produces `TUR-E0282`
- `data-literal-odd-slots` -- `#map{:a 1 :b}` produces `TUR-E0280`
- `data-literal-bad-tag` -- `#bag{1 2}` produces `TUR-E0283`
- `data-literal-set-no-hash` -- `#set{(some-opaque)}` where the element
  type lacks a `Hash` instance produces the usual constraint-resolution
  error (regression guard for the auto-inserted `(hash x)` call)
- `data-literal-sweet-exp` -- works inside `.tur.sweet`

### DL4 -- Docstrings, guide, and JSON-reader relationship

- `;;;` docstrings on `hamt-of`, `vec-of`, and `set-of`; reference the
  literal syntax from each.
- `docs/guides/data-literals-guide.md` covering `#map{...}`, `#set{...}`,
  `[...]` in expression position, key-form rules, type inference notes,
  the `Hash[A]` constraint behind `#set{...}`, and the interplay with
  `#json(...)` (if that plan also lands -- otherwise note that data
  literals supersede it for non-JSON-sourced cases).
- Update [json-reader-macro-plan.md](json-reader-macro-plan.md): either
  mark it as superseded for the mixed-value case, or scope it down to
  "JSON-source-paste only."
- Update `CLAUDE.md` Sweet-Expression Style section to mention the new
  literal forms (since `.tur.sweet` is the most common place they will
  appear).

### DL5 -- Constrained literal helpers for `#set{...}`

Polish pass on `set-of` once it's exercised by fixtures:

- Confirm the auto-inserted `(hash x)` resolves through the existing
  `Hash[A]` typeclass dispatch without surfacing the constraint to the
  caller (the literal should *look* like `#set{1 2 3}`, not require an
  explicit `Hashable` annotation at every call site).
- If `Set[A]` ends up wanting a single-shot bulk constructor for
  efficiency (avoiding N persistent re-allocations), add
  `set-from-cons-list` / `set-from-vec` and have `set-of` lower
  through it. Decide based on profiling, not speculation.
- Confirm `set-eq?` interacts correctly with literal-built sets
  (regression risk: the literal threads results through a wrapper
  helper that could expose a different pointer identity).

### DL6 -- `just docs` + snapshot audit

Regenerate codegen snapshots affected by elaboration changes, run
`bash tests/run.sh`, confirm zero `FAIL` lines.

The biggest regression risk is DL1's binding-form allow-list -- a missed
entry means a binding spec gets wrongly lowered to `(vec-of ...)`,
breaking the consuming macro. A secondary risk is `set-of`'s expansion
allocating an unbounded chain of intermediate sets when DL5's bulk
constructor is not in place; spot-check codegen output for the
`data-literal-set-basic` fixture. The fixture suite plus full snapshot
run should surface any miss.

---

## Error codes

| Code | Condition |
|---|---|
| `TUR-E0280` | Odd number of slot forms in `#map{...}` (unmatched key) |
| `TUR-E0281` | Unexpected EOF inside `#map{...}` or `#set{...}` |
| `TUR-E0282` | Invalid key form in `#map{...}` (must be keyword, string, or int literal) |
| `TUR-E0283` | Unknown `#<tag>{...}` dispatch tag |

---

## Non-goals for DL0--DL6

- **Computed keys** in `#map{...}` -- restricted to literal keys; revisit
  if a real use case emerges.
- **Ordered set literals** -- `#set{...}` lowers to the unordered
  `Set[A]` from `stdlib/set.tur`. An ordered/sorted-set literal would
  need its own type and is out of scope.
- **Multiset / bag literals** -- the `#<tag>{...}` table reserves
  `#bag{...}` for the day a `Bag` type lands, but neither the type nor
  the literal are in this plan.
- **Quasiquote / unquote-splicing** -- a separate, larger feature. Data
  literals cover the common "literal shape, computed values" case
  without needing it.
- **Type annotation on literals** -- e.g. `##<MySchema>{...}`. The JSON
  reader plan reserves analogous syntax (JR2); the same reservation
  can apply here, but is not in this plan.
- **Pattern matching against literals** -- `(match x [#map{:k v} ...])`
  is appealing but lives with the pattern-matching subsystem, not the
  reader.

---

## Future work

### Quasiquote for runtime data

The escape hatch for everything data literals cannot express --
templating ASTs, building maps whose *keys* are computed, generating
code-like structures. The standard Lisp approach is backtick + comma +
comma-at:

```turmeric
`(:event :user-created :payload #map{:id ~user-id :name ~user-name})
```

This is a larger language change (reader rules for unquote, splicing
semantics, interaction with the macro system) and worth a dedicated
plan once a concrete forcing use case appears. Data literals alone
cover the majority of construction ergonomics without requiring it.

### Reader-macro plugin API

If the project grows several literal forms (`#map{`, `#set{`, `#json(`,
`#sql`, `#html`...), it may be worth exposing a registration API
rather than continuing to hand-code branches into `reader.c`. Not
urgent -- the dispatch table is small and a handful more entries is
fine.

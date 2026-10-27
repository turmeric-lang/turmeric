# `tur/schema` -- Runtime Schema Validation Plan (SC0--SC6)

> **Status:** SC0--SC4 and SC6 shipped in `stdlib/schema.tur`
> (scalar/literal/object/array/optional/union/transform/recursive schemas;
> accumulating, dot-path-tagged errors; `schema-decode` /`schema-decode!` and
> the error/result accessors; docstrings + `docs/guides/schema-guide.md`).
> Fixtures: `tests/fixtures/schema-decode-{object,array,optional,union,
> literal,errors,recursive}`. SC5 (`HasSchema` typeclass + generic `decode`
> + the `#json-str<T>` reader-macro family) and SC7 (`Functor`/`Applicative`/
> `Alternative` instances) are **not started** -- they need return-type-directed
> typeclass dispatch and reader-table support, which are compiler-level changes
> beyond the pure-stdlib core. **The compiler work and the remaining SC5/SC7
> stdlib are designed in
> [docs/return-type-dispatch-and-schema-sc5-sc7-plan.md](return-type-dispatch-and-schema-sc5-sc7-plan.md).**
>
> **Implementation note:** decoding is anchored on the tagged JSON node
> representation from `tur/json` rather than a raw HAMT map. JSON nodes carry a
> runtime type tag, which is what makes genuine "expected :cstr, got :int"
> validation possible; a bare HAMT value carries no tag (see the "No runtime
> type tags" design note below). Object schemas are built incrementally
> (`schema/object-new` + `schema/field`) instead of from a nested-vector
> literal, because a field list mixes `:cstr` keys with schema pointers and a
> homogeneous `Vec` cannot hold both.
>
> **Location:** `stdlib/schema.tur` (pure Turmeric over existing stdlib
> primitives; no new C required until SC5).
>
> **Prerequisites:** `tur/result` (Result[A B]), `tur/hamt` (map), `tur/vec`
> (vec), `tur/option` (optional fields), `tur/typeclass` (defclass /
> definstance, for SC5+ typeclass-driven decode). All are already shipped.
>
> **Related:**
> - `docs/upcoming/json-reader-macro-plan.md` -- `#json<Schema>(...)` in JR2
>   delegates typed decoding to `schema-decode` from this library
> - `stdlib/contract.tur` -- contracts verify predicates in static code;
>   schemas validate data at dynamic boundaries (the two complement each other)
> - `stdlib/result.tur`, `stdlib/hamt.tur`, `stdlib/vec.tur`
>
> **Last updated:** 2026-05-31 (SC0--SC4, SC6 implemented)

---

## Motivation

Turmeric's type system is excellent at enforcing invariants *within* a program.
The gap is at **dynamic boundaries**: an HTTP response body, a config file, a
CLI argument map, a channel message from an external process. All of these
arrive as untyped `ptr<void>` values at runtime. Today the only tools are
`contract.tur` (panic on violation) and manual field extraction (no error
propagation, no composability).

A schema library fills this gap with:

1. **Schemas as first-class values** -- a schema is a runtime value you can
   pass to functions, store in a map, and compose with other schemas.
2. **Decode, don't just validate** -- `schema-decode` returns
   `Result<ptr<void>, SchemaError>` so callers can handle bad data gracefully
   instead of panicking.
3. **Structured error messages** -- errors identify the field path
   (`"user.address.zip": expected :cstr, got :int`) rather than crashing with a
   generic assertion failure.
4. **Composability** -- `optional`, `union`, `array-of`, `object`, `transform`
   are schema combinators that build complex schemas from simple ones, matching
   the ergonomics of libraries like Zod (TypeScript) and Serde (Rust).

The design deliberately mirrors `tur/result`'s philosophy: errors are values,
not panics.

---

## API sketch

### Scalar schemas

```turmeric
(import schema)

schema/str       ;; matches :cstr values
schema/int       ;; matches :int values
schema/float     ;; matches :float values
schema/bool      ;; matches :bool values (0 or 1)
schema/nil       ;; matches nil / null

(schema/literal "active")  ;; exact string match
(schema/literal 42)        ;; exact int match
```

### Object schema

```turmeric
(schema/object
  [["name"   schema/str]
   ["age"    schema/int]
   ["email"  (schema/optional schema/str)]])
```

### Array schema

```turmeric
(schema/array schema/int)             ;; vec of ints
(schema/array (schema/object [...]))  ;; vec of objects
```

### Combinators

```turmeric
(schema/optional s)         ;; nil is ok; non-nil must match s
(schema/union  [s1 s2 s3])  ;; first schema that matches wins
(schema/transform s f)      ;; decode with s, then apply f to the result
(schema/intersect [s1 s2])  ;; value must satisfy both schemas
```

### Decoding

```turmeric
;; Decode a HAMT map (e.g. from json-parse or #json(...)) against a schema.
;; Returns Result<ptr<void>, SchemaError>.
(schema-decode my-schema raw-value)

;; Decode or panic (for trusted data / tests).
(schema-decode! my-schema raw-value)
```

### Typed decode via `HasSchema`

Each Turmeric type that has a canonical schema declares a `HasSchema`
instance. The generic `decode` then dispatches on the target type, so
callers do not have to thread a schema value around by hand:

```turmeric
(defstruct User [name : cstr  age : int])

(definstance HasSchema User
  (schema-of [_]
    (schema/object
      [["name" schema/str]
       ["age"  schema/int]])))

;; The elaborator picks the User instance from the binding's type ascription.
(let [u : User (decode raw-map)]
  (println (User-name u)))
```

### Reader-macro family for JSON + schema

Schema decoding is most ergonomic at the JSON boundary, so the
`HasSchema` machinery is paired with a small family of reader macros.
Three forms share the same `<Type>` slot but differ in *when* the JSON
text is known:

| Form | JSON source | Parsed | Schema check |
|---|---|---|---|
| `#json<T>({...})` | literal in source | compile time | compile time (SC5 elaborator) |
| `#json-str<T>(expr)` | runtime `:cstr` expression | runtime | runtime (via `HasSchema T`) |
| `#json-file<T>(path)` | runtime file read | runtime | runtime (via `HasSchema T`) |

The runtime forms desugar to ordinary calls -- no new compiler work
beyond what SC5 already provides:

```turmeric
;; Source
(def u #json-str<User>(body))

;; Desugars to (with the type annotation propagated for HasSchema dispatch)
(def u : User
  (result/unwrap-or-panic
    (decode (json-parse body))))
```

`#json-str<T>(expr)` is the everyday form for HTTP/IPC boundaries:
`body` is a `:cstr` from the network and the macro names the expected
shape inline. A `?`-suffixed variant returns the `Result` instead of
panicking, mirroring `schema-decode` vs. `schema-decode!`:

```turmeric
;; Panics on parse / schema error
(def u #json-str<User>(body))

;; Returns Result<User, Vec<SchemaError>>
(def r #json-str?<User>(body))
(when (err? r)
  (println (schema-error-message (err-val r))))
```

Implementation is two reader-table entries (`#json-str<` and
`#json-str?<`) that consume `<Type>(expr)` and emit a `decode!` /
`decode` call with a type ascription. No JSON sub-parser is needed for
these forms -- the JSON text is opaque to the compiler.

The `#json-file<T>(path)` variant is the same shape but threads
`(io/read-file path)` in front of `json-parse`; it is listed for
completeness and tracked under "Future work" rather than SC5 proper.

### Schema combinator typeclasses

A `Schema` is morally a decoder
(`-> :ptr<void> (Result a (Vec SchemaError))`), so the standard parser-
combinator hierarchy applies. After SC7, schemas compose with the rest of
`stdlib/typeclass.tur`:

```turmeric
(definstance Functor Schema     (fmap [f s]  (schema/transform s f)))
(definstance Alternative Schema (<|> [a b]   (schema/union [a b])))
(definstance Applicative Schema (pure [v]    (schema/always v))
                                (<*> [sf sa] ...))

;; Applicative-style object schema (matches Aeson / Serde / parsec):
(definstance HasSchema User
  (schema-of [_]
    (<$> ->User
         (field "name" schema/str)
         <*> (field "age"  schema/int))))
```

### Schema errors

```turmeric
(defstruct SchemaError
  [path    : cstr    ;; dot-separated field path, e.g. "user.address.zip"
   message : cstr    ;; human-readable: "expected :cstr, got :int"
   value   : ptr<void>])  ;; the offending value
```

### Named schemas and recursive schemas

```turmeric
;; Bind a schema to a name for reuse and self-reference.
(def Address
  (schema/object
    [["street" schema/str]
     ["city"   schema/str]
     ["zip"    schema/str]]))

;; Forward-declared recursive schema (e.g. tree nodes).
(schema/rec [self]
  (schema/object
    [["value"    schema/int]
     ["children" (schema/array self)]]))
```

---

## Design decisions

### Schemas are `ptr<void>` at runtime

A schema value is a heap-allocated tagged struct (discriminant int + payload).
The discriminant identifies the schema kind (scalar, object, array, optional,
union, literal, transform, rec). The payload is kind-specific.

This means schemas compose without any compiler magic -- they're plain Turmeric
values. `schema/object`, `schema/array`, etc. are ordinary `defn`s that allocate
and return schema structs.

### Decode produces `ptr<void>`, not a typed struct

`schema-decode` returns `Result<ptr<void>, SchemaError>`. The decoded value is
a HAMT map, a vec, or a primitive -- the same representation that `#json(...)`
produces. The caller extracts fields with typed accessors.

This is the correct default for SC0--SC4. Typed decode (where the result is a
`defstruct` value with known field types) is the SC5 extension and requires
compiler integration.

### Errors accumulate, not fail-fast

`schema-decode` collects *all* schema violations in the decoded value, not just
the first one. This gives the caller a complete picture of what is wrong -- the
same choice Zod makes with `.safeParse`. The return type is
`Result<ptr<void>, Vec<SchemaError>>` (a vec of errors, not a single error).

### Typed decode is typeclass-driven, not compiler-driven

An earlier draft put `schema-decode-into` in the elaborator: look up the
`defstruct` field list, match keys to fields, emit type-checked extraction
calls. Typeclass dispatch achieves the same thing without new compiler
integration -- each type declares its own `HasSchema` instance, and the
generic `decode` resolves to that instance at the call site. The schema in
the instance can be hand-written, derived from `defstruct` fields by a
helper macro, or generated, and none of those choices require touching the
elaborator. This also keeps the bridge to `#json<T>(...)` clean: the reader
macro desugars to `(decode (json-parse "..."))` and lets typeclass
resolution do the rest.

### Schemas form a parser-combinator hierarchy

`schema/transform`, `schema/union`, and the existing object builder are not
ad-hoc combinators -- they are `fmap`, `<|>`, and an applicative `<*>` in
disguise. Making the instances explicit (SC7) lets schemas reuse the operator
notation in `stdlib/typeclass.tur` (`<$>`, `<*>`, `<|>`) and means a user who
already knows the typeclass vocabulary does not have to learn a parallel
schema-specific one. Two trivial schema kinds are added to support this:
`SCHEMA_ALWAYS` (for `pure`) and `SCHEMA_NEVER` (for `empty`).

### No runtime type tags

Turmeric values don't carry runtime type tags (unlike Clojure or Python).
`schema-decode` infers the kind of a `ptr<void>` value from context: HAMT maps
decode against `schema/object`; vecs decode against `schema/array`; primitives
decode against `schema/str` / `schema/int` etc. by reading the int64 bit
pattern. This imposes a constraint: schemas must be anchored at a root kind so
the decoder knows what to expect at the top level.

---

## Phases

### SC0 -- Schema value types and constructors (`stdlib/schema.tur`)

Define the schema discriminant constants and the low-level struct layout.
Implement scalar constructors (`schema/str`, `schema/int`, etc.) and
`schema/literal`. No decoding yet.

```turmeric
;; Schema kind discriminants
(def SCHEMA_STR      0)
(def SCHEMA_INT      1)
(def SCHEMA_FLOAT    2)
(def SCHEMA_BOOL     3)
(def SCHEMA_NIL      4)
(def SCHEMA_LITERAL  5)
(def SCHEMA_OBJECT   6)
(def SCHEMA_ARRAY    7)
(def SCHEMA_OPTIONAL 8)
(def SCHEMA_UNION    9)
(def SCHEMA_TRANSFORM 10)
(def SCHEMA_REC      11)
```

### SC1 -- `schema-decode` for scalars and objects

Implement `schema-decode` for the four scalar kinds + `schema/object`.
Return `Result<ptr<void>, Vec<SchemaError>>`. Test with HAMT maps produced
by `hamt/new` + `hamt/set`.

Fixture: `tests/fixtures/schema-decode-object/` -- round-trip a simple
two-field object schema.

### SC2 -- Array, optional, union, literal

Extend `schema-decode` to handle `schema/array`, `schema/optional`,
`schema/union`, and `schema/literal`. Add `schema/transform`.

Fixtures:
- `schema-decode-array` -- array of ints
- `schema-decode-optional` -- optional field present and absent
- `schema-decode-union` -- two-variant union
- `schema-decode-literal` -- exact match pass and fail

### SC3 -- Error path accumulation and formatting

Implement the dot-path tracker so errors report `"user.address.zip"` rather
than a bare message. Add `schema-error-message` helper that formats a
`Vec<SchemaError>` into a human-readable string.

Fixture: `schema-decode-errors` -- nested schema with multiple violations;
assert the error paths are correct.

### SC4 -- Recursive schemas (`schema/rec`)

Implement `schema/rec` using a thunk / lazy cell so schemas can refer to
themselves. The thunk is forced on first decode.

Fixture: `schema-decode-recursive` -- a tree schema, three levels deep.

### SC5 -- `HasSchema` typeclass and generic `decode`

Define the `HasSchema` typeclass and the `decode` / `decode!` generics that
dispatch on the result type:

```turmeric
(defclass HasSchema a
  [schema-of : (-> :unit Schema)])

(defn decode [raw : ptr<void>] : (Result a (Vec SchemaError))
  where (HasSchema a)
  (schema-decode (schema-of (the a)) raw))

(defn decode! [raw : ptr<void>] : a
  where (HasSchema a)
  (schema-decode! (schema-of (the a)) raw))
```

Each consumer declares the canonical schema for its own type via
`definstance HasSchema T`. Typeclass dispatch replaces the elaborator work
proposed in earlier drafts -- no new compiler integration is required, since
the instance lookup mechanism already does "find the right schema for this
type."

**Bridge to `#json<T>(...)` and `#json-str<T>(expr)`:** After SC5, the
JR2 compile-time form `#json<User>({"name": "alice"})` desugars to
`(decode (json-parse "..."))`, with typeclass resolution finding the
right `schema-of` at the call site. The runtime sibling
`#json-str<User>(body)` -- where `body` is a `:cstr` expression -- uses
the same desugaring against a value not known until run time, and is
the form that HTTP handlers, IPC consumers, and CLI front-ends should
reach for. Both rely on the same `HasSchema User` instance; the only
difference is whether the JSON text is a literal or a variable.
Custom field name mapping (the old `#[json-name]` annotation idea) is
just whatever the instance author writes inside `schema-of` -- there is
no special elaborator path for it.

Fixtures:
- `schema-decode-typed-user` -- a `defstruct` with a `HasSchema` instance;
  assert `decode` returns a fully-typed struct value.
- `schema-decode-typed-missing-instance` -- attempting to `decode` a type
  with no `HasSchema` instance is a compile-time error from the
  typeclass dispatcher (not a runtime panic).
- `schema-decode-json-str-runtime` -- `#json-str<User>(body)` where
  `body` is bound at runtime to a valid JSON string; round-trips to a
  fully-typed `User` value.
- `schema-decode-json-str-result` -- `#json-str?<User>(bad-body)`
  returns an `err` carrying a `Vec<SchemaError>` with the expected
  field path, no panic.

### SC6 -- `just docs` + guide

- `;;;` docstrings on every exported defn in `stdlib/schema.tur`.
- `docs/guides/schema-guide.md` covering the decode workflow, error handling,
  the `HasSchema` instance pattern, and the connection to `#json(...)` and
  `defstruct`.
- `just docs` regeneration.

### SC7 -- Schema combinator instances (`Functor`, `Applicative`, `Alternative`)

Add the two trivial schema kinds required by the laws -- `SCHEMA_ALWAYS`
(succeeds with a fixed value, supports `pure`) and `SCHEMA_NEVER` (always
fails, supports `empty`) -- then write the instances:

```turmeric
(definstance Functor Schema
  (fmap [f s]
    (schema/transform s f)))

(definstance Applicative Schema
  (pure [v]
    (schema/always v))
  (<*> [sf sa]
    ;; Validation semantics: ALWAYS run both arms against the same input,
    ;; concatenate error vectors left-to-right. Only when both arms succeed
    ;; do we apply the decoded function to the decoded argument.
    (schema/ap sf sa)))

(definstance Alternative Schema
  (<|> [s1 s2]
    (schema/union [s1 s2]))
  (empty []
    schema/never))
```

After SC7, object schemas can be written applicatively. Add a `field`
helper that lifts a single map key into a schema for the whole object so
the applicative chain works as expected:

```turmeric
;; field : cstr -> Schema -> Schema
;; (field "name" schema/str) is a Schema that decodes any HAMT map and
;; extracts the "name" field as a :cstr.
(definstance HasSchema User
  (schema-of [_]
    (<$> ->User
         (field "name" schema/str)
         <*> (field "age"  schema/int))))
```

Fixtures:
- `schema-functor-transform` -- `fmap f s` agrees with `schema/transform s f`.
- `schema-alternative-union` -- `<|>` agrees with `schema/union` for two-arm
  inputs.
- `schema-applicative-user` -- build a `User` schema via `<$>` / `<*>` and
  round-trip a value end-to-end.
- `schema-applicative-error-accumulation` -- decoding `{"name": 42, "age":
  "x"}` produces *two* path-tagged errors, one per field, not just the first.

#### Docstring requirements for SC7 instances

The `;;;` docstrings on `definstance Applicative Schema` and on
`schema/ap` must explain the Validation choice and its consequences, since
a reader who knows the typeclass laws will otherwise expect
`(<*>) = ap`. At minimum cover:

1. **Validation semantics.** `<*>` always evaluates both arms against the
   same input and concatenates errors. This is the same accumulation
   policy as `schema-decode` (see SC3) -- decoding `{"name": 42, "age":
   "x"}` reports both field errors at once, not one per round-trip.
2. **No `Monad Schema` instance, deliberately.** The lawful `>>=` for
   Validation is fail-fast, which would silently contradict the
   accumulating `<*>` via `ap = liftM2 ($)`. Rather than ship an
   instance that violates `(<*>) = ap`, we omit `Monad` entirely.
   Field-chain decoders (applicative) and tagged-union decoders
   (`Alternative`) cover the use cases people actually write.
3. **Escape hatch if monadic decoding is ever needed.** If a future
   schema must decide its shape based on a value it just decoded
   (e.g. read `:kind` then pick a sub-schema), prefer a dedicated
   `schema/dispatch : Schema k -> (k -> Schema a) -> Schema a` combinator
   over a `Monad` instance. If that turns out to be insufficient, the
   intended path is a second, distinct type `SchemaM` that *is* a fail-fast
   monad, defined alongside `Schema` rather than replacing it. This keeps
   the two semantics syntactically distinct so a reader cannot mistake one
   for the other.
4. **Performance note.** Validation is O(arms) per decode regardless of
   where errors fall. This is the intended trade -- error quality over
   microseconds at the validation boundary.

The same notes belong on the eventual `docs/guides/schema-guide.md`
section that introduces applicative-style object schemas (SC6).

---

## Non-goals for SC0--SC6

- **Serialization** -- turning a Turmeric struct back into JSON or a HAMT map.
  Useful, but a separate concern; add as SC7 if wanted.
- **Schema introspection / reflection** -- iterating a schema's fields
  programmatically to generate code or docs. The `schema/rec` thunk design
  does not preclude it, but it is not planned here.
- **SMT-backed static proof** -- `tur/contract` + `tur/refinements` handle
  static checking. Schemas are purely a runtime tool.
- **`schema/coerce`** -- auto-coercing `"42"` to `42` etc. Explicit
  `schema/transform` covers this if needed; implicit coercion is surprising.

---

## Relation to `tur/contract`

| | `tur/contract` | `tur/schema` |
|---|---|---|
| When | Compile-time + runtime | Runtime only |
| Data source | Internal (known types) | External (untyped maps) |
| On failure | Panic | `Result` (error value) |
| Composable | No (macros, not values) | Yes (schemas are values) |
| Type-checked | Yes | Partial (SC5+) |

The two are complementary: use `tur/schema` to validate data at the boundary,
then use `tur/contract` to enforce invariants on the decoded typed value inside
the program.

---

## Future work

- **`schema/coerce`** -- explicit coercion schema for lenient parsing
  (e.g. accept `"true"` for a bool field).
- **Schema serialization** -- write a schema value to JSON so it can be
  published as a machine-readable API contract.
- **`schema-gen`** -- generate random values satisfying a schema, for
  property-based testing. Natural companion to `tur/check` (if a property
  testing library is added).
- **OpenAPI / JSON Schema import** -- given a JSON Schema or OpenAPI spec
  (as a `ptr<void>` HAMT), produce a `tur/schema` value. Enables using
  published API schemas directly without hand-translating them.

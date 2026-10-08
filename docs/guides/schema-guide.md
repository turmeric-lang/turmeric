---
title: Runtime Schema Validation
category: Data Structures and Libraries
description: Validate untyped boundary data (HTTP bodies, config, IPC) with composable schema values using stdlib/schema.tur, with accumulating, path-tagged errors.
---

# Runtime Schema Validation with `tur/schema`

> **Scope:** scalar/object/array/optional/union/literal/transform/recursive
> schemas with accumulating path-tagged errors; the `HasSchema` typeclass with
> return-type-directed `decode!` and the `#json-str<T>(...)` / `#json-file<T>(...)` reader macros; a
> combinator layer (`always`/`never`/`ap`/`field-of`/`fmap`/`alt`, Validation
> semantics); and `Functor`/`Applicative`/`Alternative` typeclass instances
> over the phantom `(Schema a)` wrapper -- so object decoders can be assembled
> applicatively with `fmap`/`ap`/`alt-or`, including building a struct
> field-by-field. See "Applicative combinators" and "HKT instances" below.

Turmeric's type system enforces invariants *within* a program. The gap is at
**dynamic boundaries** -- an HTTP response body, a config file, a channel
message -- where data arrives untyped at runtime. `tur/contract` panics on a
violation; manual field extraction gives no error propagation. `tur/schema`
fills the gap with **schemas as first-class values** that **decode** data into
a `Result`, accumulating every violation with a dot-separated field path.

```
                      tur/contract            tur/schema
  When                compile + runtime       runtime only
  Data source         internal (known types)  external (untyped)
  On failure          panic                   Result (error value)
  Composable          no (macros)             yes (schemas are values)
```

## Loading the library

`schema.tur` is not auto-loaded (to keep it out of every program's codegen).
Load it explicitly, alongside `json.tur` -- schema decoding is anchored on the
tagged JSON node representation, because those nodes carry the runtime type tag
(`0=null 1=bool 2=int 3=float 4=string 5=array 6=object`) that makes genuine
"expected `:cstr`, got `:int`" validation possible.

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")
```
```sweet-exp
load("stdlib/json.tur")
load("stdlib/schema.tur")
```

The input to `schema-decode` is a JSON node, from either `json/decode` (runtime
text) or the `#json(...)` reader macro (compile-time literal).

## Scalar and literal schemas

```turmeric
(schema/str)               ; matches JSON strings, decodes to the :cstr
(schema/int)               ; matches JSON integers
(schema/float)             ; matches JSON floats
(schema/bool)              ; matches JSON booleans
(schema/nil)               ; matches JSON null

(schema/literal-str "active")  ; matches only the exact string "active"
(schema/literal-int 42)        ; matches only the exact integer 42
```
```sweet-exp
schema/str()
; matches JSON strings, decodes to the :cstr
schema/int()
; matches JSON integers
schema/float()
; matches JSON floats
schema/bool()
; matches JSON booleans
schema/nil()
; matches JSON null
schema/literal-str("active")
; matches only the exact string "active"
schema/literal-int(42)
; matches only the exact integer 42
```

## Decoding: errors are values

`schema-decode` returns a `Result<ptr<void>, Vec<SchemaError>>`. Inspect it
with the helper accessors:

```turmeric
(let [r (schema-decode (schema/int) (json/decode "42"))]
  (if (schema-decode-ok? r)
    (schema-decode-value r)            ; => 42
    (schema-error-message (schema-decode-errors r))))
```
```sweet-exp
let [r (schema-decode (schema/int) (json/decode "42"))]
  if schema-decode-ok?(r)
    schema-decode-value(r)
    ; => 42
    schema-error-message(schema-decode-errors(r))
```

| Helper | Purpose |
|---|---|
| `schema-decode-ok?`     | did decoding succeed? |
| `schema-decode-value`   | the decoded value (on success) |
| `schema-decode-errors`  | the `Vec` of `SchemaError` (on failure) |
| `schema-decode!`        | decode or **abort** with a formatted message (trusted data / tests) |

### What the decoded value is

The decoded value mirrors the schema shape, so you keep working with ordinary
Turmeric values:

| Schema | Decoded value |
|---|---|
| scalar  | the underlying value (`:cstr` ptr / `:int` / float bits / bool) |
| literal | the matched value |
| object  | the validated JSON object node (read fields with `json/get`) |
| array   | a freshly allocated `Vec` of decoded element values |
| optional / union / transform / rec | the inner decoded value |

## Object schemas

A field list mixes `:cstr` keys with schema pointers, which a homogeneous
`Vec` cannot hold, so object schemas are built incrementally:

```turmeric
(defn user-schema [] : int
  (schema/field
    (schema/field (schema/object-new) "name" (schema/str))
    "age" (schema/int)))

(schema-decode (user-schema) (json/decode "{\"name\": \"alice\", \"age\": 30}"))
;; => ok
```
```sweet-exp
defn user-schema [] :int
  schema/field(schema/field(schema/object-new() "name" schema/str()) "age" schema/int())
schema-decode(user-schema() json/decode("{\"name\": \"alice\", \"age\": 30}"))
;; => ok
```

A field whose schema is `(schema/optional ...)` may be **absent** entirely or
present-but-`null`; any other missing field is a `"missing required field"`
error.

## Arrays, optionals, unions, transforms

```turmeric
(schema/array (schema/int))                        ; [1, 2, 3] -> Vec of ints
(schema/optional (schema/str))                     ; null / absent ok
(schema/union (vec-of (schema/int) (schema/str)))  ; first matching arm wins
(schema/transform (schema/int) (fn [x] (* x 2)))   ; decode, then map
```
```sweet-exp
schema/array(schema/int())
; [1, 2, 3] -> Vec of ints
schema/optional(schema/str())
; null / absent ok
schema/union(vec-of(schema/int() schema/str()))
; first matching arm wins
schema/transform(schema/int() (fn [x] {x * 2}))
; decode, then map
```

Union arms are all schema pointers (`:int`), so a plain `(vec-of ...)` works.
`schema/transform` applies its function only when the inner schema succeeds, so
the function never sees invalid data.

## Errors accumulate, with paths

Decoding does **not** fail fast. Every violation is collected, each tagged with
a dot-separated path (`address.zip`) or an array index (`children[0].value`):

```turmeric
(defn address-schema [] : int
  (schema/field
    (schema/field (schema/object-new) "city" (schema/str))
    "zip" (schema/str)))

(defn user-schema [] : int
  (schema/field
    (schema/field (schema/object-new) "name" (schema/str))
    "address" (address-schema)))

(let [r (schema-decode (user-schema)
          (json/decode
            "{\"name\": 42, \"address\": {\"city\": \"NYC\", \"zip\": 10001}}"))]
  (println (schema-error-message (schema-decode-errors r))))
;; name: expected :cstr, got :int
;; address.zip: expected :cstr, got :int
```
```sweet-exp
defn address-schema [] :int
  schema/field(schema/field(schema/object-new() "city" schema/str()) "zip" schema/str())
defn user-schema [] :int
  schema/field(schema/field(schema/object-new() "name" schema/str()) "address" address-schema())
let [r (schema-decode (user-schema)
          (json/decode
            "{\"name\": 42, \"address\": {\"city\": \"NYC\", \"zip\": 10001}}"))]
  println(schema-error-message(schema-decode-errors(r)))
;; name: expected :cstr, got :int
;; address.zip: expected :cstr, got :int
```

Walk the errors programmatically with `schema-error-count`, `schema-error-at`,
`schema-error-path`, and `schema-error-text`. This is the same accumulation
policy as Zod's `.safeParse` -- a complete picture of what is wrong, not just
the first failure.

## Recursive schemas

Self-referential data (trees, nested comments) uses `schema/rec`, which passes
the schema itself in as `self`. The body is forced lazily on first decode and
memoized:

```turmeric
(defn tree-schema [] : int
  (schema/rec
    (fn [self]
      (schema/field
        (schema/field (schema/object-new) "value" (schema/int))
        "children" (schema/array self)))))

(schema-decode (tree-schema)
  (json/decode
    "{\"value\": 1, \"children\": [{\"value\": 2, \"children\": []}]}"))
;; => ok, validated to arbitrary depth
```
```sweet-exp
defn tree-schema [] :int
  schema/rec((fn [self] schema/field(schema/field(schema/object-new() "value" schema/int()) "children" schema/array(self))))
schema-decode(tree-schema() json/decode("{\"value\": 1, \"children\": [{\"value\": 2, \"children\": []}]}"))
;; => ok, validated to arbitrary depth
```

A violation deep in the tree still reports a full path, e.g.
`children[0].value: expected :int, got :cstr`.

## Relation to `tur/contract`

The two are complementary: use `tur/schema` to validate data **at the
boundary**, turning untyped JSON into values you trust, then use `tur/contract`
to enforce invariants on those values **inside** the program.

## Typed decoding with `HasSchema`

`schema-decode` hands back the *validated node* (read fields with `json/get!`);
to land directly in a typed value, implement the `HasSchema` typeclass. Its one
method, `decode!`, is **return-type-directed**: the concrete instance is chosen
from the expected type at the use site (an ascription `(:: e T)` or a typed
binding), not from any argument.

```turmeric
(load "stdlib/json.tur")
(load "stdlib/schema.tur")

(defstruct User :copy [name : cstr age : int])

(defn user-schema [] : int
  (schema/field
    (schema/field (schema/object-new) "name" (schema/str))
    "age" (schema/int)))

(definstance HasSchema [User]
  (decode! [node]
    (let [v (schema-decode! (user-schema) node)]
      (make-struct User
        (json/get-string (json/get! v "name"))
        (json/get-int    (json/get! v "age"))))))

;; The ascription drives instance selection; decode! returns a real User.
(let [u (:: (decode! (json/decode body)) User)]
  (.name u))
```
```sweet-exp
load("stdlib/json.tur")
load("stdlib/schema.tur")
defstruct User :copy [name :cstr age :int]
defn user-schema [] :int
  schema/field(schema/field(schema/object-new() "name" schema/str()) "age" schema/int())
definstance HasSchema [User] decode!([node] let([v (schema-decode! (user-schema) node)] make-struct(User json/get-string(json/get!(v "name")) json/get-int(json/get!(v "age")))))
;; The ascription drives instance selection; decode! returns a real User.
let [u (:: (decode! (json/decode body)) User)]
  .name(u)
```

`decode!` returns the typed value directly (a genuine by-value struct -- the
compiler bridges the typeclass dictionary's `int64` carrier ABI back to the
concrete struct at the resolved call site). It **panics** on any schema
violation, like `schema-decode!`; for graceful handling, validate with
`schema-decode` first and branch on `schema-decode-ok?`.

> **`name : cstr` vs `name : String`.** `json/get-string` *borrows* a pointer
> into the decoded JSON node, so a `cstr` field holds that borrow and dangles the
> moment the node is freed -- yet the `User` is meant to outlive the node. For a
> field decoded from boundary data, prefer an owned `String`
> (`[name : String age : int]`, filled with `(string/from-cstr (json/get-string
> ...))`), which copies the bytes into a value the struct owns. See
> [strings-guide.md](strings-guide.md).

Ascribing `decode!` to a type with no `HasSchema` instance is a **compile-time**
error (`no instance 'HasSchema T'`).

## The `#json-str<T>(...)` reader macro

`#json-str<T>(expr)` desugars to
`(:: (decode! (json/decode expr)) T)` -- a one-liner from a runtime JSON
`:cstr` to a typed value:

```turmeric no-check
(defn parse-user [body :cstr] :User
  #json-str<User>(body))
```
```sweet-exp
defn parse-user [body :cstr] :User
  #json-str<User>(body)
```

Unlike `#json(...)`, the inner is an ordinary Turmeric expression (read with the
normal reader), not a verbatim JSON blob. The bare `#json<T>(...)` literal form
also wraps its node tree in `(:: node T)`.

### `#json-str?<T>(...)` -- the Result-returning form

Add a `?` and a schema violation becomes a value instead of a process death:

```turmeric no-check
(defn try-user [body :cstr] :(Result User int)
  #json-str?<User>(body))

;; (ok? r) -- decoded; (ok-val r) is the User
;; (err? r) -- the body did not satisfy User's schema
```

It expands to the panicking form behind a catch boundary:

```turmeric no-check
(:: (catch-unwind (fn [] : T (:: (decode! (json/decode expr)) T)))
    (Result T int))
```

which is the whole design: `HasSchema` has one method and each instance's
schema lives inside its own `decode!` body, so there is nothing else for a
reader macro to branch on. Two consequences worth knowing:

- **The violation detail still goes to stderr.** `schema-decode!` prints every
  failing path before it raises, on the recovered path too. The `Result` tells
  you *that* it failed; stderr tells you *what* failed.
- **The err payload is the panic handle**, carried as `:int` at this surface --
  `(Result T int)`. Branch with `ok?` / `err?`; there is no structured error
  value to destructure yet.

### `#json-file<T>(...)` and `#json-file?<T>(...)` -- reading a file

The same family over a path instead of a string:

```turmeric no-check
(defn load-config [] :Config
  #json-file<Config>("config.json"))

(defn try-load-config [] :(Result Config int)
  #json-file?<Config>("config.json"))
```

`#json-file<T>(path)` desugars to `(:: (decode! (json/decode-file! path)) T)`.
`json/decode-file!` (stdlib/json.tur) reads the whole file into a private
buffer, parses it exactly as `json/decode` would, frees the buffer, and hands
back the node tree -- so there is no `ptr<void>` to bridge and nothing for
the caller to free but the nodes. Two failure modes, kept distinct:

- **An unreadable path** is a panic raised by `json/decode-file!` itself,
  with the path in the message (`json/decode-file!: cannot read config.json`).
- **A file that does not satisfy the schema** is the same `schema-decode!`
  panic the string form raises, violation paths printed first.

The `?` form puts one catch boundary around both, so `(err? r)` covers "no
such file" and "wrong shape" alike -- the `Result` says *that* it failed and
stderr says *what*. Malformed JSON in a readable file decodes to the null
node and then fails the schema, as with `#json-str`. The interpreter runs the
same function as a native, so `--interpret` agrees.

## Applicative combinators

Object decoders compose from smaller pieces with the **Validation applicative**:
`schema/ap` decodes both arms against the same input and **accumulates** their
errors (it does not fail fast), so a malformed object reports every bad field at
once.

| Combinator | Role | Notes |
|---|---|---|
| `schema/always v` | `pure` | always succeeds with `v` (ignores the node) |
| `schema/never msg` | `empty` | always fails with `msg`; identity of `schema/union` |
| `schema/field-of k s` | extract | decode an object, pull key `k` via schema `s` |
| `schema/ap sf sa` | `<*>` | apply the function decoded by `sf` to the value decoded by `sa`, accumulating errors (function arm is a top-level fn) |
| `schema/ap-fat sf sa` | `<*>` | like `schema/ap`, but the function arm may be a capturing closure (fat-closure apply); backs the `Applicative [Schema]` `ap` method |
| `schema/fmap s f` | `<$>` | map `f` over the decoded value (alias for `schema/transform`) |
| `schema/alt a b` | `<\|>` | two-arm first-match union |

```turmeric
(defn double-it [x : int] : int (* x 2))

;; pure(double-it) <*> field-of("n", int)  -- on {"n":21} => 42
(schema-decode! (schema/ap (schema/always double-it)
                           (schema/field-of "n" (schema/int)))
                (json/decode "{\"n\": 21}"))
```
```sweet-exp
defn double-it [x :int] :int
  {x * 2}
;; pure(double-it) <*> field-of("n", int)  -- on {"n":21} => 42
schema-decode!(schema/ap(schema/always(double-it) schema/field-of("n" schema/int())) json/decode("{\"n\": 21}"))
```

Decoding `{"a": "x", "b": "y"}` against an `ap` of two `int` fields yields **two**
path-tagged errors (`a: expected :int, got :cstr` and `b: ...`), one per arm.

### Why no `Monad`

The lawful `>>=` for Validation is fail-fast, which contradicts the accumulating
`<*>`. Per the design, `Monad Schema` is deliberately omitted; if you genuinely
need to choose a later schema based on a decoded value, decode in two explicit
steps (decode the discriminant, then dispatch on it) rather than reaching for a
monad. Note also the O(arms) cost: `ap`/`union` decode every arm.

## HKT instances

The combinators above are also surfaced as `Functor`/`Applicative`/`Alternative`
typeclass instances over a phantom wrapper, so object decoders can be assembled
with the standard HKT method names and **chain across operators**:

```turmeric
(defstruct Schema [A] (raw :int))   ; provided by stdlib/schema.tur

(definstance Functor     [Schema] (fmap [c f] ...))
(definstance Applicative [Schema] (pure [x] ...) (ap [ff fa] ...))
(definstance Alternative [Schema] (empty [a] ...) (alt-or [a b] ...))
```
```sweet-exp
defstruct Schema [A] raw(:int)
; provided by stdlib/schema.tur
definstance Functor [Schema] fmap([c f] ...)
definstance Applicative [Schema] pure([x] ...) ap([ff fa] ...)
definstance Alternative [Schema] empty([a] ...) alt-or([a b] ...)
```

`(Schema a)` is a **transparent int newtype**: a parametric struct whose single
field is a concrete `:int`, so the type parameter is phantom and the value has
*one* C representation (int64) at every site. That single representation is what
lets an HKT result flow with its `(Schema b)` type, so the next operator can
dispatch on it -- `(ap (fmap c ctor) g)` type-checks because the `fmap` result is
still a `(Schema _)`.

Wrap with `(make-struct Schema raw)` and unwrap with `(.raw s)`; both compile to
the identity (the value *is* the schema pointer). Build a struct field by field:

```turmeric
;; ->User is a curried constructor: name -> (age -> boxed User)
(defn user-schema [] : int
  (let [name-s (:: (make-struct Schema (schema/field-of "name" (schema/str))) (Schema int))
        age-s  (:: (make-struct Schema (schema/field-of "age"  (schema/int))) (Schema int))]
    (.raw (ap (fmap name-s ->User) age-s))))   ; Validation: both fields decoded, errors accumulated
```
```sweet-exp
;; ->User is a curried constructor: name -> (age -> boxed User)
defn user-schema [] :int
  let [name-s (:: (make-struct Schema (schema/field-of "name" (schema/str))) (Schema int))
        age-s  (:: (make-struct Schema (schema/field-of "age"  (schema/int))) (Schema int))]
    .raw(ap(fmap(name-s ->User) age-s))
; Validation: both fields decoded, errors accumulated
```

`ap` is built on `schema/ap-fat`, which applies the (possibly capturing) function
arm through the fat-closure protocol, so a multi-argument curried constructor
composes. Decoding `{"name": 42, "age": "x"}` still yields **two** path-tagged
errors -- the applicative is Validation, not fail-fast, even when assembling a
struct. See the `schema-hkt-functor`, `schema-hkt-alternative`,
`schema-applicative-user`, and `schema-applicative-user-errors` fixtures.

> `stdlib/schema.tur` pulls in the `Applicative`/`Alternative` class stubs for
> you (and `Functor` is loaded globally), so `(load "stdlib/schema.tur")` is
> enough -- no `(load "stdlib/typeclass.tur")` is required to use the instances.

See [docs/archive/history/schema-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/schema-plan.md) for the full design and the
rationale behind the Validation (accumulating) semantics.

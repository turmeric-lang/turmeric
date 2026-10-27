---
title: JSON Guide
category: Data Structures and Libraries
description: Compile-time #json(...) reader macro and runtime tur/json library for parsing and serializing JSON
tags:
  - serialization
  - parsing
---

# JSON in Turmeric: the `#json(...)` reader macro and `tur/json`

> **Status:** the `#json(...)` reader macro and the runtime `tur/json`
> library are both shipping.

Turmeric handles JSON two ways:

- **Compile time** -- the `#json(...)` reader macro embeds a verbatim JSON blob
  in source, validates it while compiling, and lowers it to a `tur/json`
  tagged-node tree (the same value `json/decode` produces). No runtime parse
  step.
- **Runtime** -- the `tur/json` library (`json/decode`, `json/encode`) parses
  and serializes JSON strings whose contents are not known until the program
  runs.

Because both produce the *same* node tree, `#json(...)` is effectively a
`json/decode` whose input is validated by the compiler.

## The `#json(...)` reader macro

The macro uses round parens as the outer fence, so a top-level object reads
without a doubled brace and a top-level array has no doubled bracket:

```turmeric no-check
#json({"a": 1, "b": 2})   ; object
#json([1, 2, 3])          ; array
#json(42)                 ; scalar
```
```sweet-exp
#json({"a": 1, "b": 2})   ; object
#json([1, 2, 3])          ; array
#json(42)                 ; scalar
```

### What the reader produces

The JSON is parsed at compile time and emitted as `tur/json` constructor calls,
which the normal typechecker elaborates:

| JSON              | Emitted constructor call                          |
|-------------------|---------------------------------------------------|
| `{"a": 1}`        | `(json/object-put (json/object-new) "a" (json/int 1))` |
| `[1, 2, 3]`       | `(json/array-push (json/array-push (json/array-new) (json/int 1)) ...)` |
| `"hello"`         | `(json/string "hello")`                           |
| `42`              | `(json/int 42)`                                   |
| `3.14`            | `(json/float 3.14)`                               |
| `true` / `false`  | `(json/bool 1)` / `(json/bool 0)`                 |
| `null`            | `(json/null)` -- a real null node                 |

Every node is a uniform handle (`:int`), so values can be **heterogeneous and
nested** -- the type of each value is carried as a runtime tag, recoverable with
`json/type` and the `json/get-*` accessors. This is the key difference from the
`#map{...}` / `[...]` data literals, which are monomorphic `:int` collections.

Retrieve object fields by key with `json/get!` (returns the value node) and
extract the scalar with the typed accessor:

```turmeric no-check
(json/get-string (json/get! #json({"name": "alice", "age": 30}) "name"))  ; => "alice"
(json/get-int    (json/get! #json({"name": "alice", "age": 30}) "age"))   ; => 30
```
```sweet-exp
json/get-string(json/get!(#json({"name": "alice", "age": 30}) "name"))  ; => "alice"
json/get-int(json/get!(#json({"name": "alice", "age": 30}) "age"))      ; => 30
```

(`json/get` is the total variant -- it returns an `Option` node; `json/get!`
panics on a missing key.)

Index into arrays with `json/array-get` / `json/array-len`:

```turmeric no-check
(json/array-len #json([10, "x", true]))                     ; => 3
(json/get-int    (json/array-get #json([10, "x", true]) 0)) ; => 10
(json/get-string (json/array-get #json([10, "x", true]) 1)) ; => "x"
```
```sweet-exp
json/array-len(#json([10, "x", true]))                     ; => 3
json/get-int(json/array-get(#json([10, "x", true]) 0))     ; => 10
json/get-string(json/array-get(#json([10, "x", true]) 1))  ; => "x"
```

`null` is genuinely distinct from `0`/`false` -- it is a node whose
`(json/type node)` is `0`:

```turmeric no-check
(json/type #json(null))   ; => 0  (null)
(json/type #json(false))  ; => 1  (bool)
(json/type #json(0))      ; => 2  (int)
```
```sweet-exp
json/type(#json(null))   ; => 0  (null)
json/type(#json(false))  ; => 1  (bool)
json/type(#json(0))      ; => 2  (int)
```

### Why a tagged node tree (and not a HAMT/vec)?

A Vec/HAMT stores `int64` slots, and `vec-of`/`hamt-of` are **monomorphic** in
a single `:int` carrier -- so a collection's elements must all share one type
(the `vec-of` docstring spells this out). JSON, by contrast, is heterogeneous
(`[1, "x", true, null]`, mixed-type objects). Representing that on top of
homogeneous storage requires a **boxed/tagged value**: each value becomes the
same type -- a JSON node handle -- with the variation moved into a runtime tag.
That is exactly the `tur/json` node, which is why `#json(...)` targets it
rather than collapsing everything to `:int` (and `null` to a misleading `0`).

### Errors

Malformed JSON is reported at compile time with line/column pointing into the
`#json(...)` block:

| Code        | Condition                                   |
|-------------|---------------------------------------------|
| `TUR-E0270` | Malformed JSON inside a `#json(...)` block  |
| `TUR-E0271` | Unexpected EOF inside a `#json(...)` block  |

### Optional type hint

The `#json<Type>(...)` form wraps its node tree in an ascription
`(:: <node> Type)`. For a literal blob this is a compile-time type check on the
node's own representation; the typed-decode path lives in the `tur/schema`
reader family `#json-str<T>(expr)`, which desugars to
`(:: (decode! (json/decode expr)) T)`. See the
[schema guide](schema-guide.md) for `HasSchema` and typed decoding.

## Runtime JSON: `tur/json`

When the JSON is not known at compile time (read from a file, a socket, user
input), use the runtime library:

```turmeric
(let [node (json/decode "{\"x\": 1, \"y\": 2}")]
  ...)                        ; node is a heap JSON value tree
(json/encode node)            ; => "{\"x\":1,\"y\":2}"
```
```sweet-exp
let [node json/decode("{\"x\": 1, \"y\": 2}")]
  ...                        ; node is a heap JSON value tree
json/encode(node)            ; => "{\"x\":1,\"y\":2}"
```

`json/decode` returns a node tree (null / bool / int / float / string / array /
object); see `stdlib/json.tur` for the accessor API. Use the reader macro for
fixed, source-embedded data and `tur/json` for dynamic data.

## When to use which

| Need                                            | Use                  |
|-------------------------------------------------|----------------------|
| Embed a fixed JSON blob, validated at compile   | `#json(...)`         |
| Parse a JSON string only known at runtime       | `json/decode`        |
| Serialize a value to a JSON string              | `json/encode`        |
| Build a collection with computed slot values    | `#map{...}` / `#set{...}` / `[...]` data literals |

# A struct with a `defopaque` field leaks its box on every construction

**Severity: medium (leak).** A `defstruct` with a field of a `defopaque` type
lives on the carrier ABI: each constructor call mallocs a box, and nothing
frees it. This covers a field of type `String`, which is an opaque over a
pointer, so the leak is easy to reach by accident. The box leaks:

- wherever the value lives: a `let`, a function result, or an element of a
  `Vec`, where `vec-free` frees the buffer but not the element boxes;
- whether or not the struct is `:copy`;
- whether the opaque's base is an int or a pointer.

A multi-variant `defdata` with a `String` payload, such as
`(defdata Inl (Text :String) (Soft))`, leaks its box the same way.

Found 2026-10-16 while designing the node arena for the CommonMark block
parser (markdown-library-plan M1). The parser keeps opaque values out of its
node structs for this reason (see "Workaround").

## Repro

```turmeric
(defopaque K :int)
(defstruct Node :copy [f0 : K f1 : int f2 : int])
(defn main [] : int
  (let [n (Node (:: 3 K) 1 2)]
    (do (println (.f1 n)) 0)))
```

```sh
TUR_SAFFRON_GC=1 TUR_CC_FLAGS="-O1 -g -std=c99 -fsanitize=address -L$PWD/build/src" \
  CC=cc ./build/tur build repro.tur -o repro
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=use_stacks=0:use_registers=0 ./repro
```

```
Direct leak of 24 byte(s) in 1 object(s) allocated from:
    #1 in ctor_Node_Node
```

Changing the struct changes the result:

| Struct | Result |
| --- | --- |
| `f0 : int` or `f0 : bool` | clean |
| a nullary `defdata` kind field | clean |
| a one-int `defstruct` id field | clean |
| `f0 : String` (`(defstruct P [name : String age : int])`) | leaks 16 B |

## Root cause

`adt_is_byvalue_product_d` (`src/compiler/types.c:4067`) decides whether a
struct is a by-value aggregate:

- It admits scalar fields, inline by-value aggregates and type applications.
- It rejects any other `TY_ADT` field (`types.c:4090`).
- An opaque field is a `TY_ADT` with `is_opaque` set, so the whole struct goes
  on the carrier ABI: a heap box held as the int64 word.

The carrier ABI has no owner for that box, and nothing frees it, unlike a
by-value aggregate's element boxes, which `vec-free` frees through
`tur-vec-elem-wide?`.

[struct-field-with-opaque-sibling-lowered-to-int](../archive/struct-field-with-opaque-sibling-lowered-to-int.md)
(resolved 2026-10-11) chose this layout on purpose. It kept these structs on
the carrier ABI rather than change the C signature of every such struct at an
inline-C boundary, and fixed their field reads and writes. It doesn't mention
the leak.

## Fix directions

1. **Lay an opaque field out by value.** An opaque value is one word, its
   declared base, so `adt_is_byvalue_product_d` can admit it as it admits an
   `int` or a `cstr` field. Measured 2026-10-16:
   - **What the one-line change fixes.** Admitting an `is_opaque` `TY_ADT`
     field (a `continue` before the rejection) fixes every repro above,
     including a `(Vec Node)` of opaque-field nodes.
   - **What it breaks.** 6 of 3860 `tests/run.sh` fixtures:
     - **Five representation-check failures, one cause.** The slot of a
       pointer opaque stays `int64_t` (`adt_field_c_type`'s default), and the
       by-value read path returns it from a `void *` function without the cast
       the carrier path had. The fixtures are
       `saffron-record-opaque-ptr-field`, `parsec-guide-step8-key-value`, and,
       through `R7rsPort`'s `io` field, `r7rs-tail-calls`,
       `region-escape-via-callcc` and `docs-r7rs-guide-examples`. The slot
       should take the opaque's C name, or the read should cast.
     - **One older CPS limitation, now exposed.** `cps-while-owning-invariant`
       stops compiling. Its record has an opaque field and a closure field, and
       by value it hits a limitation that exists without the change: a CPS
       `handle` whose body passes a by-value struct with a closure field fails
       `handle_delim_ok`. The same program with an `int` field in place of the
       opaque one already fails on `main`. The fixture would need another
       one-word owning carrier.
   - **What it leaves unmeasured.** This is the ABI change the earlier report
     declined: every inline-C body that takes or returns such a struct as a
     word would have to change, and turmeric-spices was not measured.
2. **Give the carrier box an owner.** Keep the layout and free the box where a
   by-value aggregate would be dropped: at scope exit for a `let`, in
   `vec-free` / `vec-set!` for a `Vec` element (fold the carrier record into
   `tur-vec-elem-wide?`), and at a consuming `match`. This is the same missing
   owner as
   [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md),
   for a record instead of a sum.

## Workaround

Keep opaque types out of struct fields that are built often:

- **Ids.** Use a one-field `(defstruct Id :copy [ix : int])` rather than
  `(defopaque Id :int)`. It is a by-value struct, still a distinct type.
- **Kinds.** Use a nullary `defdata`.
- **Text.** Keep `String`s in a `(Vec String)` the owner releases, and store
  their indices in the struct.

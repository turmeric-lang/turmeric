---
title: Track C U3 (row-typed schemas) — compiler blockers retired by turmeric #479–#483
category: Spice-uplift blocker assessment (Track C / U3)
status: UNBLOCKED -- every compiler prerequisite verified present. Targets 1-3 shipped except the `http` CLIENT half; target 4 (json) is moot.
verified-on: turmeric main @ 99cc8b32 (post #479/#480/#481/#482/#483; built from source)
verified-by: turmeric-spices Claude (Track C, branch claude/track-c-u3-turmeric-cgmo6u)
plan: docs/archive/spices-type-features-uplift-plan.md (Phase U3)
---

# U3 — row-typed schemas: unblocked as of turmeric main @ 99cc8b32

## TL;DR

Phase **U3 (row-typed schemas)** of the spices type-features uplift can be
done now. The type-level mechanism (P0 typed-field `#row{k : T ...}`
literals) shipped earlier; the remaining gap was the **runtime decoder
machinery** the U3 decoder model is built on — constrained-instance
`Encode`/`Decode` over containers, and struct/`defdata` fields that carry
parametric containers. A cluster of turmeric-main fixes (**#479, #480, #481,
#482, #483**) retires that gap. None of these are in the `v0.22.0` release
(tag @ `5318de7d`), so **U3 work must build `tur` from main**, not from a
release tarball.

There is **no remaining compiler blocker for U3.**

## What U3 is (from the plan)

> **Goal:** carry "what's in this row/header/column-set" at the type level
> using the same `#row{...}` phantom machinery the `Query` value uses in ECS.

Targets: `frame` (`Frame<#row{...}>`), `postgres`/`sqlite`
(`Result<#row{...}>`, `Stmt<params cols>`), `http`/`httpd`
(`Request<#row{headers...}>`), `json` (object shapes as rows). The plan's
**"U3 decoder model"** specifies that every backend's per-row decoder returns
`Result<T, Vec<DecodeError>>` with a shared `DecodeError` (path +
expected-type + got-tag) and validation-applicative error accumulation,
generated from the static row by a `derive-decoder` macro (sibling of P2a's
`derive-json`). The deliverable for U3 is a **negative fixture per target**:
code that *fails to compile* with a known-wrong row.

That decoder model is what needs the container/parametric machinery below.

## The blockers and the fixes that retire them

| U3 building block it enables | turmeric fix | commit |
|---|---|---|
| `Encode`/`Decode [Cons]` — dispatch a class method on a `:heap` list element | #479 | `12c6f31c` |
| `Decode [Option]` / nested container *construct* in a constrained instance body (`(ok (some …))`) | #480 | `eca8bc98` |
| constrained generic `defn` returning a parametric container | #481 | `15cf5fec` |
| `defstruct` field of a by-value parametric struct (`(Option cstr)`) — laid out inline, not collapsed to the int64 carrier | #482 | `e00a7104` |
| `defdata` constructor field that is an applied type constructor (`(Box a)`, `(Pair2 :cstr a)`, `(f (Fix f))`) | #483 | `99cc8b32` |

Why each matters for U3:

- **#479/#480/#481** are the read-side, construct-side, and return-side of
  the same carrier-ABI machinery: a constrained `Encode`/`Decode` instance
  over a *container* element type. The json `Decode` typeclass the U3 decoder
  model reuses (from P2a) needs all three to decode `Option`/list/nested-`Result`
  fields without falling back to the int64 carrier. Before these, json could
  *encode* optional/contained fields but not *decode* them (the two reports
  filed in turmeric #478, now closed by #479/#480).
- **#482** lets a `DecodeError` record (or any row-derived `defstruct`) carry a
  by-value parametric field such as `(Option cstr)` inline. Without it the
  field collapsed to `long int` and `make-struct` / `.field` mismatched.
- **#483** lets `DecodeError` be a sum type whose constructors carry applied
  types, and lets `Result<T, Vec<DecodeError>>`-shaped accumulators be
  expressed directly. It also unblocks the `Fix F` self-application U5 will
  want, but U3 needs it for the error-accumulator ADT.

## Verification (turmeric main @ 99cc8b32, `tur` built from source)

Built `tur` from a fresh clone of main at `99cc8b32`
(`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`).
All checks below were run against that binary.

| Check | Expected | Result |
|---|---|---|
| `Encode [Cons]` over `int`/`float`/`cstr` (the #479 repro) | `42` / `7.1` / `"hi"` | ✅ |
| `Decode [Option]` over `int`/`cstr` (the #480 repro) | `42` / `hi` | ✅ |
| `defstruct` field of `(Option cstr)`/`(Option int)` (turmeric fixture `defstruct-field-byvalue-parametric-struct`) | `1` / `0` / `42` / `9` | ✅ |
| `defdata` field of applied ctor (turmeric fixture `defdata-applied-type-field`) | `7` | ✅ |
| Typed-field row `(Tbl #row{id : int  name : cstr})` accept (turmeric fixture `typed-field-row-accept`) | `3` | ✅ |
| **U3 negative deliverable**: passing a `#row{uid:int label:cstr}` where `#row{id:int name:cstr}` is expected | fails to compile | ✅ `TUR-E0001` row-mismatch |

The first two repros previously produced a hard C compile error / silent
miscompile; they now compile and run correctly. The last row demonstrates the
U3 "negative fixture" deliverable already works: two rows with identical
element types but different field names refuse to unify.

## U3 target 1 (`frame`): implemented

The first U3 target landed in this branch as `spices/frame/src/frame/typed.tur`
(exported as the `frame/typed` module). `Frame` is a phantom row-typed newtype
over the existing `:int` frame handle — the same machinery ECS's `Query` value
uses — so a function pinned to a concrete schema rejects a frame with any other
column-set row at the elaborator.

Shape (mirrors `ecs/query.tur`):

```turmeric
(defstruct Frame [^&cols] (handle :int))
(defn frame-typed  [^&cols] [h : int]          : (Frame cols) (make-struct Frame h))
(defn frame-handle [^&cols] [f : (Frame cols)] : int          (.handle f))
;; typed delegates: tframe-nrows / tcol-int32-at / tcol-utf8-at / ...
```

`frame/typed` passes the CI `tur check` step and the `tur test tests/frame`
suite (`tests/frame/typed_test.tur`). Its definitions are row-*polymorphic*; a
concrete `#row{...}` is only needed at a call site that names a specific
schema, and that no longer needs a flag either.

### Concrete-schema demonstrations

**Note (2026-09-09):** `-Xdata-literals` is no longer needed. `#row{...}` parses
ungated on turmeric v0.46.0, and passing the flag now only produces
`warning [TUR-W0050]: -Xdata-literals is no longer needed; the feature is on by
default`. Concrete-row call sites check flag-free.

The negative demonstrations below now ship as real `errors/` fixtures for
`postgres` and `httpd` (see "Target status" at the bottom). Both were originally
verified against `tur` @ `99cc8b32`:

```turmeric
;; POSITIVE — a frame ascribed to the matching schema row: checks + runs.
(defn sum-users [f : (Frame #row{id : int  name : cstr})] : int (frame-handle f))
(let [users (:: (frame-typed raw) (Frame #row{id : int  name : cstr}))]
  (sum-users users))            ; => runs

;; NEGATIVE — a frame with the WRONG schema row: rejected at elaboration.
(let [wrong (:: (frame-typed raw) (Frame #row{uid : int  label : cstr}))]
  (sum-users wrong))
;; error [TUR-E0001]: function 'sum-users' arg 1:
;;   expected (type-app Frame #row{id : int name : cstr}),
;;   got (type-app Frame #row{uid : int label : cstr})
```

The negative case is the U3 deliverable: two frames whose schema rows differ
(even with identical element types) are distinct types, caught at compile time.

Note: the `frame` suite has 3 *pre-existing* failures against tip-of-main `tur`
(`group_test`, `interop_test`, `reshape_test` — a linker error unrelated to
U3); they fail with or without `frame/typed` and are out of scope here.

## Target status (re-verified 2026-09-09, turmeric v0.46.0)

> **Re-checked 2026-10-07 against turmeric-spices `origin/main` (388aa99):
> nothing has moved.** The table below is current. `spices/http/src/http/`
> still holds only `client`, `error`, `request`, `response` (no `typed.tur`),
> and the other four typed modules are unchanged. This file stays in
> `docs/upcoming/spices/` because exactly one item is still open: the `http`
> client half of target 3. Do not archive it until that lands or is dropped.

The section that used to sit here listed targets 2, 3 and 4 as outstanding.
Targets 2 and 3 have since shipped (target 3 except its client half). Current
state:

| # | Target | Module | Smoke test | Negative fixture | Status |
|---|---|---|---|---|---|
| 1 | `frame` | `frame/typed.tur` | `tests/frame/typed_test.tur` | -- | shipped |
| 2 | `postgres` | `postgres/typed.tur` | `tests/postgres/u3_smoke.tur` | `errors/postgres-trows-row-mismatch.tur` | shipped |
| 2 | `sqlite` | `sqlite/typed.tur` | `tests/u3_smoke.tur` | -- | shipped |
| 3 | `httpd` (server) | `httpd/typed.tur` | `tests/u3_smoke.tur` | `errors/httpd-trequest-row-mismatch.tur` | shipped |
| 3 | `http` (client) | -- | -- | -- | **NOT DONE** |
| 4 | `json` | -- | -- | -- | **moot** |

All four shipped modules are registered in their spice's `build.tur :exports`.

The two negative fixtures are the U3 deliverable and both still reject, flag-free:

```
errors/postgres-trows-row-mismatch.tur:31:16: error [TUR-E0001]: function
  'read-user' arg 1: expected (type-app TRows #row{id : int name : cstr}),
  got (type-app TRows #row{id : int age : int})

errors/httpd-trequest-row-mismatch.tur:33:19: error [TUR-E0001]: function
  'require-auth' arg 1: expected (type-app TRequest #row{authorization : cstr}),
  got (type-app TRequest #row{accept : cstr})
```

### Genuinely remaining

- **Target 3, the `http` client half.** There is no `http/typed.tur`, and
  `spices/http/build.tur` exports only `http/client`, `http/request`,
  `http/response`, `http/error`. A row-typed `Request<#row{headers...}>` /
  `Response<...>` on the client side is still to do.
- **Target 4, `json` -- moot, not pending.** The spice is 100% yyjson-backed
  (`:cmake-deps` pulls `ibireme/yyjson`; `parse`/`emit`/`encode`/`decode`/`patch`
  all wrap it), so it has no Turmeric-level recursive IR to carry a row. The
  sibling U5 doc reaches the same conclusion for the same reason -- see
  `u5-hkt-ast-feasibility-2026-06-21.md`, section "The json target is still
  moot". It should be dropped from the U3 target list rather than tracked as
  outstanding.

A `Col t` typed-column newtype and the term-level `(k in r)` membership
predicate (deferred per P0) remain follow-ups; `frame/typed`'s accessors take
the column name as a `cstr` and type-safety rides at the whole-schema level.

Shared infrastructure to factor once (per the decoder model): a `DecodeError`
(path + expected-type + got-tag), a `Result<T, Vec<DecodeError>>` return
convention, and a `derive-decoder` macro that walks the static row at
elaboration time and emits the runtime decoder (mirroring `stdlib/schema.tur`'s
validation-applicative accumulation).

## Caveat

The mechanisms above were verified in isolation, not via a full `json`/`frame`
spice-suite build against this `tur`. Building the whole spice suite against
main is the natural first step of the first U3 PR.

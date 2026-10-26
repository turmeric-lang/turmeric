---
title: polymorphic `(ok user)` inside a typeclass instance method body still fails for value-struct payloads (remaining gap after Prereq 6)
category: Codegen / ABI gap
severity: Medium. Blocks the macro-emitted Decode side of `derive-json` for `defstruct` types in `../turmeric-spices/spices/json`. Prereq 6 fixed the standalone call shape `(let [r : (Result T B) (ok (make-struct T ...))] ...)`; the typeclass-instance-method context is left over because the spec's result_type lowers to the int64 carrier (per the dispatch shim convention), which the direct-by-value Result construction synthesized by Prereq 6 can't match.
description: After Prereq 6 (2026-06-13, `docs/archive/history/polymorphic-ok-fails-for-value-struct-payload.md`), `(ok (make-struct User 7 "alice"))` at top level synthesizes a direct by-value Result__User__cstr struct construction. Inside a typeclass instance method body (e.g. `(definstance Decode [User] (decode [doc val] (ok (make-struct User ...))))`), the `ok` call's spec is interned with `result_type = int64_t` (the carrier ABI used by typeclass dispatch shims), so Prereq 6's synthesized direct-struct emission can't fire -- it would mismatch the int64 function signature. The original carrier-box helper sketch from the parent report can't fill the gap either: tur_ok's int64 ok_val slot can't hold a multi-byte value-struct, and dereferencing the carrier as Result__User__cstr reads off the end.
status: OPEN. Filed 2026-06-13 as the leftover from Prereq 6. Workaround in tree: `derive-json`'s Decode-side emission stays gated (only Encode is emitted by the macro). Hand-written `(definstance Decode [T] (decode [doc val] ```c ... return tur_ok((int64_t)(intptr_t)heap_t); ```))` using inline-C heap-spill works fine; the macro-driven path is what's blocked.
---

# polymorphic `ok` in typeclass instance method body, value-struct payload

## Summary

Prereq 6 added a synthesized-body path in `src/compiler/emit_fns.c`
for polymorphic `ok` / `err` monomorphizations when the spec resolves
to a value-struct A and the spec's C return type is the by-value
Result struct. The standalone shape works:

```turmeric
(defstruct User [id : int  name : cstr])
(let [r : (Result User cstr) (ok (make-struct User 7 "alice"))
      u                      (ok-val r)]
  (println (.name u)))   ; => alice
```

The typeclass-instance-method shape does NOT work yet:

```turmeric
(defclass Wrap [a] (wrap [v : int] : (Result a cstr)))
(definstance Wrap [User] (wrap [v] (ok (make-struct User v "x"))))

;; Inside the instance method body the spec for `ok` is interned with
;; result_type = int64_t (the carrier ABI used by typeclass dispatch
;; shims), so Prereq 6's synthesized direct-struct emission would mismatch
;; the function's int64 return signature. The Prereq 6 guard correctly
;; skips synthesis in this case, but the fallback path (inline-C body +
;; heap-spill) produces a tur_ok carrier handle whose layout doesn't match
;; what callers reading Result__User__cstr's ok_val expect.
```

The fallback path emits the carrier int64 from `tur_ok((int64_t)(intptr_t)heap_user)`,
which is a pointer to a `tur_result_box_t { bool is_ok; int64_t ok_val;
int64_t err_val; }`. Consumers that dereference this as
`Result__User__cstr` read off the end (User's ok_val field is wider
than int64).

## Where this surfaces

`derive-json`'s Decode-side emission in
`../turmeric-spices/spices/json/src/json/encode.tur`. The macro
expansion shape is exactly:

```turmeric
(definstance Decode [User]
  (decode [doc val]
    (ok (make-struct User <decoded-id> <decoded-name>))))
```

Macro plumbing (`__decode-field`, `__decode-fields`,
`__decode-make-struct`) is preserved in tree behind an Encode-only
`derive-json` emission so the Decode side can flip on once this gap
closes.

## Root-cause hypothesis

Two intertwined facts:

1. **Typeclass instance method dispatch returns int64 by convention.**
   The dispatch shim signature for a typeclass method is
   `int64_t __inst_Class_method_T(...)` regardless of the declared
   Turmeric return type. This is what lets dispatch dicts be uniform
   `(int64_t (*)(int64_t, int64_t))` function pointers without
   per-type variation.

2. **Polymorphic `ok` called from inside such a method has its spec's
   result_type lowered to int64**, matching the enclosing method's
   return ABI. Per Prereq 6's design, the synthesis path requires the
   spec's C return type to be the by-value Result struct, so it
   correctly bails in this case.

The fallback (the original inline-C body with heap-spill) ends up
producing a tur_ok carrier handle whose box layout doesn't match the
by-value Result__T__B struct layout that consumers downstream of the
dispatch shim use. The mismatch is real but doesn't surface as a
compile error -- the code links, runs, and reads garbage at the field
access.

## Proposed fix: Result__T__B layout uniformity (direction 1)

**Chosen direction**: make `Result__T__B`'s `ok_val` slot always
8 bytes -- a heap pointer to T when T is a value-struct, the T value
inline when T is already scalar/pointer-shaped. Same rule for `err_val`
relative to B. Result__T__B then has a fixed 24-byte layout
(`bool is_ok` padded to 8 + `void *ok_val` + `void *err_val`) that
matches `tur_result_box_t` byte-for-byte, so the existing tur_ok / tur_err
carrier helpers work without further plumbing.

**Sketch of the codegen change**:

```c
/* before, for value-struct T */
typedef struct Result__User__cstr {
    bool        is_ok;
    User        ok_val;   /* multi-byte inline value */
    const char *err_val;
} Result__User__cstr;

/* after */
typedef struct Result__User__cstr {
    bool        is_ok;
    User       *ok_val;   /* heap pointer, 8 bytes */
    const char *err_val;
} Result__User__cstr;
```

The pointer rule fires whenever the type-arg's value representation is
multi-byte (`!type_is_inline_scalar(T)` for an existing predicate;
practically: non-parametric `defstruct` instances larger than 8 bytes, or
any defstruct under Phase D's pass-by-ptr threshold). For scalar/pointer
T (`int`, `cstr`, opaque ints, parameterized struct in carrier form),
the slot stays inline -- no change.

**Consumer-side changes**:

- `(.ok-val r)` / `(.err-val r)`: when the slot is the pointer form,
  the accessor dereferences. Codegen patches one site (the field-access
  emit for parameterized Result) to consult the same predicate.
- `make-struct Result`: when the field's slot is pointer form, malloc
  and store the pointer instead of an inline copy. One site in
  `make-struct` emit.
- Hand-written `Decode [T]` instances using inline-C `r->ok_val`:
  inspect the slot type and deref if needed. Documented in the
  `derive-json` docstring and in the json spice README.
- `result-free` / `result-eq?` / `result-map` in `stdlib/result.tur`:
  these are inline-C bodies that read `ok_val` / `err_val` as int64
  today. They need to consult the slot type at monomorphization. Two
  options: either teach them about the pointer form (extra inline-C
  branch), or rewrite them in pure Turmeric so the codegen handles the
  layout decision uniformly.

**Why this over the per-method dispatch ABI alternative (which is now
the long-term plan, see below)**: keeps the uniform `int64_t (*)(...)`
dict layout that HKT typeclasses (`Functor`, `Monad`, `Bifunctor`,
`Applicative`) all assume. Per-method ABI is principled but unwinds
that uniformity across the entire typeclass dispatch infrastructure --
roughly a season of work for a property that's better delivered as a
single coherent ABI commit.

## Long-term plan: end-to-end monomorphization

The cumulative cost of prereqs 1-6 plus this report's open gap is a
strong signal that the hybrid int64-carrier / by-value ABI is the
underlying tech debt -- each new type feature exposes another seam.
The project's long-term direction is to commit to Rust-style
monomorphization: every value uses its natural C layout, polymorphism
is monomorphized per call site, carrier ABI gets retired. That's
tracked separately at
[`docs/upcoming/end-to-end-monomorphization-plan.md`](../upcoming/end-to-end-monomorphization-plan.md).

Direction (1) above is explicitly a short-term unblocker that will get
thrown away when the monomorphization rework lands. Tagging it that
way (rather than as an architectural commitment) keeps the local
choice from accumulating into long-term contract.

## Validation when fixed

- Flip `derive-json` in
  `../turmeric-spices/spices/json/src/json/encode.tur` to emit both
  Encode and Decode instances using the existing
  `__decode-make-struct` macro plumbing.
- Add a fixture under `../turmeric-spices/spices/json/tests/` that
  parses JSON, decodes to a User via `(:: (decode doc root) (Result User cstr))`,
  then encodes back -- collapsing to a single
  `(derive-json User (id int) (name cstr))` declaration with no
  hand-written instance.
- Confirm the main-repo fixture
  `tests/fixtures/polymorphic-ok-err-value-struct-payload/` still
  passes (the standalone path should keep working under the new
  layout because heap-pointer ok_val still round-trips through
  ok-val accessor without any caller code change).

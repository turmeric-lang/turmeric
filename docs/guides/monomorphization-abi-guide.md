---
title: Monomorphization ABI Guide
category: Performance
description: How Turmeric's end-to-end monomorphization ABI works, why the by-value path replaced the int64 carrier, how to read `__spec_*` symbols in error messages, and what the small residual ABI bridge does.
---

# Monomorphization ABI Guide

This guide explains Turmeric's codegen ABI: by-value end-to-end
monomorphization. It is aimed at two audiences:

- **Library/spice authors** who want to understand why a function's
  generated C signature looks the way it does, and how that affects
  inline-C bridging and FFI.
- **Compiler contributors** who need a working model of the by-value
  path before touching elaborator/codegen code.

If you only ever read Turmeric source and never write inline-C, you can
get away without this guide -- the language surface hides the ABI.
You'll start needing it the first time you see a symbol like
`option_map__spec__int_Option__cstr` in a link error.

## TL;DR

- A polymorphic Turmeric value uses its **natural C layout** at every
  call site (`int64_t`, `double`, a `defstruct`'s C struct, an opaque
  pointer, etc.) -- not a uniform `int64_t` "carrier".
- The elaborator picks each call site's concrete type and emits a
  **per-call-site specialization** named `<defn>__spec__<arg-types>`.
- Constrained-polymorphic functions (those with typeclass dictionaries)
  monomorphize **per dictionary**: one spec per concrete instance.
- A small **carrier bridge** still exists for two intentional cases
  (recursive combinator HKT instances whose closures return an
  HKT-applied type; consumer-side bridges where typing the producer
  would be churn-for-no-gain). It is documented and load-bearing -- not
  a migration loose end. The by-value path's audit floor is **0 carrier
  deref-copies**; Vec and MutableMap elements use typed ABIs, not the
  carrier.

## Why monomorphization, not a uniform carrier

The pre-2026 ABI threaded every polymorphic value as `int64_t` -- a
"carrier" wide enough to hold any pointer or primitive, with implicit
boxing/unboxing at type boundaries. The carrier worked, but each
type-system feature shipped that year (HKTs, sized types,
substructural caps, refinement types, associated types) ended in an
ABI-patch bug report:

- `option-none-as-null-byvalue-param-segfault`
- `defstruct-byvalue-struct-field-stored-as-int-carrier`
- `tourist-captures-reads-back-as-int-carrier-collapse`
- `letrec-self-recursive-float-carrier-collapse`
- ...and dozens more (see `docs/archive/history/`).

The pattern: a value would be classified as concrete (`Pos`,
`double`, `Option<int>`) in one phase and as a carrier (`int64_t`) in
the next, and a cast was missing at the boundary. Patches stacked up
in elaborator and codegen until the cumulative cost motivated picking
one ABI: **by-value end-to-end**.

The win:

- Struct values pass and return as real C structs. No `intptr_t`
  cast.
- Floats pass as `double`. The whole "polymorphic float carrier
  ascription" class of bugs is structurally impossible.
- Generated C is readable; debugging a codegen issue is a matter of
  reading the emitted spec, not chasing a cast that should have fired.
- Optimizers (clang -O2, ASan field-precision) see the real types.

The cost: more code generated. A `(option-map f xs)` used five places
in your project, each with different element types, lowers to five
specs (`option_map__spec__int`, `option_map__spec__float`,
`option_map__spec__Pos`, ...). This is the same trade Rust makes; in
practice the cost is fine because (a) most call sites share types,
(b) dead-spec elimination prunes unused ones, and (c) the binary stays
small because each spec is tiny.

## What gets monomorphized

| Construct | Spec naming | Example |
|---|---|---|
| Polymorphic `defn` | `<defn>__spec__<arg-types>` | `option_map__spec__int_int` |
| Typeclass method via dict | `<class>_<method>__spec__<dict>` | `Functor_fmap__spec__Option` |
| Constrained-polymorphic HOF | `<defn>__spec__<arg-types>_<dict>` | `bind__spec__int_Option_Result` |
| `^construct` polymorphic ctor | `<ctor>__spec__<carrier-args>` | `some__spec__int` |
| HKT instance method (by-value) | `<class>_<method>__spec__<F>__<element-types>` | `Functor_fmap__spec__Option__int` |

The spec name is **deterministic**: same call-site types → same spec
name → no symbol collisions across separate compilation units. If two
modules instantiate `option-map` with `int` they refer to the same
emitted symbol; only one TU actually emits the body (the rest declare
`extern`).

## How to read a spec symbol in an error message

Most ABI-flavored errors look like one of:

```
undefined reference to `pick__spec__const_char___int64_t_const_char__'
multiple definition of `ident__spec__const_char___const_char__'
```

The name is built by `emit_abi_clone_name` (`src/compiler/emit_module.c`).
Parse it:

1. Everything before `__spec__` is the originating defn/method/ctor's
   Turmeric name, behind the module prefix its own C name carries when it is
   a global of a named module: `util/ident`'s clones start
   `util__ident__spec__`, a top-level `ident`'s start `ident__spec__`.
2. Right after `__spec__` comes the C type name of the **result**.
3. Then, for each argument in order, a single `_` and that argument's C type
   name.

Every character that is not a letter or digit becomes `_`, in the name and in
each C type name. That makes the separators ambiguous, so read the pieces
against the C types you expect:

| Symbol | Meaning |
|---|---|
| `pick__spec__const_char___int64_t_const_char__` | `pick` returning `const char *` (`cstr`), args `int64_t`, `const char *` |
| `some__spec__tur_adt_Option__int_int64_t` | `some` returning `tur_adt_Option__int`, arg `int64_t` |
| `some___spec__bool_tur_adt_Option__int` | `some?` (the `?` became `_`) returning `bool`, arg `tur_adt_Option__int` |

The `__` inside `tur_adt_Option__int` belongs to the type's own C name (the
monomorph of `(Option int)`), not to the spec name. A trailing `__h1`, `__h2`,
... is a discriminator added when two specs in one TU would otherwise get the
same name.

If you see one of these in a link error under separate compilation (see the
[separate compilation internals guide](separate-compilation-internals-guide.md)):

- **`multiple definition`.** A spec of a user generic is emitted with external
  linkage by the module that defines the generic. Until 2026-10-21 the name
  carried no module, so two modules that define same-named generics
  instantiated at the same types emitted the same symbol
  ([spec-clone-names-not-module-qualified](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/spec-clone-names-not-module-qualified.md));
  the module prefix keeps them apart now. Specs of stdlib generics are
  `static` per TU and never collide.
- **`undefined reference` or a type mismatch at the call.** The owning module
  emits a borrowed spec only after the build's second pass forces it there; a
  module missing from the build set, or a forced spec the owner rebuilt with
  the wrong C types
  ([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/forced-spec-loses-aggregate-type.md)), shows up here. `<build-dir>/.tur-abi-cache/index` lists
  which owner each borrowed clone was forced onto.
- **`#[used]` defn reached only by mangled symbol.** A pure-Turmeric
  defn called only via a hand-written inline-C bridge or a C-ABI
  callback is emitted `static`. Add `#[used]` to the defn so the compiler
  keeps external linkage.

## The residual carrier bridge -- what it does, why it stays

Two classes of carrier crossings remain -- either intentional, or
load-bearing in a way that no method-level fix can address:

1. **Consumer-side bridges, by design.** Some inline-C extractors
   take an `int64_t` argument and reinterpret. The audit fixture
   coverage *requires* this to stay working so the bridge is regression-
   tested. Removing it removes the test, not a bug.
2. **HOF return-type closures.** `Monad bind` / `Applicative ap` take
   a continuation `(fn [A] (m B))` whose result is an HKT-applied type.
   Threading the result by-value end-to-end requires retyping the
   parser/combinator library (`Parser A` and its continuations) plus
   propagating generic-dispatch type information through the entire
   call graph. That's a parser-rewrite unit of work, not a localized
   ABI fix. Until it lands, the closure's return value rides the
   carrier and a tiny spill shim boxes it. Applicative `ap` preserves
   fn type through polymorphic constructors; only the residual
   continuation case rides the bridge.

Vec and MutableMap elements do not ride the carrier: Vec uses the
`:heap` typed-pointer ABI, MutableMap is typed as the honest
`(MutableMap K V)`, and the Vec inline-C producers are monomorphized
to typed pointers.

The bridge is a documented small surface, not an open migration.
Look for `tur_box_T` / `tur_unbox_T` helpers in the runtime and the
`ensure_aggregate_spill_shim` codegen helper for the spill path.

## Practical impact on day-to-day Turmeric code

You should mostly not notice the ABI. The places it surfaces:

### Inline-C blocks see real C types

A `defn` with `#fx{Unsafe}` and an inline-C body sees the parameter types
as their natural C layout:

```turmeric
(defn add-pos [a : Pos b : Pos] #{Unsafe} : Pos
  ```c
  Pos out;
  out.x = a.x + b.x;
  out.y = a.y + b.y;
  return out;
  ```)
```

Do not cast `a` and `b` from `int64_t` back to `Pos*` and dereference --
that carrier-style idiom is wrong here: `a` and `b` are passed by value.

The carrier-bridge corners (HKT continuation results, Vec/Map element
reinterprets) are the only places you still write `(int64_t)(intptr_t)`
casts. If you find yourself reaching for one outside those cases, the
typechecker probably wants a real type.

### FFI / C-ABI callbacks

When you hand a Turmeric function pointer to a C library (`qsort`
comparators, Arrow release callbacks, signal handlers, raylib draw
callbacks), the mangled spec symbol is what the C side takes. Mark the
defn `#[used]` so the compiler keeps external linkage and doesn't DCE
the body:

```turmeric
(defn #[used] compare-ints [a : ptr<void> b : ptr<void>] : int
  ```c
  int ai = *(const int*)a;
  int bi = *(const int*)b;
  return (ai > bi) - (ai < bi);
  ```)
```

Without `#[used]`, the symbol is `static` in the emitted C and the
extern C reference fails at link. With it, external linkage is
preserved and every project module compiles and links.

### Separate compilation

Each module compiles independently. Spec emission rules:

- A spec owned by a real user module is emitted with **external
  linkage** in that module and **extern declarations** elsewhere.
- A spec owned by the prelude (no real owning module) is also emitted
  with **external linkage**; the cross-TU ownership rule guarantees
  exactly one TU emits the body.

If you're maintaining the codegen, the rule of thumb is: the owning
module is whoever declared the originating defn/instance.

## Compiler-contributor cheat sheet

- **Emit pipeline.** `emit_expr.c` picks the spec for a call site;
  `emit_fns.c` emits each spec's body. The spec symbols themselves are
  assembled in `emit_module.c`.
- **Boundary types.** `CK_CARRIER` (`int64_t`) and `CK_CONCRETE` (the
  real type) are the only two `CarrierKind` flavors that matter at the
  C-emit boundary (`emit_internal.h`). There is no
  `EX_ASCRIBE CK_CONCRETE -> CK_CARRIER` bridge; the ascription path
  goes the other direction (carrier -> concrete return deref for
  by-value instance methods). Outside the documented bridge cases,
  every Concrete stays Concrete.
- **HKT specs.** A by-value HKT instance method's spec name encodes
  *both* the constructor and the element type:
  `Functor_fmap__spec__Option__int`. If you see a method spec without
  the trailing element-type segment, that's a carrier-path spec --
  legitimate for instances whose body has no by-value rewrite (e.g.
  `Functor[Parser]`) but unexpected for `Option`/`Result`.
- **Construct-recovery.** `^construct` ctors (`ok`, `err`, `some`,
  `none`) lower to a spec selected from the **expected** type at the
  call site, not the type of the argument. This is how `(none)` knows
  to emit `none__spec__int` vs `none__spec__cstr`. If a `(none)` call
  picks the wrong spec, the issue is in result-type propagation, not
  in the constructor lowering.
- **Dead-spec elimination.** A spec referenced by no module gets
  pruned. If a spec mysteriously vanishes, check whether the only
  references were behind a mangled C symbol (needs `#[used]`) or were
  themselves DCE'd.
- **Bare generic ADT rides the carrier; only concrete gets a struct.**
  An unparameterized `Result`/`Option` with no type args pinned lowers
  to the `int64_t` boxed carrier; the by-value aggregate struct
  (`tur_adt_Result__int__cstr`) exists *only* for a concrete
  monomorphized instantiation. So only concrete instantiations get
  struct DWARF and struct pretty-printers, and any construct that
  yields a bare Result (e.g. `catch-unwind` handing back
  `(Result ThunkRet Panic)` with an intentionally-open err arm) returns
  the carrier. Bridging that carrier into a declared by-value aggregate
  return needs a *structural* field-by-field rebuild from the box -- a
  *representational* `emit_type_c_name(...) == "int64_t"` test misfires,
  because `ok`/`err`/`some`/`none` constructors also collapse to the
  int64 carrier under `emit_type_c_name` yet emit the aggregate directly.
- **Aggregate param pass-mode: read the `Type`, not the C-name.**
  `type_c_name` yields the bare struct name for *both* small (by-value)
  and large (by-ref, `const T*`) products, so classifying an aggregate
  param's pass-mode by C-name string alone misclassifies a by-ref param
  as by-value (and e.g. emits `memcpy(box, &ptrvar, ...)`, copying the
  pointer's address). Classify from the `Type` / `type_struct_pass_by_ptr`,
  never from the ctype string.

## Where to look next

- `docs/archive/history/end-to-end-monomorphization-plan.md` -- the original
  rationale and "why-monomorphization" framing.
- `docs/archive/history/end-to-end-monomorphization-plan-2.md` -- the
  remaining-work plan, archived complete. Has the detailed per-phase
  progress.
- `docs/archive/m4-typeclass-per-method-abi-plan.md` and
  `docs/archive/m5-scope-audit-2026-06-18.md` -- the per-method ABI
  and constrained-poly HOF sub-plans.
- `docs/archive/history/m7-stdlib-migration-execution.md` -- the by-value HKT
  stdlib migration log.
- `docs/guides/hkt-guide.md` -- the user-facing HKT story; this
  guide is the ABI underneath it.
- `docs/guides/name-mangling-guide.md` -- the full mangler rules, of
  which the spec-suffix rules in this guide are one piece.
- `docs/guides/c-integration-guide.md` -- inline-C, `#[used]`, FFI
  patterns; this guide is the type story behind it.

# Plan: Route B — Monomorphised typed value slots

> **Status:** Complete — TS1–TS6 landed
> **Last Updated:** 2026-05-27
> **Type:** Compiler + runtime + stdlib
> **Companion to:** [defalias-plan.md](defalias-plan.md)
> **GS5 compiler follow-up:** [typed-slots-gs5-compiler-support-plan.md](typed-slots-gs5-compiler-support-plan.md)
> **Supersedes (eventually):** the `defalias Sample :int` pattern in
> `turmeric-spices/spices/signal`

---

## Overview

Turmeric's value layer assumes "every value fits in an `int64_t`": fat
closure thunks, list/option/result/pair/vec cells, ADT payloads, and
HKT helpers all carry generic values as `int64_t`.  This is fine for
pointers and integers, but `:float` values must be hand-bit-cast at
every boundary (see `dsp.tur:13-18` and the rationale in
[defalias-plan.md](defalias-plan.md)).

Route B replaces this assumption with **monomorphised typed slots**:
whenever the compiler knows the concrete type of a value at a slot
position, it emits the matching C type (`double`, `int32_t`, …).
The `int64_t` carrier survives only as the fallback for genuinely
generic positions (HKT bodies, existentials, polymorphic captures),
and the compiler inserts explicit reinterpret coercions at those
boundaries.

End state for the signal spice: `:float` flows through `Sample`-typed
cons cells, closure captures, and `Signal` lambda returns with no
union/bit-cast in user code; `defalias Sample :int` retires.

---

## Goals and non-goals

### Goals

1. Eliminate hand-written `union { double d; int64_t i; }` casts in
   spice code whose types are statically known.
2. Preserve the existing `int64_t` carrier ABI for genuinely
   polymorphic call sites (HKT, generic containers used at unknown
   element types), so the cutover is incremental.
3. Make typed-slot codegen the default for fn types, struct fields,
   ADT payloads, and stdlib containers whose element type is concrete
   at the use site.

### Non-goals

- A tagged uniform value representation (Route A).  Rejected — see
  the analysis recorded in the design discussion that preceded this
  plan: doubles slot size, taxes hot paths, fights the existing
  monomorphisation strategy for sized numerics.
- NaN-boxing.  Same reasons, plus it constrains the 64-bit integer
  range.
- Changing the `*args*` representation or any other public ABI that
  external code depends on.
- Removing `defalias`.  `defalias` stays useful as a documentation /
  readability primitive even after the bit-cast tax is gone.

---

## Current state (mapped 2026-05-25)

| Surface | File:line | Today's behaviour |
|---|---|---|
| Fat closure struct | `src/compiler/emit_expr.c:1462-1500` | `struct { int64_t __fn; <typed captures> }` — captures already typed; `__fn` hardcoded |
| Fat closure (POLY_WRAP) | `src/compiler/emit_expr.c:2370-2410` | Same pattern; thunk cast to `int64_t(*)(void*, int64_t)` |
| Direct closure call | `src/compiler/emit_expr.c:1105-1137, 1154-1179` | Thunk cast site already knows arg/ret `Type *` — good |
| Thunk env layout | `src/compiler/emit_fns.c:60-71` | `__fn` is `int64_t`; capture fields use `type_c_name()` (already typed) |
| Thunk fn signature | `src/compiler/emit_fns.c:74-131` | Poly params: `tur_poly_fn_t`; concrete params: `int64_t` carrier |
| `tur_poly_fn_t` typedef | `src/compiler/emit_module.c:727` | `struct { void *env; int64_t (*fn)(void*, int64_t); }` |
| Untyped containers | `stdlib/list.tur:20`, `stdlib/option.tur:38-42`, `stdlib/result.tur`, `stdlib/pair.tur:19`, `stdlib/vec.tur`, `stdlib/fix.tur:21-24`, `stdlib/free.tur:22-24` | All store value/payload as `int64_t` regardless of element type |
| Typed-but-erased | `stdlib/tlist.tur:15-17` | Parameterised over `[A]` but still stores `:int` at runtime |
| HKT helpers (cata, fmap, bind) | `stdlib/list.tur:204-228, 313-344`, `stdlib/fix.tur:70-76`, `stdlib/free.tur:73-84` | Extract thunk from fat[0], call with `int64_t` element |
| Sized numerics in struct fields | `src/compiler/types.c` (`type_c_name`) | Already emits `double`, `int32_t`, etc. for concrete sized types |
| Float builtins | `src/compiler/builtins.c:18-41, 90-99, 103-116` | Full coverage for arith/cmp/println; math funcs (sin/cos/sqrt) live in stdlib |

**Key insight from the map:** the compiler already emits typed C
slots for *struct fields* via `type_c_name()`.  The "int64 everywhere"
assumption is concentrated in (a) the fat-closure thunk ABI, (b)
stdlib containers that hardcode `:int`, and (c) the HKT helpers that
unpack those containers.  Route B is mostly about teaching (a) to
emit per-(arg, ret)-type thunk signatures, parameterising (b) over a
real type variable, and inserting explicit `:reinterpret` IR nodes
at (c)'s boundaries.

**Worktree update (2026-05-25):** TS1 and TS2 are now landed.
`float-closure` emits a shared typed thunk typedef, a fat closure with
a typed `__fn` field, and direct typed closure calls with no user-side
bit-cast. The table above predates those in-flight changes.

**Worktree update (2026-05-26):** TS3's prerequisite generic substrate is
now landed through GS4. Parameterized `defstruct` fields accept their own
type binders, instantiated struct field types survive `make-struct`,
field access, `set!`, and typed call boundaries through elaboration, and
mismatched applied-struct calls are rejected with full instantiated type
diagnostics. Codegen now also emits deterministic concrete struct-app
typedefs such as `Box__float` / `Pair__int__float` and uses them in
compound literals and typed function signatures, so direct concrete
layouts are now present in emitted C. `defn` / `fn` also now accept
explicit `[A]` binders, preserve named type variables through signatures,
and reinterpret scalar generic arguments/results across the `int64_t`
carrier boundary so generic scalar identity-style calls work for `:float`
without user bit-casts. TS3 remains in progress until the stdlib
container migration work lands.

**Worktree update (2026-05-26, GS5 slice):** the stdlib container migration
has now started. `Option`, `Pair`, and `Result` `defstruct` payloads use
their type parameters directly, and `Cons` now specializes its `head`
payload slot while leaving the `tail` link on the legacy carrier, matching
the TS3 intermediate goal of layouts like `struct { double head; int64_t tail; }`.
Focused typed-slots fixtures now show direct `make-struct` lowering to
concrete emitted C types like `Option__float`, `Pair__int__float`,
`Result__float__cstr`, and `Cons__float`. Carrier-era helper APIs are
still the remaining part of TS3, and the missing compiler work is broken
out in
[`typed-slots-gs5-compiler-support-plan.md`](typed-slots-gs5-compiler-support-plan.md).

**Worktree update (2026-05-26, TS3.1 slice):** typed accessor helpers
(`thead`, `unwrap`, `pair-fst`/`pair-snd`, `ok-val`/`err-val`) and the
typed constructor `pair` are already in place from earlier GS5 work.
This slice adds `tcons-of [A] [h :A t :int] :(Cons A)` (`stdlib/list.tur`)
as the first typed `Cons[A]` constructor that lowers through `make-struct`
to the concrete `Cons__A` layout with no inline-C bit-cast, and the
focused fixture `tests/fixtures/typed-slots/tcons-of/`. Typed
`some`/`none`/`ok`/`err` constructors are intentionally deferred until
the inactive-payload representation question is resolved (see "Open
design choices" in the archived GS5 compiler support plan).

**Worktree update (2026-05-26, TS3.2 slice):** the TS3.2 audit confirmed
that nested concrete struct-app monomorphisation, mangled typedef names
(`Cons__float`, `Pair__int__float`, etc.), and field-layout codegen are
already correct end-to-end -- including for nested struct-apps such as
`Wrap[Box[float]]` emitting `Wrap__Box__float { Box__float inner; }`.
The remaining TS3.2 gap was sized-numeric type arguments collapsing to
the generic int/float kind during struct-app instantiation (KB-013).
Root cause was the short-form sized type names (`i32`, `f32`, `i16`,
`u32`, ...) not being recognised by `typekind_from_symbol` /
`typekind_from_name`; the mangler and field codegen were correct as
soon as the type resolves.  Fixed by adding `i8`/`i16`/`i32`/`i64`/
`u8`/`u16`/`u32`/`u64`/`f32`/`f64` short forms to both kind tables.
`(Box i32)` now emits `Box__int32 { int32_t x; }` and the let-binding
also lands as the concrete typed local (resolves KB-014 as a downstream
effect of the same fix).  Fixture:
`tests/fixtures/typed-slots/sized-numeric-struct-app/`.  Literal
inference inside `make-struct` does **not** yet propagate the expected
struct-app argument type into the value-side literal (`(make-struct Box
1)` infers `int` regardless of declared `(Box i32)` return), which is a
bidirectional-checking concern outside TS3.2 scope; ascribe the literal
explicitly: `(make-struct Box (:: 1 i32))`.

**Worktree update (2026-05-26, TS3.4 slice):** added the three named
acceptance fixtures the plan called out:
`tests/fixtures/typed-slots/cons-float/` (`.head`/`.tail` direct read
of a `Cons__float` layout), `option-float/` (Option[float] payload
round-trip via `.value` and `unwrap`, plus `.is-some` on a none-shaped
cell), and `polymorphic-cons-boundary/` (a `Cons[float]` flowing
through an `[A] [c :(Cons A)] :(Cons A)` identity fn and back, with
both ends reading `.head` as `double`).  The cons-float fixture
exercises `.head` rather than `thead`/`thead` repeated, because a
pre-existing specialization-cache bug (KB-015) emits a spurious
`int64<->double` reinterpret around the second call to a generic
typed accessor.  All 17 typed-slots fixtures pass; full
`tests/run.sh` regression unchanged at 884 passed / 110 failed
(110 are pre-existing per KB-006).

**Worktree update (2026-05-26, TS3.3 slice):** the TS3.3 boundary probe
confirmed that polymorphic round-trips of typed aggregate containers
(`(defn id-cons [A] [c :(Cons A)] :(Cons A) c)` called with
`(Cons float)`) already work end-to-end via the GS5 substrate.  The
real remaining gap was at *scalar* boundaries: `(:: f int)` for
`f :float` was emitting an implicit C value cast that truncated `1.5`
to `1`, instead of the bit-reinterpret that the TS2 EX_REINTERPRET
codegen was designed to emit.  Fix lives in `elab_ascribe`
(`src/compiler/elab_types.c`): when the source and target kinds are
distinct same-size scalars (any `:int`/`:float`/`:i32`/`:f32`/...
cross-kind pair where `type_size_bytes` matches), produce an
`EX_REINTERPRET` node rather than the carrier-erasing `EX_ASCRIBE`
node.  Otherwise behaviour is unchanged.  Fixture:
`tests/fixtures/typed-slots/ascribe-reinterpret/` -- exercises
`float`↔`int`, `bool`↔`int`, and `i32`↔`f32` round trips.  Container
boundary reinterprets are not needed in addition because aggregate
struct-apps already preserve their concrete type across polymorphic
boundaries via the GS5 instantiation work; the previous concern
about "16-byte struct in 8-byte int64 slot" was a user-pattern issue
(chained Cons cells need a heap-pointer constructor, not a value
cast), separate from TS3.3.

**Worktree update (2026-05-26, TS4 slice):** the concrete-payload slice of
TS4 is now locked down via the `adt-float-payload` fixture
(`tests/fixtures/typed-slots/adt-float-payload/`).  Investigation
confirmed that `defdata` with concrete primitive field types (e.g.
`(defdata MaybeF (JustF :float) (NothingF))`) already monomorphises
correctly today through `emit_module.c:658-740`: the `tur_adt_MaybeF`
typedef emits `struct { double _0; } JustF`, `ctor_JustF` takes a
`double` arg directly, and the `match` destructure binds `double x`
without any user-side bit-cast.  Polymorphic ADTs declared as
`(defdata Maybe [a] (Just a) ...)` still carry `a` on the legacy
`int64_t` carrier (see `elab_structs.c:1184` -- the TY_TYVAR field
explicitly resolves to `TY_INT` at the runtime layer), so a `:float`
payload through a polymorphic position still needs the TS3.3 ascribe
reinterpret pattern at the user-visible boundary.  Per-instance ADT
monomorphisation (`tur_adt_Maybe__float`, `ctor_Just__float`, match
specialization) mirrors the GS5 struct-app work in scope and is broken
out as a follow-up:
[`upcoming/typed-slots-ts4-poly-adt-plan.md`](upcoming/typed-slots-ts4-poly-adt-plan.md).
All 17 typed-slots fixtures pass; `tests/run.sh` regression unchanged at
884 passed / 110 failed (110 pre-existing per KB-006).

## Progress checklist

- [x] TS1 — Typed thunk ABI
- [x] TS2 — Reinterpret coercion node
- [x] TS3 — Typed primitive containers (TS3.1–TS3.4 landed; Vec deferred to TS3b; inactive-payload Option/Result constructors and KB-015 specialization-cache bug noted)
- [x] TS4 — Typed ADT payloads (concrete-payload slice; polymorphic-ADT monomorphisation deferred — see [`typed-slots-ts4-poly-adt-plan.md`](upcoming/typed-slots-ts4-poly-adt-plan.md))
- [x] TS5 — HKT helpers use TS1 tur_poly_fn_t protocol; `__cons-fmap` added; `fmap-float-list` fixture passes
- [x] TS6 — Signal spice migration complete; `defalias Sample :int` deleted; all C unions removed

---

## Phase plan

Each phase is independently shippable and leaves the tree green
(`just test` passes, signal spice continues to typecheck under its
current `defalias`-based scheme).

### Phase TS1 — Typed thunk ABI

**Goal.** When a closure's argument and return types are concrete
primitives, emit a thunk whose C signature uses those types directly.
The fat-closure struct gains a per-signature `__fn` type.

**Compiler changes.**

- `src/compiler/emit_module.c:727` — keep `tur_poly_fn_t` (still used
  for genuinely polymorphic positions) and add a *typedef factory*
  that emits a unique `typedef ... tur_thunk_<sig>_t;` for each
  distinct concrete signature seen during this compilation unit.
  Cache the typedefs so repeated `:float -> :float` lambdas reuse
  one name.
- `src/compiler/emit_fns.c:60-71` — when all captures are concrete
  *and* the fn's arg/ret are concrete, emit
  `struct { tur_thunk_<sig>_t __fn; <typed captures> }`.  When
  anything is polymorphic, fall back to today's layout.
- `src/compiler/emit_fns.c:74-131` — emit the thunk body with native
  parameter types when concrete; keep `int64_t` carrier when
  polymorphic.
- `src/compiler/emit_expr.c:1486` — when emitting `fat->__fn = ...`,
  emit the cast through the right typedef.
- `src/compiler/emit_expr.c:1105-1137, 1154-1179` — at the
  invocation cast, use the same typedef.  These sites already have
  the `Type *` of every arg, so the cast becomes precise.

**Compatibility.**  Polymorphic captures, polymorphic args, or
polymorphic returns keep today's `int64_t`-carrier ABI.  HKT helpers
that currently rely on `int64_t(*)(void*, int64_t)` continue to work
unchanged because the fallback ABI is unchanged.

**Acceptance test.**  Add `tests/fixtures/typed-slots/float-closure.tur`:

```turmeric
(defn make-add [a : float] (fn [b : float] : float {a + b}))
(defn test [] : float ((make-add 1.5) 2.25))   ; => 3.75
```

`tur emit-c` on this fixture must show a `double(*)(void*, double)`
thunk signature and **no** `union` in the emitted body.

### Phase TS2 — Reinterpret coercion node

**Goal.** Make bit-cast between `int64_t` carrier and concrete
primitive an explicit IR node, so phase TS3+ can rely on the compiler
to emit the cast at exactly the polymorphic↔concrete boundary
(instead of users writing unions).

**Compiler changes.**

- `src/compiler/types.h` — add `EX_REINTERPRET` (source type, target
  type, expression).  Legal only between same-size scalar types.
- `src/compiler/elab_*.c` — never produced by user syntax in TS2;
  produced only by the compiler when an `:int` carrier must flow
  into a typed slot or vice versa.
- `src/compiler/emit_expr.c` — emit as
  `((union { <src> s; <dst> d; }){.s = <expr>}).d` for size-equal
  primitives.

**Acceptance test.**  Unit-test the codegen for a synthetic IR tree
that wraps a `double` into the carrier and unwraps it.  No user-
visible surface yet.

### Phase TS3 — Typed primitive containers

**Goal.** `Cons<:float>`, `Option<:float>`, `Result<:float, :cstr>`,
`Pair<:int, :float>`, etc. store their payload as the actual C type
when the element type is a concrete primitive.

**Stdlib changes.**

- `stdlib/list.tur:20` — replace `(defstruct Cons [value :int next :int])`
  with the parameterised form already used in `stdlib/tlist.tur`,
  and migrate `list.tur` consumers to `tlist`.  (Or: introduce
  `Cons<A>` directly in `list.tur` and let TS3 codegen specialise
  layout per `A`.)
- `stdlib/option.tur:38-42`, `stdlib/result.tur`, `stdlib/pair.tur:19`,
  `stdlib/vec.tur` — same parameterisation.

**Compiler changes.**

- Struct-field codegen already uses `type_c_name()`.  Extend the
  monomorphisation machinery so that `Cons<:float>` emits
  `struct { double value; int64_t next; }` instead of the
  type-erased `int64_t value` form.
- At the boundary where a generic container flows into a
  polymorphic context (e.g. a `Cons<:float>` is passed to a
  function typed `Cons<a>`), insert a TS2 reinterpret on field
  access.

**Acceptance test.**  `tests/fixtures/typed-slots/cons-float.tur`:

```turmeric
(let [xs (cons 1.5 (cons 2.5 (nil-value)))]
  (head xs))   ; => 1.5, emitted as direct `double` field access
```

### Phase TS4 — Typed ADT payloads

**Goal.** `(defdata Maybe [a] (Just a) Nothing)` with concrete `a`
emits a payload field of the right C type.

**Compiler changes.**

- `src/compiler/emit_expr.c:2713-2748` — `tur_tagged_t` currently
  stores payloads as `int64_t`.  When the variant's payload type
  is monomorphised to a concrete primitive, emit a typed payload
  union per variant.
- Insert TS2 reinterprets where a typed ADT flows into a
  polymorphic position.

### Phase TS5 — HKT helpers use reinterpret

**Goal.** The hand-written `int64_t(*)(int64_t)` casts in
`__functor_list_fmap`, `cata`, `free-bind`, etc. become
compiler-inserted TS2 reinterprets at the polymorphic boundary,
rather than ad-hoc unions in stdlib.

**Stdlib changes.**

- `stdlib/list.tur:204-228, 313-344`, `stdlib/fix.tur:70-76`,
  `stdlib/free.tur:73-84` — replace inline-C bit-cast/thunk-extract
  with the TS2-emitted reinterpret + TS1 typed thunk call.

**Compatibility.** This phase is the riskiest: HKT is the seam
where typed and untyped worlds meet.  Land it behind a
per-instance flag if necessary.

### Phase TS6 — Signal spice migration

**Goal.** Drop `(defalias Sample :int)` and the unions in
`turmeric-spices/spices/signal/src/signal/{dsp,synth,envelope}.tur`.

**Spice changes.**

- Annotate all `Sample`-typed params as `:float`.
- Replace inline-C bit-casts with plain expressions:
  ```turmeric
  ; before
  (defn sine [freq phase] (let [fv freq pv phase] (fn [sig] (fn [t]
    ```c
    double f = ((union { int64_t i; double d; }){.i = fv}).d;
    ...
    ```))))

  ; after
  (defn sine [freq : float phase : float]
    (let [fv freq pv phase]
      (fn [sig] (fn [t : float] : float
        {sin({2.0 * 3.14159265358979323846 * fv * t} + pv)}))))
  ```
- Delete `requires.typecheck-skip` (when present) and confirm CI is
  green.
- Remove the `defalias Sample :int` declaration; leave a one-line
  comment noting the historical bit-cast pattern.

---

## File change summary

| Phase | File | Change |
|---|---|---|
| TS1 | `src/compiler/emit_module.c` | Add typed-thunk typedef factory |
| TS1 | `src/compiler/emit_fns.c` | Emit typed thunk struct + signature when concrete |
| TS1 | `src/compiler/emit_expr.c` | Cast `__fn` through typed typedef at emit + call sites |
| TS2 | `src/compiler/types.h` | Add `EX_REINTERPRET` |
| TS2 | `src/compiler/emit_expr.c` | Emit reinterpret as size-equal union |
| TS3 | `stdlib/{list,option,result,pair,vec}.tur` | Parameterise containers over `[A]` |
| TS3 | `src/compiler/emit_expr.c` (struct field codegen) | Monomorphise field layout per concrete `A` |
| TS3 | Compiler boundary code | Insert TS2 reinterprets at generic↔concrete container boundaries |
| TS4 | `src/compiler/emit_expr.c` (tagged-union codegen) | Typed ADT payload monomorphisation |
| TS5 | `stdlib/{list,fix,free}.tur` | Replace hand-written bit-casts with TS2 reinterprets |
| TS6 | `../turmeric-spices/spices/signal/src/signal/*.tur` | Drop `defalias Sample :int`, retype as `:float`, remove unions |

---

## Test plan

### Per-phase fixtures (`tests/fixtures/typed-slots/`)

| Phase | Fixture | Asserts |
|---|---|---|
| TS1 | `float-closure.tur` | `tur run` returns `3.75`; emitted C has `double(*)(void*, double)` thunk and no `union` |
| TS1 | `int-closure-still-works.tur` | Existing `:int -> :int` closures keep their ABI |
| TS2 | (compiler unit test) | Synthetic reinterpret IR emits well-formed C |
| TS3 | `cons-float.tur` | `(head (cons 1.5 ...))` returns `1.5`; field access is direct `double` read |
| TS3 | `option-float.tur` | `(some-val 2.5)` round-trips a float without bit-cast |
| TS3 | `polymorphic-cons-boundary.tur` | A `Cons<:float>` passed through a `Cons<a>`-typed fn round-trips via reinterpret |
| TS4 | `adt-float-payload.tur` | `(Just 1.5)` payload is `double` in emitted struct |
| TS5 | `fmap-float-list.tur` | `(fmap (fn [x :float] :float {x * 2.0}) (cons 1.5 ...))` produces correct doubles |
| TS6 | (signal spice) | All four `tur check` calls under `spices/signal/` exit 0 without `defalias`, without unions |

### Regression coverage

- Full `just test` must pass at the end of every phase.
- HKT/Functor/Monad fixtures continue to work — TS5 is the only
  phase that touches them, and it must preserve every existing
  semantics.
- Sized numerics (`stdlib/sized-buf.tur`) unchanged.
- WASM build (`just wasm`) regenerates without changes to the public
  JS interface.

---

## Resolved design decisions

*(Resolved 2026-05-26 — see commit log for discussion.)*

- **Per-compilation-unit thunk typedef collisions — resolved: hash-named, file-scope typedefs.**
  Name typedefs after a stable structural hash of the signature
  (`tur_thunk_<hash>_t`).  Emit at file scope, not `static` (C typedefs
  can't be `static`-qualified).  Under C11, redeclaring the same typedef
  in another TU is harmless as long as it names the same type — the hash
  guarantees that.  If two TUs disagree on the type behind the same hash,
  that is a real bug worth surfacing as a link-time error.
- **`tur_poly_fn_t` future — resolved: keep indefinitely.**
  It is the canonical "I don't know the signature" carrier shape; HKT,
  existentials, and polymorphic captures need it.  The
  `tur_thunk_<hash>_t` family is purely an *additional* set of concrete
  specialisations layered on top.  No retirement planned.
- **`Cons<:int>` migration — resolved: option (b), replace with
  parameterised form + one-release compat typedef.**
  Replace the legacy `(defstruct Cons [value :int next :int])` with the
  parameterised `Cons<A>` and have bare `Cons` (no args) desugar to
  `Cons<:int>` for source-compat.  The emitted C name for `Cons<:int>` is
  deterministically `Cons__int`; additionally emit
  `typedef Cons__int Cons;` for one release as an ABI compat shim, then
  remove.  Avoids the parallel-family trap of option (a).
- **`int32 ↔ float32` reinterpret — resolved: allow same-size scalar
  pairs across the int/float kind boundary.**
  TS2's size check looks at C-ABI size, not Turmeric kind, so `:f32 ↔ :i32`
  and `:f64 ↔ :i64` are both legal.  Cross-size pairs
  (e.g. `:f32 ↔ :i64`) remain rejected.  Codegen is identical to the
  same-kind case (size-equal union trick).
- **`Vec<:float>` realloc — resolved: gate Vec specialisation behind its
  own sub-phase TS3b.**
  Audit of `stdlib/vec.tur` (2026-05-26) shows *all eight* functions touch
  raw `int64_t` indexing or `sizeof(int64_t)` sizing — every load/store
  site, plus the realloc growth path in `vec-push!` and the comparator
  cast in `vec-eq?`.  Rewriting these to use `sizeof(A)` and `A*` indexing
  is mechanical but pervasive enough that bundling it with the
  Cons/Option/Pair/Result migration risks blocking the smaller wins.  Land
  TS3a (Cons/Option/Pair/Result) first; tackle Vec in TS3b.

---

## Work plan

- [x] TS1.1 — Typed-thunk typedef factory in `emit_module.c`
- [x] TS1.2 — Per-(arg,ret) thunk emission in `emit_fns.c`
- [x] TS1.3 — Update closure-build and invocation casts in `emit_expr.c`
- [x] TS1.4 — `float-closure.tur` fixture + emitted-C assertion
- [x] TS2.1 — Add `EX_REINTERPRET` IR node + codegen
- [x] TS2.2 — Compiler unit test for synthetic reinterpret
- [x] TS3.1 — Parameterise stdlib containers over `[A]` (Cons/Option/Pair/Result; Vec deferred to TS3b; carrier constructors `some`/`none`/`ok`/`err` deferred pending inactive-payload representation)
- [x] TS3.2 — Monomorphised struct field layout per `A` (nested concrete struct-app layouts already correct; sized-numeric short-form aliases fixed; bidirectional literal inference still open)
- [x] TS3.3 — Insert TS2 reinterprets at container boundaries (`::` now emits EX_REINTERPRET for distinct same-size scalar kinds; aggregate boundaries already preserved by GS5 instantiation)
- [x] TS3.4 — Container fixtures (`cons-float`, `option-float`, `polymorphic-cons-boundary` landed; uses `.head`/`.value` direct access to sidestep KB-015)
- [x] TS4.1 — Typed ADT payload codegen (concrete primitive fields only; polymorphic ADT monomorphisation deferred)
- [x] TS4.2 — ADT fixture (`adt-float-payload`)
- [x] TS5.1 -- Migrate `list`, `fix`, `free` HKT helpers to TS1 tur_poly_fn_t protocol; added `__cons-fmap`
- [x] TS5.2 -- Acceptance fixture `fmap-float-list` passes (int list fmap + float bit-cast list fmap)
- [x] TS6.1 -- Migrated signal spice: deleted `defalias Sample :int`; retyped DSP params as `:float`; removed all C union bit-casts (replaced with memcpy / TS2 `::` reinterpret)
- [x] TS6.2 -- All four `tur check` exits 0: `dsp.tur`, `envelope.tur`, `synth.tur` clean; `arrow_tests.tur` clean (stdlib `arrow.tur:326` error is pre-existing, unrelated to TS6)

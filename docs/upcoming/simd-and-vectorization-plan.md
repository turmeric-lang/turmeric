# SIMD and vectorization -- typed numeric arrays, then explicit vector types

> **Status: N0, N1 and N2 (approaches A + B) LANDED 2026-10-28** (`stdlib/numvec.tur`;
> each phase's "As built" note in section 3); N2's `-fvect-cost-model=cheap`
> question decided (no) 2026-10-29; the generic-type question (open question
> 1) re-measured 2026-10-29 -- recommendation: a generated class over the five
> types, not a `(NumVec T)` rewrite (section 3.1, "2026-10-29") -- **built
> 2026-10-29** (3.1, "As built: NumArray"). **S0 built 2026-10-29** as the
> `tur-simd` spice (S0, "As built"; its findings reshape S1's type list).
> **S1 GRADUATED 2026-10-30** -- `(load "stdlib/simd.tur")` needs no flag
> (S1, "S1 graduated"). **S1a built 2026-10-29** (S1, "As built: S1a");
> **S1b** (masks, comparisons, `select`, `shuffle`, conversions, no 32-byte
> types, the generic-position restriction lifted as measured unneeded)
> 2026-10-30 (S1, "As built: S1b"). N2
> approaches D/E not started.
> **Open questions reviewed 2026-10-29** (section 6): the class is `NumArray`
> with `na-` methods (Q2, decided); D and E deferred (Q3); wasm `-msimd128`
> measured at 1.0-29x on the kernels, bit-identical results, and turned
> on by default (Q5, done); multiversioning deferred (Q6); the store-loop
> gap is the new Q7, built 2026-10-29: views, `P-map!`, fixed-operation
> kernels and no panic check after a call that cannot panic.
> Proposed 2026-10-24. Written in response to
> "are (1) SIMD functions for manual calling and (2) vectorization
> optimizations, likely in MIR, useful or viable for Turmeric?", with Racket's
> `flvector` raised as a related idea. Short answer: (1) yes, as an
> experiment-gated set of vector types lowered to GCC/Clang vector
> extensions; (2) yes, but in the C the emitter writes, not in MIR. Both rest
> on a `flvector`-style typed numeric array, which is phase N0.
> **2026-10-24 update:** the generic-vs-five-types question was prototyped
> (section 3.1; author leans generic with `defalias` names, one compiler
> bug in the way), N2's options were expanded to seven (A-G), and wasm
> stays open.
> **Type:** Stdlib / compiler (emitter, elaborator) / driver
> **Depends on:** the sized primitives (`int32`, `uint8`, `float32`, ...;
> `src/compiler/elab_core.c:49`), the inline-C site discipline
> (`emit_direct_call_site_mode`, `src/compiler/emit_core.c:2026`), the
> experiments registry (`src/runtime/experiments.c`).
> **Related:** [`docs/archive/history/unboxing-and-monomorphization-plan.md`](../archive/history/unboxing-and-monomorphization-plan.md)
> and [`docs/archive/outstanding-followups-plan.md`](../archive/outstanding-followups-plan.md)
> (both name "SIMD / vector primitives" as a separate axis whose prerequisite
> is sized primitives -- those now exist),
> [`docs/guides/performance-guide.md`](../guides/performance-guide.md),
> [`docs/guides/value-representations-guide.md`](../guides/value-representations-guide.md),
> [`docs/guides/jit-guide.md`](../guides/jit-guide.md).

## 0. Summary

Five phases, each useful on its own and each landing in its own PR:

| Phase | What | Compiler work | Gate |
|---|---|---|---|
| **N0** | Typed numeric arrays: `F64Vec`, `F32Vec`, `I64Vec`, `I32Vec`, `U8Vec` | none (stdlib) | none -- new API surface, not a semantic change |
| **N1** | Bulk kernels over them (`sum`, `dot`, `axpy!`, `scale!`, `fill!`, `copy!`, `min`/`max`) as fixed-order C that gcc and clang vectorize at the default `-O2` | none (stdlib) | none |
| **N2** | An element-access path cheap enough that a Turmeric-written loop over an N0 array vectorizes | emitter / elaborator | none, unless it adds a form |
| **S0** | A spice prototyping explicit SIMD on inline-C vector extensions | none | none (it is a spice) |
| **S1** | Compiler-known vector types `f64x2`, `f64x4`, `f32x4`, `f32x8`, `i32x4`, `i64x2`, `u8x16` | elaborator, emitter, preamble, turi natives | `--enable=simd` |

Explicitly **not** in this plan: a vectorizer inside MIR, `-march=native` by
default, or `-ffast-math` anywhere global (section 5).

## 1. Findings

Measured 2026-10-24 on this repo at `289d688d`, gcc 13.3, x86-64 with
AVX2/AVX-512, Release `tur`.

### 1.1 Turmeric loops over `Vec` do not vectorize, even at `-O3`

A tail-recursive dot product over two `(Vec float)` and an integer sum over a
`(Vec int)`, through `tur emit-c` and `gcc -O3 -fopt-info-vec-missed`:

```
probe.c:11169:21: missed: not vectorized: control flow in loop.
probe.c:11191:21: missed: not vectorized: control flow in loop.
```

Each `vec-get` (`stdlib/vec.tur:108`) emits, per element:

1. `tur_site_push` / `tur_site_pop` -- two writes to a thread-local, because
   the inline-C body can `tur_panic` (`binding_panics_in_inline_c` ->
   `emit_direct_call_site_mode`, `src/compiler/emit_core.c:2026`);
2. the bounds check and `tur_panic` branch inside the body;
3. `if (tur_panicking) return ...;` after the call;
4. a `union { int64_t s; double d; }` reinterpretation of the stored word.

The control flow from (2) and (3) is what the vectorizer refuses. The same
loop written in C over `const double *` vectorizes at `-O3`.

### 1.2 Storage is not the problem; width and access are

`Vec float` already stores doubles unboxed: eight bytes each, contiguous, in
an `int64_t *data` slot array. So unlike Racket before `flvector`, Turmeric is
not paying an allocation per float. What it lacks is:

- **Narrow element types.** Every `Vec` slot is eight bytes, so a `float32`,
  `int32` or `uint8` array uses half, half, or an eighth of each SIMD
  register. Image, audio and ML-shaped data are all narrow.
- **A cheap access path** (1.1).
- **A layout the emitter can rely on.** `Vec A` is generic over a carrier
  word; nothing tells the emitter "this is a `double *`".

`SizedBuf` (`stdlib/sized-buf.tur`) and `SizedMatrix` are flat too, but are
`int64_t`-only and typed through a `:int` carrier.

### 1.3 Benchmark

Dot product of 4096 doubles, repeated 200,000 times (819M multiply-adds;
the arrays stay in L1/L2, so it is compute-bound). "Emitted shape" is a C
model of 1.1's four steps, not compiled Turmeric. All rows printed the same
result.

| version | `-O2` (default) | `-O3` (`--release`) | `-O3 -ffast-math` | `-O3 -march=native -ffast-math` |
|---|---|---|---|---|
| emitted shape | 2.53s | 2.45s | 2.44s | 2.61s |
| plain `double *` loop (N0+N1 shape) | 1.08s (2.3x) | 1.02s | 0.52s (4.8x) | 0.26s (~10x) |
| explicit `f64x4` vector extension (S1 shape) | 0.59s (4.3x) | 0.71s | 0.76s | 0.27s (~9.7x) |

What the table says:

- **Removing the per-element overhead is worth 2.3x before any SIMD.**
- **A float reduction needs permission to reassociate.** At plain `-O3`, gcc
  must keep the scalar addition order, so the dot product gains almost
  nothing. Only `-ffast-math` (or `-fassociative-math`) unlocks it.
- **Explicit SIMD gets 4.3x on today's default flags**, because writing four
  partial sums *is* the reassociation; the programmer chose it.
- **AVX2-width registers need `-march`.** Turmeric deliberately never sets
  it (`performance-guide.md`, "Build flags").

The gcc report also shows that the default `-O2` (gcc 12+'s "very cheap"
cost model) vectorizes none of the clean loops either, because their trip
count is not known at compile time. `--release` (`-O3`) does.

### 1.4 The engines

- **cc**: gcc/clang. GCC vector extensions and `<immintrin.h>` intrinsics both
  work in inline-C today (checked: a `vector_size(16)` multiply and an
  `_mm_mul_pd`, both print `18.775`).
- **`tur jit`** (c2mir): MIR's IR has scalar integer, float and block types
  only (`external/mir/mir.h:179`). c2mir ignores `vector_size`, so the typedef
  becomes a scalar and the program fails to compile (`excess elements in
  scalar initializer`); `<immintrin.h>` does not parse. In both cases the
  engine refuses and falls back to cc with TUR-W0070 -- safe, but no JIT.
  The emitted C can detect c2mir with `__MIRC__` (already used in the
  preamble, `src/compiler/emit_module.c:14695`).
- **turi** (interpreter): no SIMD; natives are scalar C.
- **wasm** (Emscripten): built without `-msimd128`
  (`src/driver/cmd_build.c:319`), so vector extensions lower to scalar code.

## 2. Goals and non-goals

Goals:

1. Numeric code over arrays can reach the speed of the equivalent C, on the
   cc engine, without inline-C.
2. Explicit SIMD is expressible in Turmeric with real types (no `:int`
   handles), portable across x86-64, arm64 and wasm from the same source.
3. All three engines give **the same answer** for every new operation,
   bit for bit. Speed may differ by engine; results may not.

Non-goals: a vectorizer in MIR; changing default C flags (`-march`,
`-ffast-math`); auto-vectorizing `Vec A`, HAMTs or lists; GPU.

## 3. Phases

### N0 -- typed numeric arrays (`flvector`-style)

Racket's `flvector` is a fixed-length, homogeneous, unboxed array of
float64 with its own fast operations. N0 is that, for each sized element type
worth having:

| Type | Element | C layout |
|---|---|---|
| `F64Vec` | `float` | `struct { int64_t len; double *data; }` |
| `F32Vec` | `float32` | `struct { int64_t len; float *data; }` |
| `I64Vec` | `int` | `struct { int64_t len; int64_t *data; }` |
| `I32Vec` | `int32` | `struct { int64_t len; int32_t *data; }` |
| `U8Vec` | `uint8` | `struct { int64_t len; uint8_t *data; }` |

- **Real types.** Each is a `defopaque` (or a `defstruct` with a typed
  pointer), never `:int` at the public surface (CLAUDE.md, "No Lazy `:int`
  Stand-Ins"). Whether the five share one parametric `(NumVec T)` or are
  five types is open question 1; section 3.1 has what a prototype found. The
  author leans generic, with honest names kept through `defalias`.
- **Fixed length**, like `flvector`. Growth stays `Vec`'s job; a fixed length
  lets the emitter and gcc trust `len` across a loop. `f64vec->vec` /
  `vec->f64vec` convert.
- **The data block is 32-byte aligned** (`aligned_alloc`), so S1 aligned
  loads are legal on it and gcc can skip its peeling prologue.
- **Surface per type** (shown for `F64Vec`): `f64vec-new n init`,
  `f64vec-of x ...`, `f64vec-len`, `f64vec-ref`, `f64vec-set!`,
  `f64vec-copy`, `f64vec->vec`, `vec->f64vec`, `f64vec-show`. `f64vec-ref`
  keeps a bounds check and panics like `vec-get` does today; N2 is where the
  loop-friendly path comes from.
- **Region hooks.** An element store writes a number, never a pointer, so
  `f64vec-set!` needs no `TUR_REGION_NOTE` (CLAUDE.md, "Region Store Hooks"
  covers "a caller's word that can outlive a bracket" -- a pointer). Say so in
  a comment on each setter so the next reader does not "fix" it.
- **Ownership.** Drop glue frees `data` and the header; no element drop.
- **Interpreter.** turi gets natives over the same struct, so the three engines
  share one layout.
- **Fixtures** (ASCII, floats probed with fractional values like `7.1` and
  `3.25` per CLAUDE.md): construction, ref/set, out-of-bounds panic and its
  site on both engines (`expected.stderr`), conversions, and a
  `requires.leak-check` fixture for the drop glue.

**As built (2026-10-28).** `stdlib/numvec.tur`, generated by
`tools/gen-numvec.py` (ctest `tur_numvec_generated` keeps the two in step);
`(load "stdlib/numvec.tur")`. What differs from the sketch above:

- **Five types, not generic.** Section 3.1's recommendation waits on the
  associated-type report, so this is the five-type subset it describes;
  the per-type names (`f64vec-ref`, ...) become the generic instances' bodies
  later and stay valid as aliases.
- **Header is `{ int64_t len; T *data; void *raw; }`.** `raw` is the malloc
  block `data` is carved from (aligned up to 32 bytes by hand rather than
  `aligned_alloc`, which the emitted C's `-std=gnu99` does not declare and
  which Windows lacks); `free` is given `raw`.
- **Ownership is real drop glue.** Each type is `defopaque ^affine` with a
  `Drop` instance, so a let-bound array is freed at scope exit and the
  accessors take `^borrow`. Two compiler fixes came with it: a temporary
  passed straight to a `^borrow` parameter is now freed after the call when
  the call returns a scalar and no earlier argument has an effect
  (`borrowed-drop-temporary-released`; `(f64vec-len (f64vec-new 4 0.0))` used
  to leak), and a closure capturing a `^borrow` parameter no longer frees it
  (`closure-borrowed-drop-capture-not-dropped`). What remains, by design:
  a temporary handed to a non-scalar-returning call (`P-copy`, `P-show`) is
  not freed -- its result might point into it -- so the module header says
  to bind it first
  ([borrowed-drop-temporary-leaks-outside-the-hoist](../reported/borrowed-drop-temporary-leaks-outside-the-hoist.md));
  a closure capturing an OWNED array takes it over, so the scope cannot also
  use it; and partially applying a setter spends the array
  ([partial-application-of-borrow-param-consumes-affine-arg](../reported/partial-application-of-borrow-param-consumes-affine-arg.md)).
- **Every op that touches the header is an inline-C body of its own** --
  `new`, `free`, `copy`, `len`, `ref`, `set!` -- never a Turmeric wrapper over
  a raw body, so a panic (an index out of range, a negative length) names the
  caller's line on every engine, and `(u8vec-set! a)` curries like any
  function. The narrow element types take narrow parameters (`init :
  float32`, `x : uint8`), which drew TUR-W0037 on every program loading the
  module; the warning now exempts stdlib bodies (advice to a body's author,
  not to its users).
- **`P-of` is a template macro over one procedural macro.** `P-of` mints the
  array's binder with `(gensym)` -- fresh against the program's names -- and
  hands it to `numvec-of__`, a `defmacro*` that counts the elements and emits
  one flat `(let [v (new n zero)] (do (set! v 0 x0) ... v))`. A per-element
  template recursion stops at the 256-deep expansion limit, and
  `syntax-gensym` is fresh only against the macro-time env
  ([syntax-gensym-not-fresh-against-program-names](../archive/syntax-gensym-not-fresh-against-program-names.md)).
  A binding initialised by that `let` was at first never dropped; the
  compiler now drops it
  ([affine-let-init-yielding-moved-local-skips-scope-drop](../archive/affine-let-init-yielding-moved-local-skips-scope-drop.md)).
- **`P-show` returns an owned `String`** `"#f64vec[1.5 2.5]"`; there is no
  `Show` instance because `show` would consume an affine array.
- **turi** has natives for every inline-C body (`NUMVEC_NATIVES`, rows in
  `native_caps.c`), so the whole module runs under `--interpret` without the
  inline-C JIT, and `tests/run-turi.sh` lists it as fully native-backed. Each
  element type has its own handle kind (`TURI_HK_F64VEC`, ...), so a sandboxed
  program cannot re-type a `U8Vec` handle as an `I64Vec` and index it at the
  wrong stride (`tests/turi/sandbox-eval.c`, `forgery/numvec-width`).
  Fixtures: `numvec-*` and `errors/numvec-*`; all pass under `run.sh`,
  `run-turi.sh`, `run-jit.sh`, and `run-leak-check.sh`.

### 3.1 N0's type shape -- generic or five types (prototyped 2026-10-24)

Whatever the surface, the C underneath is five sets of loops: a `float32`
kernel walks a `float *`, a `float64` one a `double *`. The question is only
how Turmeric names and reaches them. A prototype against `289d688d`:

**What works.** One `(defopaque NumVec [t] :int)`, a class keyed on the
*whole container type*, one instance per element type with an inline-C
body over the real C pointer, and generic code over the class:

~~~turmeric
(defopaque NumVec [t] :int)
(defalias F64Vec (NumVec float))
(defalias F32Vec (NumVec float32))
(defclass NumSum [v] (nv-sum [v] : float))
(definstance NumSum [(NumVec float)]   (nv-sum [v] : float ```c /* double * loop */ ```))
(definstance NumSum [(NumVec float32)] (nv-sum [v] : float ```c /* float * loop */ ```))
(defn total [V] [(NumSum V)] [v : V] : float (nv-sum v))
~~~

`total` gets one specialization per instance (`total__spec__double_int64_t`
and `..._int64_t__h1`) that calls that instance's C loop directly, so there is
no runtime dispatch and gcc sees each loop concretely. The aliases are
transparent: `a : F64Vec` and `a : (NumVec float)` are the same type. Correct
on cc and `tur jit`.

**What does not work today:**

1. **A class keyed on the element** -- `(defclass NumElem [t] (ne-sum [v :
   (NumVec t)] : t))` -- refuses at every call, even at a concrete type:
   `TUR-E0020: ambiguous method dispatch ... receiver type is erased
   (int64_t)`. Dispatch reads the first parameter's type, and the opaque's
   carrier hides the `t` inside it. So instances must be keyed on
   `(NumVec float)`, not on `float`.
2. **An element-typed result through an associated type** -- `(type Elem :
   Type)` and `(nv-sum [v] : Elem)`, so `nv-sum` on a `(NumVec float32)` returns
   a `float32` -- fails inside a generic with `no instance 'NumSum tyvar'`,
   though it works with plain (non-applied) opaques. Filed as
   [assoc-type-method-at-applied-opaque-instance-no-instance](../archive/assoc-type-method-at-applied-opaque-instance-no-instance.md).
   Until it is fixed, a generic `nv-sum` or `nv-ref` cannot return the honest
   element type, which most of the API needs.
3. **No dispatch on the result type.** A constructor that takes only a length
   cannot pick an instance. Constructors either take an element first
   (`(nv-new 7.1 n)` dispatches on `7.1`'s type) or stay per-type
   (`f64vec-new`).
4. **The interpreter** refuses an inline-C defn returning an opaque
   (`the REPL JIT returns only int-class, float, cstr, bool and unit
   results`). N0 needs turi natives whichever shape is chosen.

**The trade, as it stands:**

| | Generic `(NumVec T)` + aliases | Five types |
|---|---|---|
| Names users see | `F64Vec` etc. via `defalias`, plus `(NumVec T)` in generic code | `F64Vec` etc. |
| Writing a generic algorithm (`mean`, `normalize!`) | once, over the class | needs a class anyway, keyed on five types |
| Function names | one family (`nv-ref`, `nv-sum`), plus per-type constructors | five families (`f64vec-ref`, `f32vec-ref`, ...) |
| C written | five instances | five sets of functions |
| Blocked on | item 2 for element-typed results | nothing |
| Extensible by users | yes -- anyone can add an instance (a feature, or a hole: a `NumVec Complex` would need its own kernels) | no |

Recommendation: **generic, with `defalias` names**, starting after item 2 is
fixed (a compiler fix, not a design change). If N0 has to start before that,
the five-type version is a strict subset: its per-type functions become the
generic instances' bodies later, and the names stay valid as aliases.

**2026-10-29: item 2 fixed, and the question re-measured against the shipped
five types.** Item 2 was narrower than it read: only a method with an
UNTYPED parameter (`(nv-sum [v] : Elem)`) went down the failing path; the
usual spelling, `(nv-sum [s : v] : Elem)`, already worked
([archived report](../archive/assoc-type-method-at-applied-opaque-instance-no-instance.md)).
A prototype of the cheaper shape -- **keep `F64Vec` ... `U8Vec`, add one class
keyed on them with `(type Elem : Type)`** -- then found three compiler bugs,
all fixed the same day:

- a float associated-type value held in a generic's body (a `let` of a
  method result, a `^mut`, an `if`, a generic calling a generic) was
  value-converted to the int carrier: `1.25` printed `4.94066e-324`
  (`assoc-type-projection-in-generic-body`);
- a class method's `^borrow` was dropped by the parser, so `(na-len a)` spent
  the array and could not be called twice (`class-method-borrow-param`);
- `(unsafe ...)` colored its function and every caller for the CPS backend,
  so a generic loop over an unchecked class method ran 5x slower than the
  same loop inline (`cps-coloring-unsafe-transparent`).

Measured after them, 100,000 sums of a 4096-element `I32Vec`: the generic
loop over the class 0.074s at `-O3` against 0.075s inline (it vectorizes);
0.13s against 0.066s at `-O2`, where the instance-method layer is not
inlined. cc, turi and `tur jit` agree on every probe.

So a generic layer is sensible, in this shape: **a `NumArray` class
generated by `tools/gen-numvec.py` beside the five types**, its instances
delegating to the per-type functions, an associated `Elem`, and `^borrow`
methods. It gives one generic algorithm per operation, honest element
types and no runtime cost at `--release`, changes no existing type or name,
and needs no new turi natives. Converting the types themselves to
`(NumVec T)` + `defalias` buys nothing more -- `(Elem V)` already names the
element in a signature -- and costs type-identity churn (the turi handle
kinds, a `Drop` per applied type, every fixture's types). Left, for whoever
writes generic code over it:

- **A lambda inside a constrained generic calls one instance for every
  spec** on the compiled engines
  ([closure-in-constrained-generic-dispatches-one-instance](../reported/closure-in-constrained-generic-dispatches-one-instance.md),
  high, pre-existing). The layer's own methods take no lambdas, and
  `P-for` / `P-fold` / `P-build` expand to loops, not closures -- but a
  generic `na-map!` taking a function, or user generic code with a local
  lambda, would hit it. Fix it before shipping anything higher-order.
- Arithmetic and comparison on an `(Elem V)` value need `(Num (Elem V))` /
  `(Ord (Elem V))` in the generic's constraints; then `{x + y}` and `(< x y)`
  work (fixed 2026-10-29,
  [operator-on-constrained-tyvar-refused](../archive/operator-on-constrained-tyvar-refused.md)).
  A literal at `(Elem V)` is refused
  ([literal-at-associated-type-unchecked](../archive/literal-at-associated-type-unchecked.md));
  conversions and constants go through `NumCast` (stdlib/typeclass.tur, added
  2026-10-29): under `(NumCast (Elem V))`, `(as float x)` reads as
  `(to-float x)`, and a constant is `(from-int 0)` / `(from-float 0.5)`.
- An instance body does not inherit the class method's `#fx{Unsafe}`; it
  writes its own `(unsafe ...)`.
- Constructors stay per type (item 3).

**As built: NumArray (2026-10-29).** Generated by `tools/gen-numvec.py` at
the end of `stdlib/numvec.tur`: the class, an instance per type, and three
generic loops.

- **Three associated types:** `Elem`; `Sum`, what `na-sum` returns (`int`
  for the integer types, so a class keyed on `Elem` alone could not type it);
  and `View`, so a generic store loop goes through views as Q7's do.
- **Methods** -- every operation all five types have: `na-len`, `na-ref`,
  `na-set!`, `na-ref-unchecked`, `na-set-unchecked!`, `na-view`,
  `na-view-ref`, `na-view-set!`, `na-fill!`, `na-copy!`, `na-add!`,
  `na-sub!`, `na-add-scalar!`, `na-sum`, `na-min`, `na-max`, `na-copy`,
  `na-free`, `na-show`. Not `scale!`, `axpy!`, `dot`, `mul!`: no U8Vec
  version. **Loops:** `na-for`, `na-fold`, `na-map!`, the per-type macros'
  shapes over the class.
- **Four compiler changes it needed:**
  1. A generic call whose receiver has the class's associated type --
     `(na-view-set! w ...)` with `w : (View V)` -- dispatched every
     specialization to the representative instance (an int32 store into a
     double array, silently). `emit_dispatch_tyvar` now maps such a receiver
     to `V`, as it already did for a result
     (`assoc-typed-receiver-dispatch`).
  2. A method argument at an associated type now gets it as its expected type
     -- `(na-add-scalar! c (from-int 1))` infers `(Elem V)` as an ascription
     would have.
  3. The post-call panic check is now omitted after any call that cannot
     panic, not only a leaf inline-C one: a Turmeric body made of variables,
     literals, arithmetic, `unsafe` and calls that cannot panic counts too,
     and a class-method call is judged by the instance method its
     specialization resolves to. Without it every instance-method call in a
     generic loop kept its check, and none vectorized.
  4. (Not a change: the elaborator cannot yet call a method whose only
     class-typed parameter is an associated type, so the view methods take
     the array too and ignore it --
     [assoc-typed-receiver-not-a-dispatch-arg](../reported/assoc-typed-receiver-not-a-dispatch-arg.md).)
- **Vectorized:** `probe-generic-map` (`na-map!`, in place) in all three of
  its specializations and `probe-generic-fold` (`na-fold`), at gcc `-O3` and
  clang `-O2` -- `tests/check-numvec-vectorized.py`, 91 of 91.
- **Known gap:** a generic-typed local is never dropped, so `na-copy`'s
  result must be released with `na-free`
  ([generic-affine-local-never-dropped](../reported/generic-affine-local-never-dropped.md)).
- `tests/fixtures/numvec-class` calls every method through generics on all
  five types; cc, `tur jit` and turi agree, leak-checked.

### N1 -- bulk kernels

Stdlib functions whose bodies are plain C loops over `restrict` pointers, so
gcc vectorizes them at `-O3` without help:

- `f64vec-fill!`, `f64vec-copy!` (with a length check up front), `f64vec-scale!`,
  `f64vec-add!`, `f64vec-axpy!` (`y += a*x`), `f64vec-min`, `f64vec-max`,
  `f64vec-dot`, `f64vec-sum`; the same for the other element types where they
  make sense (no `dot` on `U8Vec`; `sum` on `U8Vec` widens to `int`).
- **Reductions have one defined order, the same on every engine and ISA.**
  The kernel accumulates into **eight interleaved partial sums** (lane `k`
  takes elements `i` with `i mod 8 = k`), then adds them in a fixed tree.
  This is the reassociation `-ffast-math` would have made, but chosen by
  the library, written in plain C, and identical under cc, c2mir, turi,
  `-march=native`, and wasm. It gets the vectorized speed at `-O3` with no
  fast-math flag, and it is reproducible. A strict left-to-right
  `f64vec-sum-ordered` sits beside it for callers who need the scalar answer.
  Eight covers AVX-512 with `double` and AVX2 with `float`.
- **Bounds checks happen once per call, outside the loop.** A kernel that
  panics does so before its loop starts, so the loop body has no control flow.
- **A vectorization check.** A small script (gcc-only; clang has
  `-Rpass=loop-vectorize` if wanted later) emits the kernels, compiles them
  with `-O3 -fopt-info-vec-optimized`, and fails if a listed kernel's loop is
  not reported as vectorized. Registered as a ctest, run on the Linux CI
  leg. Without it, a later preamble change could quietly put control flow
  back in a kernel.
- **Benchmark.** `benchmarks/bench-f64vec-dot.tur` beside a C baseline, in the
  style of `fmap-vec.tur` / `fmap-vec-baseline.c`.

**As built: N1 (2026-10-28).** Every kernel above exists for each type it
makes sense for: `fill!`, `copy!`, `add!`, `sum`, `min`, `max` everywhere;
`scale!`, `axpy!`, `dot` except on `U8Vec`; `sum-ordered` on the float types.
Choices made on the way:

- **The kernel C is written once**, in `tools/gen-numvec.py`, and becomes
  both the stdlib inline-C body and the interpreter's native
  (`src/turi/numvec_kernels.inc`, generated), so the engines run the same
  arithmetic in the same order. `numvec-kernels` prints every float result's
  bit pattern; cc, `tur jit` and turi agree bit for bit.
- **Eight lanes for every element width** (open question 4). It is the plan's
  own number, covers AVX2 for `float32` and AVX-512 for `float64`, and does
  not depend on the build machine. Sixteen for `float32` would only help
  AVX-512, which tur never targets without `-march`.
- **No FMA contraction.** A multiply and the add consuming it are separate
  statements (clang contracts within one expression), the emitted C is ISO
  `-std=c99` (gcc does not contract there), and the generated `.inc` turns
  contraction off for the interpreter's own build.
- **Integer arithmetic wraps**, computed in the unsigned type; `int32` and
  `uint8` sums widen to `int`, and an `int32` product is formed in 64 bits.
- **`min` / `max` propagate NaN**: the result is the first NaN by index.
  The lanes are a plain `x < m ? x : m` (one SSE `minsd`, no `-ffast-math`),
  which never lets a NaN in, and a running sum of the elements beside them
  is NaN when any element is; only then does a second pass find the first
  one, so a NaN-free array never pays for it. (The sum is also NaN for an
  array holding both infinities; the second pass then finds no NaN and the
  lanes answer -- `numvec-kernels` covers it.)
- **Fast at the default `-O2`, not only at `--release`** (2026-10-29). gcc's
  `-O2` vectorizes no loop whose trip count it cannot see, and only `-O3`
  fully unrolls an inner lane loop, so the first spelling of every kernel
  ran 2-5x slower in a default build. Each kernel's spelling is now chosen by
  measurement (`tools/gen-numvec.py`, "How a loop is spelled"): the eight
  steps written out, so `-O2` vectorizes the main loop itself; loads first
  then stores where two arrays are involved (`add!`, `axpy!`), since `-O2`
  cannot reorder a store past a load it cannot prove independent; 32-bit
  lanes flushed every 2^24 elements for the u8 sum; and the plain loop kept
  under clang for `add!` / `scale!` / `axpy!` / integer `min` / `max`, where
  clang vectorizes it already and runs the written-out body slower. Every
  spelling does the same arithmetic in the same order, so the result is
  unchanged (`numvec-kernels` still matches bit for bit). Measured over
  200,000 calls on 4096 elements, gcc 13 / clang 18, best of three:

  | kernel | gcc `-O2` | gcc `-O3` | clang `-O2` | clang `-O3` |
  |---|---|---|---|---|
  | `f64vec-sum` | 0.272 -> 0.068 | 0.073 -> 0.067 | 0.073 -> 0.067 | 0.081 -> 0.068 |
  | `f64vec-dot` | 0.308 -> 0.198 | 0.187 -> 0.179 | 0.321 -> 0.312 | 0.326 -> 0.314 |
  | `f64vec-min` | 0.470 -> 0.374 | 1.079 -> 0.372 | 0.336 -> 0.189 | 0.367 -> 0.188 |
  | `f32vec-min` | 0.292 -> 0.302 | 1.064 -> 0.366 | 0.377 -> 0.128 | 0.343 -> 0.125 |
  | `f64vec-add!` | 0.465 -> 0.183 | 0.211 -> 0.185 | 0.184 -> 0.180 | 0.186 -> 0.198 |
  | `f64vec-axpy!` | 0.417 -> 0.213 | 0.222 -> 0.205 | 0.209 -> 0.207 | 0.212 -> 0.207 |
  | `f64vec-scale!` | 0.264 -> 0.091 | 0.133 -> 0.084 | 0.124 -> 0.129 | 0.114 -> 0.097 |
  | `i64vec-sum` | 0.271 -> 0.070 | 0.143 -> 0.069 | 0.083 -> 0.068 | 0.079 -> 0.067 |
  | `i32vec-min` | 0.524 -> 0.135 | 0.215 -> 0.136 | 0.143 -> 0.147 | 0.139 -> 0.141 |
  | `u8vec-sum` | 0.264 -> 0.111 | 0.125 -> 0.114 | 0.130 -> 0.075 | 0.132 -> 0.072 |

  Note the old float `min` at `-O3`: 1.08s, slower than at `-O2`, because
  gcc unrolled the lane loop into branches -- `--release` made it worse.

  **The two left open, closed the same day.** Same harness, the column's
  number before -> after:

  | kernel | gcc `-O2` | gcc `-O3` | clang `-O2` | clang `-O3` |
  |---|---|---|---|---|
  | `f64vec-dot` | 0.191 -> 0.186 | 0.183 -> 0.180 | 0.308 -> 0.190 | 0.325 -> 0.180 |
  | `f32vec-dot` | 0.071 -> 0.070 | | 0.267 -> 0.075 | |
  | `f64vec-min` | 0.352 -> 0.249 | 0.355 -> 0.238 | 0.184 -> 0.125 | 0.191 -> 0.128 |
  | `f32vec-min` | 0.293 -> 0.239 | 0.357 -> 0.235 | 0.125 -> 0.124 | 0.123 -> 0.131 |

  - clang's `dot` was not the length check (moving the loop into a function
    of its own did not help either): with the accumulators in an array and a
    `s[i & 7]` tail, clang paired lane 0 with lane 4, 1 with 5, ..., and
    assembled each vector from two half loads (`movsd` + `movhpd`). Eight
    scalar accumulators and a tail written out as seven guarded steps
    (element `i + k` still lands in lane `k`) give it adjacent pairs.
  - gcc's float `min` / `max`: gcc vectorizes no `x < m ? x : m` reduction
    without `-ffast-math` ("unsupported use in stmt"), so the floor is eight
    scalar `minsd` chains, 0.16s with no NaN tracking at all. The old
    per-lane int NaN flag doubled that; the running sum costs one add per
    element and, kept to two chains, leaves the lanes their registers (eight
    chains of it spilled one `min` lane to the stack, 0.30s). It is the only
    tracker measured that helps clang as well. gcc stays ~2x behind clang
    here, which vectorizes the reduction.
- **The vectorization check** is `tests/check-numvec-vectorized.py` (ctest
  `tur_numvec_vectorized`), over `tests/vectorize/numvec-probe.tur`, x86-64
  with gcc only (it skips elsewhere). It compiles the probe at `-O2` and
  `-O3` and requires 53 reports of a vectorized main loop: at `-O2` on the
  main loop's own `for` line (the old spelling's inner lane loop is also
  "loop vectorized" at `-O2`, at its own line, yet runs at a quarter of the
  speed -- putting `f64vec-sum` back in that spelling fails the check). Not
  on the list, because SSE2 has no instruction for them: 64-bit integer
  multiply and min/max (`i64vec-scale!`, `-dot`, `-min`, `-max`) and
  `i32vec-dot`'s widening multiply; nor `-copy!` (a `memmove`); nor float
  `min` / `max`, which gcc does not vectorize without `-ffast-math` and whose
  scalar chains beat the spelling it did vectorize. At `-O3` only:
  `i64vec-axpy!` and the u8 sum / `min` / `max`.
  Two traps the probe has to avoid, both recorded in it: a constant length
  lets gcc unroll instead of vectorize, and code it estimates runs once (only
  from `main`) is optimized for size, which skips vectorization entirely.
- **Benchmark:** `benchmarks/f64vec-dot-results.md`. `f64vec-dot` over 4096
  doubles x 200,000 takes 0.18s in a default build and at `--release` alike,
  against 2.17s for the same dot product over `(Vec float)`: 12x, as fast as
  the same loop in C, and faster than a plain C loop without `-ffast-math`.

### N2 -- a loop-friendly element access path

N1 covers the common kernels; N2 is for the loop someone writes themselves.
The goal is: a Turmeric loop over an `F64Vec` emits C whose loop body has no
calls to a panicking body, no `tur_site_*` writes and no `tur_panicking`
checks, so gcc can vectorize it at `-O3`.

The site wrap and the panic check exist only because the callee's inline-C
body can panic. So an accessor whose body cannot panic needs neither, and
every approach below is a way to get a non-panicking access while keeping
it safe. They differ in who carries the proof that the index is in range.
Open question 3 picks; they are not exclusive.

| | Approach | Who proves the index | Index arithmetic (`i+1`, stencils) | Cost | Engines |
|---|---|---|---|---|---|
| A | Unchecked accessor | the programmer | anything | trivial | all |
| B | Element-iteration macro | the macro | none -- no index exposed | small | all |
| C | Higher-order kernels taking a lambda | the library | none | large | all, fast only on cc |
| D | Branded indices | the type checker | checked re-entry | medium | all |
| E | Refinement-typed index | the SMT solver | anything provable | large | all |
| F | Bounds-check hoisting in the compiler | the compiler | simple counted loops | large, fragile | all |
| G | Cheaper checked path | nobody -- still checked | anything | medium | all; no vectorization |

- **A. Unchecked accessor.** `nv-ref-unchecked` under `#fx{Unsafe}`, a body
  with no `tur_panic`. Trivial. Puts the proof on the programmer, so it is a
  building block for B and C rather than the main answer.
- **B. Element-iteration macro**, after Racket's `(for ([x (in-flvector v)])
  ...)`, which its `for` macro specializes into an unchecked loop.
  `(nv-for [x v] body)` binds `x` to each element; `(nv-for [x a y b] body)`
  walks two arrays together after one length check; `(nv-for-index [i x v]
  body)` also binds the index read-only; `(nv-build n [i] expr)` builds a new
  array like `for/flvector`. The macro expands to a tail-recursive local loop
  (already emitted as a C `goto` loop, section 1.1) whose reads use A. Safe,
  because no index the user can get wrong is ever in scope. Covers maps,
  zips, folds and reductions; does not cover stencils (`v[i-1] + v[i+1]`) or
  gathers.
- **C. Higher-order kernels** (`nv-map!`, `nv-fold`, `nv-zip-with!` taking a
  `(fn [float] float)`). The nicest surface, but only fast if the lambda is
  inlined into the C loop. An inline-C kernel calling a Turmeric closure per
  element is an indirect call per element, which neither vectorizes nor
  beats B. Needs specialization of the kernel per lambda; deferred until the
  emitter does that for other reasons.
- **D. Branded indices.** `(SizedBuf n)` already carries a phantom length
  `n`. Give `(NumVec n T)` the same, and an index type `(Ix n)` that only
  `(nv-indices v)` produces. `nv-ref` taking an `(Ix n)` cannot be out of
  range, so it is unchecked. `i+1` leaves the brand and comes back through a
  checked `(nv-ix v k) : (Option (Ix n))`. Safe without a solver; the brand
  makes signatures heavier, and it changes `NumVec`'s type, so decide before
  N0 ships.
- **E. Refinement-typed index.** `nv-ref` taking
  `#refine{i : int | 0 <= i < (nv-len v)}`, discharged by the existing solver
  (`src/compiler/refine_*.c`); where it cannot prove it, the call falls back
  to the checked version. Covers stencils and arbitrary arithmetic. The most
  work; the solver must reason about `nv-len` across a loop, which is what
  the `reflected-measures` experiment is for.
- **F. Bounds-check hoisting.** The compiler recognises a counted loop
  `i < (nv-len v)` and drops or hoists the checks inside it. Invisible to
  users. Turmeric loops are tail-recursive functions, not a loop IR, so the
  pattern matching is fragile, and a missed match silently loses the speed.
- **G. A cheaper checked path.** Keep the check, cut the rest: set the call
  site only on the failure branch instead of around every call, and do the
  compare at the call site instead of behind a call. Loops still do not
  vectorize, since the check is still control flow (gcc 14+ and recent LLVM
  can vectorize some early-exit loops, but not reliably enough to plan on).
  It is the only option that also speeds up every existing `vec-get`, so it
  is worth doing separately from this plan.

Recommendation: **A + B first** (small, safe, every engine), N1 kernels for
the common cases, then **D or E** when stencil-shaped code needs it. G as its
own piece of work for `Vec`. C waits.

Acceptance: the probe from 1.1 rewritten over `F64Vec` reports
`loop vectorized` under `gcc -O3 -fopt-info-vec-optimized` for an integer
sum, and for a float sum written with explicit partial sums. Add it to the
N1 vectorization check.

**As built: N2 (2026-10-28), approaches A and B.**

- **A:** `P-ref-unchecked` and `P-set-unchecked!`, `#fx{Unsafe}`, with no
  bounds check in the compiled body. Their interpreter twins keep one, so a
  wrong index in an interpreted (possibly sandboxed) program panics instead
  of reading past the block; a correct program cannot tell.
- **B:** `P-for` (one array, or two walked together after one length check),
  `P-for-index`, `P-fold` (added to the plan's list: a reduction needs an
  accumulator) and `P-build`. They expand to a `while` loop over `^mut`
  counters with gensym'd names; the body runs outside `unsafe`. An array
  argument that is not a name is bound once and freed after the loop.
- **Acceptance met:** an integer sum through `i64vec-fold`, an `int32` sum
  through `i32vec-for`, and a float sum with two explicit partial sums over
  `f64vec-ref-unchecked` all report `loop vectorized`; they are in the
  vectorization check.
- **Not needed after all: an emitter change.** The post-call
  `if (tur_panicking) return` the emitter writes after every call does not
  stop these loops vectorizing -- gcc inlines the leaf accessor, sees
  nothing in a read-only loop that writes the flag, and hoists the test.
  (A first probe that suggested otherwise was misread; an emitter change to
  drop the test after leaf inline-C calls was written, measured to buy
  nothing, and reverted.)
- **Known limit: a loop that STORES into an array does not vectorize** --
  `P-build`, or `P-for-index` with `P-set-unchecked!`. The emitted C is
  compiled `-fno-strict-aliasing`, so gcc must assume a stored element may
  be the array's own header, and reloads the data pointer every iteration.
  Fix directions: an N1 kernel for the common shape, or a way to hand a loop
  the data pointer once (a borrowed view type) so the header is not re-read.
  **Closed 2026-10-29 by open question 7:** `P-build` and the new `P-map!`
  store through a view, and the post-call panic test that also blocked the
  loop is gone.

Also in N2, as a separate small decision: whether `tur build` (not only
`--release`) should pass `-fvect-cost-model=cheap`, so unknown-trip-count
loops vectorize at the default `-O2`. Measure compile time on the fixture
corpus before deciding; the default `-O2` was chosen for build time.

**Decided 2026-10-29: no.** Measured, it costs nothing and buys nothing:

- Compile time, 282 fixtures from a seeded sample, each gcc run three times:
  gcc CPU 255.8s -> 256.5s (+0.3%), median per-file ratio 1.00, `.text`
  +0.04%. (It does change array-heavy code: on the numvec probe 12 -> 59
  loops vectorized, `.text` +14%.)
- Runtime, the 74 cc-engine programs of `benchmarks/audit/corpus.txt` plus
  the two dot benchmarks, five alternating runs: geometric mean 0.9976,
  every program over 20 ms within +-5%, no output change -- and
  `bench-f64vec-dot` 0.292 -> 0.296s, nothing.
- It cannot help the kernels: what `-O3` had and `-O2` lacked was full
  unrolling of the inner lane loop, not the cost model. The kernels'
  spelling closed that gap instead (N1 "As built").
- clang rejects the flag (`unknown argument`), so it would have needed a
  compiler check on every build.

### S0 -- an explicit-SIMD spice

Before touching the compiler, a spice in turmeric-spices (plan under
`docs/upcoming/spices/` if it grows) with inline-C bodies over GCC vector
extensions: load from and store to an N0 array, lanewise arithmetic,
`splat`, horizontal sum. Its job is to validate the API names and op set
against two or three real kernels (a dot product, an image blend over
`U8Vec`, a biquad filter over `F32Vec`) and to measure what crossing a
function boundary costs.

The vector values in S0 cannot be first-class Turmeric values yet -- a
16/32-byte value does not fit a carrier word -- so S0 exposes whole kernels
or operates in place on N0 arrays. That limit is the motivation for S1.

**As built (2026-10-29): `tur-simd`** (turmeric-spices `spices/simd`).
`c/simd/tur_simd.h` is the proposed op set as one C function per operation
(`simd_f64x2_add`, `simd_u8x16_widen_lo`, `simd_f64x4_hsum2`, ...) over
`f64x2 f64x4 f32x4 i32x4 u8x16 u16x8`, with S1's two lowerings behind the
same names: vector extensions under gcc/clang, a struct of lanes under
c2mir. All four builds -- gcc, clang, gcc forced onto lanes, and c2mir itself
(a small driver over `external/mir`) -- print the same bits for every
operation. The kernels: `simd-dot-f64` / `-f32` (bit-identical to N1's
`f64vec-dot` / `f32vec-dot` through the eight-lane tree), `simd-blend-u8!`,
and `simd-biquad4-f32!` (four interleaved channels, a channel per lane --
the only way a recurrence gets lanes). `tests/` holds each to its scalar
result to the bit; `bench/` measured, ns per element on x86-64:

| | gcc -O2 | clang -O2 | gcc -O2 -mavx2 |
|---|---|---|---|
| dot, f64x2 accumulators | 0.22 | 0.22 | 0.24 |
| dot, f64x4 accumulators | 0.60 | 0.22 | 0.14 |
| dot, N1 `f64vec-dot` | 0.22 | 0.22 | 0.13 |
| blend, explicit u16 lanes | 0.29 | 0.15 | |
| blend, scalar C (auto-vectorized) | 0.66 | 1.03 | |
| biquad x4, explicit f32x4 | 0.88 | 0.92 | |
| biquad x4, scalar C, channels side by side | 0.89 | 0.87 | |
| biquad x4, four scalar Turmeric filters | 7.05 | 10.6 | |

What it settles for S1:

1. **The portable vector types are the 16-byte ones.** On gcc without
   `-mavx`, an `f64x4` lives in memory -- the dot's accumulators are stored
   and reloaded every iteration, 2.7x slower than N1 -- and crosses a call in
   memory too (`-Wpsabi`). clang splits it into registers. `f64x2` matches N1
   everywhere. S1 should ship `f64x2 f32x4 i32x4 i64x2 u8x16` (+ `u16x8`,
   below) and treat `f64x4` / `f32x8` as AVX-only or leave them out.
2. **Every operation must be emitted inline.** With each operation a real
   call (`-DTUR_SIMD_OUTOFLINE`), every kernel ran 3-7x slower than inlined
   and slower than the scalar Turmeric loop. The lowering section's "helper
   calls on both engines" must be always-inlined helpers or C operators on
   cc, never an out-of-line function or a Turmeric call.
3. **The case for explicit types is lane width and lane layout, not speed
   over the auto-vectorizer.** The dot gains nothing over N1. The blend
   wins 2.3x (gcc) / 7x (clang) over the same scalar C because C promotes
   `uint8_t` arithmetic to 32 bits -- explicit 16-bit lanes are something
   only a vector type says. The biquad wins 8-12x over what a Turmeric
   program writes today, but scalar C written four channels abreast matches
   it: what Turmeric lacks is a way to hold four independent lanes, which is
   what an `f32x4` value is.
4. **Op set, as the kernels used it:** `load`, `store`, `splat`, `lane`,
   lanewise `add`/`sub`/`mul`, a `u16` `shr`, widening (`u8x16` -> two
   `u16x8`) and narrowing, and a fixed-order `hsum`. So **S1 needs `u16x8`
   and the widen/narrow pair**, which the list below did not have. No kernel
   needed `shuffle`, masks or `select`; keep them, but they are not on the
   critical path.
5. **The struct-of-lanes lowering is correct, and slow**: on gcc the dot ran
   at 3.8 ns/elem (17x the vector form), the blend at scalar speed. Right for
   `tur jit`'s run-edit-run loop, as planned.
6. **Toolchain:** c2mir lexed every identifier starting `u8` as `88...`, so
   a pasted `u8x16` broke -- already fixed on `main` by the mir#19 sync
   ([archived](../archive/c2mir-paste-of-u8-prefixed-token.md)); and
   `tur jit` does not pass a spice's `:c-includes` to c2mir, so an S0 kernel
   runs on the cc fallback under it
   ([jit-ignores-spice-c-includes](../reported/jit-ignores-spice-c-includes.md)).

### S1 -- compiler-known vector types (`--enable=simd`)

Types `f64x2`, `f64x4`, `f32x4`, `f32x8`, `i32x4`, `i64x2`, `u8x16`, plus
mask types from comparisons (`m64x2`, ...; or one mask type per width).
**Revised by S0 (2026-10-29):** the 16-byte types are the portable set, plus
`u16x8` and widening/narrowing; `f64x4` / `f32x8` only with AVX, if at all
(S0, "As built", items 1 and 4).

**Lowering.**

- **cc**: GCC/Clang vector extensions, declared in the preamble:
  `typedef double tur_f64x4 __attribute__((vector_size(32)));`. Not
  `<immintrin.h>`: vector extensions are portable to SSE, AVX, NEON and wasm
  simd128 from the same text, and an `f64x4` on an SSE2-only target becomes
  two 128-bit operations, which is correct and still faster than scalar.
- **`tur jit`**: under `#ifdef __MIRC__` the same typedef names are a struct
  of lanes (`typedef struct { double v[4]; } tur_f64x4;`) and every operation
  is a preamble helper looping over lanes. Correct, scalar speed, and no
  TUR-W0070 fallback. The emitter writes operations as helper calls on both
  engines (e.g. `tur_f64x4_add(a, b)`, an inline `a + b` on cc) so the two
  preambles are the only place the difference lives.
- **turi**: the same struct-of-lanes layout with scalar natives.
- **wasm**: add `-msimd128` to the Emscripten flags when the program uses a
  vector type (open question 5).

**Values.** A vector value is a by-value aggregate. A wide (> 8 byte)
by-value aggregate crossing a generic boundary is heap-boxed today
(`value-representations-guide.md`, "wide by-value aggregate"), which would
erase the point. So in S1 a vector type may appear in a local, a
parameter, a return, a struct field and an N0 load/store, but **not** as a
type argument to anything generic (`Vec`, `Option`, a typeclass dictionary
method). That is a compile error in S1 naming the plan, not a silent box.
Lifting it is a later phase if needed.

**Operations.** Lanewise `+ - * /` (via `Num` instances, so `{a + b}` works),
`min`, `max`, `abs`, `sqrt`; `splat`; `lane` and `with-lane` with
constant indices; `load`/`store` from and to an N0 array at an index
(one bounds check per load, outside any lane loop); comparisons returning a
mask; `select mask a b`; `shuffle` with constant indices; horizontal
`hsum` with the same fixed tree order as N1 so it agrees across engines;
conversions between widths (`f32x4->f64x2-lo`, ...) as named functions,
never implicit, spelled with `TUR_AS` where they convert so
`tests/check-emitted-float-conversions.py` stays at zero findings.

**Gate.** Per CLAUDE.md's experimental-features rule: one `EXPERIMENTS[]`
row (`"simd"`, a summary, `plan_path` pointing here, `introduced`,
`expires_at`, `XF_LIFECYCLE_PROTOTYPE`, `&g_opt_simd`), and
`experiment_warn_if_used("simd")` where the elaborator first resolves a
vector type name.

**Graduation.** All three engines agree on the S1 fixture set; the
generic-position restriction is either lifted or accepted as permanent and
documented; at least one spice kernel from S0 has moved onto S1 types and
measured no slower than its inline-C version.

**As built: S1a (2026-10-29), behind `--enable=simd`.** `(load
"stdlib/simd.tur")` -- generated by `tools/gen-simd.py`, with the interpreter
twins in `src/turi/simd_natives.inc` -- gives the six 16-byte types S0
settled on, `F64x2 F32x4 I64x2 I32x4 U16x8 U8x16`. Loading it without the flag
is an error naming the flag (the load expander is the one place a program
first reaches a vector type; `errors/simd-needs-enable`); with it, TUR-W0060.

- **Representation -- a deliberate departure.** Not new TypeKinds: each type
  is a `^copy` defstruct with one field per lane (`l0`, ...), which every
  engine already passes by value, so the elaborator, emitter and interpreter
  needed nothing new for the values themselves. An operation is an inline-C
  body with S1's two lowerings in one text: on cc the struct is reinterpreted
  as a `vector_size(16)` vector through a union, so `{a + b}` is one
  instruction; under c2mir (`__MIRC__`) it is a loop over the lanes; turi has
  a native per operation over the struct's fields. The names are capitalized
  like `F64Vec`'s, and `(.l1 v)` reads a lane on every engine.
- **Operations:** `splat`, `lane`, `load` / `store!` (one bounds check per
  call) and `load-unchecked` / `store-unchecked!` (`#fx{Unsafe}`), `add` `sub`
  `mul` `div` `neg` `min` `max`, `sqrt` / `abs` (floats), `hsum` (N1's tree),
  `u8x16-widen-lo` / `-hi`, `u16x8-narrow`, `u16x8-shr` (a count outside
  0-15 gives zero lanes, so a constant shift can never panic), and a `Num`
  instance per type. Integer lanes wrap (signed lanes compute in unsigned
  vectors); an integer lane divided by zero panics.
- **Agreement:** `simd-ops` calls every operation on every type, floats as
  bit patterns and integers where they wrap; cc, `tur jit` and turi
  (`tur --interpret`, whose natives were confirmed to be the code that ran)
  print the same 101 lines, checked against hand-worked values.
- **Graduation's kernel test -- met with the unchecked accesses.** S0's
  byte-image blend written in Turmeric on `U8x16` / `U16x8`, against S0's
  hand-written vector C in the same program (identical bytes), ns/elem:

  | | gcc -O2 | clang -O2 |
  |---|---|---|
  | S0 inline-C kernel | 0.30 | 0.15 |
  | S1, `load` / `store!` | 0.34 | 0.43 |
  | S1, `load-unchecked` / `store-unchecked!` | 0.30 | 0.15 |

  Getting there took four changes the first cut lacked: a load or store is
  one 16-byte `__builtin_memcpy` (sixteen lane copies kept the body from
  inlining), widen / narrow are a shuffle plus `__builtin_convertvector` (lane
  copies were scalarized), `u16x8-shr` cannot panic, and the can't-panic
  rule accepts `#if` conditionals, `__attribute__`, the vector builtins, libm
  and the memcpy family, so none of these operations carries a panic check.
- **Under clang, a checked access in a hot loop costs a call.** clang builds
  use `TUR_TLS_FRESH`, which reads `tur_panicking` through a non-inlined
  accessor (fiber-tls-address-reuse), so every panic check after a checked
  `load` is a function call: 2.9x on the blend. That is why the unchecked
  forms exist; gcc reads the flag directly and pays 1.13x.
- **Left for S1b (below):** the generic-position refusal, comparisons /
  masks / `select` / `shuffle`, float/int conversions and the 32-byte
  decision. Moving the tur-simd spice onto these types waits for its CI,
  which builds turmeric `main`.

**As built: S1b (2026-10-30), behind the same flag.** The rest of S1's
operation list, generated the same way and agreeing on all three engines
(`simd-masks-shuffle-convert`: NaN lanes, unsigned lanes above the signed
range, float lanes outside int32's range, shuffle indices that wrap -- cc,
`tur jit` and turi print the same 120 lines, checked by hand; gcc and clang
both).

- **Masks are four more `^copy` structs**, `M64x2 M32x4 M16x8 M8x16`, integer
  lanes as wide as the vector's (F64x2 and I64x2 share M64x2, F32x4 and I32x4
  share M32x4). A comparison sets a lane to -1 where it held and 0 where not
  -- what the vector compare instruction produces, so `P-lt` is one
  `cmpltpd` / `pcmpgtd`. `P-select` and the mask operations (`M-and`,
  `M-or`, `M-not`, `M-any`, `M-all`) take any **nonzero** lane as set, so a
  mask built by hand (`(M64x2 5 0)`) means what it looks like rather than
  mixing bits; after inlining, the `!= 0` folds away against a comparison's
  -1 / 0. `select` is an and / andnot / or over the lanes' bits, never a
  branch. Float comparisons are IEEE: a NaN lane fails all of `eq lt le gt
  ge`. Unsigned lanes compare unsigned.
- **`P-shuffle v idx`** takes lane `idx[k] mod L` of `v`; the index vector
  is the integer type of the same width (I64x2 for F64x2, I32x4 for F32x4,
  the integer types index themselves). Not restricted to constants, as the
  original list said: with constant indices gcc and clang both fold it to
  one `shufps` / `pshufd` (checked in the assembly), and a variable index is
  a lane loop, since baseline x86-64 has no variable shuffle below SSSE3's
  `pshufb`. The `mod L` means there is no out-of-range case and nothing to
  panic.
- **Conversions:** `i32x4->f32x4` (`cvtdq2ps`, round to nearest),
  `f32x4->i32x4` (truncating and **saturating**: NaN gives 0, at or above
  2^31 gives INT32_MAX, below -2^31 gives INT32_MIN -- a C cast is undefined
  there and `cvttps2dq` alone gives INT32_MIN for all three; the vector
  lowering zeroes those lanes before the conversion and patches them after),
  `f32x4-widen-lo` / `-hi` to F64x2 (exact) and `f64x2-narrow lo hi` to
  F32x4 (round to nearest, overflow to infinity). The plan said to spell them
  with `TUR_AS`; they are inline-C bodies, which
  `check-emitted-float-conversions.py` skips as reviewed code, so they are
  plain casts and `__builtin_convertvector`.
- **No 32-byte types -- decided.** S0 measured an `f64x4` without AVX
  living in memory on gcc and losing to `F64x2`, and the portable target set
  (x86-64 baseline, arm64, wasm simd128) has 16-byte registers only.
  `F64x4` / `F32x8` would pay off only under `-march` with AVX, which the
  performance guide does not make the default; a program that wants them
  can write two 16-byte halves today. Revisit with function
  multiversioning (Q6), if that is ever built.
- **None of the new operations can panic**, so none carries a panic check
  after the call.
- **The generic-position refusal is not built -- its premise did not
  hold.** S1 planned a compile error for a vector in generic code because a
  wide aggregate crossing a generic boundary is boxed. Measured on the S1
  types: a generic function, constrained (`[(Num T)]`) or not, is
  specialized per type and takes the vector by value
  (`ident__spec__tur_adt_F64x2`), and a generic ADT is too
  (`tur_adt_Option__F64x2`). The one position that boxes is a carrier
  container's element -- a `Vec` element is `malloc`ed at the push and freed
  by `vec-free`, as for every multi-word element -- which is correct, only
  slower than the numeric arrays the vectors load from. A refusal would
  have forbidden working, unboxed code to guard one slow-but-correct case,
  so the restriction is lifted rather than enforced (graduation's "lifted or
  accepted" clause). `simd-generic-positions` pins all four positions on
  cc, `tur jit` and turi; the module header now says which one boxes.

**S1 graduated (2026-10-30), after v0.72.1 shipped it behind the flag.** The
`simd` row is gone from `EXPERIMENTS[]` and `(load "stdlib/simd.tur")` needs
no flag; the name is in `GRADUATED[]`, so a leftover `--enable=simd` (or
tur-simd's `:experiments ["simd"]`) is a TUR-W0063 no-op until it ages out,
eligible at 0.74.0 (`simd-graduated-enable-noop`). Graduated straight from
prototype; the three tests above, as met:

- **Engines agree:** `simd-ops`, `simd-masks-shuffle-convert`,
  `simd-generic-positions` and `simd-load-out-of-bounds-panics` give the same
  output on cc (gcc and clang), `tur jit` and turi.
- **Generic positions:** lifted, measured (S1b above).
- **A spice kernel on S1 types, no slower than its inline C:** tur-simd's
  `simd/s1` (turmeric-spices#131) rewrites S0's three kernels in Turmeric on
  these types, the same bits as the inline-C ones. ns/elem, `-O2`, x86-64:

  | kernel | gcc: inline C | gcc: S1 | clang: inline C | clang: S1 |
  |---|---|---|---|---|
  | dot f64 (`F64x2`) | 0.242 | 0.249 | 0.251 | 0.237 |
  | blend u8 (`U8x16` / `U16x8`) | 0.299 | 0.299 | 0.146 | 0.151 |
  | biquad x4 f32 (`F32x4`) | 0.969 | 0.985 | 0.974 | 1.32 |

  The dot product and the blend meet the test on both compilers. The biquad
  is 1.35x slower on clang: a vector carried around a latency-bound loop is
  kept as the two halves the x86-64 ABI passes a 16-byte float struct in,
  split and rejoined each iteration. A performance gap, not a wrong answer,
  and open as
  [docs/reported/simd-loop-carried-vector-split-by-clang.md](../reported/simd-loop-carried-vector-split-by-clang.md);
  graduation does not wait on it.

The gate only ever refused the load -- there is no off path to keep -- so
the refusal fixture (`errors/simd-needs-enable`) retired rather than inverted.

## 4. Engine matrix after each phase

| | cc | `tur jit` | turi | wasm |
|---|---|---|---|---|
| N0/N1 | vectorized kernels at `-O2` and `-O3` (gcc and clang) | same C, MIR scalar code | the same kernel C, built with the host's `tur` | vectorized with `-msimd128`, on by default (Q5) |
| N2 | read loops vectorize at `-O3`; store loops through `-map!` / `-build` at clang `-O2` and gcc `-O3` (Q7) | scalar | scalar | scalar |
| S1 | vector extensions (SSE2 baseline, AVX with `-march`) | struct-of-lanes helpers | struct-of-lanes natives | simd128, on by default (Q5) |

Results identical in every cell.

## 5. Rejected and deferred

- **A vectorizer in MIR.** The project linked in the original question,
  [SparrowLii/vectorization](https://github.com/SparrowLii/vectorization),
  is a loop vectorizer over *Rust's* MIR inside a patched rustc -- unrelated
  to vnmakarov's MIR, which `tur jit` uses. vnmakarov's MIR has no vector
  types; the one upstream request
  ([vnmakarov/mir#58](https://github.com/vnmakarov/mir/issues/58)) was
  closed in 2020. Adding one means vector types in the IR, vector register
  allocation, instruction selection on four back ends and vector-extension
  parsing in c2mir, through a fork limited to one open PR at a time
  (CLAUDE.md, "MIR fork"). And it would speed up the engine meant for the
  run-edit-run loop; deployed code goes through gcc/clang, whose vectorizers
  already exist. Revisit only if the JIT becomes a deployment engine.
- **Global `-ffast-math`.** It changes results program-wide, including NaN
  and infinity handling, and would make the cc engine disagree with turi.
  N1's fixed partial-sum order gets the reduction speedup without it.
- **`-march=native` by default.** Already declined in the performance guide:
  the binary needs the machine that built it. `TUR_CC_FLAGS` remains the way
  to opt in.
- **Function multiversioning** (`__attribute__((target_clones("avx2",
  "default")))` on the N1 kernels): would give AVX2 speed in a portable
  binary on x86-64 Linux (glibc ifunc). Deferred 2026-10-29 (open question
  6): `target_clones` needs ifunc, so it is Linux-only; the portable spelling
  is explicit dispatch, below.

## 6. Open questions

1. Five types, or one `(NumVec T)`? **Re-measured 2026-10-29: keep the five
   types and generate a `NumArray` class over them** (section 3.1); a
   `(NumVec T)` rewrite buys nothing the class's `(Elem V)` does not. The
   element-typed blocker
   ([assoc-type-method-at-applied-opaque-instance-no-instance](../archive/assoc-type-method-at-applied-opaque-instance-no-instance.md))
   is fixed. Before anything higher-order goes in the class:
   [closure-in-constrained-generic-dispatches-one-instance](../reported/closure-in-constrained-generic-dispatches-one-instance.md).
2. Names. Honest names, `F64Vec`, not `FlVec` (as shipped). **Decided
   2026-10-29:** there is no generic type; the generic layer is the class
   **`NumArray`**, its methods prefixed **`na-`** (`na-len`, `na-sum`,
   `na-ref`, ...), and the per-type families (`f64vec-new`, `f64vec-sum`)
   stay -- constructors only ever exist per type (3.1, item 3). **Built the
   same day** (3.1, "As built: NumArray").
3. Which N2 approaches (A-G in N2)? Recommended: A + B first, D or E later,
   G separately. **A + B landed 2026-10-28. D and E deferred 2026-10-29.**
   Both remove the bounds check by proving the index in range, and the
   check is not what stops a loop vectorizing today -- Q7 is. D (a phantom
   length brand) would also change the array's type, which N0 shipped
   without; it can still come later as a separate branded type. E
   (refinement-typed indices) waits on the solver reasoning about a length
   across a loop, which is the `reflected-measures` experiment's job.
   Revisit either when a measured loop is held back by its bounds check
   rather than by Q7.
4. Is eight the right partial-sum count for N1 reductions on every target,
   or should it be fixed per element width (8 for `float64`, 16 for
   `float32`)? Either way it must not depend on the build machine.
   **Decided 2026-10-28: eight for every width** (N1 "As built").
5. wasm: turn on `-msimd128` always (every current browser supports it) or
   only when the program uses S1 types? **Measured 2026-10-29; decision
   pending.** Two builds take the flag: the prebuilt web modules (turi's
   numvec natives run there) and `tur build --target wasm` (emcc, `-O2`;
   the stdlib kernels and any N2 loop). The kernel C is the same in both, so
   it was measured once: the 44 kernels from `src/turi/numvec_kernels.inc`
   built with clang 18 `--target=wasm32 -O2`, with and without
   `-msimd128`, run under node 22 (V8), 4096 elements, 20,000 calls,
   interleaved, best of 15, ns per element:

   | kernel | `-O2` | `-O2 -msimd128` | speedup |
   |---|---|---|---|
   | `f64vec-sum` | 0.34 | 0.11 | 3.0x |
   | `f64vec-dot` | 0.60 | 0.31 | 1.9x |
   | `f64vec-min` | 0.45 | 0.39 | 1.2x |
   | `f64vec-add!` | 0.45 | 0.25 | 1.8x |
   | `f64vec-axpy!` | 0.51 | 0.31 | 1.7x |
   | `f64vec-scale!` | 0.26 | 0.18 | 1.4x |
   | `f64vec-fill!` | 0.22 | 0.11 | 1.9x |
   | `f32vec-sum` | 0.33 | 0.08 | 4.3x |
   | `f32vec-dot` | 0.52 | 0.10 | 5.0x |
   | `f32vec-min` | 0.45 | 0.29 | 1.5x |
   | `i64vec-sum` | 0.34 | 0.12 | 2.9x |
   | `i32vec-sum` | 0.35 | 0.15 | 2.4x |
   | `i32vec-dot` | 0.73 | 0.33 | 2.2x |
   | `i32vec-min` | 0.65 | 0.09 | 7.6x |
   | `u8vec-sum` | 0.29 | 0.29 | 1.0x |
   | `u8vec-max` | 0.74 | 0.03 | 29x |

   Every result is bit-identical between the two builds -- the fixed
   eight-lane order (N1) is what makes the flag safe to turn on. No kernel
   got slower; the u8 sum gains nothing (its widening to 32-bit lanes is
   the cost). Kernel code grew 20% (13.1 KB -> 15.8 KB of the module). The
   cost is reach: an engine without wasm SIMD refuses the *whole* module,
   not only the SIMD functions -- Chrome 91 and Firefox 89 (2021), Safari
   16.4 (2023) and later accept it.

   **Decided and done 2026-10-29: on by default, not tied to S1.** `tur
   build --target wasm` passes `-msimd128` unless the manifest says
   `:web #{:simd false}` (`PkgWebOpts.simd`, beside `:tail-calls`; checked
   by `tests/run-build-project.sh` with a stub emcc), and both Try Turmeric
   modules build with it (`tur_wasm` in `src/CMakeLists.txt`,
   `tools/wasm-jit/build-module.sh`).
6. Multiversioned N1 kernels on x86-64 Linux -- worth it, and does a
   portable fallback exist for the other hosts? **Deferred 2026-10-29.**
   The portable spelling is not `target_clones` (glibc ifunc, Linux only)
   but explicit dispatch: each hot kernel compiled twice in the same unit,
   once under `__attribute__((target("avx2")))`, and a pointer chosen on
   first call with `__builtin_cpu_supports("avx2")` -- gcc and clang on
   Linux, macOS and Windows (MinGW). The AVX2 copy must keep the same
   eight-lane order and no FMA (`target("avx2")` alone, not `"avx2,fma"`),
   or the engines stop agreeing bit for bit. Revisit when a measured program
   is kernel-bound at SSE2 width.
7. **The store-loop gap (added 2026-10-29).** A Turmeric loop that writes
   into an array -- `P-build`, or `P-for-index` with `P-set-unchecked!` --
   does not vectorize on any compiler (N2 "As built", "Known limit"). Each
   store goes through the header (`h->data[i] = x`), and under
   `-fno-strict-aliasing` the compiler must assume the stored element could
   be the header's own `data` word, so it reloads the pointer every
   iteration. A probe of the bare C shape (`dst[i] = i * k`,
   `d[i] = d[i] * k`, `dst[i] = src[i] + 1`; no emitter output yet -- see the
   second blocker below), vectorized loops reported:

   | | gcc `-O2` | gcc `-O3` | clang `-O2` |
   |---|---|---|---|
   | through the header, as today | none | none | none |
   | data pointer loaded once into a local | none | the two maps | all three |

   Loading the pointer once fixes clang and gcc `-O3`. gcc's `-O2` still
   declines (its very-cheap cost model refuses a loop whose trip count it
   cannot see -- the same wall N1's written-out spellings climbed). Options:

   - **(a) A borrowed view.** `(f64vec-view a)` gives a `^borrow`ed handle
     whose value *is* the data pointer; `view-ref` / `view-set!` index it
     directly. In the emitted C it is a local, so nothing reloads it.
     `P-build` and `P-for-index` take the view themselves, so the common
     loops get it without the user writing it. It needs the view not to
     outlive the array (a borrow, like `^borrow` params) and costs one more
     type per element width.
   - **(b) More N1 kernels** for the shapes that recur: `map!` (in place),
     `map2!`, `build` from a function of the index. Vectorized everywhere,
     including gcc `-O2`, but each needs a lambda per element, and a lambda
     inside generic code hits
     [closure-in-constrained-generic-dispatches-one-instance](../reported/closure-in-constrained-generic-dispatches-one-instance.md);
     only a kernel with a fixed operation (`add-scalar!`, `mul!`) avoids it.
   - **(c) Do nothing** and document the gap: a store loop runs scalar, at
     the speed of the equivalent C loop under gcc `-O2` today.

   **Decided 2026-10-29: (a), plus a few fixed-operation kernels from (b).
   Built the same day (below).**
   The audience that matters is macOS (clang) and `--release` builds, where
   (a) pays off fully; default gcc `-O2` builds keep the kernels.

   **A second blocker, found while deciding.** The emitted loop (`tur
   emit-c` of a `f64vec-for-index` that stores through
   `f64vec-set-unchecked!`) has an `if (tur_panicking) return;` after the
   store call, and under `-fno-strict-aliasing` a `double` store may write
   that flag, so the test cannot leave the loop. (In a read-only loop
   nothing writes it and it is hoisted -- why N2 needed no emitter change.)
   That C, with the function kept out of line, edited by hand:

   | the emitted store loop | gcc `-O2` | gcc `-O3` | clang `-O2` |
   |---|---|---|---|
   | as emitted | no | no | no |
   | post-store panic tests removed | no | no | no |
   | tests removed and the data pointer in a local | no | yes | yes |

   Both have to go. (When the array is built in the same function the loop
   can vectorize already: the compiler then knows its block is fresh.) So
   the work is:

   1. **Emitter: no post-call panic test after a call that cannot panic.**
      At least the unchecked store; the general rule is an inline-C callee
      whose body cannot reach `tur_panic` -- whether the emitter infers that
      from the body or the generator marks the accessors is the first design
      call. A first version of this change was written during N2 and
      reverted because it bought nothing for read loops; it is what store
      loops need.
   2. **Views.** A `defopaque` per width (`F64View` ...) whose carrier is
      the data pointer, from `(P-view a)` (`^borrow` on `a`), with
      `P-view-ref` / `P-view-set!` under `#fx{Unsafe}` like the unchecked
      accessors. An N0 array never resizes, so a view stays valid exactly as
      long as its array; nothing checks that, which is why making one is
      `#fx{Unsafe}`. A store writes a number, not a pointer, so no region
      note is needed. turi: the view can be the array handle itself, with
      the interpreter twins keeping their bounds check, as `P-ref-unchecked`
      does.
   3. **Macros that store through a view**, so users get it without writing
      one: `P-build` switches to a view, plus `P-map!` (`(P-map! [x v]
      expr)`, in place) and `P-map2!` (`(P-map2! dst [x a y b] expr)`).
      Because they are macros the per-element expression is inlined into the
      loop -- no lambda, so the closure-dispatch report above does not apply
      to them.
   4. **A few fixed-operation N1 kernels**, vectorized at gcc `-O2` too:
      `sub!`, `mul!` (element-wise), `add-scalar!`. Pick the rest by use.

   Acceptance: each new macro's loop reports `loop vectorized` under clang
   `-O2` and gcc `-O3` in the vectorization check, cc, turi and `tur jit`
   agree bit for bit, and a timed store loop (the probe above) improves.

   **As built (2026-10-29).** All four, with one change of surface: one
   `P-map!` taking one source or two -- `(P-map! dst [x src] expr)`,
   `(P-map! dst [x a y b] expr)`, dst may be a source -- in place of a
   separate in-place `P-map!` and a `P-map2!`; it reads as `P-for` does.

   - **Emitter** (`fn_def_inline_c_cannot_panic`, `emit_call_cannot_panic`
     in `src/compiler/emit_core.c`): the body is read as text, and makes no
     call when no identifier but a keyword (`sizeof`, `if`, `return`, ...)
     is followed by `(`; a `#` directive, `tur_panic`, or a `__TUR_VAL_N__`
     sub-expression rules it out. Checked at the hoisted-call site and the
     `: nil` statement call. 158 fixture snapshots lose 7,345 lines and gain
     none -- `list-head` / `list-tail`'s checks in the stackless runtime are
     most of them -- and the suite is otherwise unchanged.
   - **Views:** `F64View` ... `U8View` (`defopaque`, not affine),
     `P-view` / `P-view-ref` / `P-view-set!`, all `#fx{Unsafe}`; in turi a
     view is the array handle, its twins bounds-checked, with
     `native_caps.c` rows (the handle kind still applies).
   - **Kernels:** `P-sub!` and `P-add-scalar!` on every type, `P-mul!` except
     on `U8Vec` (as `scale!`). Vectorized at gcc `-O2` and `-O3` like
     `add!` / `scale!` (`i64vec-mul!` excepted: SSE2 has no 64-bit multiply).
   - **Acceptance met:** `tests/check-numvec-vectorized.py` (87 of 87) now
     also compiles the probe with clang `-O2 -Rpass=loop-vectorize` and
     requires the four store probes (`-map!` from one source, from two, in
     place, and `-build`) there as well as at gcc `-O3`. The fixture
     `numvec-store-loops` prints float results as bit patterns and runs on
     cc, `tur jit` and turi alike (leak-checked). Timed, an in-place
     `{x * k}` over 4096 doubles, 200,000 passes, best of three:

     | | `-for-index` + `-set-unchecked!` | `-map!` |
     |---|---|---|
     | gcc `-O2` | 0.162s | 0.150s |
     | gcc `-O3` | 0.186s | 0.148s |
     | clang `-O2` | 0.267s | 0.104s |
     | gcc `-O3 -march=native` | 0.084s | 0.089s |
     | clang `-O2 -march=native` | 0.325s | 0.063s |

     2.6x under clang at the SSE2 baseline (5.2x with AVX); a smaller win
     under gcc `--release`, whose own runtime alias check already rescues
     the old loop once AVX is on; nothing at gcc's default `-O2`, as
     expected -- the kernels are the answer there.

## Appendix -- reproducing 1.1 and 1.3

The probe and benchmark used for this plan:

```sh
# 1.1: does the emitted loop vectorize?
./build-release/tur emit-c probe.tur > probe.c
gcc -O3 -std=gnu99 -fno-strict-aliasing -w -c probe.c -o /dev/null \
    -fopt-info-vec-missed 2>&1 | grep 'not vectorized'

# 1.3: four C variants of one dot product
for f in "-O2" "-O3" "-O3 -ffast-math" "-O3 -march=native -ffast-math"; do
  gcc $f -fno-strict-aliasing -w -o bench bench.c && ./bench
done
```

`probe.tur` is a tail-recursive `(dot-go a b i n acc)` over two
`(Vec float)` filled with `7.1` and `3.25`, plus the same shape over a
`(Vec int)`. `bench.c` has three versions of the dot product: the emitted
shape (bounds check, thread-local site push/pop, panic flag, union
reinterpretation), a plain `const double *` loop, and an `f64x4` vector
extension with four partial sums. They are not committed; N1's benchmark
replaces them.

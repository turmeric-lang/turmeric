# SIMD and vectorization -- typed numeric arrays, then explicit vector types

> **Status: N0, N1 and N2 (approaches A + B) LANDED 2026-10-28** (`stdlib/numvec.tur`;
> each phase's "As built" note in section 3); N2's `-fvect-cost-model=cheap`
> question decided (no) 2026-10-29; the generic-type question (open question
> 1) re-measured 2026-10-29 -- recommendation: a generated class over the five
> types, not a `(NumVec T)` rewrite (section 3.1, "2026-10-29"). N2
> approaches D/E, S0 and S1 not started.
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
| **N1** | Bulk kernels over them (`sum`, `dot`, `axpy!`, `scale!`, `fill!`, `copy!`, `min`/`max`) as plain C loops gcc vectorizes | none (stdlib) | none |
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

### S1 -- compiler-known vector types (`--enable=simd`)

Types `f64x2`, `f64x4`, `f32x4`, `f32x8`, `i32x4`, `i64x2`, `u8x16`, plus
mask types from comparisons (`m64x2`, ...; or one mask type per width).

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

## 4. Engine matrix after each phase

| | cc | `tur jit` | turi | wasm |
|---|---|---|---|---|
| N0/N1 | vectorized kernels at `-O3`; scalar but cheap at `-O2` | same C, MIR scalar code | scalar natives | scalar (no simd128) |
| N2 | user loops vectorize at `-O3` | scalar | scalar | scalar |
| S1 | vector extensions (SSE2 baseline, AVX with `-march`) | struct-of-lanes helpers | struct-of-lanes natives | simd128 if enabled |

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
  binary on x86-64 Linux (glibc ifunc). Deferred to open question 6; not
  portable to macOS or Windows as-is.

## 6. Open questions

1. Five types, or one `(NumVec T)`? **Re-measured 2026-10-29: keep the five
   types and generate a `NumArray` class over them** (section 3.1); a
   `(NumVec T)` rewrite buys nothing the class's `(Elem V)` does not. The
   element-typed blocker
   ([assoc-type-method-at-applied-opaque-instance-no-instance](../archive/assoc-type-method-at-applied-opaque-instance-no-instance.md))
   is fixed. Before anything higher-order goes in the class:
   [closure-in-constrained-generic-dispatches-one-instance](../reported/closure-in-constrained-generic-dispatches-one-instance.md).
2. Names. **Author leans toward honest names: `F64Vec`, not `FlVec`.** With a
   generic type, those become `defalias`es of `(NumVec float)` etc., and the
   function family is `nv-*` (or `numvec-*`) rather than `f64vec-*`. Still
   open: the generic type's own name, and whether per-type function names
   (`f64vec-new`) survive for constructors (3.1, item 3).
3. Which N2 approaches (A-G in N2)? Recommended: A + B first, D or E later,
   G separately. **A + B landed 2026-10-28.** D would change `NumVec`'s type
   (a phantom length), which N0 shipped without; it can still be added as a
   separate branded type.
4. Is eight the right partial-sum count for N1 reductions on every target,
   or should it be fixed per element width (8 for `float64`, 16 for
   `float32`)? Either way it must not depend on the build machine.
   **Decided 2026-10-28: eight for every width** (N1 "As built").
5. wasm: turn on `-msimd128` always (every current browser supports it) or
   only when the program uses S1 types? **Undecided**; it does not block
   anything before S1.
6. Multiversioned N1 kernels on x86-64 Linux -- worth it, and does a
   portable fallback exist for the other hosts?

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

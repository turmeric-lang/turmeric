# MIR/c2mir on aarch64 mis-passes floating-point aggregates across the native boundary

**Status: RESOLVED 2026-08-26.** Fixed properly, in MIR: the aarch64 back end
now implements the AAPCS64 HFA rule. Pin bumped to
[`472fa4c6`](https://github.com/turmeric-lang/mir/commit/472fa4c6fa608ba515e2214e2c2bc8c0e934c8d9)
(`cmake/mir.cmake`). See "Resolution" below; the original analysis is kept
intact underneath because it is what made the fix tractable.

**Severity: high** (silent wrong answers, data-dependent) -- but scoped: it
fires only where c2mir-generated code calls a *natively compiled* function
that takes or returns a floating-point aggregate by value. Pure `tur jit`
programs are unaffected because both sides are c2mir and agree with each
other.

Found while scoping F4 (struct-by-value) of
[jit-ffi-c2mir-plan](jit-ffi-c2mir-plan.md). This was the blocker that kept
F4's *interpreter* half from covering FP aggregates.

## Resolution

The "Fix directions" section below called for changes in two layers of the
vendored fork and warned it was "not small". That is what was done. A third
layer it also listed -- the hand-encoded `_MIR_get_ff_call` thunks in
`mir-aarch64.c` -- turned out **not** to need touching: turmeric's dynamic-FFI
thunks are *generated C compiled by c2mir* (`render_thunk_source` in
`src/turi/jit_ffi.c`), so they inherit the c2mir/generator fix. That is why the
`call-ptr` path works below without a single hand-encoded instruction changing.

**`c2mir/aarch64/caarch64-ABI-code.c`** -- `hfa_flatten`/`hfa_type` classify an
aggregate by flattening it to at most four scalar leaves and requiring them all
to be the same FP type. Nested structs and constant-bound arrays flatten;
unions, bitfields, over-aligned aggregates and `long double` deliberately do
not and keep the previous general-purpose treatment. The classifier also
asserts `size == n * member_size`, which is what lets the member count be
recovered downstream from the block size alone. `target_return_by_addr_p` stops
sending an HFA back by address, and the return hooks declare one typed `F`/`D`
result per member.

Two things were load-bearing and non-obvious:

- **The return direction needed almost nothing in the generator.** It already
  routed `MIR_T_F`/`MIR_T_D` results through `v0..v7` on both the call side and
  the `MIR_RET` side. Declaring the results with their real types was the whole
  fix. Worth knowing before anyone re-derives that half.
- **`target_add_arg_proto`/`target_add_call_arg_op` had to stop delegating to
  the `simple_*` helpers**, which hardcode `MIR_T_BLK` and never consult
  `target_get_blk_type`. Without that, the classification would never have
  reached an argument at all and the whole change would have been a no-op on
  the argument side while looking complete.

**`mir-aarch64.h` / `mir-gen-aarch64.c`** -- two of MIR's five reserved block
classes carry the HFA cases (`MIR_T_BLK_HFA_F`/`_D`), with arms on the call
side, the callee-side gather in `target_machinize`, and the pre-pass that sizes
the outgoing argument area. All three must agree or the frame is mis-sized.
Members reach `v0..v7` via `get_arg_reg` and are kept live with
`setup_call_hard_reg_args`, which -- unlike the base/index trick the plain BLK
path uses -- has no two-register limit, so four members are expressible.
Assignment is all-or-nothing: an HFA that does not fit entirely in the
remaining v registers goes wholly on the stack and sets NSRN to 8 so a later,
smaller HFA cannot backfill. On Apple targets varargs have already forced
`fp_arg_num` to 8, so they take the stack arm, which is what that ABI wants.

### The support matrix, re-measured

Every row measured against the same program built by `cc`, on Apple arm64 /
macOS 27.0 / Apple clang 21. "fixed" marks a row the original matrix further
down reported as BROKEN.

| aggregate | as arg | as return |
| --- | --- | --- |
| `{float,float}` / `{double,double}` | fixed | fixed |
| `{float}` / `{double}` (1-member HFA) | fixed | fixed |
| `{float x4}` / `{double x4}` | fixed | fixed (32b, not by address) |
| `{float x3}`, `float[3]`, nested `{{f,f},f}` | fixed | -- |
| HFA after scalar FP args consume v regs | fixed | -- |
| 3 x `{double x4}` (exhausts v0..v7 -> stack) | fixed | -- |
| `{int,int}`, `{double,i64}`, 24b by-ref | unchanged | unchanged |

The original repro now prints `152.25` under both `tur run` and `tur jit`,
where `tur jit` printed `225`.

The single-member rows are not redundant with the two-member ones, for the
reason this report itself flagged: a one-argument probe of a 1-member HFA
*passes even when broken*, because the value wrongly read from `v0` is usually
the one the caller just materialized there. The tests use two HFA arguments so
at most one can be accidentally right.

Both FFI directions are covered, including the inbound one -- a natively
compiled caller passing an HFA *into* a c2mir-generated callback, which is the
only thing that exercises the callee-side gather.

Tests, arch-gated in `tests/run-flags.sh` and SKIPped off aarch64:
`jit-ffi-call-ptr-hfa`, `jit-ffi-extern-c-hfa-compiled` (asserted against
`tur run` rather than a literal, so the two backends are compared directly),
`jit-ffi-callback-hfa`, and `jit-ffi-extern-c-nonhfa-still-jits` -- the last so
that widening the classifier to "any aggregate containing a float" cannot pass
while silently breaking INTEGER-class aggregates that work today.

MIR's own `make test` (c2mir gcc/lacc/new torture suites) reports the same 6
failures before and after, exit 0 both times.

### What was removed

The interim refusals are gone, because there is nothing left to refuse:
`tur_jit_ffi_struct_supported`'s aarch64 HFA carve-out and the
`tur_jit_ffi_is_hfa` predicate that existed only to drive it, plus the
short-lived compiled-path refusal (TUR-E0711) and the `g_target_c2mir` backend
flag that gated it. **Reverting the MIR pin below `472fa4c6` therefore silently
reinstates the miscall rather than diagnosing it** -- noted in
`cmake/mir.cmake` next to the pin.

## Summary

AAPCS64 classifies a struct whose members are all the same floating-point
type, 1-4 of them, as a **HFA** (Homogeneous Floating-point Aggregate) and
passes it in the SIMD registers `v0..v7`, one member per register. MIR's
aarch64 backend has no HFA concept: it passes *every* aggregate <= 16 bytes
in the general-purpose registers `x0..x7`, and larger ones by reference.

Within a single c2mir compilation that is self-consistent, so it is
invisible. It becomes wrong the moment c2mir-compiled code calls a function
compiled by clang/gcc: the caller writes the members into `x0,x1`, the
callee reads them from `v0,v1`. The value the callee sees is whatever was
left in the SIMD registers, so the failure is **data-dependent** -- some
call sites accidentally produce the right answer, which makes it far more
dangerous than a consistent crash.

## Repro

Host: Apple arm64, macOS 27.0. Build:
`cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON -DTUR_DEBUG_SANITIZE=OFF`

```sh
cat > /tmp/sbv_helper.c <<'EOF'
typedef struct { float a, b; } F2;
double __sbv_f2(F2 v) { return (double)v.a * 100.0 + (double)v.b; }
EOF
cc -shared -fPIC -o /tmp/libsbvhelper.dylib /tmp/sbv_helper.c

cat > /tmp/q.tur <<'EOF'
(defn probe [] : float
  ```c
  /* __tur_autolink__: -L/tmp -lsbvhelper */
  typedef struct { float a, b; } F2;
  extern double __sbv_f2(F2);
  F2 v; v.a = 1.5f; v.b = 2.25f;
  return __sbv_f2(v);
  ```)
(defn main [] : int (println (probe)) 0)
EOF

./build-jit/tur run /tmp/q.tur    # 152.25  <- correct (cc compiles the caller)
./build-jit/tur jit /tmp/q.tur    # 225     <- WRONG (c2mir compiles the caller)
```

`225` is `2.25 * 100 + 0`: the callee read `v0` (which happened to hold the
last float materialized, `2.25`) as `a`, and `v1` (zero) as `b`.

## Support matrix measured on arm64

`tur run` (native cc) vs `tur jit` (c2mir), same source:

| aggregate | as arg | as return |
| --- | --- | --- |
| `{int,int}` (8b) | OK | OK |
| `{i64,i64,i64}` (24b, by reference) | OK | -- |
| `{double,i64}` (mixed -> INTEGER class) | OK | -- |
| `{float}` / `{double}` (1-member HFA) | **BROKEN** | -- |
| `{float,float}` / `{double,double}` | **BROKEN** | **BROKEN** |
| `{float x4}` / `{double x4}` | **BROKEN** | -- |

The single-member rows need care: a naive probe *passes*, because the value
the callee wrongly reads out of `v0` is very often the same value the caller
just materialized into `v0` on its way to the store. Two FP-aggregate
arguments disambiguate it -- at most one can be accidentally right:

```c
double __sbv_f1x2(F1 a, F1 b) { return (double)a.x * 100.0 + (double)b.x; }
/* a.x = 3.0, b.x = 4.0 -> native 304, c2mir 400 (a read as 4.0, b as 0) */
```

So the correct predicate is **every HFA/HVA is broken**, not "2-4 members
are broken."

## Root cause

Two places, both in the vendored MIR fork
(`rjungemann/mir` @ `9c221f96`, pinned in `cmake/mir.cmake:77-80`):

1. `c2mir/aarch64/caarch64-ABI-code.c:81-84` -- `target_get_blk_type`
   returns a bare `MIR_T_BLK` for every aggregate, with the comment
   *"one BLK is enough"*. Contrast the x86-64 sibling
   (`c2mir/x86_64/cx86_64-ABI-code.c:356-361`), which runs `classify_arg`
   over the SysV eightbyte classes and returns a *discriminated*
   `get_blk_type (n_qwords, qword_types)`. MIR reserves 5 block classes
   (`MIR_BLK_NUM`, `mir.h:163`) precisely so a target can encode this;
   aarch64 uses exactly one of them.

2. `mir-gen-aarch64.c:332-361` (call lowering) and `mir-aarch64.c:350-400`
   (the hand-encoded interface thunks) then route every `MIR_blk_type_p`
   argument through `int_arg_num` / `n_xregs`, i.e. `x0..x7`, splitting it
   into qwords. There is no path that puts an aggregate member into
   `v0..v7` -- `fp_arg_num` is only ever touched for scalar `MIR_T_F` /
   `MIR_T_D` arguments.

So even if (1) started classifying HFAs, (2) has nowhere to send them yet.

## Fix directions

A real fix is a MIR-fork change in both layers, and it is not small:

- `caarch64-ABI-code.c`: add an HFA/HVA classifier (all members recursively
  the same FP type, count <= 4, honoring nested structs and arrays) and
  return a distinct blk class for it.
- `mir-gen-aarch64.c`: in `target_get_call_arg_reg` / the blk branch around
  line 332, consume `fp_arg_num` and emit per-member `ldr s/d` into
  `v0..v7` for the HFA class, plus the matching callee-side gather near
  line 787 and the return path.
- `mir-aarch64.c`: same for the hand-encoded `_MIR_get_ff_call` thunk
  (raw instruction words -- `gen_ld_pat` currently only encodes GP loads).

That is vendored-backend work with hand-encoded aarch64 instructions, and
landing it means pushing to `rjungemann/mir` and bumping the pin in
`cmake/mir.cmake` (mind the CACHE-VARIABLE trap documented at
`cmake/mir.cmake:67-76`).

Until then the honest move -- and what the F4 work does -- is to **refuse
the case rather than mis-call it**: classify the aggregate at thunk-build
time and return a clean diagnostic for an HFA on aarch64, instead of
emitting a thunk that silently reads the wrong registers.

**x86-64 verified clean, 2026-08-21.** Measured on an x86-64 host against
cc-compiled callees for every SysV class -- packed float pair (one SSE
eightbyte), two-SSE, INTEGER+SSE in both member orders, single-GP,
MEMORY-class (>16 bytes), nested, and aggregate returns of each. All
correct, so the `#if defined(__aarch64__)` scope of the refusal is right.
(The same sweep did find a *nested*-aggregate miscall on every
architecture, but that was a turi sig-rendering bug, not an ABI one --
fixed, see [jit-ffi-c2mir-plan](jit-ffi-c2mir-plan.md).)

**The refusal now covers both directions, 2026-08-21.** F5 callbacks
gained aggregate parameters and returns, which is the same hazard
mirrored: for an inbound HFA the *natively compiled caller* writes
`v0..v7` and the c2mir-generated callback would read `x0..x7`. The
provider refuses an HFA in a callback signature exactly as it does in a
call signature.

**The compiled `tur jit` path now refuses too, 2026-08-26 (TUR-E0711).**
Everything above was about the *interpreter's* thunk engine. The whole-program
`tur jit` path had no check at all, so the "not only a jit-ffi problem"
paragraph below was live on the most ordinary spelling there is -- an
`extern-c` with a record parameter. Measured on arm64 macOS before the fix:

```turmeric
(defstruct D2 [a : float b : float])
(extern-c __sbv_d2 [v : D2] : float)     ;; double __sbv_d2(D2)
```

`tur run` printed `152.25`; `tur jit` printed `226.5`. No diagnostic, exit 0.

`extern-c` is now refused at elaboration when the slot is an HFA, the host is
aarch64, and c2mir is the backend (`g_target_c2mir`, set by `cmd_jit` -- so
`--engine jit` and a manifest `:engine "jit"` are covered too, since both
re-dispatch through it). The native `tur run` / `tur build` path is untouched
and still correct: this narrows what the JIT accepts, not what the language
means.

The predicate is deliberately the *same* one the interpreter uses. The
aggregate-signature renderer moved out of `eval.c` into `jit_ffi_hook.c`
(`tur_jit_ffi_agg_sig_render` / `tur_jit_ffi_adt_is_hfa`) so both paths ask one
classifier -- two copies of an ABI rule is two things to keep in step, and the
point of a refusal is that the two paths agree on what they will not do.

Covered by `jit-ffi-extern-c-hfa-refused-compiled` and, so the refusal cannot
quietly widen, `jit-ffi-extern-c-nonhfa-still-jits` -- a mixed `{double,int}`
aggregate is INTEGER-class, not an HFA, and must keep working end-to-end
(verified: `152` through `tur jit`, matching native). Both are arch-gated in
`tests/run-flags.sh` beside the existing interpreter test and SKIP off aarch64.

### Still silent: an HFA declared inside an inline-C fence

**This report stays OPEN, and not only because the MIR fix is outstanding.**
The original repro at the top of this file -- a bare `extern double
__sbv_f2(F2);` inside a ` ```c ` block -- still returns `225` under `tur jit`
against `152.25` native, with no diagnostic. Re-verified 2026-08-26, after the
`extern-c` refusal landed.

That case is not fixable at the same seam, and the difference is not effort but
information: an `extern-c` declares its types to the elaborator, whereas the
body of an inline-C block is opaque text that only c2mir ever parses. Detecting
an HFA there means classifying C types, which is a c2mir change -- the same
vendored-fork work the real fix needs. So the coverage line is exactly:

| how the native callee is declared | `tur jit` on aarch64 |
| --- | --- |
| `extern-c` with a record slot | refused, TUR-E0711 |
| `call-ptr` / `callback-ptr` (interpreter) | refused, provider diagnostic |
| `extern` inside an inline-C fence | **still silently miscalls** |

## Also worth knowing

*(Everything from here down is the original 2026-08-21 analysis, preserved as
written. It describes the broken behaviour in the present tense; see
"Resolution" at the top for what is true now.)*

This is not only a jit-ffi problem. Any `tur jit` program that calls an
external C function taking or returning an FP aggregate -- e.g. a spice
binding to raylib's `Vector2`-flavored API, exactly the case
[ffi-spices-integration-plan](../upcoming/ffi-spices-integration-plan.md)
targets -- hits it today, with no diagnostic. `tur run` (cc) is correct, so
the two backends disagree, which is the shape of bug that gets blamed on
the spice.

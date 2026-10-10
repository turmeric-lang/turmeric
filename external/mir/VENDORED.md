# Vendored MIR

This directory is a copy of the parts of [MIR](https://github.com/vnmakarov/mir)
that `tur`'s JIT engine compiles: the three translation units `mir.c`,
`mir-gen.c` and `c2mir/c2mir.c`, and every file they `#include` on any target
MIR supports. That is about 2.7 MB of a ~55 MB checkout; most of the rest is
`c-benchmarks/`. `cmake/mir.cmake` builds the `tur_mir` library from it.

The files come from the [turmeric-lang/mir](https://github.com/turmeric-lang/mir)
fork (formerly `rjungemann/mir`; the PR numbers below are that repository's),
at the commit recorded in [`UPSTREAM`](UPSTREAM). MIR is MIT-licensed; its
licence is [`LICENSE`](LICENSE).

## Why a copy, not a fetch or a submodule

Until 2026-10-02, `-DTUR_JIT=ON` cloned the fork with `FetchContent` at configure
time. That network fetch was the reason `TUR_JIT` defaulted OFF: a default
`cmake -S . -B build` must not reach the network. A copy lets a plain clone, a
source tarball, Homebrew, the Dockerfile and every CI job build the JIT with no
network and no extra step, so `TUR_JIT` now defaults ON (`CMakeLists.txt`). A
submodule would have needed `--recursive` everywhere and checks out the whole
55 MB.

## Changing MIR

**Do not edit these files by hand.** The next sync reverts the edit silently.

1. Make the fix in the fork, with a test under its `c-tests/` or `mir-tests/`,
   and merge it there.
2. Re-sync: `bash tools/update-mir.sh <commit>`. The script copies the file list,
   rewrites `UPSTREAM`, and checks that the three TUs still compile from the copy.
3. Add the fix to the log below, rebuild with `-DTUR_JIT=ON`, and run
   `bash tests/run-jit.sh`.

To try a MIR change before pushing it anywhere, build against a local checkout
without touching this directory: `cmake -DTUR_MIR_SOURCE_DIR=$HOME/src/mir ...`.
`bash tools/update-mir.sh --from <dir>` copies such a checkout in as it stands.

Point `tools/update-mir.sh` back at `vnmakarov/mir`
(`MIR_REPOSITORY=https://github.com/vnmakarov/mir.git`) when upstream has
equivalents of every fix below.

## Fixes the fork carries over upstream

```text
The pin points at the turmeric-lang/mir fork (formerly rjungemann/mir): upstream a8ab7c31 (master tip and
full history mirrored there) plus three fixes on fix/make-one-ret-distinct-targets:
  b79e3681 -- make_one_ret merged multi-value rets through the LAST ret's
    operand list, which aliases when simplify canonicalizes a trailing
    `ret 0, 0` to `ret t, t`, returning { second-word, second-word } for a
    two-word struct -- exactly the emitted tail-loop for a self-recursive
    carrier-struct function
    (docs/archive/history/mir-two-word-struct-return-goto-loop-miscompile.md).
  41ff4d94 -- try_spilled_reg_mem overran its 2-entry op_nums[] when one
    insn used the same spilled register in three operand positions
    (`mul r,r,r` from coalesced `r = r * r`), smashing rewrite_insn's frame
    (jit-engine-j0-findings.md section 15).
  90633091 -- c2mir gave the aarch64 target header's fake __uint128_t
    (`struct {unsigned long hi, lo;}`) alignment 8 where AAPCS64 requires
    16.  On Apple that skews the whole signal-context chain, since
    _STRUCT_ARM_NEON_STATE64 is `__uint128_t __v[32]`: ucontext_t comes out
    864 vs clang's 880, so any struct embedding one disagrees between
    JIT-compiled and host-compiled code with no diagnostic.  Carries two
    supporting c2mir fixes -- _Alignas was unparseable in spec_qual_list
    (struct members) and ignored for layout when it did parse
    (jit-engine-j0-findings.md section 30.1,
    docs/archive/history/jit-arm64-uint128-align-struct-layout-skew.md).
  d7e19e8d -- c2mir accepted `#pragma pack` and silently ignored it, laying
    the struct out at natural alignment.  Unlike c2mir's other gaps this one
    does not refuse the input: it compiles, runs, and is wrong (a pack(4)
    struct measured 16 bytes where clang gives 12), with no diagnostic beyond
    "unknown pragma".  <mach/message.h> wraps every Mach message trailer in
    `#pragma pack(push, 4)`, so mach_msg_context_trailer_t came out 64 against
    the SDK's own asserted 60.  The packing is tracked in the preprocessor
    (the only stage that sees the directive) and stamped onto each emitted
    token, so it stays correct across #include nesting; the parser lifts it
    onto the struct node and the layout code caps member alignment
    (docs/archive/jit-c2mir-ignores-pragma-pack.md).
  9c5ad5ef -- `enum [tag] : type` (the C23 enum-type-specifier) was a syntax
    error, so any program including <malloc/malloc.h> failed to parse:
    malloc.h:96 is `typedef enum __enum_options : uint64_t {...}`, and
    __enum_options expands to nothing for a compiler that does not advertise
    __flag_enum__.  struct enum_type already carried enum_basic_type, so size
    /align/conversion needed no change; the rule added is that a fixed
    underlying type IS the enum's type -- never widened or narrowed to fit the
    enumerators -- while a plain enum keeps the range-based inference.
  9c221f96 -- struct_declaration accepted a GCC attribute only AFTER the
    declarator, never leading, though `declaration` already swallows one
    there.  <dirent.h>:84 is `__unused long __padding;` and sys/cdefs.h:172
    defines __unused unconditionally, so the whole DIR struct failed to parse
    -- surfacing far away as "undeclared identifier d" at every later
    `DIR *d = opendir(...)` use, with nothing pointing at the attribute.
  9127f8e1 -- #pragma pack accepted only a literal number, but the
    MinGW/UCRT headers spell it `pack(push,_CRT_PACKING)` (a macro), so
    every UCRT header tripped "expected ')'" plus a misbalanced pop --
    which inside a real nested push silently pops the OUTER region early.
    Object-like macro args are now chased to their number (clang/MSVC
    semantics; measured: MinGW gcc 16 silently IGNORES macro-arg pack
    directives, no warning, but _CRT_PACKING is 8 == the x64 natural cap,
    so the two semantics agree on actual UCRT layout).
  472fa4c6 -- the aarch64 back end had no AAPCS64 HFA concept: it passed every
    aggregate <= 16 bytes in x0..x7, where a conforming compiler puts a
    Homogeneous Floating-point Aggregate (1-4 members, all the same FP type)
    in v0..v7.  Self-consistent within one c2mir compilation, so pure-JIT code
    was fine and nothing complained; wrong the instant c2mir code called a
    natively compiled function taking or returning one, with the callee
    reading whatever was left in the SIMD registers.  DATA-DEPENDENT wrong
    answers, no diagnostic -- `tur run` printed 152.25 where `tur jit` printed
    225 for the same source.  `struct { float x, y; }` vector APIs are exactly
    this shape.  Classification is in caarch64-ABI-code.c, register assignment
    in mir-gen-aarch64.c, sharing two of MIR's five reserved block classes
    (docs/archive/mir-aarch64-fp-aggregate-abi.md).

  07ad0148 -- MIR_set_lazy_gen_interface had never worked on win64: three
    stacked defects in the hand-written wrapper assembly.  _MIR_get_wrapper
    patched its four immediates 10 bytes short (the win64 start_pat opens
    with two 5-byte shadow-space stores that the offsets did not account
    for), so called_func overwrote those stores and the wrapper began
    with a decode of `mov %rax,(%rax)`.  Behind that, _MIR_get_wrapper_end
    reserved 0x28 bytes where alignment needs a multiple of 16 -- the
    adjacent comment already said 0x40 -- and parked xmm0..3 on the 32
    bytes of shadow space the generation hook owns, so the original call
    lost its first two float arguments in transit.  This is what kept
    TUR_JIT_GEN=lazy (the DEFAULT) unusable on Windows; the windows-jit CI
    job was build-only because of it.  Covered by mir-tests/wrapper-abi.c.

    BEWARE reverting the pin below this commit: turmeric no longer carries the
    refusals that used to catch this shape (they were deleted with the fix, in
    jit_ffi_hook.c and elab_fns.c), so an older MIR silently reinstates the
    miscall rather than diagnosing it.

  b7991fcc (merged as 79cb2905, rjungemann/mir#4) -- arithmetic_conversion
    asked MIR_LONG_MAX about every signed type of rank long or above when the
    other operand was unsigned int, so on LLP64 (win64: 32-bit long)
    `long long OP unsigned int` was typed, and computed, as unsigned int:
    `(int64_t)5u - 7u` came out 4294967294.  Now follows C11 6.3.1.8 rank by
    rank; every LP64 result keeps its width.  Silent wrong answers on the
    Windows JIT only (a base-1e9 bignum borrow); covered by
    c-tests/new/llp64-uint-llong-conv.c
    (docs/archive/c2mir-llp64-long-long-vs-unsigned-int.md).

  5f20fb89 (merged as 96c34860, rjungemann/mir#5) -- c2mir reserved a
    struct/union statement expression's result slot at the frame size so
    far, while the function body was still being checked; the stack
    variables are laid out only afterwards, from offset 0, so the slot
    overlapped the first of them -- in practice a by-value struct
    parameter, which the `({ ... })` copy-out then overwrote.  The slots
    are now assigned after the stack layout.  Silent wrong answers on
    x86-64 (a sibling argument or parameter replaced; `-ei` too, so the
    front end, not MIR-gen); covered by c-tests/new/stmtexpr-struct-slot-overlap.c
    (docs/archive/jit-x86-64-struct-valued-statement-expression-miscompiles.md).
    The emitter already stopped producing the shape, so nothing in the
    generated C depends on this; user inline C still can.

  e502b185 (merged as 3c0d8c84, turmeric-lang/mir#6) --
    c2mir rejected every program including <ucontext.h> on Linux aarch64
    ("sys/user.h:30:1: syntax error on struct"): glibc's <sys/user.h>,
    reached through <sys/procfs.h>, declares `__uint128_t vregs[32]`, and the
    target header declared the 16-aligned stand-in only under __APPLE__.  So
    `tur jit` always fell back to cc on linux-aarch64, which failed the
    v0.60.0 release's archive JIT step
    (docs/archive/jit-xopen-source-guard-inert-on-glibc.md).  Declaring it
    alone would have traded the refusal for a silent skew: glibc's
    mcontext_t has `__reserved[4096] __attribute__ ((__aligned__ (16)))`,
    which <sys/cdefs.h> erases for a compiler that is not gcc/clang and c2mir
    ignored anyway, so ucontext_t was 4544/8 against gcc's 4560/16 and
    glibc's getcontext/swapcontext wrote past a JIT-allocated one.  The fix
    also lets attributes through that idiom (an empty function-like
    `#define __attribute__` is ignored outside pedantic mode), merges a run
    of attribute specifiers into one list (glibc <pthread.h>), and makes a
    member's `aligned (N)` raise its alignment as _Alignas does.  Verified
    under qemu-user with arm64 glibc 2.39: ucontext_t, mcontext_t and
    FiberBlock match aarch64 gcc's layout, and a cross-built `tur jit` runs
    hello-world and the fiber/async/channel corpus natively.  Covered by
    c-tests/new/aarch64-linux-uint128-user-h.c and
    c-tests/new/attr-aligned-member.c.  Not tested on macOS or MinGW
    headers, which the `__attribute__` change reaches where they use the
    same erase idiom.

  d9f75585 (turmeric-lang/mir#7, not merged yet -- move the pin to its
    merge commit) -- c2mir had no 128-bit integer:
    x86-64 did not declare __uint128_t at all and aarch64 declared it as a
    layout-only struct, so user inline C doing arithmetic on one failed to
    parse and `tur jit` fell back to cc with TUR-W0070
    (docs/archive/c2mir-rejects-uint128.md).  __int128 is now a keyword,
    __int128_t, __uint128_t and __SIZEOF_INT128__ are predefined on every
    target (the aarch64 stand-in is gone; the layout is the same), and
    every operation is lowered to 64-bit MIR with no runtime library:
    carries, the full 64x64 product from 32-bit halves, shift-subtract
    division, correctly rounded float conversions.  Constants fold in two
    64-bit halves.  An __int128 argument or result travels as two 64-bit
    ones -- self-consistent, and native code agrees except where its ABI
    moves an argument (AAPCS64's even register pair; x86-64 putting it on
    the stack when one register is left).  Unsupported uses (bit-fields,
    switch, va_arg, variadic arguments) are diagnosed, so they fall back
    rather than miscompile.  Covered by c-tests/new/int128-ops.c and
    int128-misc.c there, tests/fixtures/jit-uint128-arith here.  Defining
    __SIZEOF_INT128__ also sends mir-hash.h down its __uint128_t path in
    the self-compiled c2mir of the bootstrap tests.

  8056049 (turmeric-lang/mir#8, stacked on #7) -- c2mir converted to _Bool
    by casting to MIR_T_U8, keeping the low 8 bits: `bool b = 256L` and
    `(bool) 0.5` were false in initialization, assignment, compound
    assignment, ++/--, casts, arguments and returns, in generated code, the
    interpreter and the constant folder alike, and a _Bool bit-field stored
    bit 0 of the int-converted value.  Silent wrong answers under `tur jit`
    only (docs/archive/c2mir-bool-conversion-truncates.md).  The __int128
    conversion hook (gen_conv_if) now compares with zero for a _Bool target,
    the assignment code converts ++/--/compound results to the left side's
    type, cast_value folds with != 0, and a _Bool bit-field keeps its type.
    Covered by c-tests/new/bool-conversion.c there,
    tests/fixtures/jit-inline-c-bool-conversion here.

  d81ebcc3 (turmeric-lang/mir#9, stacked on #8) -- an element of a union's
    array member got its element type's alias, not the union's, so MIR-gen
    let `u.words[0]` read memory from before `u.ld = x`: wrong answers in
    generated code on every target (the interpreter ignores aliases), and
    put_ldouble in mir.c -- exactly that shape -- made a self-compiled c2mir
    write every non-zero long double constant as 0
    (docs/archive/mir-x86-64-long-double-union-pun.md).  N_IND keeps the
    union alias; the __int128 change's ld_pow2 workaround is removed.
    Covered by c-tests/new/union-array-member-alias.c there,
    tests/fixtures/jit-inline-c-union-array-pun here.
```

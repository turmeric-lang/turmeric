# c2mir mis-lexes a `##` operand that starts with `u8`

**Severity:** low (a compile error under `tur jit`, which then falls back to
cc). MIR's C front end (`external/mir/c2mir/c2mir.c`) re-lexes the result of a
token paste as if a leading `u8` were a C11 string-literal prefix (`u8"..."`):

```c
#define CAT(a, b) a##b
int CAT(u8, x)(void) { return 3; }   /* c2mir: "wrong number: 88x" */
int main(void) { return u8x(); }
```

and `CAT(s_, u8x)` -- the `u8` now in the second operand -- comes out as an
undeclared name rather than `s_u8x`.  gcc and clang accept both.  Any paste
operand starting `u8` is affected; `u7x16`, `U8x`, `L8x`, `u16x8`, `i32x4`,
`f64x4` are fine.

Found writing tur-simd's `tur_simd.h` (turmeric-spices, phase S0 of
simd-and-vectorization-plan), whose generator macros pasted
`simd_##T##_load` with `T = u8x16`.  S1's JIT preamble would hit the same:
`u8x16` is one of its planned type names.

## Repro

Through c2mir directly -- a 40-line driver over the vendored sources
(`c2mir_init` / `c2mir_compile` / `MIR_link` / `MIR_interp`, as
`src/jit_engine.c` does) compiling the three lines above.  Under `tur jit`, an
inline-C body containing such a paste fails to compile and the program runs
on the cc fallback (TUR-W0070).

## Workaround

Do not paste a `u8`-prefixed token: pass the whole function-name prefix into
the macro (`TS_BASIC(simd_u8x16, u8x16, ...)` then `F##_load`, where the
operand starts `s`).  That is what `tur_simd.h` does.

## Fix directions

In c2mir's paste handling, lex the concatenated spelling with the identifier
rules first, and treat `u8` / `u` / `U` / `L` as literal prefixes only when
the pasted text continues with a quote.  It belongs in the turmeric-lang/mir
fork, batched with other MIR fixes (CLAUDE.md, "MIR fork": one PR at a time).

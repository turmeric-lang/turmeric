/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   wasm32 / Emscripten target.  This is an ILP32 target: int, long and
   pointers are all 32 bits, long long is 64 bits.  Note that `long` is
   32 bits here, unlike every other c2mir target, so size_t/ptrdiff_t
   must be 32 bits too -- getting this wrong makes every libc call that
   takes a size_t mismatch the real signature.

   long double is mapped to double (8 bytes) rather than Emscripten's
   128-bit long double.  Consequence: the %L printf length modifier is
   not usable from compiled code.  */

#include <stdint.h>

/* Variadic arguments are passed through a buffer the caller builds, the way
   Emscripten lowers f(fixed..., ...) to f(fixed..., void *va_buf).  c2mir
   therefore builds that buffer at the call site, where each argument's real C
   type is still known, and calls the lowered non-variadic signature.

   Doing it any later does not work: mir-interp.c derives variadic argument
   types from an operand's value_mode, which distinguishes only
   int/uint/float/double, so every integer would arrive as a 64-bit one and the
   buffer would be laid out in uniform slots -- right for %d, silently
   truncating for a genuine long long.  */
#define VA_BUF_TARGET_P 1

#define MIR_CHAR_BIT 8

typedef int8_t mir_schar;
typedef int16_t mir_short;
typedef int32_t mir_int;
typedef int32_t mir_long;
typedef int64_t mir_llong;

#define MIR_SCHAR_MIN INT8_MIN
#define MIR_SCHAR_MAX INT8_MAX
#define MIR_SHORT_MIN INT16_MIN
#define MIR_SHORT_MAX INT16_MAX
#define MIR_INT_MIN INT32_MIN
#define MIR_INT_MAX INT32_MAX
#define MIR_LONG_MIN INT32_MIN
#define MIR_LONG_MAX INT32_MAX
#define MIR_LLONG_MIN INT64_MIN
#define MIR_LLONG_MAX INT64_MAX

typedef uint8_t mir_uchar;
typedef uint16_t mir_ushort;
typedef uint32_t mir_uint;
typedef uint32_t mir_ulong;
typedef uint64_t mir_ullong;
typedef uint32_t mir_wchar;
typedef uint16_t mir_char16;
typedef uint32_t mir_char32;

#define MIR_UCHAR_MAX UINT8_MAX
#define MIR_USHORT_MAX UINT16_MAX
#define MIR_UINT_MAX UINT32_MAX
#define MIR_ULONG_MAX UINT32_MAX
#define MIR_ULLONG_MAX UINT64_MAX
#define MIR_WCHAR_MIN 0
#define MIR_WCHAR_MAX UINT32_MAX

typedef mir_schar mir_char;
#define MIR_CHAR_MIN MIR_SCHAR_MIN
#define MIR_CHAR_MAX MIR_SCHAR_MAX

typedef float mir_float;
typedef double mir_double;
typedef double mir_ldouble;

typedef uint8_t mir_bool;
typedef int32_t mir_ptrdiff_t;
typedef uint32_t mir_size_t;

#define MIR_SIZE_MAX UINT32_MAX

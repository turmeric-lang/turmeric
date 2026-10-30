#!/usr/bin/env python3
"""gen-simd.py -- generate stdlib/simd.tur and its interpreter twins.

Phase S1 of docs/upcoming/simd-and-vectorization-plan.md, behind
`--enable=simd`: six 16-byte vector types (the portable set S0 settled on)
and the operations S0's kernels needed.  Like gen-numvec.py, the same
operation is written once here for every type, and becomes

  - stdlib/simd.tur: a `^copy` defstruct per type (one field per lane, l0 ..)
    and an inline-C body per operation.  The body has two lowerings in one
    text: GCC/Clang vector extensions (the struct reinterpreted as a
    `vector_size(16)` vector through a union, so `a + b` is one instruction),
    and, under MIR's c2mir (`tur jit`, `__MIRC__`), plain C over the lanes;
  - src/turi/simd_natives.inc: the interpreter's native for each operation,
    over the struct's lane fields (turi_struct_field / turi_make_struct).

Every lowering does each lane's arithmetic the same way -- signed lanes wrap
(computed unsigned), floats are never fused, a horizontal sum adds in one
fixed tree -- so cc, `tur jit` and turi agree bit for bit.

`python3 tools/gen-simd.py` rewrites both files; `--check` exits 1 if either
is stale.  stdlib/simd.tur goes through `tur fmt` (`$TUR`, else build/tur).
"""
import os
import subprocess
import sys
import tempfile

# T: type, p: prefix, E: element type, C: C element, L: lanes, F: float?,
# U: unsigned C type the scalar lowering wraps in (wide enough that a product
# of two lanes cannot overflow it), VU: the unsigned C lane type the vector
# lowering wraps in, A/ap: the numvec array a vector loads from (or None),
# SUM: what -hsum returns, e0/e1/e2: literal elements for examples (e1 < e0),
# M: the mask type a comparison returns, IX: the index vector -shuffle takes.
TYPES = [
    dict(T="F64x2", M="m64x2", IX="i64x2", p="f64x2", E="float", C="double", L=2, F=True,
         A="F64Vec", ap="f64vec", SUM="float",
         e0="7.1", e1="3.25", e2="-1.5"),
    dict(T="F32x4", M="m32x4", IX="i32x4", p="f32x4", E="float32", C="float", L=4, F=True,
         A="F32Vec", ap="f32vec", SUM="float32",
         e0="(as float32 7.1)", e1="(as float32 3.25)", e2="(as float32 -1.5)"),
    dict(T="I64x2", M="m64x2", IX="i64x2", p="i64x2", E="int", C="int64_t", L=2, F=False,
         U="uint64_t", VU="uint64_t", A="I64Vec", ap="i64vec", SUM="int",
         e0="7", e1="3", e2="-2"),
    dict(T="I32x4", M="m32x4", IX="i32x4", p="i32x4", E="int32", C="int32_t", L=4, F=False,
         U="uint32_t", VU="uint32_t", A="I32Vec", ap="i32vec", SUM="int",
         e0="(as int32 7)", e1="(as int32 3)", e2="(as int32 -2)"),
    dict(T="U16x8", M="m16x8", IX="u16x8", p="u16x8", E="uint16", C="uint16_t", L=8, F=False,
         U="uint32_t", VU="uint16_t", A=None, ap=None, SUM="int",
         e0="(as uint16 7)", e1="(as uint16 3)", e2="(as uint16 65535)"),
    dict(T="U8x16", M="m8x16", IX="u8x16", p="u8x16", E="uint8", C="uint8_t", L=16, F=False,
         U="uint32_t", VU="uint8_t", A="U8Vec", ap="u8vec", SUM="int",
         e0="(as uint8 7)", e1="(as uint8 3)", e2="(as uint8 255)"),
]
# The mask types: integer lanes as wide as the vector's, -1 where a comparison
# held and 0 where it did not.  src: a vector type of the same lane width, for
# the examples.
MASKS = [
    dict(T="M64x2", p="m64x2", E="int", C="int64_t", L=2, src="f64x2"),
    dict(T="M32x4", p="m32x4", E="int32", C="int32_t", L=4, src="i32x4"),
    dict(T="M16x8", p="m16x8", E="int16", C="int16_t", L=8, src="u16x8"),
    dict(T="M8x16", p="m8x16", E="int8", C="int8_t", L=16, src="u8x16"),
]
BY_P = {t["p"]: t for t in TYPES + MASKS}
for _m in MASKS:
    _s = BY_P[_m["src"]]
    _m["set"] = "(%s-lt (%s-splat %s) (%s-splat %s))" % (_s["p"], _s["p"], _s["e1"], _s["p"],
                                                       _s["e0"])


def cstruct(t):
    return "tur_adt_" + t["T"]


def lanes(t):
    return range(t["L"])


def vec_typedefs(t, wrap=False):
    """The vector type a cc body names: tsv (the lane type), or with wrap
    tsu (the unsigned lane type, in which integer arithmetic wraps)."""
    if wrap:
        return ["typedef %s tsu __attribute__((vector_size(16)));" % t["VU"]]
    return ["typedef %s tsv __attribute__((vector_size(16)));" % t["C"]]


def two_lowerings(vec_lines, lane_lines):
    return (["#if !defined(__MIRC__)"] + vec_lines + ["#else"] + lane_lines
            + ["#endif"])


# ---------------------------------------------------------------------------
# Each operation: name, params [(name, kind)] with kind T / E / int / A (the
# numvec array, borrowed), ret kind (T / E / SUM / nil / int / another type's
# T, written "T:<p>"), the C body for type t, the turi native's body, which
# types it exists for, its summary and docs.  A native body reads its
# arguments with the helpers in render_inc's preamble (lane_f / lane_i) and
# builds a struct with mk_T.

def k_splat(t):
    return "\n".join(["%s r;" % cstruct(t)]
                     + ["r.l%d = x;" % k for k in lanes(t)] + ["return r;"])


def n_splat(t):
    if t["F"]:
        x = "(%s)nv_dbl(a[0])" % t["C"]
        out = ["%s x = %s;" % (t["C"], x)]
        out += ["f[%d] = turi_float((double)x);" % k for k in lanes(t)]
    else:
        out = ["%s x = (%s)a[0].as_int;" % (t["C"], t["C"])]
        out += ["f[%d] = turi_int((int64_t)x);" % k for k in lanes(t)]
    return out + ["return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])]


def k_lane(t):
    return "\n".join([
        "if (k < 0 || k >= %d) {" % t["L"],
        "  tur_panic(\"%s-lane: lane out of range\");" % t["p"],
        "  return 0;",
        "}",
        "union { %s s; %s a[%d]; } u = {v};" % (cstruct(t), t["C"], t["L"]),
        "return u.a[k];"])


def n_lane(t):
    return ["int64_t k = a[1].as_int;",
            "if (k < 0 || k >= %d) {" % t["L"],
            "  turi_runtime_panic(env, \"%s-lane: lane out of range\");" % t["p"],
            "  return turi_int(0);",
            "}",
            "bool found; return turi_struct_field(a[0], (uint32_t)k, &found);"]


def k_load(t):
    # Out of bounds, the copy reads a static zero block instead of returning a
    # separate zero value: the two paths then merge a pointer, not a struct,
    # and clang keeps the vector in one register (it split a merged struct
    # into two integer halves and reassembled them, 3x slower).
    return "\n".join([
        "static const %s tsz[%d] = {0};" % (t["C"], t["L"]),
        "struct { int64_t len; %s *data; void *raw; } *h = (void*)(intptr_t)arr;" % t["C"],
        "const %s *d = tsz;" % t["C"],
        "if (i < 0 || i > h->len - %d)" % t["L"],
        "  tur_panic(\"%s-load: index out of bounds\");" % t["p"],
        "else",
        "  d = h->data + i;",
        "%s r;" % cstruct(t)]
        # one 16-byte copy is one unaligned vector load, and keeps the body
        # small enough to inline; c2mir copies lane by lane
        + two_lowerings(["__builtin_memcpy(&r, d, sizeof r);"],
                        ["r.l%d = d[%d];" % (k, k) for k in lanes(t)])
        + ["return r;"])


def n_load(t, nm="load"):
    conv = "turi_float((double)d[%d])" if t["F"] else "turi_int((int64_t)d[%d])"
    return (["TuriNumVecRep *h = nv_of(a[0]); int64_t i = a[1].as_int;",
             "if (!h || i < 0 || i > h->len - %d) {" % t["L"],
             "  turi_runtime_panic(env, \"%s-%s: index out of bounds\");" % (t["p"], nm),
             "  return turi_int(0);",
             "}",
             "const %s *d = (const %s *)h->data + i;" % (t["C"], t["C"])]
            + ["f[%d] = %s;" % (k, conv % k) for k in lanes(t)]
            + ["return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])])


def k_store(t):
    return "\n".join([
        "struct { int64_t len; %s *data; void *raw; } *h = (void*)(intptr_t)arr;" % t["C"],
        "if (i < 0 || i > h->len - %d) {" % t["L"],
        "  tur_panic(\"%s-store!: index out of bounds\");" % t["p"],
        "  return;",
        "}",
        "%s *d = h->data + i;" % t["C"]]
        + two_lowerings(["__builtin_memcpy(d, &v, sizeof v);"],
                        ["d[%d] = v.l%d;" % (k, k) for k in lanes(t)]))


def n_store(t, nm="store!"):
    get = "(%s)lane_f(a[2], %d)" if t["F"] else "(%s)lane_i(a[2], %d)"
    return (["TuriNumVecRep *h = nv_of(a[0]); int64_t i = a[1].as_int;",
             "if (!h || i < 0 || i > h->len - %d) {" % t["L"],
             "  turi_runtime_panic(env, \"%s-%s: index out of bounds\");" % (t["p"], nm),
             "  return turi_nil();",
             "}",
             "%s *d = (%s *)h->data + i;" % (t["C"], t["C"])]
            + ["d[%d] = %s;" % (k, get % (t["C"], k)) for k in lanes(t)]
            + ["return turi_nil();"])


def scalar_binop(t, op, k):
    """One lane of a wrapping (integer) or plain (float) binary op."""
    if t["F"]:
        return "a.l%d %s b.l%d" % (k, op, k)
    return "(%s)((%s)a.l%d %s (%s)b.l%d)" % (t["C"], t["U"], k, op, t["U"], k)


def k_binop(op):
    def body(t):
        if t["F"]:
            vec = (vec_typedefs(t)
                   + ["union { %s s; tsv v; } ua = {a}, ub = {b}, ur;" % cstruct(t),
                      "ur.v = ua.v %s ub.v;" % op, "return ur.s;"])
        else:
            vec = (vec_typedefs(t, wrap=True)
                   + ["union { %s s; tsu v; } ua = {a}, ub = {b}, ur;" % cstruct(t),
                      "ur.v = ua.v %s ub.v;" % op, "return ur.s;"])
        lane = (["%s r;" % cstruct(t)]
                + ["r.l%d = %s;" % (k, scalar_binop(t, op, k)) for k in lanes(t)]
                + ["return r;"])
        return "\n".join(two_lowerings(vec, lane))
    return body


def n_binop(op):
    def body(t):
        out = []
        for k in lanes(t):
            if t["F"]:
                out.append("{ %s x = (%s)lane_f(a[0], %d), y = (%s)lane_f(a[1], %d); "
                           "%s r = x %s y; f[%d] = turi_float((double)r); }"
                           % (t["C"], t["C"], k, t["C"], k, t["C"], op, k))
            else:
                out.append("{ %s x = (%s)lane_i(a[0], %d), y = (%s)lane_i(a[1], %d); "
                           "%s r = (%s)((%s)x %s (%s)y); f[%d] = turi_int((int64_t)r); }"
                           % (t["C"], t["C"], k, t["C"], k, t["C"], t["C"], t["U"], op,
                              t["U"], k))
        return out + ["return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])]
    return body


def k_div(t):
    if t["F"]:
        return k_binop("/")(t)
    signed = t["C"].startswith("int")
    out = ["%s r;" % cstruct(t)]
    for k in lanes(t):
        out.append("if (b.l%d == 0) {" % k)
        out.append("  tur_panic(\"%s-div: division by zero\");" % t["p"])
        out.append("  return a;")
        out.append("}")
        if signed:
            # INT_MIN / -1 overflows; the wrapped quotient is -INT_MIN = INT_MIN
            out.append("r.l%d = b.l%d == -1 ? (%s)(0 - (%s)a.l%d) : a.l%d / b.l%d;"
                       % (k, k, t["C"], t["U"], k, k, k))
        else:
            out.append("r.l%d = a.l%d / b.l%d;" % (k, k, k))
    return "\n".join(out + ["return r;"])


def n_div(t):
    if t["F"]:
        return n_binop("/")(t)
    signed = t["C"].startswith("int")
    out = []
    for k in lanes(t):
        out.append("{ %s x = (%s)lane_i(a[0], %d), y = (%s)lane_i(a[1], %d);"
                   % (t["C"], t["C"], k, t["C"], k))
        out.append("  if (y == 0) { turi_runtime_panic(env, \"%s-div: division by zero\"); "
                   "return turi_int(0); }" % t["p"])
        if signed:
            out.append("  %s r = y == -1 ? (%s)(0 - (%s)x) : x / y;" % (t["C"], t["C"], t["U"]))
        else:
            out.append("  %s r = x / y;" % t["C"])
        out.append("  f[%d] = turi_int((int64_t)r); }" % k)
    return out + ["return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])]


def k_neg(t):
    if t["F"]:
        vec = (vec_typedefs(t)
               + ["union { %s s; tsv v; } ua = {a}, ur;" % cstruct(t),
                  "ur.v = -ua.v;", "return ur.s;"])
        lane = ["%s r;" % cstruct(t)] + ["r.l%d = -a.l%d;" % (k, k) for k in lanes(t)]
    else:
        vec = (vec_typedefs(t, wrap=True)
               + ["union { %s s; tsu v; } ua = {a}, ur;" % cstruct(t),
                  "ur.v = -ua.v;", "return ur.s;"])
        lane = (["%s r;" % cstruct(t)]
                + ["r.l%d = (%s)(0 - (%s)a.l%d);" % (k, t["C"], t["U"], k) for k in lanes(t)])
    return "\n".join(two_lowerings(vec, lane + ["return r;"]))


def n_neg(t):
    out = []
    for k in lanes(t):
        if t["F"]:
            out.append("{ %s x = (%s)lane_f(a[0], %d); %s r = -x; f[%d] = turi_float((double)r); }"
                       % (t["C"], t["C"], k, t["C"], k))
        else:
            out.append("{ %s x = (%s)lane_i(a[0], %d); %s r = (%s)(0 - (%s)x); "
                       "f[%d] = turi_int((int64_t)r); }"
                       % (t["C"], t["C"], k, t["C"], t["C"], t["U"], k))
    return out + ["return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])]


def k_minmax(which):
    cmp = "<" if which == "min" else ">"

    def body(t):
        # b[k] < a[k] ? b[k] : a[k] -- one compare-and-select per lane, which
        # compilers vectorize; a NaN in b never wins, a NaN in a is kept
        return "\n".join(["%s r;" % cstruct(t)]
                         + ["r.l%d = b.l%d %s a.l%d ? b.l%d : a.l%d;" % (k, k, cmp, k, k, k)
                            for k in lanes(t)] + ["return r;"])
    return body


def n_minmax(which):
    cmp = "<" if which == "min" else ">"

    def body(t):
        out = []
        for k in lanes(t):
            get = "lane_f" if t["F"] else "lane_i"
            mk1 = "turi_float((double)r)" if t["F"] else "turi_int((int64_t)r)"
            out.append("{ %s x = (%s)%s(a[0], %d), y = (%s)%s(a[1], %d); "
                       "%s r = y %s x ? y : x; f[%d] = %s; }"
                       % (t["C"], t["C"], get, k, t["C"], get, k, t["C"], cmp, k, mk1))
        return out + ["return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])]
    return body


def k_unary_math(fn):
    """sqrt / abs, per lane: no vector-extension operator exists for them, and
    each lane is one instruction (sqrtsd, andpd) compilers pair up."""
    def body(t):
        sfx = "f" if t["C"] == "float" else ""
        vec = (["%s r;" % cstruct(t)]
               + ["r.l%d = __builtin_%s%s(a.l%d);" % (k, fn, sfx, k) for k in lanes(t)]
               + ["return r;"])
        lane = (["%s r;" % cstruct(t)]
                + ["r.l%d = %s%s(a.l%d);" % (k, fn, sfx, k) for k in lanes(t)]
                + ["return r;"])
        # the lane lowering calls libm: without its prototype c2mir takes
        # sqrt() to return int
        return "\n".join(["/* __tur_include__: #include <math.h> */"]
                         + two_lowerings(vec, lane))
    return body


def n_unary_math(fn):
    def body(t):
        sfx = "f" if t["C"] == "float" else ""
        out = ["{ %s x = (%s)lane_f(a[0], %d); %s r = %s%s(x); f[%d] = turi_float((double)r); }"
               % (t["C"], t["C"], k, t["C"], fn, sfx, k) for k in lanes(t)]
        return out + ["return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])]
    return body


def float_tree(t, lane):
    if t["L"] == 2:
        return "%s + %s" % (lane(0), lane(1))
    return "(%s + %s) + (%s + %s)" % (lane(0), lane(1), lane(2), lane(3))


def k_hsum(t):
    if t["F"]:
        return "return %s;" % float_tree(t, lambda k: "a.l%d" % k)
    # integer lanes widen to int and the sum wraps modulo 2^64
    return ("return (int64_t)(" + " + ".join("(uint64_t)(int64_t)a.l%d" % k for k in lanes(t))
            + ");")


def n_hsum(t):
    if t["F"]:
        out = ["%s v%d = (%s)lane_f(a[0], %d);" % (t["C"], k, t["C"], k) for k in lanes(t)]
        return out + ["%s r = %s;" % (t["C"], float_tree(t, lambda k: "v%d" % k)),
                      "return turi_float((double)r);"]
    return ["uint64_t s = 0;"] + [
        "s += (uint64_t)(int64_t)(%s)lane_i(a[0], %d);" % (t["C"], k) for k in lanes(t)] + [
        "return turi_int((int64_t)s);"]


def k_widen(half):
    def body(t):
        u16 = BY_P["u16x8"]
        off = 0 if half == "lo" else 8
        idx = ", ".join(str(off + k) for k in range(8))
        # a shuffle to the half, then a lane-width conversion: one punpck
        vec = ["typedef uint8_t tsb __attribute__((vector_size(16)));",
               "typedef uint8_t tsh __attribute__((vector_size(8)));",
               "typedef uint16_t tsw __attribute__((vector_size(16)));",
               "union { %s s; tsb v; } ua = {a};" % cstruct(t),
               "union { %s s; tsw v; } ur;" % cstruct(u16),
               "tsh h = __builtin_shufflevector(ua.v, ua.v, %s);" % idx,
               "ur.v = __builtin_convertvector(h, tsw);",
               "return ur.s;"]
        lane = (["%s r;" % cstruct(u16)]
                + ["r.l%d = a.l%d;" % (k, off + k) for k in range(8)] + ["return r;"])
        return "\n".join(two_lowerings(vec, lane))
    return body


def n_widen(half):
    def body(t):
        off = 0 if half == "lo" else 8
        return (["f[%d] = turi_int((int64_t)(uint8_t)lane_i(a[0], %d));" % (k, off + k)
                 for k in range(8)] + ["return mk(env, \"U16x8\", f, 8);"])
    return body


def k_narrow(t):
    u8 = BY_P["u8x16"]
    vec = ["typedef uint8_t tsb __attribute__((vector_size(16)));",
           "typedef uint8_t tsh __attribute__((vector_size(8)));",
           "typedef uint16_t tsw __attribute__((vector_size(16)));",
           "union { %s s; tsw v; } ul = {lo}, uh = {hi};" % cstruct(t),
           "union { %s s; tsb v; } ur;" % cstruct(u8),
           "tsh nl = __builtin_convertvector(ul.v, tsh), nh = __builtin_convertvector(uh.v, tsh);",
           "ur.v = __builtin_shufflevector(nl, nh, %s);" % ", ".join(str(k) for k in range(16)),
           "return ur.s;"]
    lane = (["%s r;" % cstruct(u8)]
            + ["r.l%d = (uint8_t)lo.l%d;" % (k, k) for k in range(8)]
            + ["r.l%d = (uint8_t)hi.l%d;" % (8 + k, k) for k in range(8)]
            + ["return r;"])
    return "\n".join(two_lowerings(vec, lane))


def n_narrow(t):
    return (["f[%d] = turi_int((int64_t)(uint8_t)lane_i(a[0], %d));" % (k, k) for k in range(8)]
            + ["f[%d] = turi_int((int64_t)(uint8_t)lane_i(a[1], %d));" % (8 + k, k)
               for k in range(8)]
            + ["return mk(env, \"U8x16\", f, 16);"])


def k_shr(t):
    # A count of 16 or more shifts every bit out, so the lanes are zero; a
    # negative count is taken as one of those too.  No panic, so a constant
    # shift in a loop carries no check.
    guard = ["if ((uint64_t)n > 15) {",
             "  %s z = {0};" % cstruct(t),
             "  return z;",
             "}"]
    vec = (vec_typedefs(t)
           + ["union { %s s; tsv v; } u = {v};" % cstruct(t),
              "u.v = u.v >> (uint16_t)n;", "return u.s;"])
    lane = (["%s r;" % cstruct(t)]
            + ["r.l%d = (uint16_t)(v.l%d >> n);" % (k, k) for k in range(8)] + ["return r;"])
    return "\n".join(guard + two_lowerings(vec, lane))


def n_shr(t):
    return (["uint64_t sh = (uint64_t)a[1].as_int;"]
            + ["f[%d] = turi_int(sh > 15 ? 0 : (int64_t)(uint16_t)((uint16_t)lane_i(a[0], %d) >> sh));"
               % (k, k) for k in range(8)]
            + ["return mk(env, \"U16x8\", f, 8);"])


def k_load_unchecked(t):
    return "\n".join([
        "struct { int64_t len; %s *data; void *raw; } *h = (void*)(intptr_t)arr;" % t["C"],
        "const %s *d = h->data + i;" % t["C"],
        "%s r;" % cstruct(t)]
        + two_lowerings(["__builtin_memcpy(&r, d, sizeof r);"],
                        ["r.l%d = d[%d];" % (k, k) for k in lanes(t)])
        + ["return r;"])


def k_store_unchecked(t):
    return "\n".join([
        "struct { int64_t len; %s *data; void *raw; } *h = (void*)(intptr_t)arr;" % t["C"],
        "%s *d = h->data + i;" % t["C"]]
        + two_lowerings(["__builtin_memcpy(d, &v, sizeof v);"],
                        ["d[%d] = v.l%d;" % (k, k) for k in lanes(t)]))


def mask_typedef(m):
    return "typedef %s tsm __attribute__((vector_size(16)));" % m["C"]


def mk_ret(t):
    return "return mk(env, \"%s\", f, %d);" % (t["T"], t["L"])


def k_cmp(op):
    def body(t):
        m = BY_P[t["M"]]
        # a vector comparison is already -1 / 0 per lane, as wide as the lane
        vec = (vec_typedefs(t) + [mask_typedef(m),
               "union { %s s; tsv v; } ua = {a}, ub = {b};" % cstruct(t),
               "union { %s s; tsm v; } ur;" % cstruct(m),
               "ur.v = (tsm)(ua.v %s ub.v);" % op, "return ur.s;"])
        lane = (["%s r;" % cstruct(m)]
                + ["r.l%d = a.l%d %s b.l%d ? -1 : 0;" % (k, k, op, k) for k in lanes(t)]
                + ["return r;"])
        return "\n".join(two_lowerings(vec, lane))
    return body


def n_cmp(op):
    def body(t):
        m = BY_P[t["M"]]
        get = "lane_f" if t["F"] else "lane_i"
        return (["f[%d] = turi_int((%s)%s(a[0], %d) %s (%s)%s(a[1], %d) ? -1 : 0);"
                 % (k, t["C"], get, k, op, t["C"], get, k) for k in lanes(t)] + [mk_ret(m)])
    return body


def k_select(t):
    m = BY_P[t["M"]]
    # the vector's bits viewed as mask-width integers: one and / andnot / or
    vec = [mask_typedef(m),
           "union { %s s; tsm v; } um = {m};" % cstruct(m),
           "union { %s s; tsm v; } ua = {a}, ub = {b}, ur;" % cstruct(t),
           "tsm mm = (tsm)(um.v != 0);",
           "ur.v = (ua.v & mm) | (ub.v & ~mm);",
           "return ur.s;"]
    lane = (["%s r;" % cstruct(t)]
            + ["r.l%d = m.l%d ? a.l%d : b.l%d;" % (k, k, k, k) for k in lanes(t)]
            + ["return r;"])
    return "\n".join(two_lowerings(vec, lane))


def n_select(t):
    return (["bool found;"]
            + ["f[%d] = lane_i(a[0], %d) != 0 ? turi_struct_field(a[1], %d, &found) "
               ": turi_struct_field(a[2], %d, &found);" % (k, k, k, k) for k in lanes(t)]
            + [mk_ret(t)])


def k_shuffle(t):
    # One text for both lowerings: with constant indices clang and gcc fold the
    # lane reads into one shufps / pshufd; with variable ones it is a lane loop
    # (baseline x86-64 has no variable shuffle below SSSE3's pshufb).
    return "\n".join(["union { %s s; %s a[%d]; } u = {v};" % (cstruct(t), t["C"], t["L"]),
                      "%s r;" % cstruct(t)]
                     + ["r.l%d = u.a[idx.l%d & %d];" % (k, k, t["L"] - 1) for k in lanes(t)]
                     + ["return r;"])


def n_shuffle(t):
    return (["bool found;"]
            + ["f[%d] = turi_struct_field(a[0], (uint32_t)(lane_i(a[1], %d) & %d), &found);"
               % (k, k, t["L"] - 1) for k in lanes(t)]
            + [mk_ret(t)])


def k_i32_to_f32(t):
    f = BY_P["f32x4"]
    vec = ["typedef int32_t tsi __attribute__((vector_size(16)));",
           "typedef float tsf __attribute__((vector_size(16)));",
           "union { %s s; tsi v; } ua = {a};" % cstruct(t),
           "union { %s s; tsf v; } ur;" % cstruct(f),
           "ur.v = __builtin_convertvector(ua.v, tsf);",
           "return ur.s;"]
    lane = (["%s r;" % cstruct(f)] + ["r.l%d = (float)a.l%d;" % (k, k) for k in range(4)]
            + ["return r;"])
    return "\n".join(two_lowerings(vec, lane))


def n_i32_to_f32(t):
    return (["f[%d] = turi_float((double)(float)(int32_t)lane_i(a[0], %d));" % (k, k)
             for k in range(4)] + ["return mk(env, \"F32x4\", f, 4);"])


# float -> int32, truncating and saturating: NaN is 0, at or above 2^31 is
# INT32_MAX, below -2^31 is INT32_MIN.  Every lane is defined, where a C cast
# of an out-of-range float is undefined behaviour.
F2I = ("%(x)s != %(x)s ? 0 : %(x)s >= 2147483648.0f ? 2147483647 "
       ": %(x)s < -2147483648.0f ? (-2147483647 - 1) : (int32_t)%(x)s")


def k_f32_to_i32(t):
    i = BY_P["i32x4"]
    # the out-of-range lanes are zeroed before cvttps2dq and given their
    # saturated value after, so the conversion itself only sees lanes in range
    vec = ["typedef float tsf __attribute__((vector_size(16)));",
           "typedef int32_t tsi __attribute__((vector_size(16)));",
           "union { %s s; tsf v; } ua = {a};" % cstruct(t),
           "union { %s s; tsi v; } ur;" % cstruct(i),
           "tsf x = ua.v;",
           "tsi big = (tsi)(x >= 2147483648.0f), small = (tsi)(x < -2147483648.0f);",
           "tsi bad = (tsi)(x != x) | big | small;",
           "union { tsf f; tsi i; } c = {x};",
           "c.i &= ~bad;",
           "tsi r = __builtin_convertvector(c.f, tsi);",
           "ur.v = (r & ~bad) | (big & 2147483647) | (small & (-2147483647 - 1));",
           "return ur.s;"]
    lane = (["%s r;" % cstruct(i)]
            + ["r.l%d = %s;" % (k, F2I % {"x": "a.l%d" % k}) for k in range(4)]
            + ["return r;"])
    return "\n".join(two_lowerings(vec, lane))


def n_f32_to_i32(t):
    out = []
    for k in range(4):
        out.append("{ float x = (float)lane_f(a[0], %d); int32_t r = %s; "
                   "f[%d] = turi_int((int64_t)r); }" % (k, F2I % {"x": "x"}, k))
    return out + ["return mk(env, \"I32x4\", f, 4);"]


def k_f32_widen(half):
    def body(t):
        d = BY_P["f64x2"]
        off = 0 if half == "lo" else 2
        vec = ["typedef float tsf __attribute__((vector_size(16)));",
               "typedef float tsh __attribute__((vector_size(8)));",
               "typedef double tsd __attribute__((vector_size(16)));",
               "union { %s s; tsf v; } ua = {a};" % cstruct(t),
               "union { %s s; tsd v; } ur;" % cstruct(d),
               "tsh h = __builtin_shufflevector(ua.v, ua.v, %d, %d);" % (off, off + 1),
               "ur.v = __builtin_convertvector(h, tsd);",
               "return ur.s;"]
        lane = ["%s r;" % cstruct(d), "r.l0 = (double)a.l%d;" % off,
                "r.l1 = (double)a.l%d;" % (off + 1), "return r;"]
        return "\n".join(two_lowerings(vec, lane))
    return body


def n_f32_widen(half):
    def body(t):
        off = 0 if half == "lo" else 2
        return (["f[%d] = turi_float((double)(float)lane_f(a[0], %d));" % (k, off + k)
                 for k in range(2)] + ["return mk(env, \"F64x2\", f, 2);"])
    return body


def k_f64_narrow(t):
    f = BY_P["f32x4"]
    vec = ["typedef double tsd __attribute__((vector_size(16)));",
           "typedef float tsh __attribute__((vector_size(8)));",
           "typedef float tsf __attribute__((vector_size(16)));",
           "union { %s s; tsd v; } ul = {lo}, uh = {hi};" % cstruct(t),
           "union { %s s; tsf v; } ur;" % cstruct(f),
           "tsh nl = __builtin_convertvector(ul.v, tsh), nh = __builtin_convertvector(uh.v, tsh);",
           "ur.v = __builtin_shufflevector(nl, nh, 0, 1, 2, 3);",
           "return ur.s;"]
    lane = ["%s r;" % cstruct(f), "r.l0 = (float)lo.l0;", "r.l1 = (float)lo.l1;",
            "r.l2 = (float)hi.l0;", "r.l3 = (float)hi.l1;", "return r;"]
    return "\n".join(two_lowerings(vec, lane))


def n_f64_narrow(t):
    return (["f[%d] = turi_float((double)(float)lane_f(a[%d], %d));" % (k, k // 2, k % 2)
             for k in range(4)] + ["return mk(env, \"F32x4\", f, 4);"])


# ---- the mask operations: a lane is set when it is nonzero; every result
# lane is -1 or 0 ----
def k_mask_logic(op):
    def body(m):
        if op == "not":
            vexpr, lexpr = "(tsm)(ua.v == 0)", "a.l%d == 0"
            params = "ua = {a}, ur"
        else:
            c = "&" if op == "and" else "|"
            vexpr = "(tsm)((ua.v != 0) %s (ub.v != 0))" % c
            lexpr = "(a.l%%d != 0) %s (b.l%%d != 0)" % c
            params = "ua = {a}, ub = {b}, ur"
        vec = [mask_typedef(m), "union { %s s; tsm v; } %s;" % (cstruct(m), params),
               "ur.v = %s;" % vexpr, "return ur.s;"]
        lane = (["%s r;" % cstruct(m)]
                + ["r.l%d = %s ? -1 : 0;" % (k, lexpr.replace("%d", str(k))) for k in lanes(m)]
                + ["return r;"])
        return "\n".join(two_lowerings(vec, lane))
    return body


def n_mask_logic(op):
    def body(m):
        out = []
        for k in lanes(m):
            if op == "not":
                e = "lane_i(a[0], %d) == 0" % k
            else:
                e = "(lane_i(a[0], %d) != 0) %s (lane_i(a[1], %d) != 0)" % (
                    k, "&" if op == "and" else "|", k)
            out.append("f[%d] = turi_int(%s ? -1 : 0);" % (k, e))
        return out + [mk_ret(m)]
    return body


def k_mask_reduce(op):
    def body(m):
        if op == "any":
            return "return (" + " | ".join("a.l%d" % k for k in lanes(m)) + ") != 0;"
        return "return " + " & ".join("(a.l%d != 0)" % k for k in lanes(m)) + ";"
    return body


def n_mask_reduce(op):
    def body(m):
        if op == "any":
            return (["int64_t r = 0;"] + ["r |= lane_i(a[0], %d);" % k for k in lanes(m)]
                    + ["return turi_bool(r != 0);"])
        return (["bool r = true;"] + ["r = r && lane_i(a[0], %d) != 0;" % k for k in lanes(m)]
                + ["return turi_bool(r);"])
    return body


ALL = [t["p"] for t in TYPES]
FLOATS = ["f64x2", "f32x4"]
LOADABLE = [t["p"] for t in TYPES if t["A"]]

OPS = [
    dict(name="splat", types=ALL, params=[("x", "E")], ret="T", body=k_splat, nat=n_splat,
         summary="every lane x.", doc=["x -- the value"], ex="({p}-splat {e0})"),
    dict(name="lane", types=ALL, params=[("v", "T"), ("k", "int")], ret="E", body=k_lane,
         nat=n_lane, summary="lane k of v (0 is the lowest; out of range panics).",
         doc=["v -- the vector", "k -- the lane, 0 to {L1}"],
         ex="({p}-lane ({p}-splat {e0}) 0)   ; => {e0}"),
    dict(name="load", types=LOADABLE, params=[("arr", "A"), ("i", "int")], ret="T",
         body=k_load, nat=n_load,
         summary="lanes from arr[i] .. arr[i + {L1}] (out of bounds panics, once).",
         doc=["arr -- the array (borrowed)", "i -- the first element"],
         ex="(let [a ({ap}-new 8 {e0})] ({p}-load a 0))"),
    dict(name="store!", types=LOADABLE, params=[("arr", "A"), ("i", "int"), ("v", "T")],
         ret="nil", body=k_store, nat=n_store,
         summary="write v's lanes to arr[i] .. arr[i + {L1}] (out of bounds panics, once).",
         doc=["arr -- the array written (borrowed)", "i -- the first element",
              "v -- the vector"],
         ex="({p}-store! a 0 ({p}-splat {e1}))"),
    dict(name="load-unchecked", types=LOADABLE, params=[("arr", "A"), ("i", "int")], ret="T",
         body=k_load_unchecked, nat=lambda t: n_load(t, "load-unchecked"), unsafe=True,
         summary="lanes from arr[i] .. arr[i + {L1}], NOT bounds-checked.",
         doc=["arr -- the array (borrowed)",
              "i -- the first element; the caller proves 0 <= i and i + {L} <= len"],
         ex="(unsafe ({p}-load-unchecked a 0))"),
    dict(name="store-unchecked!", types=LOADABLE, params=[("arr", "A"), ("i", "int"), ("v", "T")],
         ret="nil", body=k_store_unchecked, nat=lambda t: n_store(t, "store-unchecked!"),
         unsafe=True,
         summary="write v's lanes to arr[i] .. arr[i + {L1}], NOT bounds-checked.",
         doc=["arr -- the array written (borrowed)",
              "i -- the first element; the caller proves 0 <= i and i + {L} <= len",
              "v -- the vector"],
         ex="(unsafe ({p}-store-unchecked! a 0 ({p}-splat {e1})))"),
    dict(name="add", types=ALL, params=[("a", "T"), ("b", "T")], ret="T", body=k_binop("+"),
         nat=n_binop("+"), summary="lanewise a + b.", doc=["a -- a vector", "b -- a vector"],
         ex="({p}-add ({p}-splat {e0}) ({p}-splat {e1}))"),
    dict(name="sub", types=ALL, params=[("a", "T"), ("b", "T")], ret="T", body=k_binop("-"),
         nat=n_binop("-"), summary="lanewise a - b.", doc=["a -- a vector", "b -- a vector"],
         ex="({p}-sub ({p}-splat {e0}) ({p}-splat {e1}))"),
    dict(name="mul", types=ALL, params=[("a", "T"), ("b", "T")], ret="T", body=k_binop("*"),
         nat=n_binop("*"), summary="lanewise a * b.", doc=["a -- a vector", "b -- a vector"],
         ex="({p}-mul ({p}-splat {e0}) ({p}-splat {e1}))"),
    dict(name="div", types=ALL, params=[("a", "T"), ("b", "T")], ret="T", body=k_div,
         nat=n_div, summary="lanewise a / b.", doc=["a -- a vector", "b -- a vector"],
         ex="({p}-div ({p}-splat {e0}) ({p}-splat {e1}))"),
    dict(name="neg", types=ALL, params=[("a", "T")], ret="T", body=k_neg, nat=n_neg,
         summary="lanewise -a.", doc=["a -- a vector"], ex="({p}-neg ({p}-splat {e0}))"),
    dict(name="min", types=ALL, params=[("a", "T"), ("b", "T")], ret="T",
         body=k_minmax("min"), nat=n_minmax("min"),
         summary="lanewise minimum: lane k is b[k] < a[k] ? b[k] : a[k].",
         doc=["a -- a vector", "b -- a vector"],
         ex="({p}-min ({p}-splat {e0}) ({p}-splat {e1}))"),
    dict(name="max", types=ALL, params=[("a", "T"), ("b", "T")], ret="T",
         body=k_minmax("max"), nat=n_minmax("max"),
         summary="lanewise maximum: lane k is b[k] > a[k] ? b[k] : a[k].",
         doc=["a -- a vector", "b -- a vector"],
         ex="({p}-max ({p}-splat {e0}) ({p}-splat {e1}))"),
    dict(name="sqrt", types=FLOATS, params=[("a", "T")], ret="T", body=k_unary_math("sqrt"),
         nat=n_unary_math("sqrt"), summary="lanewise square root.", doc=["a -- a vector"],
         ex="({p}-sqrt ({p}-splat {e0}))"),
    dict(name="abs", types=FLOATS, params=[("a", "T")], ret="T", body=k_unary_math("fabs"),
         nat=n_unary_math("fabs"), summary="lanewise absolute value.", doc=["a -- a vector"],
         ex="({p}-abs ({p}-splat {e2}))"),
    dict(name="hsum", types=ALL, params=[("a", "T")], ret="SUM", body=k_hsum, nat=n_hsum,
         summary="the sum of the lanes.", doc=["a -- a vector"],
         ex="({p}-hsum ({p}-splat {e1}))"),
    dict(name="widen-lo", types=["u8x16"], params=[("a", "T")], ret="T:u16x8",
         body=k_widen("lo"), nat=n_widen("lo"),
         summary="lanes 0-7 widened to sixteen bits.", doc=["a -- a byte vector"],
         ex="(u8x16-widen-lo (u8x16-splat {e2}))"),
    dict(name="widen-hi", types=["u8x16"], params=[("a", "T")], ret="T:u16x8",
         body=k_widen("hi"), nat=n_widen("hi"),
         summary="lanes 8-15 widened to sixteen bits.", doc=["a -- a byte vector"],
         ex="(u8x16-widen-hi (u8x16-splat {e2}))"),
    dict(name="narrow", types=["u16x8"], params=[("lo", "T"), ("hi", "T")], ret="T:u8x16",
         body=k_narrow, nat=n_narrow,
         summary="lo's lanes then hi's, each truncated to eight bits.",
         doc=["lo -- lanes 0-7 of the result", "hi -- lanes 8-15"],
         ex="(u16x8-narrow (u16x8-splat {e0}) (u16x8-splat {e1}))"),
    dict(name="shr", types=["u16x8"], params=[("v", "T"), ("n", "int")], ret="T",
         body=k_shr, nat=n_shr,
         summary="every lane shifted right n bits, zero-filled (n outside 0 to 15 gives zero lanes).",
         doc=["v -- the vector", "n -- the shift count"],
         ex="(u16x8-shr (u16x8-splat {e2}) 8)"),
] + [
    dict(name=nm, types=ALL, params=[("a", "T"), ("b", "T")], ret="M", body=k_cmp(op),
         nat=n_cmp(op), summary="lanewise a %s b: -1 where it holds, 0 where not." % op,
         doc=["a -- a vector", "b -- a vector"],
         ex="({p}-%s ({p}-splat {e1}) ({p}-splat {e0}))" % nm,
         notes=(["Floats compare as IEEE: a NaN lane is unequal to everything and never less",
                 "or greater."] if nm == "eq" else None))
    for (nm, op) in [("eq", "=="), ("lt", "<"), ("le", "<="), ("gt", ">"), ("ge", ">=")]
] + [
    dict(name="select", types=ALL, params=[("m", "M"), ("a", "T"), ("b", "T")], ret="T",
         body=k_select, nat=n_select,
         summary="lane k is a[k] where m's lane k is set (nonzero), else b[k].",
         doc=["m -- the mask, from a comparison", "a -- the lanes taken where m is set",
              "b -- the lanes taken where it is not"],
         ex="({p}-select ({p}-lt ({p}-splat {e1}) ({p}-splat {e0})) ({p}-splat {e1}) ({p}-splat {e0}))",
         notes=["Branch-free: both a and b are computed.  Lanewise min of a and b is",
                "`(P-select (P-lt a b) a b)`, as `P-min` is."]),
    dict(name="shuffle", types=ALL, params=[("v", "T"), ("idx", "IX")], ret="T",
         body=k_shuffle, nat=n_shuffle,
         summary="lane k is v's lane idx[k] mod {L}.",
         doc=["v -- the vector", "idx -- which lane of v each result lane takes"],
         ex="({p}-shuffle ({p}-splat {e0}) {rev})   ; lanes reversed",
         notes=["Constant indices compile to one shuffle instruction (gcc and clang); an",
                "index outside 0 to {L1} wraps, so there is no out-of-range case."]),
    dict(name="->f32x4", sep="", types=["i32x4"], params=[("a", "T")], ret="T:f32x4",
         body=k_i32_to_f32, nat=n_i32_to_f32,
         summary="each lane converted to float32, rounding to nearest (cvtdq2ps).",
         doc=["a -- the vector"], ex="(i32x4->f32x4 (i32x4-splat {e0}))"),
    dict(name="->i32x4", sep="", types=["f32x4"], params=[("a", "T")], ret="T:i32x4",
         body=k_f32_to_i32, nat=n_f32_to_i32,
         summary="each lane truncated toward zero to int32, saturating.",
         doc=["a -- the vector"], ex="(f32x4->i32x4 (f32x4-splat {e0}))   ; => lanes of 7",
         notes=["Every lane is defined: NaN gives 0, a lane at or above 2^31 gives",
                "2147483647, one below -2^31 gives -2147483648.  (A C cast is undefined",
                "there, and cvttps2dq alone gives -2147483648 for all three.)"]),
    dict(name="widen-lo", types=["f32x4"], params=[("a", "T")], ret="T:f64x2",
         body=k_f32_widen("lo"), nat=n_f32_widen("lo"),
         summary="lanes 0 and 1 converted to float (exact).", doc=["a -- the vector"],
         ex="(f32x4-widen-lo (f32x4-splat {e0}))", notes=[]),
    dict(name="widen-hi", types=["f32x4"], params=[("a", "T")], ret="T:f64x2",
         body=k_f32_widen("hi"), nat=n_f32_widen("hi"),
         summary="lanes 2 and 3 converted to float (exact).", doc=["a -- the vector"],
         ex="(f32x4-widen-hi (f32x4-splat {e0}))", notes=[]),
    dict(name="narrow", types=["f64x2"], params=[("lo", "T"), ("hi", "T")], ret="T:f32x4",
         body=k_f64_narrow, nat=n_f64_narrow,
         summary="lo's lanes then hi's, each rounded to float32.",
         doc=["lo -- lanes 0-1 of the result", "hi -- lanes 2-3"],
         ex="(f64x2-narrow (f64x2-splat {e0}) (f64x2-splat {e1}))",
         notes=["Rounds to nearest; a lane beyond float32's range becomes an infinity."]),
]

MASK_OPS = [dict(op, types=[m["p"] for m in MASKS]) for op in [
    dict(name="and", params=[("a", "T"), ("b", "T")], ret="T", body=k_mask_logic("and"),
         nat=n_mask_logic("and"), summary="lane k is set where both a's and b's are.",
         doc=["a -- a mask", "b -- a mask"], ex="({p}-and {set} {set})"),
    dict(name="or", params=[("a", "T"), ("b", "T")], ret="T", body=k_mask_logic("or"),
         nat=n_mask_logic("or"), summary="lane k is set where a's or b's is.",
         doc=["a -- a mask", "b -- a mask"], ex="({p}-or {set} {set})"),
    dict(name="not", params=[("a", "T")], ret="T", body=k_mask_logic("not"),
         nat=n_mask_logic("not"), summary="lane k is set where a's is not.",
         doc=["a -- a mask"], ex="({p}-not {set})"),
    dict(name="any", params=[("a", "T")], ret="bool", body=k_mask_reduce("any"),
         nat=n_mask_reduce("any"), summary="true when some lane of a is set.",
         doc=["a -- a mask"], ex="({p}-any {set})   ; => true"),
    dict(name="all", params=[("a", "T")], ret="bool", body=k_mask_reduce("all"),
         nat=n_mask_reduce("all"), summary="true when every lane of a is set.",
         doc=["a -- a mask"], ex="({p}-all {set})   ; => true"),
]]


def pairs():
    """Every (operation, type) the files define, in file order."""
    for t in TYPES + MASKS:
        for op in OPS + MASK_OPS:
            if t["p"] in op["types"]:
                yield op, t

NOTES = {
    "add": ["Integer lanes wrap.  Floats: one IEEE add per lane, as the scalar `+`."],
    "sub": ["Integer lanes wrap."],
    "mul": ["Integer lanes wrap: a U8x16 product is taken modulo 256, lane by lane --",
            "widen first (u8x16-widen-lo / -hi) for the full product."],
    "div": ["An integer lane divided by zero panics; MIN / -1 wraps to MIN."],
    "neg": ["Integer lanes wrap."],
    "hsum": ["Floats add in one fixed tree -- (0 + 1) for two lanes, ((0 + 1) + (2 + 3))",
             "for four -- the same on every engine.  Integer lanes are widened to int and",
             "the sum wraps modulo 2^64."],
    "load": ["The array's element type is the vector's lane type."],
    "load-unchecked": ["`#fx{Unsafe}`: for a loop that has checked its bounds once.  It can",
                       "not panic, so a call to it carries no panic check and no call site:",
                       "in a tight loop the checked `-load` costs up to 3x (clang).  The",
                       "interpreter checks the index anyway."],
    "store-unchecked!": ["`#fx{Unsafe}`, as `-load-unchecked`.  Stores numbers: no",
                         "TUR_REGION_NOTE."],
}


def kind_type(kind, t):
    if kind == "T":
        return t["T"]
    if kind.startswith("T:"):
        return BY_P[kind[2:]]["T"]
    if kind == "E":
        return t["E"]
    if kind == "SUM":
        return t["SUM"]
    if kind == "A":
        return t["A"]
    if kind == "M":
        return BY_P[t["M"]]["T"]
    if kind == "IX":
        return BY_P[t["IX"]]["T"]
    return {"int": "int", "nil": "nil", "bool": "bool"}[kind]


def fmt_vars(t):
    d = dict(t)
    d["L1"] = t["L"] - 1
    if "IX" in t:
        ix = BY_P[t["IX"]]
        lit = "%d" if ix["E"] == "int" else "(as " + ix["E"] + " %d)"
        d["rev"] = "(%s %s)" % (ix["T"], " ".join(lit % (t["L"] - 1 - k) for k in lanes(t)))
    return d


def op_name(op, t):
    return t["p"] + op.get("sep", "-") + op["name"]


def render_op(op, t):
    d = fmt_vars(t)
    lines = [";;; %s -- %s" % (op_name(op, t), op["summary"].format(**d)), ";;;"]
    # an operation's own notes are templates; NOTES are verbatim
    if op.get("notes") is not None:
        notes = [n.format(**d) for n in op["notes"]]
    else:
        notes = NOTES.get(op["name"], [])
    if notes:
        lines += [";;; " + n for n in notes] + [";;;"]
    lines.append(";;; Parameters:")
    lines += [";;;   " + x.format(**d) for x in op["doc"]]
    lines.append(";;;")
    rt = kind_type(op["ret"], t)
    if rt != "nil":
        lines += [";;; Returns:", ";;;   A %s." % rt, ";;;"]
    lines += [";;; Example:", ";;;   " + op["ex"].format(**d), ";;;", ";;; Since: S1", ""]
    ps = []
    for (n, kind) in op["params"]:
        tt = kind_type(kind, t)
        ps.append(("^borrow %s : %s" if kind == "A" else "%s : %s") % (n, tt))
    lines.append("(defn %s [%s]" % (op_name(op, t), " ".join(ps)))
    attrs = ["  #fx{Unsafe}" if op.get("unsafe") else "  #fx{}"]
    arrs = [n for (n, kind) in op["params"] if kind == "A"]
    if arrs:
        attrs.append("  #%s %s" % ("writes" if op["name"].endswith("!") else "reads", arrs[0]))
    lines += attrs
    lines.append("  : %s" % rt)
    lines.append("  ```c")
    lines += ["  " + bl for bl in op["body"](t).split("\n")]
    lines.append("  ```)")
    return "\n".join(lines) + "\n"


HDR = """;;; tur/simd -- 16-byte vector types and their lanewise operations.
;;;
;;; GENERATED by tools/gen-simd.py -- edit the template there, not this file.
;;;
;;; Phase S1 of docs/upcoming/simd-and-vectorization-plan.md, behind
;;; `--enable=simd`: loading this file without it is an error.
;;;
;;; | Type  | Lanes | Lane     | Loads from |
;;; |-------|-------|----------|------------|
;;; | F64x2 | 2     | `float`  | F64Vec     |
;;; | F32x4 | 4     | `float32`| F32Vec     |
;;; | I64x2 | 2     | `int`    | I64Vec     |
;;; | I32x4 | 4     | `int32`  | I32Vec     |
;;; | U16x8 | 8     | `uint16` | (widen a U8x16) |
;;; | U8x16 | 16    | `uint8`  | U8Vec      |
;;;
;;; Each type is a `^copy` struct with one field per lane (`l0`, `l1`, ...),
;;; so a vector is an ordinary value: a local, a parameter, a result, a struct
;;; field.  `(F64x2 7.1 3.25)` builds one, `(.l1 v)` reads a lane.  Every
;;; type has `P-splat`, `P-lane`, `P-add`, `P-sub`, `P-mul`, `P-div`, `P-neg`,
;;; `P-min`, `P-max` and `P-hsum`, and a `Num` instance, so `{a + b}` is the
;;; lanewise sum; the floats add `P-sqrt` and `P-abs`; all but U16x8 load from
;;; and store to their typed numeric array (`P-load`, `P-store!`, one bounds
;;; check per call); `u8x16-widen-lo` / `-hi`, `u16x8-narrow` and `u16x8-shr`
;;; move bytes through sixteen-bit lanes, where a product fits.
;;;
;;; Comparisons (`P-eq`, `P-lt`, `P-le`, `P-gt`, `P-ge`) return a mask of the
;;; same lane width -- M64x2, M32x4, M16x8 or M8x16, -1 where the comparison
;;; held and 0 where not -- and `P-select` picks each lane from one of two
;;; vectors by a mask, without a branch; the masks combine with `M-and`,
;;; `M-or`, `M-not` and reduce with `M-any` / `M-all`.  `P-shuffle` permutes
;;; lanes by an index vector (one instruction when the indices are
;;; constants).  `i32x4->f32x4`, `f32x4->i32x4` (truncating, saturating),
;;; `f32x4-widen-lo` / `-hi` and `f64x2-narrow` convert between lane types;
;;; every lane of every conversion is defined.
;;;
;;; Compiled with gcc or clang, an operation is one vector instruction (a
;;; GNU vector extension over the struct's bytes); under `tur jit` the same
;;; operation is a loop over the lanes.  Both compute each lane exactly as
;;; the scalar operator would -- integer lanes wrap, float operations are
;;; never fused -- so every engine gives the same bits.  The types are
;;; 16 bytes on purpose: a 32-byte vector lives in memory on gcc without AVX
;;; (the plan's S0 findings).
;;;
;;; Generic code takes a vector by value: a generic function (constrained or
;;; not) and a generic ADT such as `(Option F64x2)` are specialized per type.
;;; A `Vec` element is the exception -- each is boxed on the heap, as every
;;; multi-word element is -- so keep bulk lane data in the numeric arrays and
;;; load from them.
;;;
;;; Since: S1 (simd-and-vectorization-plan)
(load "stdlib/typeclass.tur")
(load "stdlib/numvec.tur")
"""


def render_type(t):
    fields = " ".join("l%d : %s" % (k, t["E"]) for k in lanes(t))
    out = [";; ===== %s =====" % t["T"],
           ";;; %s -- a 16-byte vector of %d %s lanes." % (t["T"], t["L"], t["E"]),
           ";;;",
           ";;; Lanes `l0` .. `l%d`; `(%s %s)` builds one." % (
               t["L"] - 1, t["T"], " ".join([t["e0"]] * t["L"])),
           ";;;",
           ";;; Since: S1",
           "(defstruct ^copy %s [%s])" % (t["T"], fields)]
    return "\n".join(out) + "\n"


def render_num(t):
    p = t["p"]
    return "\n".join([
        ";;; Num [%s] -- lanewise arithmetic, so {a + b} works." % t["T"], "",
        "(definstance Num [%s]" % t["T"],
        "  (add [x y] (%s-add x y))" % p,
        "  (sub [x y] (%s-sub x y))" % p,
        "  (mul [x y] (%s-mul x y))" % p,
        "  (div [x y] (%s-div x y))" % p,
        "  (neg [x] (%s-neg x)))" % p]) + "\n"


def render_mask_type(m):
    on = "-1" if m["E"] == "int" else "(as %s -1)" % m["E"]
    out = [";; ===== %s =====" % m["T"],
           ";;; %s -- a 16-byte mask of %d %s lanes, for the %d-lane vectors." % (
               m["T"], m["L"], m["E"], m["L"]),
           ";;;",
           ";;; A comparison sets a lane to -1 where it held and 0 where it did not;",
           ";;; `-select` and the mask operations take any nonzero lane as set.",
           ";;; `(%s %s)` builds one with every lane set." % (m["T"], " ".join([on] * m["L"])),
           ";;;",
           ";;; Since: S1",
           "(defstruct ^copy %s [%s])" % (
               m["T"], " ".join("l%d : %s" % (k, m["E"]) for k in lanes(m)))]
    return "\n".join(out) + "\n"


def render():
    out = [HDR]
    for t in TYPES:
        out.append(render_type(t))
    for m in MASKS:
        out.append(render_mask_type(m))
    for t in TYPES + MASKS:
        out += [render_op(op, t) for (op, u) in pairs() if u is t]
        if t in TYPES:
            out.append(render_num(t))
    return "".join(out)


# ---- the interpreter twins ----
def c_ident(s):
    return s.replace("->", "_to_").replace("-", "_").replace("!", "_x")


def render_inc():
    out = ["/* simd_natives.inc -- GENERATED by tools/gen-simd.py; do not edit.",
           " *",
           " * The interpreter's natives for stdlib/simd.tur (simd-and-vectorization-",
           " * plan S1): each takes and returns the vector types as TURI_STRUCT values,",
           " * lane k in field k, and does each lane's arithmetic exactly as the",
           " * compiled body does.  Included by interpreter_natives.c after the numvec",
           " * natives (it uses nv_of / nv_dbl). */",
           "",
           "#include <math.h>",
           "",
           "static double lane_f(TuriValue s, uint32_t k) {",
           "    bool found; TuriValue v = turi_struct_field(s, k, &found);",
           "    return v.tag == TURI_FLOAT ? v.as_float : (double)v.as_int;",
           "}",
           "static int64_t lane_i(TuriValue s, uint32_t k) {",
           "    bool found; TuriValue v = turi_struct_field(s, k, &found);",
           "    return v.tag == TURI_FLOAT ? (int64_t)v.as_float : v.as_int;",
           "}",
           "static TuriValue mk(TuriEnv *env, const char *name, TuriValue *f, uint32_t n) {",
           "    return turi_make_struct(env, name, f, n);",
           "}",
           ""]
    regs = []
    for op, t in pairs():
        nm = op_name(op, t)
        cid = "simdn_" + c_ident(nm)
        out.append("static TuriValue %s(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {" % cid)
        out.append("    (void)env; (void)ud; TuriValue f[16]; (void)f;")
        out.append("    if (n < %d) return turi_nil();" % len(op["params"]))
        out += ["    " + l for l in op["nat"](t)]
        out.append("}")
        regs.append((nm, cid))
    out.append("")
    out.append("static void simd_natives_register(TuriEnv *env) {")
    out += ['    turi_env_register_native(env, "%s", %s, NULL);' % r for r in regs]
    out.append("}")
    return "\n".join(out) + "\n"


def native_names():
    """Every native's name and the numvec handle kind of its array argument,
    if any -- for src/turi/native_caps.c rows."""
    rows = []
    for op, t in pairs():
        arr = any(k == "A" for (_, k) in op["params"])
        rows.append((op_name(op, t), t["ap"] if arr else None))
    return rows


ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def tur_fmt(text):
    tur = os.environ.get("TUR") or os.path.join(ROOT, "build", "tur")
    if not os.path.exists(tur):
        sys.stderr.write("gen-simd: no tur at %s to format stdlib/simd.tur "
                         "(build it, or set TUR)\n" % tur)
        sys.exit(2)
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "simd.tur")
        with open(path, "w") as f:
            f.write(text)
        r = subprocess.run([tur, "fmt", path], capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write("gen-simd: tur fmt failed:\n" + r.stderr)
            sys.exit(2)
        return open(path).read()


OUTPUTS = [("stdlib/simd.tur", lambda: tur_fmt(render())),
           ("src/turi/simd_natives.inc", render_inc)]


def main():
    if "--natives" in sys.argv:
        for (nm, ap) in native_names():
            print(nm, ap or "-")
        return 0
    stale = []
    for rel, fn in OUTPUTS:
        path = os.path.join(ROOT, rel)
        text = fn()
        cur = open(path).read() if os.path.exists(path) else ""
        if cur == text:
            continue
        if "--check" in sys.argv:
            stale.append(rel)
        else:
            open(path, "w").write(text)
    if stale:
        sys.stderr.write("stale: %s -- run python3 tools/gen-simd.py\n" % ", ".join(stale))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

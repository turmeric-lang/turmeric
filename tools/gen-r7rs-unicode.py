#!/usr/bin/env python3
"""gen-r7rs-unicode.py -- r7rs-lang-plan R10: the Unicode tables behind
`(scheme char)` for `#lang r7rs`.

Writes ONE block of C twice, byte for byte:

  stdlib/r7rs/unicode.tur    a file-scope ```c block plus three inline-C
                             wrappers the prelude calls (the compiled back end)
  src/turi/r7rs_unicode.inc  the same C, #included by interpreter_natives.c,
                             which registers natives over the wrappers (the
                             interpreter cannot run inline C)

tests/check-r7rs-unicode-sync.sh checks the two copies agree, so the back ends
cannot drift.  The data comes from the Unicode Character Database files
(r7rs-unicode-case-mapping-gaps): fetch them with tools/fetch-ucd.sh, which
pins one ICU release tag so the Unicode version the tables carry moves only
when that tag does, then

  bash tools/fetch-ucd.sh
  python3 tools/gen-r7rs-unicode.py --ucd build/ucd

What is covered, and how it is derived:
  - simple case mapping of a character (char-upcase / -downcase / -foldcase,
    and the titlecase SRFI 13's string-titlecase uses):
    UnicodeData.txt fields 12-14 and CaseFolding.txt's C+S entries, so
    `(char-upcase #\x1F80)` is #\x1F88 while `(char-upcase #\xDF)` stays
    #\xDF, as R7RS wants;
  - full case mapping of a string (string-upcase / -downcase / -foldcase):
    SpecialCasing.txt's unconditional entries (`"\xDF;"` upcases to "SS")
    and CaseFolding.txt's F entries, plus the Final_Sigma context of
    string-downcase (U+03A3 at the end of a word is U+03C2), decided with the
    Cased and Case_Ignorable properties; the language-specific entries
    (Lithuanian, Turkish, Azeri) are not applied;
  - char-alphabetic? (the Alphabetic property, Other_Alphabetic included),
    char-upper-case? and char-lower-case? (the Uppercase / Lowercase
    properties), char-numeric? and digit-value (Nd), char-whitespace? (the
    White_Space property, whose list is fixed here since PropList.txt is
    not among the files ICU ships);
  - CommonMark 0.31's Unicode whitespace (Zs plus tab, LF, FF and CR) and
    Unicode punctuation (P* and S*), for stdlib/unicode.tur
    (markdown-library-plan U), from the blank, punctuation and symbol runs;
  - the runs of SRFI 14's standard character sets (r7rs-srfi-plan S5), from
    the properties above and UnicodeData.txt's General Category: title-case
    (Lt), punctuation (P*), symbol (S*), graphic (L* N* M* S* P*) and blank
    (Zs and U+0009), as SRFI 14's 2019 CharsetDefs note defines them.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N = 0x110000
WHITE_SPACE = [(0x09, 0x0D), (0x20, 0x20), (0x85, 0x85), (0xA0, 0xA0),
               (0x1680, 0x1680), (0x2000, 0x200A), (0x2028, 0x2029),
               (0x202F, 0x202F), (0x205F, 0x205F), (0x3000, 0x3000)]


def is_char(c):
    return not (0xD800 <= c <= 0xDFFF)


def strided(pred):
    """(lo, hi, stride) runs, stride 1 or 2, covering every c with pred(c)."""
    cps = [c for c in range(N) if is_char(c) and pred(c)]
    out, i = [], 0
    while i < len(cps):
        j = i
        if j + 1 < len(cps) and cps[j + 1] - cps[j] in (1, 2):
            step = cps[j + 1] - cps[j]
            while j + 1 < len(cps) and cps[j + 1] - cps[j] == step:
                j += 1
        else:
            step = 1
        out.append((cps[i], cps[j], step))
        i = j + 1
    return out


def map_runs(single):
    """(lo, hi, stride, delta) runs of a codepoint -> codepoint map."""
    return delta_runs([(c, single(c) - c) for c in range(N) if is_char(c) and single(c) != c])


def delta_runs(m):
    """(lo, hi, stride, delta) runs of sorted (codepoint, delta) pairs; a
    delta of 0 is kept (an entry that maps a character to itself)."""
    out, i = [], 0
    while i < len(m):
        c, d = m[i]
        j = i
        if j + 1 < len(m) and m[j + 1][1] == d and m[j + 1][0] - c in (1, 2):
            step = m[j + 1][0] - c
            while j + 1 < len(m) and m[j + 1][1] == d and m[j + 1][0] - m[j][0] == step:
                j += 1
        else:
            step = 1
        out.append((c, m[j][0], step, d))
        i = j + 1
    return out


def c_srange(name, runs):
    rows = ",".join("{0x%X,0x%X,%d}" % r for r in runs)
    return ("static const r7rs_uc_srange %s[%d] = {%s};\n" % (name, len(runs), rows))


def c_map(name, runs):
    rows = ",".join("{0x%X,0x%X,%d,%d}" % r for r in runs)
    return ("static const r7rs_uc_map %s[%d] = {%s};\n" % (name, len(runs), rows))


def c_special(name, rows_in):
    rows = ",".join("{0x%X,{%s}}" % (c, ",".join("0x%X" % x for x in (out + [0, 0, 0])[:3]))
                    for c, out in rows_in)
    return ("static const r7rs_uc_special %s[%d] = {%s};\n" % (name, len(rows_in), rows))


def wrap(text, width=100):
    """Break the long table lines at commas so the files stay readable."""
    out = []
    for line in text.split("\n"):
        while len(line) > width:
            k = line.rfind(",", 0, width)
            if k <= 0:
                break
            out.append(line[:k + 1])
            line = "    " + line[k + 1:]
        out.append(line)
    return "\n".join(out)


class UCD:
    """The four files, parsed once."""
    def __init__(self, d):
        self.gc = {}
        self.decimal = {}
        self.upper = {}
        self.lower = {}
        self.title = {}
        first = None
        for line in open(os.path.join(d, "UnicodeData.txt"), encoding="ascii"):
            f = line.rstrip("\n").split(";")
            if len(f) < 15:
                continue
            c = int(f[0], 16)
            if f[1].endswith(", First>"):
                first = c
                continue
            if f[1].endswith(", Last>"):
                for x in range(first, c + 1):
                    self.gc[x] = f[2]
                first = None
                continue
            self.gc[c] = f[2]
            if f[6]:
                self.decimal[c] = int(f[6])
            if f[12]:
                self.upper[c] = int(f[12], 16)
            if f[13]:
                self.lower[c] = int(f[13], 16)
            if f[14]:
                self.title[c] = int(f[14], 16)
        self.props = {}
        rx = re.compile(r"^([0-9A-F]+)(?:\.\.([0-9A-F]+))?\s*;\s*(\w+)")
        for line in open(os.path.join(d, "DerivedCoreProperties.txt"), encoding="utf-8"):
            m = rx.match(line)
            if not m:
                continue
            lo, hi, name = int(m.group(1), 16), int(m.group(2) or m.group(1), 16), m.group(3)
            self.props.setdefault(name, set()).update(range(lo, hi + 1))
        self.fold_simple = {}
        self.fold_full = {}
        for line in open(os.path.join(d, "CaseFolding.txt"), encoding="utf-8"):
            if not line or line[0] == "#":
                continue
            f = [x.strip() for x in line.split("#")[0].split(";")]
            if len(f) < 3 or not f[0]:
                continue
            c, status, out = int(f[0], 16), f[1], [int(x, 16) for x in f[2].split()]
            if status in ("C", "S"):
                self.fold_simple[c] = out[0]
            elif status == "F":
                self.fold_full[c] = out
        self.sp_lower = {}
        self.sp_upper = {}
        for line in open(os.path.join(d, "SpecialCasing.txt"), encoding="utf-8"):
            body = line.split("#")[0].strip()
            if not body:
                continue
            f = [x.strip() for x in body.split(";")]
            if len(f) < 4:
                continue
            if len(f) >= 5 and f[4]:
                continue   # conditional: Final_Sigma is decided in C; lt/tr/az not applied
            c = int(f[0], 16)
            self.sp_lower[c] = [int(x, 16) for x in f[1].split()]
            self.sp_upper[c] = [int(x, 16) for x in f[3].split()]
        head = open(os.path.join(d, "SpecialCasing.txt"), encoding="utf-8").readline()
        m = re.search(r"SpecialCasing-([0-9.]+)\.txt", head)
        self.version = m.group(1) if m else "unknown"


def c_block(u):
    def has(name):
        ps = u.props.get(name, set())
        return lambda c: c in ps
    alpha = strided(has("Alphabetic"))
    upper = strided(has("Uppercase"))
    lower = strided(has("Lowercase"))
    cased = strided(has("Cased"))
    ci = strided(has("Case_Ignorable"))
    nd = [c for c in range(N) if is_char(c) and u.gc.get(c) == "Nd" and u.decimal.get(c) == 0]
    def cat(*prefixes):
        return lambda c: u.gc.get(c, "Cn").startswith(prefixes)
    title = strided(lambda c: u.gc.get(c) == "Lt")
    punct = strided(cat("P"))
    symbol = strided(cat("S"))
    graphic = strided(cat("L", "N", "M", "S", "P"))
    blank = strided(lambda c: c == 0x09 or u.gc.get(c) == "Zs")
    to_upper = map_runs(lambda c: u.upper.get(c, c))
    to_lower = map_runs(lambda c: u.lower.get(c, c))
    to_fold = map_runs(lambda c: u.fold_simple.get(c, c))
    # Titlecase is the uppercase mapping but for these: the digraphs (U+01C6
    # titlecases to U+01C5, uppercases to U+01C4) and Georgian Mkhedruli,
    # which uppercases to Mtavruli and titlecases to itself.
    to_title = delta_runs(sorted((c, t - c) for c, t in u.title.items()
                                 if is_char(c) and t != u.upper.get(c, c)))
    def full(table, simple):
        return sorted((c, out) for c, out in table.items()
                      if is_char(c) and out != [simple.get(c, c)])
    sp_upper = full(u.sp_upper, u.upper)
    sp_lower = full(u.sp_lower, u.lower)
    sp_fold = full(u.fold_full, u.fold_simple)
    version = u.version
    head = """/* r7rs-lang-plan R10 -- GENERATED by tools/gen-r7rs-unicode.py from Unicode %s.
 * Do not edit: regenerate.  The same text is in stdlib/r7rs/unicode.tur and
 * src/turi/r7rs_unicode.inc (tests/check-r7rs-unicode-sync.sh). */
typedef struct { uint32_t lo, hi; uint8_t stride; } r7rs_uc_srange;
typedef struct { uint32_t lo, hi; uint8_t stride; int32_t delta; } r7rs_uc_map;
typedef struct { uint32_t cp; uint32_t out[3]; } r7rs_uc_special;
""" % version
    tables = (c_srange("r7rs_uc_alpha", alpha) + c_srange("r7rs_uc_upper", upper) +
              c_srange("r7rs_uc_lower", lower) +
              c_srange("r7rs_uc_cased", cased) + c_srange("r7rs_uc_ci", ci) +
              "static const uint32_t r7rs_uc_nd0[%d] = {%s};\n"
              % (len(nd), ",".join("0x%X" % c for c in nd)) +
              c_srange("r7rs_uc_space", [(lo, hi, 1) for lo, hi in WHITE_SPACE]) +
              c_srange("r7rs_uc_title", title) + c_srange("r7rs_uc_punct", punct) +
              c_srange("r7rs_uc_symbol", symbol) + c_srange("r7rs_uc_graphic", graphic) +
              c_srange("r7rs_uc_blank", blank) +
              c_map("r7rs_uc_to_upper", to_upper) + c_map("r7rs_uc_to_lower", to_lower) +
              c_map("r7rs_uc_to_fold", to_fold) + c_map("r7rs_uc_to_title", to_title) +
              c_special("r7rs_uc_sp_upper", sp_upper) + c_special("r7rs_uc_sp_lower", sp_lower) +
              c_special("r7rs_uc_sp_fold", sp_fold))
    code = r"""/* The last run whose lo <= cp, or -1. */
static long r7rs_uc_find(const void *base, size_t n, size_t size, uint32_t cp) {
    long lo = 0, hi = (long)n - 1, best = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        uint32_t v = *(const uint32_t *)((const char *)base + (size_t)mid * size);
        if (v <= cp) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best;
}
static int r7rs_uc_in(const r7rs_uc_srange *t, size_t n, uint32_t cp) {
    long k = r7rs_uc_find(t, n, sizeof *t, cp);
    return k >= 0 && cp <= t[k].hi && (cp - t[k].lo) % t[k].stride == 0;
}
static uint32_t r7rs_uc_map1(const r7rs_uc_map *t, size_t n, uint32_t cp) {
    long k = r7rs_uc_find(t, n, sizeof *t, cp);
    if (k >= 0 && cp <= t[k].hi && (cp - t[k].lo) % t[k].stride == 0)
        return (uint32_t)((int64_t)cp + t[k].delta);
    return cp;
}
/* op 0 upper, 1 lower, 2 fold, 3 title: the simple (one-character)
 * mapping.  Titlecase is the uppercase mapping except where
 * r7rs_uc_to_title says otherwise. */
static int64_t r7rs_uc_mapc(int64_t cp, int64_t op) {
    if (cp < 0 || cp >= 0x110000) return cp;
    if (op == 3) {
        const r7rs_uc_map *t = r7rs_uc_to_title;
        long k = r7rs_uc_find(t, sizeof r7rs_uc_to_title / sizeof *r7rs_uc_to_title, sizeof *t, (uint32_t)cp);
        if (k >= 0 && (uint32_t)cp <= t[k].hi && ((uint32_t)cp - t[k].lo) % t[k].stride == 0)
            return cp + t[k].delta;
        op = 0;
    }
    if (op == 0) return r7rs_uc_map1(r7rs_uc_to_upper, sizeof r7rs_uc_to_upper / sizeof *r7rs_uc_to_upper, (uint32_t)cp);
    if (op == 1) return r7rs_uc_map1(r7rs_uc_to_lower, sizeof r7rs_uc_to_lower / sizeof *r7rs_uc_to_lower, (uint32_t)cp);
    return r7rs_uc_map1(r7rs_uc_to_fold, sizeof r7rs_uc_to_fold / sizeof *r7rs_uc_to_fold, (uint32_t)cp);
}
/* op 0 alphabetic, 1 upper-case, 2 lower-case, 3 whitespace: 1 or 0.
 * op 4: the digit value of an Nd character, or -1.
 * op 5, 6: CommonMark 0.31's Unicode whitespace (Zs, or U+0009, U+000A,
 * U+000C, U+000D) and Unicode punctuation (P* or S*), for stdlib/unicode.tur:
 * 1 or 0. */
static int64_t r7rs_uc_prop(int64_t cp, int64_t op) {
    if (cp < 0 || cp >= 0x110000) return op == 4 ? -1 : 0;
    uint32_t c = (uint32_t)cp;
    switch (op) {
        case 0: return r7rs_uc_in(r7rs_uc_alpha, sizeof r7rs_uc_alpha / sizeof *r7rs_uc_alpha, c);
        case 1: return r7rs_uc_in(r7rs_uc_upper, sizeof r7rs_uc_upper / sizeof *r7rs_uc_upper, c);
        case 2: return r7rs_uc_in(r7rs_uc_lower, sizeof r7rs_uc_lower / sizeof *r7rs_uc_lower, c);
        case 3: return r7rs_uc_in(r7rs_uc_space, sizeof r7rs_uc_space / sizeof *r7rs_uc_space, c);
        case 5: return c == 0x0A || c == 0x0C || c == 0x0D ||
                       r7rs_uc_in(r7rs_uc_blank, sizeof r7rs_uc_blank / sizeof *r7rs_uc_blank, c);
        case 6: return r7rs_uc_in(r7rs_uc_punct, sizeof r7rs_uc_punct / sizeof *r7rs_uc_punct, c) ||
                       r7rs_uc_in(r7rs_uc_symbol, sizeof r7rs_uc_symbol / sizeof *r7rs_uc_symbol, c);
        default: {
            long k = r7rs_uc_find(r7rs_uc_nd0, sizeof r7rs_uc_nd0 / sizeof *r7rs_uc_nd0, sizeof *r7rs_uc_nd0, c);
            return (k >= 0 && c - r7rs_uc_nd0[k] < 10) ? (int64_t)(c - r7rs_uc_nd0[k]) : -1;
        }
    }
}
/* SRFI 14's standard sets as the strided runs above, for stdlib/srfi/14.scm
 * to build its inversion lists from: set 0 letter (Alphabetic), 1 upper-case
 * (Uppercase), 2 lower-case (Lowercase), 3 whitespace (White_Space), 4 digit
 * (Nd), 5 title-case (Lt), 6 punctuation (P*), 7 symbol (S*), 8 graphic
 * (L* N* M* S* P*), 9 blank (Zs and U+0009) -- the definitions of SRFI 14's
 * 2019 CharsetDefs note.  Field 0 is run i's first code point, 1 its last, 2
 * its stride (1 or 2); -1 past the last run. */
static int64_t r7rs_uc_run(int64_t set, int64_t i, int64_t field) {
    const r7rs_uc_srange *t;
    size_t n;
    switch (set) {
        case 0: t = r7rs_uc_alpha; n = sizeof r7rs_uc_alpha / sizeof *r7rs_uc_alpha; break;
        case 1: t = r7rs_uc_upper; n = sizeof r7rs_uc_upper / sizeof *r7rs_uc_upper; break;
        case 2: t = r7rs_uc_lower; n = sizeof r7rs_uc_lower / sizeof *r7rs_uc_lower; break;
        case 3: t = r7rs_uc_space; n = sizeof r7rs_uc_space / sizeof *r7rs_uc_space; break;
        case 4: {
            /* Each Nd zero starts a run of ten digits. */
            size_t nd = sizeof r7rs_uc_nd0 / sizeof *r7rs_uc_nd0;
            if (i < 0 || (size_t)i >= nd) return -1;
            return field == 0 ? (int64_t)r7rs_uc_nd0[i] : field == 1 ? (int64_t)r7rs_uc_nd0[i] + 9 : 1;
        }
        case 5: t = r7rs_uc_title; n = sizeof r7rs_uc_title / sizeof *r7rs_uc_title; break;
        case 6: t = r7rs_uc_punct; n = sizeof r7rs_uc_punct / sizeof *r7rs_uc_punct; break;
        case 7: t = r7rs_uc_symbol; n = sizeof r7rs_uc_symbol / sizeof *r7rs_uc_symbol; break;
        case 8: t = r7rs_uc_graphic; n = sizeof r7rs_uc_graphic / sizeof *r7rs_uc_graphic; break;
        case 9: t = r7rs_uc_blank; n = sizeof r7rs_uc_blank / sizeof *r7rs_uc_blank; break;
        default: return -1;
    }
    if (i < 0 || (size_t)i >= n) return -1;
    return field == 0 ? (int64_t)t[i].lo : field == 1 ? (int64_t)t[i].hi : (int64_t)t[i].stride;
}
static int r7rs_uc_put(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
/* The length of the UTF-8 sequence at s and its codepoint in *cp, or 0 when
 * the bytes there are not one (they are then copied through unchanged). */
static int r7rs_uc_get(const unsigned char *s, uint32_t *cp) {
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int n = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!n) return 0;
    uint32_t v = s[0] & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return n;
}
/* op 0 upper, 1 lower, 2 fold: the full mapping of a UTF-8 string. */
static char *r7rs_uc_string(const char *s, int64_t op) {
    const r7rs_uc_special *sp = op == 0 ? r7rs_uc_sp_upper : op == 1 ? r7rs_uc_sp_lower : r7rs_uc_sp_fold;
    size_t nsp = op == 0 ? sizeof r7rs_uc_sp_upper / sizeof *r7rs_uc_sp_upper
               : op == 1 ? sizeof r7rs_uc_sp_lower / sizeof *r7rs_uc_sp_lower
                         : sizeof r7rs_uc_sp_fold / sizeof *r7rs_uc_sp_fold;
    size_t len = strlen(s);
    char *r = (char *)malloc(len * 12 + 1), *o = r;
    const unsigned char *p = (const unsigned char *)s;
    int prev_cased = 0;   /* Final_Sigma: a cased letter before, case-ignorables skipped */
    while (*p) {
        uint32_t cp;
        int n = r7rs_uc_get(p, &cp);
        if (!n) { *o++ = (char)*p++; prev_cased = 0; continue; }
        p += n;
        if (op == 1 && cp == 0x3A3 && prev_cased) {
            /* SpecialCasing Final_Sigma: not followed by a cased letter,
             * case-ignorables skipped (SpecialCasing.txt's condition). */
            const unsigned char *q = p;
            int next_cased = 0;
            while (*q) {
                uint32_t c2;
                int m = r7rs_uc_get(q, &c2);
                if (!m) break;
                if (r7rs_uc_in(r7rs_uc_ci, sizeof r7rs_uc_ci / sizeof *r7rs_uc_ci, c2)) { q += m; continue; }
                next_cased = r7rs_uc_in(r7rs_uc_cased, sizeof r7rs_uc_cased / sizeof *r7rs_uc_cased, c2);
                break;
            }
            if (!next_cased) { o += r7rs_uc_put(o, 0x3C2); prev_cased = 1; continue; }
        }
        long k = r7rs_uc_find(sp, nsp, sizeof *sp, cp);
        if (k >= 0 && sp[k].cp == cp) {
            for (int i = 0; i < 3 && sp[k].out[i]; i++) o += r7rs_uc_put(o, sp[k].out[i]);
        } else {
            o += r7rs_uc_put(o, (uint32_t)r7rs_uc_mapc(cp, op));
        }
        if (r7rs_uc_in(r7rs_uc_cased, sizeof r7rs_uc_cased / sizeof *r7rs_uc_cased, cp)) prev_cased = 1;
        else if (!r7rs_uc_in(r7rs_uc_ci, sizeof r7rs_uc_ci / sizeof *r7rs_uc_ci, cp)) prev_cased = 0;
    }
    *o = 0;
    return r;
}
"""
    return head + wrap(tables) + code


TUR_HEAD = """;;; r7rs/unicode -- the Unicode tables behind (scheme char) for `#lang r7rs`.
;;;
;;; GENERATED by tools/gen-r7rs-unicode.py -- do not edit; regenerate.  The C
;;; below is also src/turi/r7rs_unicode.inc, which the interpreter compiles in
;;; and registers as natives over these four wrappers.
;;;
;;; Since: r7rs-lang-plan R10
;; The r7rs prelude and stdlib/unicode.tur load this file; nothing else should.
"""

TUR_TAIL = """
;;; r7rs-uc-map__ -- internal: simple case mapping of a codepoint (op 0
;;; upper, 1 lower, 2 fold, 3 title).
(defn r7rs-uc-map__ [cp : int op : int] : int
  ```c
  return r7rs_uc_mapc(cp, op);
  ```)
;;; r7rs-uc-prop__ -- internal: 1/0 for alphabetic, upper, lower, whitespace
;;; (op 0-3); op 4 is the digit value of an Nd character or -1; op 5 and 6
;;; are CommonMark's Unicode whitespace and punctuation (stdlib/unicode.tur).
(defn r7rs-uc-prop__ [cp : int op : int] : int
  ```c
  return r7rs_uc_prop(cp, op);
  ```)
;;; r7rs-uc-string__ -- internal: full case mapping of a UTF-8 string (op 0
;;; upper, 1 lower, 2 fold).
(defn r7rs-uc-string__ [s : cstr op : int] : cstr
  ```c
  return r7rs_uc_string(s, op);
  ```)
;;; r7rs-uc-run__ -- internal: run i of SRFI 14 standard set `which` (field
;;; 0 its first code point, 1 its last, 2 its stride; -1 past the end).
(defn r7rs-uc-run__ [which : int i : int field : int] : int
  ```c
  return r7rs_uc_run(which, i, field);
  ```)
"""


def main():
    args = sys.argv[1:]
    if len(args) != 2 or args[0] != "--ucd":
        sys.stderr.write("usage: gen-r7rs-unicode.py --ucd <dir>   "
                         "(bash tools/fetch-ucd.sh fetches the files into build/ucd)\n")
        return 2
    u = UCD(args[1])
    block = c_block(u)
    with open(os.path.join(ROOT, "stdlib", "r7rs", "unicode.tur"), "w") as f:
        f.write(TUR_HEAD + "```c\n" + block + "```\n" + TUR_TAIL)
    with open(os.path.join(ROOT, "src", "turi", "r7rs_unicode.inc"), "w") as f:
        f.write(block)
    print("wrote stdlib/r7rs/unicode.tur and src/turi/r7rs_unicode.inc (Unicode %s)" % u.version)
    return 0


if __name__ == "__main__":
    sys.exit(main())

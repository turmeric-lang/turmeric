#!/usr/bin/env python3
"""check-annotation-spacing.py -- type annotations in the docs' Turmeric
snippets are spelled with a space after the colon: `[x : int] : int`.

The compiler reads `[x :int] :int` and `[x : int] : int` alike, but the style
CLAUDE.md shows, the stdlib sources use (3539 spaced return types against 578
fused) and tools/gendocs.py now renders in the API reference is the spaced
one.  A reader copying a snippet gets whichever spelling the page happened to
use.  This check reads every ```turmeric / ```sweet-exp fence in the docs (and
whole .tur / .tur.sweet files when given) and fails for a fused annotation in
a place where an annotation can only be one:

  - a parameter or field in the vector of `defn`, `defn-`, `fn`, `defmacro`,
    `defeffect` or `defstruct`:  `[x :int]`  ->  `[x : int]`
  - the return type after that vector (past an `#fx{...}` row):
    `] :int`  ->  `] : int`

Not touched: keywords (`#map{:a 1}`, `(get m :k)`), prose and inline-code
spans outside a fence, a defstruct's `(field :T)` groups, and a `] :word` whose
word is not recognisably a type (a lambda returning `:done` is a keyword, not
an annotation) -- the return position only counts a primitive (`int`, `bool`,
`cstr`, ...), a `ptr<..>`-style generic, a capitalised name or a `(...)` type.
This is a spacing check, not a parser.

Usage: check-annotation-spacing.py [--fix] [--selftest] [path ...]
                                                   (default: docs/, README.md)
`--fix` inserts the missing space in place (whitespace only).  `--selftest`
first runs the checker and the rewriter over the samples at the bottom of this
file.  Exit 1 when anything is, or is left, fused.  Not part of
tests/check-docs-lint.sh until the docs have been brought into line.
"""
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PATHS = ["docs", "README.md"]

# Reuse the fence / tokenizer / path helpers of the sibling check.
_spec = importlib.util.spec_from_file_location(
    "check_let_alignment", Path(__file__).with_name("check-let-alignment.py"))
_cla = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_cla)

# head -> (has a return type)
HEADS = {"defn": True, "defn-": True, "fn": True, "defmacro": True,
         "defeffect": True, "defstruct": False}
PRIMS = {"int", "float", "bool", "cstr", "void", "nil", "unit", "any", "ptr",
         "float32", "float64", "int8", "int16", "int32", "int64",
         "uint8", "uint16", "uint32", "uint64", "i32", "u8", "str", "fn"}
OPEN, CLOSE = _cla.OPEN, _cla.CLOSE


def is_fused(tok):
    """`:int` -- a colon with a name fused to it (not the bare `:`)."""
    return len(tok) > 1 and tok[0] == ":" and tok[1] not in ": \t"


def fused_at(toks, k):
    """toks[k] is a fused colon: `:int`, or a bare `:` touching a `(` type
    group (`:(Option T)`, which the tokenizer splits in two)."""
    tok, _, _, end = toks[k]
    if is_fused(tok):
        return True
    return (tok == ":" and k + 1 < len(toks) and toks[k + 1][0] == "("
            and toks[k + 1][2] == end)


def type_like_return(tok):
    """Whether the fused token after a parameter vector is a type, not a
    keyword the body returns."""
    name = tok[1:]
    if name.startswith("("):
        return True
    base = re.split(r"[<(]", name, maxsplit=1)[0]
    return base in PRIMS or base[:1].isupper() or (
        base != name and base[:1].isalpha())


def group_end(toks, i):
    """Index just past the group opened at toks[i]."""
    return _cla.group_end(toks, i)


def scan(lines):
    """[(line_idx, col)] -- where a space belongs after a fused colon.  A
    snippet the grammar cannot read (a sketch) yields what was found so far."""
    hits = []
    try:
        _scan(lines, hits)
    except _cla.Problem:
        pass
    return sorted(set(hits))


def _scan(lines, hits):
    mask = _cla.inline_c_mask(lines)
    toks = list(_cla.tokenize(lines, mask))
    n = len(toks)
    for i, (tok, _, _, _) in enumerate(toks):
        if tok not in HEADS:
            continue
        j = i + 1
        while j < n and toks[j][0] not in OPEN and toks[j][0] not in CLOSE \
                and not toks[j][0].startswith("#"):
            j += 1                        # the name, `^private`, `:heap`
        saw_vector = False
        while j < n and toks[j][0] == "[":
            end = group_end(toks, j)
            depth = 0
            for k in range(j, end):
                t = toks[k][0]
                if t in OPEN:
                    depth += 1
                elif t in CLOSE:
                    depth -= 1
                elif (depth == 1 and k + 1 < end and fused_at(toks, k + 1)
                      and not t.startswith((":", "^", "&", "#"))
                      and "<" not in t and ":" not in t):
                    hits.append((toks[k + 1][1], toks[k + 1][2]))
            saw_vector = True
            j = end
        if not (saw_vector and HEADS[tok]):
            continue
        while j < n and toks[j][0].startswith("#") and j + 1 < n \
                and toks[j + 1][0] == "{":
            j = group_end(toks, j + 1)    # `#fx{Unsafe}`
        if j < n and fused_at(toks, j) and (
                toks[j][0] == ":" or type_like_return(toks[j][0])):
            hits.append((toks[j][1], toks[j][2]))


def fix_lines(lines, hits):
    for li, col in sorted(hits, reverse=True):
        lines[li] = lines[li][:col + 1] + " " + lines[li][col + 1:]


def _frag(line, col):
    """The fused annotation at `col`, for the report."""
    return re.match(r":[^\s\])}]*", line[col:]).group(0)


def check_text(text, fix):
    """(report [(line_no, msg)], new_text or None)."""
    lines = text.split("\n")
    report = []
    spans = list(_cla.fenced_snippets(text))
    for start, _end, body, indent in spans:
        hits = scan(body)
        for li, col in hits:
            frag = _frag(body[li], col)
            report.append((start + li + 1, f"`{frag}` -- write `: {frag[1:]}`"))
        if fix and hits:
            fix_lines(body, hits)
            for k, b in enumerate(body):
                lines[start + k] = (indent + b) if b.strip() else lines[start + k]
    return report, ("\n".join(lines) if fix and report else None)


def check_file(path, fix):
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".md":
        report, new = check_text(text, fix)
    else:
        lines = text.split("\n")
        hits = scan(lines)
        report = [(li + 1, f"`{_frag(lines[li], col)}` -- write "
                   f"`: {_frag(lines[li], col)[1:]}`") for li, col in hits]
        new = None
        if fix and hits:
            fix_lines(lines, hits)
            new = "\n".join(lines)
    if new is not None and new != text:
        path.write_text(new, encoding="utf-8")
        return [] if fix else report, True
    return report, False


SELFTEST = [
    # (source, fixed)
    ("(defn f [x :int y :Foo] :int\n  x)",
     "(defn f [x : int y : Foo] : int\n  x)"),
    ("(defn f [x : int] : int\n  x)", None),
    ("defn f [x :cstr] #fx{Unsafe} :bool\n  true",
     "defn f [x : cstr] #fx{Unsafe} : bool\n  true"),
    ("(defeffect Log [msg :cstr] :void)", "(defeffect Log [msg : cstr] : void)"),
    ("(defstruct P [x :int y :int])", "(defstruct P [x : int y : int])"),
    ("(def g (fn [p :ptr<Foo>] :(Option Bar) p))",
     "(def g (fn [p : ptr<Foo>] : (Option Bar) p))"),
    # Keywords are not annotations.
    ("(def m #map{:name name :age 3})\n(get m :key)\n(fn [] :done)", None),
    ("(defn k [] :done)", None),
    # A function type has no names to annotate; inline C is C.
    ("(defn ap [f : (fn [int] int)] : int\n  ```c\n  return a ? b :c;\n  ```)",
     None),
]


def selftest():
    bad = 0
    for n, (src, want) in enumerate(SELFTEST, 1):
        lines = src.split("\n")
        hits = scan(lines)
        if want is None:
            if hits:
                print(f"FAIL check-annotation-spacing selftest {n}: flagged {hits}")
                bad += 1
            continue
        fix_lines(lines, hits)
        if "\n".join(lines) != want:
            print(f"FAIL check-annotation-spacing selftest {n}: --fix gave\n"
                  + "\n".join(lines))
            bad += 1
        again = scan(lines)
        if again:
            print(f"FAIL check-annotation-spacing selftest {n}: not idempotent {again}")
            bad += 1
    if bad:
        return 1
    print(f"PASS check-annotation-spacing selftest ({len(SELFTEST)} samples)")
    return 0


def main(argv):
    fix = "--fix" in argv
    if "--selftest" in argv and selftest():
        return 1
    argv = [a for a in argv if a not in ("--fix", "--selftest")]
    paths = []
    for arg in argv or DEFAULT_PATHS:
        p = Path(arg)
        if not p.is_absolute():
            p = ROOT / p
        if p.is_dir():
            for f in sorted(p.rglob("*")):
                rel = f.relative_to(p).parts
                if f.is_file() and _cla.wanted(f) \
                        and not (set(rel[:-1]) & _cla.SKIP_DIRS):
                    paths.append(f)
        elif p.is_file():
            paths.append(p)
        else:
            print(f"FAIL check-annotation-spacing: no such path {arg}")
            return 1
    nfail, nfixed = 0, 0
    for path in paths:
        try:
            rel = path.relative_to(ROOT)
        except ValueError:
            rel = path
        report, rewritten = check_file(path, fix)
        if rewritten:
            nfixed += 1
            print(f"fixed check-annotation-spacing: {rel}")
        for line, msg in report:
            print(f"FAIL check-annotation-spacing: {rel}:{line}: {msg}")
            nfail += 1
    if nfail:
        print(f"  {nfail} fused type annotation(s); write `[x : int] : int`"
              + ("" if fix else ", or run this script with --fix") + ".")
        return 1
    print(f"PASS check-annotation-spacing ({len(paths)} files"
          + (f", {nfixed} rewritten" if fix else "") + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

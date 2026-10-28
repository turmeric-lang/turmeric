#!/usr/bin/env python3
"""
caret-type-attrs-rewrite.py -- move the flags of a type declaration from
trailing `:keyword`s to leading `^attr`s.

    (defstruct Point :copy [x : int])        -> (defstruct ^copy Point [x : int])
    (defopaque Chan :ptr<void> :linear)      -> (defopaque ^linear Chan :ptr<void>)
    (defdata Tree [a] :heap (Leaf) ...)      -> (defdata ^heap Tree [a] (Leaf) ...)
    (exists :linear [a] ...)                 -> (exists ^linear [a] ...)

Applies to `defopaque`, `defstruct`, `defdata` and `defgadt`, for the flags
copy, move, linear, affine, heap, sealed, non-null and no-auto-ctor, and to the
`:linear` of an `exists` type.  Nothing else moves: a `:key` that is not one of
those flags in the flag position, the base type of a defopaque, field types and
every comment and blank line are left byte for byte.

Reads `.tur` files, `.tur.sweet` files (sweet-exp: a head at the start of a
line), and the ```turmeric / ```sweet-exp blocks and `(def...)` code spans of
`.md` files.  Idempotent: a second run changes nothing.

See docs/upcoming/caret-type-attributes-plan.md (P2).

Usage:
  tools/caret-type-attrs-rewrite.py [opts] PATH [PATH...]

Modes (mutually exclusive):
  --write     (default) Rewrite files in place.
  --dry-run   Print a unified diff; do not modify.
  --check     Exit 1 if any file would be rewritten.

A PATH may be a directory; it is walked for the file types above.
"""

from __future__ import annotations

import argparse
import difflib
import importlib.util
import os
import re
import sys

# The tokenizer is the one spaced-types-rewrite.py uses: it keeps every byte of
# trivia, which is what a rewrite that must leave the rest alone needs.
_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "spaced_types_rewrite", os.path.join(_HERE, "spaced-types-rewrite.py"))
_st = importlib.util.module_from_spec(_spec)
sys.modules["spaced_types_rewrite"] = _st
_spec.loader.exec_module(_st)

Tok = _st.Tok
tokenize = _st.tokenize

FLAGS = {"copy", "move", "linear", "affine", "heap", "sealed", "non-null",
         "no-auto-ctor"}
DECL_HEADS = {"defopaque", "defstruct", "defdata", "defgadt"}
HEADS = DECL_HEADS | {"exists"}

PREFIX_KINDS = {_st.TK_QUOTE, _st.TK_QUASIQUOTE, _st.TK_UNQUOTE,
                _st.TK_UNQUOTE_SPLICE}
OPENERS = set(_st.OPEN_TO_CLOSE)
CLOSERS = {_st.TK_RPAREN, _st.TK_RBRACKET, _st.TK_RBRACE}
MD_LANGS = {"turmeric", "tur", "sweet-exp", "sweet", "turmeric no-check"}
SWEET_LANGS = {"sweet-exp", "sweet"}


def _trivia(t: Tok) -> bool:
    return t.kind in _st.TRIVIA_KINDS


def rewrite_source(text: str, sweet: bool = False) -> str:
    """Return `text` with every type declaration's flags moved before its name."""
    toks = tokenize(text)
    if any(t.kind == _st.TK_LANG_LINE and "sweet" in t.text for t in toks):
        sweet = True
    texts = [t.text for t in toks]
    n = len(toks)

    def next_code(i: int) -> int:
        i += 1
        while i < n and _trivia(toks[i]):
            i += 1
        return i

    def prev_code(i: int) -> int:
        i -= 1
        while i >= 0 and _trivia(toks[i]):
            i -= 1
        return i

    def at_line_start(i: int) -> bool:
        i -= 1
        while i >= 0 and toks[i].kind == _st.TK_WS:
            i -= 1
        return i < 0 or toks[i].kind == _st.TK_NEWLINE

    def skip_group(i: int) -> int:
        """`i` is an opener; return the index just past its closer."""
        depth = 0
        while i < n:
            if toks[i].kind in OPENERS:
                depth += 1
            elif toks[i].kind in CLOSERS:
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
        return n

    def after_group(i: int) -> int:
        """The first code token after the group opened at `i`."""
        return next_code(skip_group(i) - 1)

    def is_flag_kw(i: int) -> bool:
        t = toks[i] if i < n else None
        return (t is not None and t.kind == _st.TK_KEYWORD
                and t.text[1:] in FLAGS)

    def drop(i: int) -> None:
        """Remove the token at `i` and the whitespace before it; a flag that
        stood alone on its line takes the line with it."""
        texts[i] = ""
        p = i - 1
        if p >= 0 and toks[p].kind == _st.TK_WS:
            texts[p] = ""
            p -= 1
        if p >= 0 and toks[p].kind == _st.TK_NEWLINE:
            if i + 1 < n and toks[i + 1].kind == _st.TK_NEWLINE:
                texts[i + 1] = ""            # the flag's line was only the flag
            elif i + 1 < n and not (p >= 1 and toks[p - 1].kind == _st.TK_COMMENT):
                texts[p] = ""                # `:linear)` -> join the `)` upward

    for h, tok in enumerate(toks):
        if tok.kind != _st.TK_SYMBOL or tok.text not in HEADS:
            continue
        pc = prev_code(h)
        paren_head = pc >= 0 and toks[pc].kind == _st.TK_LPAREN and \
            not any(toks[x].kind == _st.TK_NEWLINE for x in range(pc, h))
        if not paren_head and not (sweet and at_line_start(h)):
            continue

        j = next_code(h)
        if j >= n:
            continue

        if tok.text == "exists":
            if is_flag_kw(j) and toks[j].text == ":linear":
                texts[j] = "^linear"
            continue

        # Attributes already before the name (a half-migrated form) stay put;
        # new ones go after them, immediately before the name.
        while j < n and toks[j].kind == _st.TK_SYMBOL and toks[j].text.startswith("^"):
            j = next_code(j)
        ins = j
        while j < n and toks[j].kind in PREFIX_KINDS:
            j = next_code(j)
        if j >= n or toks[j].kind != _st.TK_SYMBOL:
            continue
        # A name may end in `'` (`Lens'`), which the tokenizer reads as a quote
        # token of its own, and may carry more symbol after it (`Lens'a`).
        while (j + 1 < n and toks[j + 1].kind in (_st.TK_QUOTE, _st.TK_SYMBOL)
               and not toks[j + 1].text.startswith("^")):
            j += 1

        k = next_code(j)
        found: list[int] = []
        if tok.text == "defopaque":
            if k < n and toks[k].kind == _st.TK_LBRACKET:
                k = after_group(k)
            # The base type: `:int`, `:ptr<void>`, or the spaced `: ptr<void>`.
            if k < n and toks[k].kind == _st.TK_KEYWORD:
                k = next_code(k)
            elif k < n and toks[k].kind == _st.TK_SYMBOL and toks[k].text == ":":
                k = next_code(k)
                if k < n and toks[k].kind in OPENERS:
                    k = after_group(k)
                else:
                    k = next_code(k)
            while is_flag_kw(k):
                found.append(k)
                k = next_code(k)
        elif tok.text == "defstruct":
            while is_flag_kw(k):
                found.append(k)
                k = next_code(k)
        else:  # defdata / defgadt: flags and the type-parameter vector, any order
            while k < n:
                if is_flag_kw(k):
                    found.append(k)
                    k = next_code(k)
                elif toks[k].kind == _st.TK_LBRACKET:
                    k = after_group(k)
                else:
                    break

        if not found:
            continue
        attrs = "".join("^" + toks[f].text[1:] + " " for f in found)
        for f in found:
            drop(f)
        texts[ins] = attrs + texts[ins]

    return "".join(texts)


# ---------------------------------------------------------------- Markdown

_SPAN = re.compile(r"`([^`\n]*\((?:defopaque|defstruct|defdata|defgadt|exists)\b[^`\n]*)`")


def rewrite_markdown(text: str) -> str:
    """Rewrite the turmeric / sweet-exp fences and the code spans of a .md."""
    lines = text.split("\n")
    out: list[str] = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        s = line.strip()
        if s.startswith("```") and len(s) > 3:
            lang = s[3:].strip()
            out.append(line)
            i += 1
            body: list[str] = []
            inline_c = False
            while i < n:
                b = lines[i]
                bs = b.strip()
                if inline_c:
                    if "```" in b:
                        inline_c = False
                elif bs == "```":
                    break
                elif bs.startswith("```"):
                    inline_c = True
                    # `(defn f [] ```c ... ```)` opened and closed on one line
                    if bs.count("```") >= 2:
                        inline_c = False
                body.append(b)
                i += 1
            if lang in MD_LANGS and body:
                body = rewrite_source("\n".join(body), sweet=lang in SWEET_LANGS).split("\n")
            out.extend(body)
            if i < n:
                out.append(lines[i])
                i += 1
            continue
        if s.startswith("```"):          # a bare fence with no language: leave it
            out.append(line)
            i += 1
            while i < n and lines[i].strip() != "```":
                out.append(lines[i])
                i += 1
            if i < n:
                out.append(lines[i])
                i += 1
            continue
        out.append(_SPAN.sub(lambda m: "`" + rewrite_source(m.group(1)) + "`", line))
        i += 1
    return "\n".join(out)


# ------------------------------------------------------------------- Driver

def rewrite_file_text(path: str, text: str) -> str:
    if path.endswith(".md"):
        return rewrite_markdown(text)
    return rewrite_source(text, sweet=path.endswith(".sweet"))


def _wanted(name: str) -> bool:
    return name.endswith((".tur", ".tur.sweet", ".md"))


def iter_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs[:] = sorted(d for d in dirs if d not in {".git", "build", "node_modules"})
                for f in sorted(files):
                    if _wanted(f):
                        yield os.path.join(root, f)
        elif os.path.isfile(p):
            yield p


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--check", action="store_true")
    ap.add_argument("paths", nargs="+")
    args = ap.parse_args(argv)

    changed = 0
    for path in iter_files(args.paths):
        try:
            with open(path, encoding="utf-8") as fh:
                old = fh.read()
        except (UnicodeDecodeError, OSError) as exc:
            print(f"skip {path}: {exc}", file=sys.stderr)
            continue
        new = rewrite_file_text(path, old)
        if new == old:
            continue
        changed += 1
        if args.dry_run:
            sys.stdout.writelines(difflib.unified_diff(
                old.splitlines(True), new.splitlines(True),
                f"a/{path}", f"b/{path}"))
        elif args.check:
            print(f"would rewrite {path}")
        else:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(new)
            print(f"rewrote {path}")
    if args.check and changed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

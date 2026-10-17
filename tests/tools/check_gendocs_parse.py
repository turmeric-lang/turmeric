#!/usr/bin/env python3
"""
tests/tools/check_gendocs_parse.py -- tools/gendocs.py must read a defn
signature the same way whichever annotation spelling it uses.

gendocs-misparses-the-spaced-annotation-form: the parameter vector was split
on whitespace and each ':'-prefixed token taken as the previous name's type.
That reads the FUSED form `[a :int]` correctly, but in the SPACED form
`[a : int]` -- which 612 stdlib defns and CLAUDE.md's own style guide use --
the colon is its own token, so `a` got the type ':' and a phantom parameter
named `int` appeared; the return position `] : int` came back as ':'.  53% of
the stdlib would have rendered with a wrong signature on the next
`tur run docs`.

Also pins how docstring prose renders: its Markdown `code` spans become
<code> on the API pages, and nothing else in it is read as Markdown.

Needs no `tur`: the docstring parser and gendocs' own inline renderer are what
is under test, not the guide renderer (tools/mdrender.py), which only builds
its tool when a guide is rendered.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from gendocs import _parse_def_line, _extract_return_type  # noqa: E402

CASES = [
    # (kind, text, params, return_type)
    ('defn', '(defn g [a :int b :int] :int (+ a b))',
     [('a', ':int'), ('b', ':int')], ':int'),                       # fused
    ('defn', '(defn g [a : int b : int] : int (+ a b))',
     [('a', ':int'), ('b', ':int')], ':int'),                       # spaced
    ('defn', '(defn g [a b] (+ a b))',
     [('a', None), ('b', None)], None),                             # untyped
    ('defn', '(defn arc-new [v : int] : Arc ...)',
     [('v', ':int')], ':Arc'),                                      # stdlib/arc.tur
    ('defn', '(defn greet [name : cstr] : void (println name))',
     [('name', ':cstr')], ':void'),                                 # CLAUDE.md style
    ('defn', '(defn f [x : int] #fx{Unsafe} : int x)',
     [('x', ':int')], ':int'),                                      # effect row
    ('defn', '(defn h [o : (Option int) k : ptr<Foo>] : (Result int cstr) o)',
     [('o', ':(Option int)'), ('k', ':ptr<Foo>')], ':(Result int cstr)'),  # compound
    ('defn', '(defn ap [f : (fn [int] int) x : int] : int (f x))',
     [('f', ':(fn [int] int)'), ('x', ':int')], ':int'),           # nested vector
    ('defn', '(defn v [first : cstr & rest : cstr] : void first)',
     [('first', ':cstr'), ('rest', '... :cstr')], ':void'),      # variadic, spaced
    ('defn', '(defn m [^mut v : int] : int v)',
     [('^mut v', ':int')], ':int'),                                 # UT3 annotation
    ('defstruct', '(defstruct Pt [x : float y : float])',
     [('x', ':float'), ('y', ':float')], None),                    # struct fields
]

fails = 0
for kind, text, want_params, want_ret in CASES:
    name, params, ret, _extra = _parse_def_line(kind, text)
    if params != want_params or ret != want_ret:
        fails += 1
        print(f"FAIL gendocs-parse: {text}")
        print(f"     params: got {params!r}, want {want_params!r}")
        print(f"     return: got {ret!r}, want {want_ret!r}")

for line, want in [('(defn g [a : int] : int', ':int'),
                   ('(defn g [a :int] :int', ':int'),
                   ('(defn g [a]', None)]:
    got = _extract_return_type(line)
    if got != want:
        fails += 1
        print(f"FAIL gendocs-parse: _extract_return_type({line!r}) = {got!r}, want {want!r}")

# r7rs-lang-plan R9: a `#lang r7rs` library.  Measured before the fix: the
# parser found the exports and NO definitions, and named the module after the
# file (`tur/shapes`) instead of the library (`geo/shapes`) -- so a Scheme
# library documented as an empty page.
import tempfile  # noqa: E402
from gendocs import parse_tur_file, _parse_scheme_def  # noqa: E402

SCHEME_CASES = [
    ('define', '(define (area w h) (* w h))', ('defn', 'area', [('w', None), ('h', None)])),
    ('define', '(define (sum . xs) (apply + xs))', ('defn', 'sum', [('. xs', None)])),
    ('define', '(define (f a . more) a)', ('defn', 'f', [('a', None), ('. more', None)])),
    ('define', '(define pi 3.25)', None),
    ('define-syntax', '(define-syntax swap! (syntax-rules () ...))', ('defmacro', 'swap!', [])),
    ('define-record-type', '(define-record-type point (make-point x y) point? (x px))',
     ('defstruct', 'point', [('x', None), ('y', None)])),
]
for form, text, want in SCHEME_CASES:
    got = _parse_scheme_def(form, text)
    if got != want:
        fails += 1
        print(f"FAIL gendocs-parse (scheme): {text}\n     got {got!r}, want {want!r}")

LIB = """#lang r7rs
;;; shapes -- area helpers.
;;
(define-library (geo shapes)
  (export area)
  (import (scheme base))
  (begin
    ;;; area -- a rectangle's area.
    (define (area w h) (* w h))
    (define (helper y) y)))
"""
with tempfile.NamedTemporaryFile('w', suffix='.tur', delete=False) as tf:
    tf.write(LIB)
mod = parse_tur_file(tf.name)
os.unlink(tf.name)
defs = {d['name']: d for d in mod['definitions']}
checks = [
    (mod['name'] == 'geo/shapes', f"library name: got {mod['name']!r}"),
    (mod['docstring'] is not None, "the module docstring is promoted"),
    ('area' in defs and defs['area']['exported'], "area is found and exported"),
    ('area' in defs and defs['area']['docstring'] is not None, "area keeps its ;;; docstring"),
    ('helper' in defs and not defs['helper']['exported'], "helper is found and not exported"),
]
for ok, what in checks:
    if not ok:
        fails += 1
        print(f"FAIL gendocs-parse (scheme library): {what}")

# Docstring prose renders its Markdown `code` spans as <code> -- and nothing
# else as Markdown: before the fix every summary, parameter and Returns line
# reached the API pages with its backticks showing.
from gendocs import _inline_html, _render_def_card  # noqa: E402

INLINE_CASES = [
    ('a `vec-get` call', 'a <code>vec-get</code> call'),
    ('`a` and `b`', '<code>a</code> and <code>b</code>'),
    ('`x < y` & z', '<code>x &lt; y</code> &amp; z'),          # escaped inside and out
    ('one ` stray', 'one ` stray'),                              # unmatched: literal
    ('`` a `b` c ``', '<code>a `b` c</code>'),                   # double-backtick span
    ('*args* and tur_box_*', '*args* and tur_box_*'),            # not emphasis
    ('spans `two\nlines`', 'spans <code>two lines</code>'),     # wrapped span
    ('', ''),
]
for text, want in INLINE_CASES:
    got = _inline_html(text)
    if got != want:
        fails += 1
        print(f"FAIL gendocs-inline: _inline_html({text!r}) = {got!r}, want {want!r}")

card = _render_def_card({
    'kind': 'defn', 'name': 'vec-get', 'params': [('v', ':Vec')], 'return_type': ':int',
    'docstring': {
        'summary': 'vec-get -- read slot `i` of `v`.', 'deprecated': '',
        'params': [('v', 'the `Vec` to read')], 'returns': 'The `int` at `i`.',
        'example': '(vec-get v `0`)', 'since': '',
    },
})
for want in ('read slot <code>i</code> of <code>v</code>.', 'the <code>Vec</code> to read',
             'The <code>int</code> at <code>i</code>.', '(vec-get v `0`)'):
    if want not in card:
        fails += 1
        print(f"FAIL gendocs-inline: def card lacks {want!r}")

if fails:
    print(f"gendocs-parse: {fails} case(s) failed")
    sys.exit(1)
print(f"PASS check-gendocs-parse ({len(CASES)} signatures, both spellings; "
      f"{len(SCHEME_CASES)} Scheme forms and a define-library; "
      f"{len(INLINE_CASES)} inline-code cases)")

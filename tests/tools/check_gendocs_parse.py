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

# A typeclass is documented as a class.  Measured before: `defclass` was not a
# definition kind at all, so a class's docstring and every method signature
# were dropped -- tur/comonad's page listed two instances and never mentioned
# `extract` -- and the docstring's prose (anything that was not a Parameters /
# Returns / Example line) was dropped from every page.
from gendocs import (_parse_defclass, _parse_docstring, _class_method_rows,  # noqa: E402
                     attach_class_instances, emit_docstrings_tur, collect_doc_entries)

CLASS_CASES = [
    # (source lines, name, type_params, superclasses, [(method, params, return)])
    (['(defclass Comonad [^w]', '  (extract [wa] : int)', '  (extend [wa f : fn] : int)',
      '  (duplicate [wa] : int))'],
     'Comonad', ['^w'], [],
     [('extract', [('wa', None)], ':int'), ('extend', [('wa', None), ('f', ':fn')], ':int'),
      ('duplicate', [('wa', None)], ':int')]),
    (['(defclass Arrow [a]', '  [(Category a)]', '  (>>> [f : (a b c) g : (a c d)] : (a b d))',
      '  (first [f] : a))'],
     'Arrow', ['a'], ['(Category a)'],
     [('>>>', [('f', ':(a b c)'), ('g', ':(a c d)')], ':(a b d)'), ('first', [('f', None)], ':a')]),
    (['(defclass MonadError [^m] [(Monad m)] (throw-error [err] : int)',
      '  (catch-error [ma [handler :fn]] : int))  ; a (comment) with parens'],
     'MonadError', ['^m'], ['(Monad m)'],
     [('throw-error', [('err', None)], ':int'),
      ('catch-error', [('ma', None), ('handler', ':fn')], ':int')]),          # grouped param
    (['(defclass Bifunctor [^^f]',
      '  (bimap [container : (f a b) fn-left : (fn [a] c) fn-right : (fn [b] d)]',
      '    : (f c d)))'],
     'Bifunctor', ['^^f'], [],
     [('bimap', [('container', ':(f a b)'), ('fn-left', ':(fn [a] c)'),
                 ('fn-right', ':(fn [b] d)')], ':(f c d)')]),                # wrapped return
    (['(defclass Traversable [^t] [(Functor t) (Foldable t)] (traverse [ta fn] : int))'],
     'Traversable', ['^t'], ['(Functor t)', '(Foldable t)'],
     [('traverse', [('ta', None), ('fn', None)], ':int')]),
]
for lines, want_name, want_tp, want_sup, want_methods in CLASS_CASES:
    name, extra = _parse_defclass(lines)
    got_methods = [(m['name'], m['params'], m['return_type']) for m in extra.get('methods', [])]
    if (name, extra.get('type_params'), extra.get('superclasses'), got_methods) != \
            (want_name, want_tp, want_sup, want_methods):
        fails += 1
        print(f"FAIL gendocs-defclass: {lines[0]}\n     got {name!r} {extra!r}")

# A spice documents a method in place, and declares an associated type.
name, extra = _parse_defclass([
    '(defclass StorageOps [S]',
    '  (type Elem : Type)',
    '  ;;; storage-get -- read the element at idx.',
    '  ;;; Panics on an empty slot.',
    '  (storage-get [s : S idx : Slot] : Elem)',
    '  (storage-has? [s : S idx : Slot] : bool))',
])
meths = {m['name']: m for m in extra['methods']}
for ok, what in [
    (extra['assoc_types'] == ['(type Elem : Type)'], f"assoc type: {extra['assoc_types']!r}"),
    (meths['storage-get']['doc'] is not None
     and meths['storage-get']['doc']['summary_full'] ==
     'storage-get -- read the element at idx. Panics on an empty slot.',
     "an in-body ;;; block documents the method below it"),
    (meths['storage-has?']['doc'] is None, "the doc does not leak to the next method"),
]:
    if not ok:
        fails += 1
        print(f"FAIL gendocs-defclass (in-body docs): {what}")

for text, want in [
    ('(definstance Functor [Option] (fmap [c g] c))', 'Functor[Option]'),
    ('(definstance Arrow [(->)]', 'Arrow[(->)]'),              # was 'Arrow[]'
    ('(definstance ArrowLoop [(->)] (arrow-loop [f] f))', 'ArrowLoop[(->)]'),  # was '[f]'
    ('(definstance Functor [(Result _ B)]', 'Functor[(Result _ B)]'),
    ('(definstance From [int cstr] (from [x] x))', 'From[int cstr]'),
]:
    got = _parse_def_line('definstance', text)[0]
    if got != want:
        fails += 1
        print(f"FAIL gendocs-definstance: {text}: got {got!r}, want {want!r}")

doc = _parse_docstring([
    ';;; Ord -- values with a total order,',
    ';;; wrapped onto a second line.',
    ';;;',
    ';;; Prose before any section.',
    ';;;',
    ';;; Parameters:',
    ';;;   a -- the type to order',
    ';;;',
    ';;; Methods:',
    ';;;   lt?  -- strictly before',
    ';;;           and continued',
    ';;;   gt? gte? -- after',
    ';;;',
    ';;; Superclass: Eq.',
    ';;;',
    ';;; Laws:',
    ';;;   (lte? x x) = true',
    ';;;',
    ';;; Returns: void',
    ';;;',
    ';;; Example:',
    ';;;   (lt? 2 3)   ; => true',
    ';;;   (gt? 3 2)   ; => true',
    ';;;',
    ';;; Note: after the example.',
    ';;;',
    ';;; Since: Phase 15',
])
for key, want in [
    ('summary', 'Ord -- values with a total order,'),
    ('summary_full', 'Ord -- values with a total order, wrapped onto a second line.'),
    ('description', 'Prose before any section.'),
    ('params', [('a', 'the type to order')]),           # the unindented prose left it
    ('methods', [('lt?', 'strictly before and continued'), ('gt? gte?', 'after')]),
    ('laws', '(lte? x x) = true'),
    ('returns', 'void'),                                # was dropped: same-line body
    ('example', '(lt? 2 3)   ; => true\n(gt? 3 2)   ; => true'),   # every line dedented
    ('notes', 'Superclass: Eq.\n\nNote: after the example.'),
    ('since', 'Phase 15'),
]:
    if doc.get(key) != want:
        fails += 1
        print(f"FAIL gendocs-docstring: {key}: got {doc.get(key)!r}, want {want!r}")

MOD = """;;; tur/shapes -- the module summary.
;;;
;;; Since: Phase 1

;;; Shape -- things with an area.
;;;
;;; Methods:
;;;   area -- the area of `s`
(defclass Shape [a] (area [s] : float))
;;; Shape [Sq] -- squares.
(definstance Shape [Sq] (area [s] 1.0))
(defn area-of [x] : float (area x))
"""
with tempfile.TemporaryDirectory() as td:
    path = os.path.join(td, 'shapes.tur')   # tur/shapes, as its summary says
    with open(path, 'w') as f:
        f.write(MOD.replace('(defn area-of', '(export area-of)\n(defn area-of'))
    mod = parse_tur_file(path)
defs = {d['name']: d for d in mod['definitions']}
shape = defs.get('Shape', {})
attach_class_instances([mod], link_other_modules=True)
card = _render_def_card(shape) if shape else ''
for ok, what in [
    (mod['docstring'] and mod['docstring']['summary'].startswith('tur/shapes'),
     "a blank line ends the module docstring before the class's own block"),
    (shape.get('kind') == 'defclass' and shape.get('exported'),
     "the class is found and public despite the export list"),
    (_class_method_rows(shape) == [('(area [s] :float)', 'the area of `s`')],
     f"method rows: {_class_method_rows(shape) if shape else None!r}"),
    ('<dt><code>(area [s] :float)</code></dt>' in card, "the card lists the method signature"),
    ('the area of <code>s</code>' in card, "the card carries the method description"),
    ('href="#Shape_Sq_"' in card, "the card links the instance in its own module"),
]:
    if not ok:
        fails += 1
        print(f"FAIL gendocs-defclass (module): {what}")

# `(doc 'Shape)` answers with the class, not the first instance the scan met;
# `(doc 'area)` names the method only because no definition claims `area`.
with tempfile.TemporaryDirectory() as td:
    out = os.path.join(td, 'docstrings.tur')
    emit_docstrings_tur([mod], out, verified_names=set())
    table = open(out).read()
entries = {e['name']: e for e in collect_doc_entries([mod])}
for ok, what in [
    ('{"Shape", "Shape -- things with an area.' in table, "doc-lookup Shape is the class"),
    ('(area [s] :float) -- the area of `s`' in table, "the class entry lists its methods"),
    (entries.get('Shape', {}).get('kind') == 'defclass', "doc-names Shape is the class"),
    (entries.get('area', {}).get('kind') == 'method', "an unclaimed method name is a key"),
]:
    if not ok:
        fails += 1
        print(f"FAIL gendocs-defclass (lookup): {what}")
mod['definitions'].append({'kind': 'defn', 'name': 'area', 'params': [], 'return_type': None,
                           'exported': True, 'docstring': None, 'line': 1})
if {e['name']: e for e in collect_doc_entries([mod])}['area']['kind'] != 'defn':
    fails += 1
    print("FAIL gendocs-defclass (lookup): a defn keeps a key a method also names")

# api-docs-miss-language-primitives, groups 2-4: the forms that rendered
# lossily or not at all.  Measured before: `(defstruct Pair [A B])` with no
# fields, `(Left) (Right)` with no payloads, a GADT arm's result read as more
# constructors, `(FS :nil)` for a defeffect, `(ArgSpec)` for a defopaque,
# `char` for `char=?`, and nothing at all for deftype / defdynamic / def /
# defalias.
from gendocs import _render_signature  # noqa: E402

def _sig(kind, text):
    name, params, ret, extra = _parse_def_line(kind, text)
    return _render_signature({'kind': kind, 'name': name, 'params': params,
                              'return_type': ret, **extra})

LOSSY_CASES = [
    ('defstruct', '(defstruct Pair [A B] (fst A) (snd B))',
     '(defstruct Pair [A B] (fst A) (snd B))'),
    ('defstruct', '(defstruct Cons :heap [A] (head A) (tail (Cons A)))',
     '(defstruct Cons :heap [A] (head A) (tail (Cons A)))'),
    ('defstruct', '(defstruct Lens :copy [S A] (lget (fn [S] A)) (lput (fn [A S] S)))',
     '(defstruct Lens :copy [S A] (lget (fn [S] A)) (lput (fn [A S] S)))'),
    ('defdata', '(defdata Either :copy [L R] (Left L) (Right R))',
     '(defdata Either :copy [L R] (Left L) (Right R))'),
    ('defdata', '(defdata Fr :heap [T] (Nil) (Frame T T))',
     '(defdata Fr :heap [T] (Nil) (Frame T T))'),
    ('defgadt', '(defgadt Nat [] (Zero : (Nat)) (Succ (Nat) : (Nat)))',
     '(defgadt Nat (Zero : (Nat)) (Succ (Nat) : (Nat)))'),
    ('defeffect', '(defeffect FS [] :nil ^extends IO ^capability)',
     '(defeffect FS [] :nil ^extends IO ^capability)'),
    ('defeffect', '(defeffect Fail [msg :cstr] :nil)',
     '(defeffect Fail [msg :cstr] :nil)'),
    ('defopaque', '(defopaque ArgSpec :int)', '(defopaque ArgSpec :int)'),
    ('defopaque', '(defopaque TmpFile :int :linear)', '(defopaque TmpFile :int :linear)'),
]
for kind, text, want in LOSSY_CASES:
    got = _sig(kind, text)
    if got != want:
        fails += 1
        print(f"FAIL gendocs-render: {text}\n     got {got!r}, want {want!r}")

for kind, text, want_name, want_ret in [
    ('defn', '(defn char=? [a : Char b : Char] : bool (= a b))', 'char=?', ':bool'),
    ('defn', '(defn r7rs-string<=? [a : cstr b : cstr] : bool 1)', 'r7rs-string<=?', ':bool'),
    ('defeffect', '(defeffect Fail [msg :cstr] :nil)', 'Fail', ':nil'),
]:
    name, _, ret, _ = _parse_def_line(kind, text)
    if (name, ret) != (want_name, want_ret):
        fails += 1
        print(f"FAIL gendocs-parse: {text}: got {(name, ret)!r}, want {(want_name, want_ret)!r}")

DECLS = """;;; tur/decls -- the four declaration forms.
;;
;;; Nat -- a non-negative int.
(deftype Nat #refine{ x : int | (>= x 0) })
;;; *log-level* -- the log threshold.
(defdynamic *log-level* :int 1)
;;; PI -- the circle constant.
(def PI 3.141592653589793)
;;; counter -- a mutable count.
(def ^mut counter 0)
;;; internal__ -- an internal cell.
(def internal__ 7)
;;; ---- a section banner, not a docstring for what follows ----
(def r7rs-null-value__ 0)
;;; ReplicaId -- a replica's name.
(defalias ReplicaId Sym)
(defn f [] : int
  (def local 1)
  local)
"""
with tempfile.TemporaryDirectory() as td:
    path = os.path.join(td, 'decls.tur')
    with open(path, 'w') as f:
        f.write(DECLS)
    dmod = parse_tur_file(path)
ddefs = {d['name']: d for d in dmod['definitions']}
for ok, what in [
    (ddefs.get('Nat', {}).get('kind') == 'deftype'
     and _render_signature(ddefs['Nat']) == '(deftype Nat #refine{ x : int | (>= x 0) })',
     "deftype renders as written"),
    (ddefs.get('Nat', {}).get('docstring') is not None, "deftype keeps its docstring"),
    (_render_signature(ddefs.get('*log-level*', {'kind': 'defdynamic', 'name': '', 'params': [],
                                                  'return_type': None})) ==
     '(defdynamic *log-level* :int 1)', "defdynamic renders with its type and default"),
    (ddefs.get('PI', {}).get('kind') == 'def'
     and _render_signature(ddefs['PI']) == '(def PI 3.141592653589793)', "def renders its value"),
    (ddefs.get('counter', {}).get('kind') == 'def', "a ^mut marker does not become the name"),
    ('internal__' in ddefs and not ddefs['internal__']['exported'], "a __-suffixed def is internal"),
    (_render_signature(ddefs.get('ReplicaId', {'kind': 'defalias', 'name': '', 'params': [],
                                                'return_type': None})) == '(defalias ReplicaId Sym)',
     "defalias renders as written"),
    ('local' not in ddefs, "a def inside a body is not a definition"),
    ('r7rs-null-value__' not in ddefs, "a def whose ;;; block does not name it is not a definition"),
]:
    if not ok:
        fails += 1
        print(f"FAIL gendocs-decls: {what}")

if fails:
    print(f"gendocs-parse: {fails} case(s) failed")
    sys.exit(1)
print(f"PASS check-gendocs-parse ({len(CASES)} signatures, both spellings; "
      f"{len(SCHEME_CASES)} Scheme forms and a define-library; "
      f"{len(INLINE_CASES)} inline-code cases; {len(CLASS_CASES)} defclass forms; "
      f"{len(LOSSY_CASES)} sum/struct/effect/opaque renderings and the four declaration forms)")

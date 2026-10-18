#!/usr/bin/env bash
# tests/run-editor-syntax.sh -- saffron-lang-plan S8: the editor syntax packs.
#
# Two packs, two very different levels of assurance, and the script says which
# is which rather than pretending they are equal:
#
#   vim     ACTUALLY LOADED.  Vim is scriptable, so this opens a sample file,
#           asks `synIDattr(synIDtrans(synID(l, c, 1)))` for the highlight group
#           at specific positions, and compares.  That is the real thing, not a
#           regex approximation -- and it is what caught the fused-vs-spaced
#           annotation bug: `y :float` highlighted as a KEYWORD LITERAL while
#           `y : float` highlighted as a TYPE, because Vim gives a later-defined
#           item priority and the two rules were the wrong way round.
#
#   vscode  STRUCTURE AND PATTERNS ONLY.  There is no TextMate engine here, so
#           this validates the JSON, checks the grammar is actually wired into
#           package.json, and runs each `match` regex against inputs it must and
#           must not match.  Python's `re` is not Oniguruma, so a pattern can
#           pass here and still behave differently in VS Code; the checks are
#           limited to constructs where the two agree.  This is a guard against
#           rot, not a rendering test.
#
# Skips cleanly (exit 0) when a tool is missing.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
fails=0

VIMDIR="$ROOT/vim-syntax"
TMGRAMMAR="$ROOT/vscode-syntax-ext/syntaxes/turmeric.tmLanguage.json"

SAMPLE="$(mktemp -d)/sample.tur"
cat > "$SAMPLE" <<'EOF'
#lang saffron
;;; add -- adds two values.
;; ordinary comment
(defn add [a b] (+ a b))
(defn typed [x : int y :float] : bool (is? x int))
(defn m [^mut w : World] (set! w 1))
(def pi 3.25)
(println "hi" true nil)
#map{:a 1}
(defn p [x : #refine{v : int | (> v 0)}] x)
EOF
# r7rs-lang-plan R9: the Scheme lexemes, active in a `#lang r7rs` file only.
SCHEME_SAMPLE="$(dirname "$SAMPLE")/scheme.tur"
cat > "$SCHEME_SAMPLE" <<'EOF'
#lang r7rs
(define-library (m) (export f))
(define v '#(#t #\space |two words| #x1F))
#| block |# #;(skip)
EOF

# ---------------------------------------------------------------- vim ------
if ! command -v vim >/dev/null 2>&1; then
    echo "SKIP: vim not available -- vim syntax pack not exercised"
else
    PROBE="$(mktemp)"
    OUT="$(mktemp)"
    # line, col, expected TRANSITIVE highlight group.  Transitive because the
    # pack links to standard groups (Function->Identifier, String->Constant,
    # SpecialComment->Special, StorageClass->Type), and those links are what a
    # colourscheme actually reads.
    cat > "$PROBE" <<VIM
set nocompatible
syntax on
set runtimepath+=$VIMDIR
filetype plugin on
edit $SAMPLE
setfiletype turmeric
redir! > $OUT
for [l, c, want, what] in [
      \\ [1, 2,  'PreProc',    '#lang directive'],
      \\ [1, 7,  'Type',       'saffron base'],
      \\ [2, 3,  'Special',    ';;; docstring'],
      \\ [3, 3,  'Comment',    ';; comment'],
      \\ [4, 3,  'Statement',  'defn'],
      \\ [4, 7,  'Identifier', 'the bound name'],
      \\ [5, 20, 'Type',       'SPACED annotation'],
      \\ [5, 27, 'Type',       'FUSED annotation'],
      \\ [6, 11, 'Type',       '^mut attribute'],
      \\ [7, 10, 'Constant',   'float literal'],
      \\ [8, 11, 'Constant',   'string'],
      \\ [9, 2,  'PreProc',    '#map dispatch'],
      \\ [9, 6,  'Constant',   'keyword literal in value position'],
      \\ [10, 37, 'Constant',  'a #refine | is not a Scheme bar symbol outside r7rs']]
  let got = synIDattr(synIDtrans(synID(l, c, 1)), 'name')
  echo (got ==# want ? 'ok  ' : 'BAD ') . what . ' want=' . want . ' got=' . got
endfor
echo 'ft=' . &filetype
edit $SCHEME_SAMPLE
setfiletype turmeric
for [l, c, want, what] in [
      \\ [1, 7,  'Type',       'r7rs base'],
      \\ [2, 2,  'Statement',  'define-library is a Scheme form'],
      \\ [3, 12, 'PreProc',    'the #( vector opener'],
      \\ [3, 14, 'Constant',   '#t boolean'],
      \\ [3, 17, 'Constant',   '#\\space character'],
      \\ [3, 27, 'Identifier', 'inside a |two words| symbol'],
      \\ [3, 38, 'Constant',   '#x1F radix number'],
      \\ [4, 4,  'Comment',    'inside a #| |# block comment'],
      \\ [4, 13, 'Comment',    'the #; datum comment']]
  let got = synIDattr(synIDtrans(synID(l, c, 1)), 'name')
  echo (got ==# want ? 'ok  ' : 'BAD ') . what . ' want=' . want . ' got=' . got
endfor
redir END
qall!
VIM
    vim -es -u NONE -i NONE -S "$PROBE" >/dev/null 2>&1
    if [ ! -s "$OUT" ]; then
        echo "FAIL vim: the probe produced no output (syntax file failed to load?)"
        fails=$((fails + 1))
    else
        if grep -q '^BAD ' "$OUT"; then
            echo "FAIL vim: wrong highlight group(s)"
            grep '^BAD ' "$OUT" | sed 's/^/       /'
            fails=$((fails + 1))
        else
            echo "ok   vim: $(grep -c '^ok  ' "$OUT") highlight positions correct"
        fi
        if ! grep -q '^ft=turmeric$' "$OUT"; then
            echo "FAIL vim: ftdetect did not set filetype=turmeric for a .tur file"
            fails=$((fails + 1))
        fi
    fi
    rm -f "$PROBE" "$OUT"
fi

# ------------------------------------------------------------- vscode ------
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not available -- TextMate grammar not exercised"
else
    if python3 - "$TMGRAMMAR" "$ROOT/vscode-syntax-ext/package.json" <<'PY'
import json, re, sys
gpath, ppath = sys.argv[1], sys.argv[2]
g = json.load(open(gpath))            # raises if malformed
pkg = json.load(open(ppath))
bad = []

grammars = pkg.get("contributes", {}).get("grammars", [])
if not any(x.get("scopeName") == g["scopeName"] for x in grammars):
    bad.append("package.json does not wire up scopeName %r" % g["scopeName"])

repo = g["repository"]
# (rule, text, should_match) -- only constructs where Python re and Oniguruma agree.
cases = [
    ("lang-directive",  "#lang saffron",                  True),
    ("lang-directive",  "#lang turmeric/sweet stringed",  True),
    ("lang-directive",  "(defn f [] 0)",                  False),
    ("doc-comment",     ";;; add -- adds.",               True),
    ("definition",      "(defn add [a b] (+ a b))",       True),
    ("definition",      "(defstruct Point [x : float])",  True),
    ("type-annotation", "(defn f [a : int] : int 0)",     True),
    ("type-annotation", "(defn f [a :int] :int 0)",       True),
    ("param-attribute", "(defn f [^mut w : World] 0)",    True),
    ("number",          "(+ 7.1 2)",                      True),
    ("dispatch",        "#map{:a 1}",                     True),
    # `is?` and `set!` end in a non-word char, so a trailing `\b` matches
    # nothing -- the bug this pair exists to catch.
    ("special-form",    "(is? x Circle)",                 True),
    ("special-form",    "(set! x 1)",                     True),
    ("special-form",    "(with-region f)",                True),
    ("special-form",    "(isotope x)",                    False),
    ("special-form",    "(setter x)",                     False),
    # r7rs-lang-plan R9: the Scheme layer.
    ("scheme-boolean",  "(list #t #false)",               True),
    ("scheme-boolean",  "#tail",                          False),
    ("scheme-char",     "(write #\\space)",               True),
    ("scheme-char",     "#\\x41",                          True),
    ("scheme-number",   "#x1F #e1.5",                     True),
    ("scheme-datum-comment", "#;(skip)",                  True),
    ("scheme-special-form", "(define-library (m))",       True),
    ("scheme-special-form", "(let* ((a 1)) a)",           True),
    ("scheme-special-form", "(beginner x)",               False),
]
for rule, text, want in cases:
    r = repo.get(rule)
    if not r or "match" not in r:
        bad.append("rule %r has no `match`" % rule); continue
    if bool(re.search(r["match"], text)) != want:
        bad.append("%s: %r should%s match" % (rule, text, "" if want else " not"))

# the float literal must be taken whole
m = re.search(repo["number"]["match"], "(+ 7.1 2)")
if not m or m.group(0) != "7.1":
    bad.append("number: 7.1 tokenised as %r" % (m.group(0) if m else None))

# The Scheme layer is scoped to `#lang r7rs`: its region begins only there.
sf = repo.get("scheme-file", {})
if not re.search(sf.get("begin", "(?!)"), "#lang r7rs"):
    bad.append("scheme-file: does not begin at `#lang r7rs`")
for other in ("#lang saffron", "#lang turmeric", "(defn f [] 0)"):
    if re.search(sf.get("begin", "(?!)"), other):
        bad.append("scheme-file: begins at %r" % other)
if g["patterns"][0].get("include") != "#scheme-file":
    bad.append("scheme-file is not the first top-level pattern")

if bad:
    for b in bad: print("       " + b)
    sys.exit(1)
print("ok   vscode: grammar JSON valid, wired up, %d pattern checks pass" % len(cases))
PY
    then :; else
        echo "FAIL vscode: TextMate grammar checks failed"
        fails=$((fails + 1))
    fi
fi

rm -rf "$(dirname "$SAMPLE")"
if [ "$fails" -ne 0 ]; then
    echo "run-editor-syntax: $fails check(s) failed"
    exit 1
fi
echo "run-editor-syntax: all checks passed"

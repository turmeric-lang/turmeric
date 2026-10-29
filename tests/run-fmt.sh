#!/usr/bin/env bash
# tests/run-fmt.sh -- integration tests for `tur fmt`
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests/run-fmt.sh: $TUR not built" >&2; exit 2; }

PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL $1: $2"; }

# ---------------------------------------------------------------------------
# Test: --stdin round-trips a well-formed s-expression
# ---------------------------------------------------------------------------
NAME="fmt-stdin-identity"
INPUT='(defn add [x :int y :int] :int (+ x y))'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$INPUT" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$INPUT', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin normalises extra whitespace
# ---------------------------------------------------------------------------
NAME="fmt-stdin-normalize"
INPUT='(defn add   [x :int y :int] :int (+ x y))'
EXPECTED='(defn add [x :int y :int] :int (+ x y))'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$EXPECTED', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin keeps character literals as characters.  A `#\A` reads as a
# marked F_INT; printing its code (`65`) would also change its type from Char
# to int (parsec-guide-plan P2).
# ---------------------------------------------------------------------------
NAME="fmt-stdin-char-literals"
INPUT='(f #\A #\space #\newline #\( #\\ #\u1f #\u0)'
EXPECTED='(f #\A #\space #\newline #\( #\\ #\u1f #\null)'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$EXPECTED', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin keeps a literal's spelling.  `0xFF` printed from its value was
# `255`, and `\x` escapes of bytes 0x80 and up came back as raw UTF-8, so an
# ASCII file stopped being ASCII (fmt-rewrites-hex-literals-and-string-escapes).
# Hex digits print upper case; a string written in raw UTF-8 stays raw.
# ---------------------------------------------------------------------------
NAME="fmt-stdin-literal-spelling"
INPUT='(f 0xFF 0xff 0b101 -0x10 0xFFFFFFFFFFFFFFFF 0x1Fu8 255 "caf\xc3\xa9" "a\x1bb")'
EXPECTED='(f 0xFF 0xFF 0b101 -0x10 0xFFFFFFFFFFFFFFFF 0x1Fu8 255 "caf\xc3\xa9" "a\x1bb")'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$EXPECTED', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin exit code is 0 on success
# ---------------------------------------------------------------------------
NAME="fmt-stdin-exit-ok"
printf '(println 42)\n' | "$TUR" fmt --stdin > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0, got $RC"
fi


# ---------------------------------------------------------------------------
# Test: --stdout formats a file and writes to stdout (file unchanged)
# ---------------------------------------------------------------------------
NAME="fmt-stdout-file"
TMPDIR_FMT=$(mktemp -d)
printf '(defn foo   [x :int] :int x)\n' > "$TMPDIR_FMT/test.tur"
ORIG=$(cat "$TMPDIR_FMT/test.tur")
ACTUAL=$("$TUR" fmt --stdout "$TMPDIR_FMT/test.tur" 2>/dev/null)
AFTER=$(cat "$TMPDIR_FMT/test.tur")
if [ "$ACTUAL" = "(defn foo [x :int] :int x)" ] && [ "$ORIG" = "$AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "stdout='$ACTUAL', file_unchanged=$([ "$ORIG" = "$AFTER" ] && echo yes || echo no)"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check exits 0 when file is already formatted
# ---------------------------------------------------------------------------
NAME="fmt-check-already-formatted"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/clean.tur"
"$TUR" fmt --check "$TMPDIR_FMT/clean.tur" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for already-formatted file, got $RC"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check exits 1 when file needs formatting
# ---------------------------------------------------------------------------
NAME="fmt-check-needs-format"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt --check "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 1 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 1 for unformatted file, got $RC"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check does not modify the file
# ---------------------------------------------------------------------------
NAME="fmt-check-no-modify"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
BEFORE=$(cat "$TMPDIR_FMT/dirty.tur")
"$TUR" fmt --check "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
if [ "$BEFORE" = "$AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "--check modified the file"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --dry-run is an alias for --check
# ---------------------------------------------------------------------------
NAME="fmt-dry-run-alias"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt --dry-run "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
BEFORE='(defn add   [x :int y :int] :int (+ x y))'
if [ "$RC" -eq 1 ] && [ "$AFTER" = "$BEFORE" ]; then
    pass "$NAME"
else
    fail "$NAME" "exit=$RC, file_unchanged=$([ "$BEFORE" = "$AFTER" ] && echo yes || echo no)"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: in-place format modifies file and exits 0
# ---------------------------------------------------------------------------
NAME="fmt-inplace"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
EXPECTED='(defn add [x :int y :int] :int (+ x y))'
if [ "$RC" -eq 0 ] && [ "$AFTER" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "exit=$RC, content='$AFTER'"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: directory walk skips build/ and .git/
# ---------------------------------------------------------------------------
NAME="fmt-walk-skip-dirs"
TMPDIR_FMT=$(mktemp -d)
mkdir -p "$TMPDIR_FMT/src" "$TMPDIR_FMT/build" "$TMPDIR_FMT/.git"
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/src/ok.tur"
printf '(defn bad   [x :int] :int x)\n' > "$TMPDIR_FMT/build/skip.tur"
printf '(defn bad   [x :int] :int x)\n' > "$TMPDIR_FMT/.git/skip.tur"
BUILD_BEFORE=$(cat "$TMPDIR_FMT/build/skip.tur")
GIT_BEFORE=$(cat "$TMPDIR_FMT/.git/skip.tur")
"$TUR" fmt "$TMPDIR_FMT" > /dev/null 2>&1
BUILD_AFTER=$(cat "$TMPDIR_FMT/build/skip.tur")
GIT_AFTER=$(cat "$TMPDIR_FMT/.git/skip.tur")
if [ "$BUILD_BEFORE" = "$BUILD_AFTER" ] && [ "$GIT_BEFORE" = "$GIT_AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "formatter touched files in skipped directories"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: unknown flag exits 2
# ---------------------------------------------------------------------------
NAME="fmt-unknown-flag"
printf '(println 1)\n' | "$TUR" fmt --unknown-flag > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 2 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 2 for unknown flag, got $RC"
fi

# ---------------------------------------------------------------------------
# Test: --lang sweet-exp accepted with --stdin
# ---------------------------------------------------------------------------
NAME="fmt-lang-sweet-exp-stdin"
printf 'defn add [x :int y :int] :int\n  +(x y)\n' | "$TUR" fmt --stdin --lang sweet-exp > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for --lang sweet-exp, got $RC"
fi

# ---------------------------------------------------------------------------
# Test: --lang tursweet still accepted as deprecated alias
# ---------------------------------------------------------------------------
NAME="fmt-lang-tursweet-alias-stdin"
printf 'defn add [x :int y :int] :int\n  +(x y)\n' | "$TUR" fmt --stdin --lang tursweet > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for --lang tursweet (deprecated alias), got $RC"
fi

# ---------------------------------------------------------------------------
# Regression: comments that live *inside* a form (between a defn signature and
# its body) must survive a format pass -- they used to be silently dropped.
# ---------------------------------------------------------------------------
NAME="fmt-preserves-interior-comment"
read -r -d '' INTERIOR_INPUT <<'EOF'
(defn f [x : int] : int
  ;; this comment must survive
  (+ x 1))
EOF
ACTUAL=$(printf '%s\n' "$INTERIOR_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qF ";; this comment must survive" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "comment dropped or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# Regression: comments inside a `[...]` vector -- a defstruct field vector, a
# defn parameter vector, a multi-pair let binding vector.  Every one of these
# used to be DELETED: the vector printers called fmt_emit_inline with no
# span_has_comment guard, and the broken printers never consulted the source
# gaps.  `tur fmt` writes in place, so the text was destroyed with exit 0.
#
# The corpus this bug hid in is why the case is spelled out here rather than
# left to FT7/FT8: no sampled stdlib file puts a comment inside a bracket
# vector, so both idempotence checks passed over a format pass that was losing
# source.  Assert preservation AND idempotence -- the second symptom is
# downstream of the first (once the comments are gone, pass 2 collapses the
# form further), so a fix that stopped only the collapse would still pass an
# idempotence-only check while deleting the comments.
# ---------------------------------------------------------------------------
NAME="fmt-preserves-comments-in-vectors"
read -r -d '' VECCOMMENT_INPUT <<'EOF'
(defstruct P
  [a : int    ; first field
   b : int    ; second field
   c : int])

(defn f [x : int   ; the input
         y : int] : int
  (let [s (+ x y)  ; the sum
        t 2]
    (* s t)))
EOF
ACTUAL=$(printf '%s\n' "$VECCOMMENT_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
VECCOMMENT_MISSING=""
for c in "; first field" "; second field" "; the input" "; the sum"; do
    printf '%s\n' "$ACTUAL" | grep -qF "$c" || VECCOMMENT_MISSING="$VECCOMMENT_MISSING [$c]"
done
if [ -n "$VECCOMMENT_MISSING" ]; then
    fail "$NAME" "comment(s) deleted:$VECCOMMENT_MISSING; got: $ACTUAL"
elif [ "$ACTUAL" != "$ROUNDTRIP" ]; then
    fail "$NAME" "fmt(fmt(x)) != fmt(x); pass1: $ACTUAL"
else
    pass "$NAME"
fi

# ---------------------------------------------------------------------------
# A comment that trailed an element on its own source line must stay on that
# line.  Relocating it one line down parks it above the NEXT element, where it
# reads as a comment about that one -- a different claim about the code than
# the author made, and not something a formatter gets to decide.
# ---------------------------------------------------------------------------
NAME="fmt-trailing-comment-stays-on-its-line"
read -r -d '' TRAILING_INPUT <<'EOF'
(defstruct Q
  [alpha : int   ; belongs to alpha
   beta : int])  ; belongs to beta
EOF
ACTUAL=$(printf '%s\n' "$TRAILING_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qE "alpha : int +; belongs to alpha" \
   && printf '%s\n' "$ACTUAL" | grep -qE "beta : int\]\) +; belongs to beta" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "trailing comment relocated or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# A comment between the last element and the closing bracket must not swallow
# that bracket.  emit_comments_indented ends output mid-comment-line, so a `]`
# written straight after it lands INSIDE the comment -- turning silent comment
# loss into a file that no longer parses.
# ---------------------------------------------------------------------------
NAME="fmt-comment-before-close-bracket"
read -r -d '' CLOSEBRACKET_INPUT <<'EOF'
(defn h [aaaaaaaaaaaaaaaa : int bbbbbbbbbbbbbbbb : int cccccccccccccccc : int
         ;; a note that sits before the closing bracket
         ] : int
  aaaaaaaaaaaaaaaa)
EOF
ACTUAL=$(printf '%s\n' "$CLOSEBRACKET_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
# The ']' must be on a line of its own, i.e. not trailing the comment text.
if printf '%s\n' "$ACTUAL" | grep -qF ";; a note that sits before the closing bracket" \
   && ! printf '%s\n' "$ACTUAL" | grep -qE ";.*\]" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "closing bracket swallowed by comment or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# Regression: a defn parameter list that overflows the line width must break
# one parameter per line, keeping each `name : type` pair together -- it used
# to split the name and its annotation onto separate lines.
# ---------------------------------------------------------------------------
NAME="fmt-param-vector-no-split"
PARAM_INPUT='(defn g [alpha : ptr<void> beta : ptr<void> gamma : ptr<void> delta : ptr<void>] : int (do-thing alpha))'
ACTUAL=$(printf '%s\n' "$PARAM_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
# A correctly-grouped break keeps "alpha : ptr<void>" on one line; a split
# would leave a line that is just ": ptr<void>".
if printf '%s\n' "$ACTUAL" | grep -qE "alpha : ptr<void>" \
   && ! printf '%s\n' "$ACTUAL" | grep -qE "^[[:space:]]*: ptr<void>" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "param name/type split or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# Regression: a let/loop binding vector with two or more pairs must break
# one pair per line, even when the whole form fits in the line width -- the
# house style keeps each binding pair on its own line (a single pair may inline).
# ---------------------------------------------------------------------------
NAME="fmt-let-multipair-broken"
LET_INPUT='(defn g [s] : int (let [iter (alloc s) body (loop s iter)] (destroy iter)))'
ACTUAL=$(printf '%s\n' "$LET_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
# The two pairs must land on separate lines: after the fix `body` starts a line.
# A single-pair let must still inline (guards against over-breaking).
SINGLE=$(printf '(defn f [] : int (let [a 1] a))\n' | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qE "^[[:space:]]+body " \
   && ! printf '%s\n' "$ACTUAL" | grep -qE "iter \(alloc s\) body" \
   && printf '%s\n' "$SINGLE" | grep -qE "\(let \[a 1\] a\)" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "multi-pair let not broken / single-pair over-broken / not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# caret-type-attributes-plan: the flags of a defopaque / defstruct / defdata /
# defgadt are `^attr`s before the name, and the attributes and the name are the
# header together.  A declaration too long for one line used to put the first
# attribute on the head line and the NAME on a line of its own:
#     (defdata ^copy
#       MdKind
#       (MdKDocument) ...
# The keyword spelling is migrated (below), then laid out the same way.
# ---------------------------------------------------------------------------
NAME="fmt-type-decl-attrs-stay-with-name"
ATTR_LONG='(defdata ^copy ^heap Tokenizer-State-With-A-Long-Name (TokenStart :int :int) (TokenEnd :int :int) (TokenError :cstr))'
ACTUAL=$(printf '%s\n' "$ATTR_LONG" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
KW_LONG='(defdata Tokenizer-State-With-A-Long-Name :copy (TokenStart :int :int) (TokenEnd :int :int) (TokenError :cstr))'
KW_ACTUAL=$(printf '%s\n' "$KW_LONG" | "$TUR" fmt --stdin 2>/dev/null)
OPQ=$(printf '%s\n' '(defopaque ^linear ^sealed A-Very-Long-Opaque-Handle-Type-Name-For-A-Resource :ptr<void>)' | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | head -1 | grep -qxF "(defdata ^copy ^heap Tokenizer-State-With-A-Long-Name" \
   && printf '%s\n' "$ACTUAL" | sed -n 2p | grep -qE "^  \(TokenStart" \
   && printf '%s\n' "$KW_ACTUAL" | head -1 | grep -qxF "(defdata ^copy Tokenizer-State-With-A-Long-Name" \
   && printf '%s\n' "$OPQ" | head -1 | grep -qxF "(defopaque ^linear ^sealed A-Very-Long-Opaque-Handle-Type-Name-For-A-Resource" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "attributes split from the name or not idempotent; got: $ACTUAL // $OPQ"
fi

# ---------------------------------------------------------------------------
# caret-type-attributes-plan P3: `tur fmt` moves a declaration's flag keywords
# to `^attr`s before the name (the compiler rejects them, TUR-E0717).  A declaration
# whose source holds a comment is left as written, since the printer re-emits
# comments from the gaps between neighbouring forms; and --check reports a file
# still on the old spelling.
# ---------------------------------------------------------------------------
NAME="fmt-migrates-type-decl-keyword-flags"
MIG_IN='(defstruct Point :copy :heap [x : int])
(defopaque Chan :ptr<void> :linear :sealed)
(defopaque Tagged [A] :int :affine)
(defdata Tree [a] :heap (Leaf) (Node (Tree a) a (Tree a)))
(defstruct ^move Cell :linear [n : int])
(defmacro mk [name] `(defstruct ~name :copy [x : int]))
(def e (pack 1 (exists :linear [a] a)))'
MIG_WANT='(defstruct ^copy ^heap Point [x : int])

(defopaque ^linear ^sealed Chan :ptr<void>)

(defopaque ^affine Tagged [A] :int)

(defdata ^heap Tree [a] (Leaf) (Node (Tree a) a (Tree a)))

(defstruct ^move ^linear Cell [n : int])

(defmacro mk [name] `(defstruct ^copy ~name [x : int]))

(def e (pack 1 (exists ^linear [a] a)))'
ACTUAL=$(printf '%s\n' "$MIG_IN" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$MIG_WANT" ] && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "keyword flags not moved before the name, or not idempotent; got: $ACTUAL"
fi

NAME="fmt-keeps-commented-type-decl-keyword-flag"
COM_IN='(defstruct Commented :copy ; keep this comment with the flag
  [x : int])'
ACTUAL=$(printf '%s\n' "$COM_IN" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qF "; keep this comment with the flag" \
   && printf '%s\n' "$ACTUAL" | grep -qF ":copy" \
   && ! printf '%s\n' "$ACTUAL" | grep -qF "^copy"; then
    pass "$NAME"
else
    fail "$NAME" "a declaration with a comment was rewritten or lost its comment; got: $ACTUAL"
fi

NAME="fmt-check-flags-old-type-decl-spelling"
OLD_F=$(mktemp "${TMPDIR:-/tmp}/fmt-old-XXXXXX.tur")
NEW_F=$(mktemp "${TMPDIR:-/tmp}/fmt-new-XXXXXX.tur")
printf '(defstruct Point :copy [x : int])\n' > "$OLD_F"
printf '(defstruct ^copy Point [x : int])\n' > "$NEW_F"
if ! "$TUR" fmt --check "$OLD_F" >/dev/null 2>&1 && "$TUR" fmt --check "$NEW_F" >/dev/null 2>&1; then
    pass "$NAME"
else
    fail "$NAME" "--check should reject the keyword spelling and accept the attribute one"
fi
rm -f "$OLD_F" "$NEW_F"

# ---------------------------------------------------------------------------
# caret-type-attributes-plan P5: a defeffect's trailing `^extends Parent` /
# `^capability` move before the name, a defn's `#[no-unwind]` / `#[used]` /
# `(export-as "c")` become `^no-unwind` / `^used` / `^export-as "c"` (the
# compiler rejects the old spelling, TUR-E0718).  A defeffect with a comment in it is left alone.
# ---------------------------------------------------------------------------
NAME="fmt-migrates-defeffect-and-defn-attributes"
P5_IN='(defeffect FS [] :nil ^extends IO ^capability)
(defeffect Write [s : cstr] : nil ^extends IO)
(defeffect ^private Hidden [] : nil ^capability)
(defn #[no-unwind] #[used] f [] : int 7)
(defn (export-as "my_g") g [] : int 8)
(defn ^deprecated "old" (export-as "my_h") h [] : int 9)'
P5_WANT='(defeffect ^capability ^extends IO FS [] :nil)

(defeffect ^extends IO Write [s : cstr] : nil)

(defeffect ^private ^capability Hidden [] : nil)

(defn ^no-unwind ^used f [] : int 7)

(defn ^export-as "my_g" g [] : int 8)

(defn ^deprecated "old" ^export-as "my_h" h [] : int 9)'
ACTUAL=$(printf '%s\n' "$P5_IN" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$P5_WANT" ] && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "attributes not moved before the name, or not idempotent; got: $ACTUAL"
fi

NAME="fmt-keeps-commented-defeffect-trailing-attrs"
P5C_IN='(defeffect Commented [] : nil ; keep this comment with the attribute
  ^extends IO)'
ACTUAL=$(printf '%s\n' "$P5C_IN" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qF "; keep this comment with the attribute" \
   && printf '%s\n' "$ACTUAL" | head -1 | grep -qF "(defeffect Commented" \
   && printf '%s\n' "$ACTUAL" | grep -qF "^extends"; then
    pass "$NAME"
else
    fail "$NAME" "a defeffect with a comment was rewritten or lost its comment; got: $ACTUAL"
fi

NAME="fmt-defn-header-skips-pre-name-attrs"
# A defn too long for one line keeps its name, parameters and result type in
# the header when it carries pre-name attributes; the parameter vector used to
# fall to a line of its own because the attributes shifted every index.
P5L_IN='(defn ^no-unwind ^export-as "c_long" long-name-function-here [alpha : int beta : int gamma : int delta : int epsilon : int] : int (+ alpha beta gamma delta epsilon 1 2 3))'
ACTUAL=$(printf '%s\n' "$P5L_IN" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | head -1 | grep -qF '(defn ^no-unwind ^export-as "c_long" long-name-function-here [alpha : int' \
   && printf '%s\n' "$ACTUAL" | grep -qF "] : int" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "pre-name attributes split the header or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# FT7: bootstrap -- every hand-authored stdlib file is already self-formatted.
# docstrings.tur is excluded: it is an auto-generated artifact (gendocs.py
# --emit-tur) whose inline-C literal bodies the formatter does not round-trip,
# matching the exclusion in the FT8 idempotence check below.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# fmt-drops-comments-in-handle-and-binding-modifier-gaps: comments in the gaps
# the header/arm printers walked with a bare ' ' -- a `handle` scrutinee/arm
# gap, a `case` arm, a `defpackage` entry, a `loop` head, a trailing comment
# inside a call before its `)` -- and a `^mut` binding pair, which the pair
# walk split as name=`^mut` value=`y`.  Each case asserts the comment survives
# AND the second pass equals the first.
# ---------------------------------------------------------------------------
fmt_gap_case() {
    local name="$1" needle="$2" input="$3"
    local actual roundtrip
    actual=$(printf '%s\n' "$input" | "$TUR" fmt --stdin 2>/dev/null)
    roundtrip=$(printf '%s\n' "$actual" | "$TUR" fmt --stdin 2>/dev/null)
    if printf '%s\n' "$actual" | grep -qF -- "$needle" && [ "$actual" = "$roundtrip" ]; then
        pass "$name"
    else
        fail "$name" "comment dropped or not idempotent; got: $actual"
    fi
}
read -r -d '' GAP_HANDLE <<'EOF'
(defn run [] : int
  (handle (perform (Ask))
    ;; the arm below re-opens Write
    (Ask [] k) (resume k 1)))
EOF
fmt_gap_case "fmt-preserves-handle-arm-gap-comment" ";; the arm below re-opens Write" "$GAP_HANDLE"
read -r -d '' GAP_MUT <<'EOF'
(defn main [] : int
  (let [^mut y 0]
    (println y)) ; trailing on the last body form
  0)
EOF
fmt_gap_case "fmt-preserves-mut-binding-and-trailing-comment" "; trailing on the last body form" "$GAP_MUT"
ACTUAL=$(printf '%s\n' "$GAP_MUT" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qF "(let [^mut y 0]"; then
    pass "fmt-keeps-mut-binding-pair-on-one-line"
else
    fail "fmt-keeps-mut-binding-pair-on-one-line" "^mut pair split; got: $ACTUAL"
fi
read -r -d '' GAP_CASE <<'EOF'
(defn f [x : int] : int
  (case x
    ;; the zero arm
    0 10
    1 20 ;; the one arm
    _ 0))
EOF
fmt_gap_case "fmt-preserves-case-arm-comments" ";; the one arm" "$GAP_CASE"
read -r -d '' GAP_PKG <<'EOF'
(defpackage app
  :name "app"
  ;; pinned for the raygui shim
  :version "0.1.0")
EOF
fmt_gap_case "fmt-preserves-defpackage-entry-comment" ";; pinned for the raygui shim" "$GAP_PKG"
read -r -d '' GAP_LOOP <<'EOF'
(defn run-loop [] : int
  (loop
    ; Allocate out-params for the next request.
    (def out-method 1)
    (recur)))
EOF
fmt_gap_case "fmt-preserves-loop-head-comment" "; Allocate out-params for the next request." "$GAP_LOOP"
read -r -d '' GAP_CALL <<'EOF'
(defn main [] : int
  (let [n 40]
    (println (* n 2)) ;; 80
    )
  0)
EOF
fmt_gap_case "fmt-preserves-trailing-comment-before-close" ";; 80" "$GAP_CALL"
read -r -d '' MUT_SUGAR <<'EOF'
(defn main [] : int
  (let [x 42]
    (let [r &mut x] ; a mutable borrow
      (println 1)))
  0)
EOF
fmt_gap_case "fmt-mut-borrow-sugar-idempotent" "; a mutable borrow" "$MUT_SUGAR"

# ---------------------------------------------------------------------------
# r7rs-lang-plan R9: `#lang r7rs` is re-indented, never reprinted.  The form
# printer rewrote `#t` to `true`, `#\x` to a constructor call, `|two words|`
# to two symbols, `,` to `~` and `#e1.5e2` to 150 -- a different program.
# ---------------------------------------------------------------------------
TMPDIR_R7=$(mktemp -d)
R7_LEXEMES='#lang r7rs
(define v (quote #(1 "two\n" #\x #\( #\space |two words| #u8(1 255) #e1.5e2 #x1F)))
(define (f x) `(,x ,@(list #t #f) . #;skipped tail))
#| a block
   comment |#
(write [1 2])'
printf '%s\n' "$R7_LEXEMES" > "$TMPDIR_R7/lex.tur"
NAME="fmt-r7rs-lexemes-verbatim"
ACTUAL=$("$TUR" fmt --stdout "$TMPDIR_R7/lex.tur" 2>&1)
if [ "$ACTUAL" = "$R7_LEXEMES" ]; then
    pass "$NAME"
else
    fail "$NAME" "a formatted Scheme file changed: $(printf '%s' "$ACTUAL" | head -4 | tr '\n' '|')"
fi

# ---------------------------------------------------------------------------
# Test: `#reads` / `#writes` frames survive formatting.
#
# Both read as the lists `(reads ...)` / `(writes ...)` and neither has a paren
# spelling, so printing them as calls produced a file that does NOT COMPILE --
# `unknown function or operator 'reads'` -- silently, exit 0.  A single-name
# frame normalizes to the unbracketed spelling; `#writes []` ("writes nothing")
# must keep its brackets, since a frameless `#writes` is a different claim.
# ---------------------------------------------------------------------------
NAME="fmt-frame-annotations-round-trip"
FRAME_IN='(defn r-one [^borrow v : (Vec int)] #reads v : int (vec-len v))
(defn r-brk [^borrow v : (Vec int)] #reads [v] : int (vec-len v))
(defn w-one [v : (Vec int) x : int] #writes v : nil (vec-push! v x))
(defn w-brk [v : (Vec int) x : int] #writes [v] : nil (vec-push! v x))
(defn w-none [x : int] #writes [] : int x)'
FRAME_EXPECTED='(defn r-one [^borrow v : (Vec int)] #reads v : int (vec-len v))

(defn r-brk [^borrow v : (Vec int)] #reads v : int (vec-len v))

(defn w-one [v : (Vec int) x : int] #writes v : nil (vec-push! v x))

(defn w-brk [v : (Vec int) x : int] #writes v : nil (vec-push! v x))

(defn w-none [x : int] #writes [] : int x)'
ACTUAL=$(printf '%s\n' "$FRAME_IN" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$FRAME_EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected:|$(printf '%s' "$FRAME_EXPECTED" | tr '\n' '|')| got:|$(printf '%s' "$ACTUAL" | tr '\n' '|')|"
fi

NAME="fmt-frame-annotations-still-compile"
TMPDIR_FR=$(mktemp -d)
printf '%s\n' "$FRAME_IN" '(defn main [] : int 0)' > "$TMPDIR_FR/fr.tur"
"$TUR" fmt "$TMPDIR_FR/fr.tur" > /dev/null 2>&1
if "$TUR" check "$TMPDIR_FR/fr.tur" > /dev/null 2>&1; then
    pass "$NAME"
else
    fail "$NAME" "a formatted file with #reads/#writes no longer compiles: $("$TUR" check "$TMPDIR_FR/fr.tur" 2>&1 | grep -m1 error)"
fi
rm -rf "$TMPDIR_FR"

NAME="fmt-r7rs-reindent"
printf '%s\n' '#lang r7rs' '(define (f x)' '        (if (> x 0)' '     (list x' '  "a' '   b")' '  #f))' \
    '(let ((a 1)' '  (b 2))' '   (g a' '  b))' > "$TMPDIR_R7/ind.tur"
EXPECTED=$(printf '%s\n' '#lang r7rs' '(define (f x)' '  (if (> x 0)' '    (list x' '          "a' '   b")' '    #f))' \
    '(let ((a 1)' '      (b 2))' '  (g a' '     b))')
ACTUAL=$("$TUR" fmt --stdout "$TMPDIR_R7/ind.tur" 2>&1)
if [ "$ACTUAL" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected:|$(printf '%s' "$EXPECTED" | tr '\n' '|')| got:|$(printf '%s' "$ACTUAL" | tr '\n' '|')|"
fi

NAME="fmt-r7rs-idempotent"
PASS1=$("$TUR" fmt --stdout "$TMPDIR_R7/ind.tur" 2>/dev/null)
PASS2=$(printf '%s\n' "$PASS1" | "$TUR" fmt --stdin 2>/dev/null)
if [ -n "$PASS1" ] && [ "$PASS1" = "$PASS2" ]; then
    pass "$NAME"
else
    fail "$NAME" "fmt(fmt(x)) != fmt(x)"
fi

NAME="fmt-r7rs-stdin-lang"
ACTUAL=$(printf '(display #t)\n' | "$TUR" fmt --stdin --lang r7rs 2>&1)
if [ "$ACTUAL" = '(display #t)' ]; then
    pass "$NAME"
else
    fail "$NAME" "--stdin --lang r7rs: got '$ACTUAL'"
fi

NAME="fmt-r7rs-parse-error"
printf '#lang r7rs\n(define (f x)\n' > "$TMPDIR_R7/bad.tur"
"$TUR" fmt --check "$TMPDIR_R7/bad.tur" > /dev/null 2>&1
RC=$?
if [ "$RC" -ne 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "an unbalanced Scheme file was accepted (exit 0)"
fi
rm -rf "$TMPDIR_R7"

NAME="fmt-bootstrap-stdlib"
BOOTSTRAP_DIRTY=""
BOOTSTRAP_SEEN=0
BOOTSTRAP_WHY=""
while IFS= read -r -d '' f; do
    BOOTSTRAP_SEEN=$((BOOTSTRAP_SEEN + 1))
    if ! "$TUR" fmt --check "$f" > /dev/null 2>&1; then
        BOOTSTRAP_DIRTY="$BOOTSTRAP_DIRTY $f"
        # Record WHY, not just WHICH.  `--check` exits 1 both for "would
        # change" and for an I/O error or a crash, and the bare file list is
        # unactionable when the failure does not reproduce on the machine
        # reading it -- which is exactly what happened: three stdlib files
        # failed on CI, passed locally on a fresh sanitized build, and the
        # log said nothing that could distinguish the two cases.  Capture the
        # exit code, any stderr, and a bounded diff.
        _rc_out=$("$TUR" fmt --check "$f" 2>&1 >/dev/null); _rc=$?
        BOOTSTRAP_WHY="$BOOTSTRAP_WHY
  --- $f (fmt --check exit $_rc)"
        if [ -n "$_rc_out" ]; then
            BOOTSTRAP_WHY="$BOOTSTRAP_WHY
      stderr: $(printf '%s' "$_rc_out" | head -3 | tr '\n' '|')"
        fi
        BOOTSTRAP_WHY="$BOOTSTRAP_WHY
$("$TUR" fmt --diff "$f" 2>&1 | head -20 | sed 's/^/      /')"
    fi
done < <(find stdlib -name '*.tur' -not -name 'docstrings.tur' -print0)
# Same guard as FT8 below: this check also passes by default, so an
# enumeration that yields nothing would report success having tested nothing.
if [ "$BOOTSTRAP_SEEN" -eq 0 ]; then
    fail "$NAME" "no files checked -- stdlib enumeration produced nothing"
elif [ -z "$BOOTSTRAP_DIRTY" ]; then
    pass "$NAME"
else
    fail "$NAME" "stdlib is not self-formatted:$BOOTSTRAP_DIRTY$BOOTSTRAP_WHY"
fi

# ---------------------------------------------------------------------------
# FT8: idempotence -- fmt(fmt(x)) == fmt(x) on a sample of stdlib files
# ---------------------------------------------------------------------------
NAME="fmt-idempotence-stdlib"
IDEMPOTENT_FAIL=0
IDEMPOTENT_SEEN=0
# The sample is bounded inside the loop rather than with `head -z`. `-z` is a
# GNU coreutils extension that BSD/macOS head does not have, and the way it
# failed was invisible: head errored, the pipeline produced nothing, the loop
# body never ran, IDEMPOTENT_FAIL stayed 0, and this reported PASS while
# checking zero files. Formatter idempotence had no coverage on macOS at all
# and the summary line said everything was fine.
while IFS= read -r -d '' f; do
    [ "$IDEMPOTENT_SEEN" -ge 20 ] && break
    IDEMPOTENT_SEEN=$((IDEMPOTENT_SEEN + 1))
    PASS1=$("$TUR" fmt --stdout "$f" 2>/dev/null)
    PASS2=$(printf '%s\n' "$PASS1" | "$TUR" fmt --stdin 2>/dev/null)
    if [ "$PASS1" != "$PASS2" ]; then
        fail "$NAME" "$f: fmt(fmt(x)) != fmt(x)"
        IDEMPOTENT_FAIL=1
        break
    fi
done < <(find stdlib -name '*.tur' -not -name 'docstrings.tur' -print0)
# Checking nothing is a failure, not a pass. Without this the next portability
# break in the file enumeration would go silent exactly as `head -z` did.
if [ "$IDEMPOTENT_SEEN" -eq 0 ]; then
    fail "$NAME" "no files checked -- stdlib enumeration produced nothing"
elif [ "$IDEMPOTENT_FAIL" -eq 0 ]; then
    pass "$NAME"
fi

# ---------------------------------------------------------------------------
# fmt-reprints-sweet-as-s-expressions: a sweet buffer is checked and kept as
# written -- the layout IS the syntax -- as r7rs/sweet always was.  It used to
# come back as s-expressions under a header still announcing sweet.
# ---------------------------------------------------------------------------
NAME="fmt-sweet-kept-as-written"
SWEET_SRC=$(printf 'defn double [x]\n  {x * 2}\n\ndefn main []\n  println(double(21))\n  0')
ACTUAL=$(printf '%s\n' "$SWEET_SRC" | "$TUR" fmt --stdin --lang sweet 2>/dev/null)
if [ "$ACTUAL" = "$SWEET_SRC" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected the sweet buffer unchanged, got '$ACTUAL'"
fi

NAME="fmt-sweet-file-keeps-header-and-body"
TMPDIR_SW=$(mktemp -d)
printf '#lang turmeric/sweet\ndefn double [x]\n  {x * 2}\n' > "$TMPDIR_SW/b.tur.sweet"
ACTUAL=$("$TUR" fmt --stdout "$TMPDIR_SW/b.tur.sweet" 2>/dev/null)
EXPECTED=$(printf '#lang turmeric/sweet\ndefn double [x]\n  {x * 2}')
if [ "$ACTUAL" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$EXPECTED', got '$ACTUAL'"
fi

NAME="fmt-sweet-parse-error-still-reported"
printf 'defn f [x]\n  (+ x\n' | "$TUR" fmt --stdin --lang sweet > /dev/null 2>&1
RC=$?
if [ "$RC" -ne 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected a nonzero exit for an unbalanced sweet buffer"
fi

# ---------------------------------------------------------------------------
# format-subcommand-shreds-a-sweet-buffer: `tur format` takes its reader from
# the extension / `#lang` line (or --lang), instead of printing a sweet
# buffer one token per line under the Turmeric reader.
# ---------------------------------------------------------------------------
NAME="format-sweet-file-kept"
printf '%s\n' "$SWEET_SRC" > "$TMPDIR_SW/nb.tur.sweet"
ACTUAL=$("$TUR" format "$TMPDIR_SW/nb.tur.sweet" 2>/dev/null)
if [ "$ACTUAL" = "$SWEET_SRC" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected the sweet file unchanged, got '$ACTUAL'"
fi

NAME="format-stdin-lang-sweet"
ACTUAL=$(printf '%s\n' "$SWEET_SRC" | "$TUR" format --lang sweet 2>/dev/null)
if [ "$ACTUAL" = "$SWEET_SRC" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected the sweet buffer unchanged, got '$ACTUAL'"
fi

NAME="format-check-sweet-already-formatted"
"$TUR" format --check "$TMPDIR_SW/nb.tur.sweet" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for --check on a kept-as-written file, got $RC"
fi
rm -rf "$TMPDIR_SW"

# ---------------------------------------------------------------------------
# lang-flags-take-different-vocabularies: `tur fmt --lang` also takes any base
# `tur dialects` lists, so one vocabulary works for every --lang on the binary.
# ---------------------------------------------------------------------------
NAME="fmt-lang-accepts-dialect-bases"
LANG_FAIL=""
LANG_SEEN=0
while IFS= read -r base; do
    [ -n "$base" ] || continue
    LANG_SEEN=$((LANG_SEEN+1))
    case "$base" in r7rs*) INPUT='(display 1)' ;; *sweet) INPUT='println(1)' ;; *) INPUT='(println 1)' ;; esac
    printf '%s\n' "$INPUT" | "$TUR" fmt --stdin --lang "$base" > /dev/null 2>&1 \
        || LANG_FAIL="$LANG_FAIL $base"
done < <("$TUR" dialects 2>/dev/null | awk 'NR>1 && $1 ~ /^[a-z]/ {print $1}')
if [ "$LANG_SEEN" -eq 0 ]; then
    fail "$NAME" "no bases checked -- \`tur dialects\` listed nothing"
elif [ -z "$LANG_FAIL" ]; then
    pass "$NAME"
else
    fail "$NAME" "rejected base(s):$LANG_FAIL"
fi

NAME="fmt-lang-unknown-still-rejected"
printf '(x)\n' | "$TUR" fmt --stdin --lang no-such-dialect > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 2 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 2 for an unknown dialect, got $RC"
fi

# ---------------------------------------------------------------------------
# An attribute `#[name]` reads as the symbol `#name`; the printer must spell it
# back rather than write `#name`, which does not parse.  (A defn's own
# `#[used]` / `(export-as ..)` are rewritten to `^used` / `^export-as` first.)
# ---------------------------------------------------------------------------
NAME="fmt-hash-attribute-roundtrips"
INPUT='(foo #[bar] 1)'
OUT=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>&1)
if [ "$OUT" = "$INPUT" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$INPUT', got '$OUT'"
fi

# ---------------------------------------------------------------------------
# A directory walk passes over `*.html.tur` template inputs (not Turmeric); a
# file named explicitly is still formatted.
# ---------------------------------------------------------------------------
NAME="fmt-walk-skips-html-templates"
TMPDIR_T=$(mktemp -d)
printf '<h1><%%= title %%></h1>\n' > "$TMPDIR_T/page.html.tur"
printf '(defn   f [] : int 1)\n' > "$TMPDIR_T/ok.tur"
"$TUR" fmt --check "$TMPDIR_T" > /dev/null 2>&1
RC_DIR=$?
"$TUR" fmt --check "$TMPDIR_T/page.html.tur" > /dev/null 2>&1
RC_FILE=$?
if [ "$RC_DIR" -eq 1 ] && [ "$RC_FILE" -ne 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected walk exit 1 (ok.tur would change) and explicit file to fail on its markup, got $RC_DIR / $RC_FILE"
fi
rm -rf "$TMPDIR_T"

# ---------------------------------------------------------------------------
# fmt-expands-user-reader-macros: a user reader macro's invocation is printed
# back as written, and a `#use-reader-macros` directive survives.  Formatting
# used to print the macro's EXPANSION (`#tag{a}` became `(+ "a")`) and drop
# the directive line entirely -- so `tur fmt --check` in CI rewrote, or could
# no longer read, every file using a user reader macro.
# ---------------------------------------------------------------------------
NAME="fmt-keeps-user-reader-macros"
TMPDIR_T=$(mktemp -d)
cat > "$TMPDIR_T/macros.tur" <<'EOF'
(reader-macros/define 'tag :raw-brace '(str-concat "tag:" $body))
(reader-macros/define 'sum :datum-bracket '(+ $body))
EOF
cat > "$TMPDIR_T/main.tur" <<'EOF'
#use-reader-macros "macros.tur"
(defn main [] : int
  (println #tag{<p class="a">  {x}  \} </p>})
  (println   #sum[1 2   3])
  0)
EOF
cat > "$TMPDIR_T/want.tur" <<'EOF'
#use-reader-macros "macros.tur"
(defn main [] : int
  (println #tag{<p class="a">  {x}  \} </p>})
  (println #sum[1 2   3])
  0)
EOF
"$TUR" fmt "$TMPDIR_T/main.tur" > /dev/null 2>&1
"$TUR" fmt --check "$TMPDIR_T/main.tur" > /dev/null 2>&1
RC_CHECK=$?
if diff -q "$TMPDIR_T/main.tur" "$TMPDIR_T/want.tur" > /dev/null && [ "$RC_CHECK" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "check rc=$RC_CHECK; got: $(cat "$TMPDIR_T/main.tur")"
fi
rm -rf "$TMPDIR_T"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "fmt summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1

#!/usr/bin/env bash
# The bare-name constructor alias must be ABSENT whenever the name it would
# define has more than one meaning.
#
# A constructor's emitted C symbol is `ctor_<Adt>_<Ctor>`, and a constructor
# name owned by exactly one ADT additionally gets a bare `ctor_<Ctor>` macro
# alias so hand-written inline C keeps working (stdlib/either.tur documents
# `ctor_Left(v)`).  That alias must fail CLOSED: when the name is ambiguous no
# alias is emitted, so inline C naming it fails at cc pointing at the
# constructor -- rather than silently binding to whichever ADT was emitted
# first.
#
# It did not always fail closed.  The uniqueness test keyed on the RAW
# constructor name while the alias was guarded on the MANGLED one, so `b-c` and
# `b_c` in two different ADTs read as two distinct unique names, both emitted an
# alias, and the second `#define` was dropped by its own `#ifndef`.  Inline C
# calling `ctor_b_c` then reached the FIRST ADT's constructor with no diagnostic
# at any layer -- and with both ADTs on the int64 carrier, C's type system could
# not see it either.  The repro asked for a constructor at tag 0 and got tag 1.
# See docs/archive/separator-fold-collides-emitted-c-names.md.
#
# This is a harness rather than a fixture because the property is about what is
# NOT in the emitted C, and pinning that with a whole-program expected.c
# snapshot would be four thousand lines to assert one absent line.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "check-ctor-alias-ambiguity: no compiler at $TUR" >&2; exit 1; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

pass=0
fail=0
pass() { echo "PASS $1"; pass=$((pass + 1)); }
fail() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

# --- 1. Ambiguous after mangling: `b-c` and `b_c` both fold to `b_c`. --------
src="$WORKDIR/ambiguous.tur"
cat > "$src" <<'EOF'
(defdata X [t] (XNil) (b-c t))
(defdata Y [t] (b_c t) (YNil))
(defn main [] : int 0)
EOF
out="$WORKDIR/ambiguous.c"
if ! "$TUR" emit-c "$src" > "$out" 2>/dev/null; then
    fail "ambiguous-no-alias" "emit-c failed"
elif grep -q '^#define ctor_b_c ' "$out"; then
    fail "ambiguous-no-alias" \
        "a bare \`ctor_b_c\` alias was emitted for two constructors that mangle \
alike (\`b-c\` in X, \`b_c\` in Y).  Only one #define can win, so inline C \
naming ctor_b_c silently reaches one ADT's constructor and never the other's.  \
The uniqueness test and the #ifndef guard must key on the same (mangled) name."
else
    pass "ambiguous-no-alias (no bare ctor_b_c for two constructors that mangle alike)"
fi

# Both qualified symbols must still exist and be distinct -- the alias is what
# is withheld, not the constructors.
# separator-fold-collides-emitted-c-names (direction 3, 2026-09-02): ADT and
# constructor names now use the injective mangler, so `b-c` and `b_c` are
# DIFFERENT C names (`b_hyc` / `b_unc`) -- each gets its own bare alias and the
# qualified symbols carry the escapes.  The ambiguity this file guards is now
# only the genuine one: two ADTs owning the SAME constructor spelling (below).
if grep -q 'ctor_X_b_hyc(' "$out" && grep -q 'ctor_Y_b_unc(' "$out" \
   && grep -q '^#define ctor_b_hyc ' "$out" && grep -q '^#define ctor_b_unc ' "$out"; then
    pass "distinct-under-injective-mangling (ctor_X_b_hyc / ctor_Y_b_unc, each with its bare alias)"
else
    fail "distinct-under-injective-mangling" \
        "b-c and b_c must mangle to distinct qualified symbols (b_hyc / b_unc), each with a bare alias"
fi
src_same="$WORKDIR/same-name.tur"
cat > "$src_same" <<'EOF'
(defdata P [t] (PNil) (Mk t))
(defdata Q [t] (Mk t) (QNil))
(defn main [] : int 0)
EOF
out_same="$WORKDIR/same-name.c"
if ! "$TUR" emit-c "$src_same" > "$out_same" 2>/dev/null; then
    fail "same-name-no-alias" "emit-c failed"
elif grep -q '^#define ctor_Mk ' "$out_same"; then
    fail "same-name-no-alias" \
        "a bare \`ctor_Mk\` alias was emitted for a constructor name two ADTs own; \
only one #define can win, so inline C naming ctor_Mk silently reaches one ADT."
elif grep -q 'ctor_P_Mk(' "$out_same" && grep -q 'ctor_Q_Mk(' "$out_same"; then
    pass "same-name-no-alias (no bare ctor_Mk; ctor_P_Mk and ctor_Q_Mk both emitted)"
else
    fail "same-name-no-alias" "withholding the alias must not withhold the constructors themselves"
fi

# --- 2. Inline C naming the ambiguous bare name must FAIL, loudly. ----------
src2="$WORKDIR/inline-c-ambiguous.tur"
cat > "$src2" <<'EOF'
(defdata X [t] (XNil) (b-c t))
(defdata Y [t] (b_c t) (YNil))
(defn make-y [n : int] : int
  ```c
  return ctor_b_c(n);
  ```)
(defn main [] : int (println (make-y 42)) 0)
EOF
if "$TUR" build "$src2" -o "$WORKDIR/bin" > "$WORKDIR/build.log" 2>&1; then
    fail "ambiguous-inline-c-rejected" \
        "inline C naming the ambiguous \`ctor_b_c\` BUILT.  It resolved to one \
of the two constructors silently -- exactly the wrong answer this check exists \
to prevent."
elif grep -q "ctor_b_c" "$WORKDIR/build.log"; then
    pass "ambiguous-inline-c-rejected (build fails naming ctor_b_c)"
else
    fail "ambiguous-inline-c-rejected" \
        "the build failed, but nothing in the output names ctor_b_c, so a \
reader cannot tell what went wrong"
fi

# --- 3. The unambiguous case still gets its alias. -------------------------
# This is the surface the alias exists for; withholding it everywhere would
# "fix" the bug by breaking every inline-C caller in the tree.
src3="$WORKDIR/unambiguous.tur"
cat > "$src3" <<'EOF'
(defdata Solo [t] (OnlyOne t) (SoloNil))
(defn main [] : int 0)
EOF
out3="$WORKDIR/unambiguous.c"
if ! "$TUR" emit-c "$src3" > "$out3" 2>/dev/null; then
    fail "unambiguous-alias-present" "emit-c failed"
elif grep -q '^#define ctor_OnlyOne ctor_Solo_OnlyOne$' "$out3"; then
    pass "unambiguous-alias-present (ctor_OnlyOne aliases ctor_Solo_OnlyOne)"
else
    fail "unambiguous-alias-present" \
        "a constructor name owned by exactly one ADT lost its bare-name alias; \
hand-written inline C calling it (stdlib/either.tur documents ctor_Left(v)) \
would stop linking"
fi

echo ""
echo "check-ctor-alias-ambiguity summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

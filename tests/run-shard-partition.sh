#!/usr/bin/env bash
# tests/run-shard-partition.sh -- assert TUR_TEST_SHARD partitions the corpus.
#
# Sharding is only safe if the slices are DISJOINT and their union is the WHOLE
# corpus.  Get either wrong and CI goes green having skipped fixtures, or burns
# time running some of them twice -- neither of which any other test would
# notice, because every individual shard still reports a clean summary.
#
# The membership answers come from the harness itself (TUR_TEST_LIST=1 prints
# what an invocation would run and exits), never from a second copy of the
# enumeration here.  A copy would agree with itself forever while drifting from
# what CI actually runs, which is the one failure this test exists to catch.
#
# Enumeration only: no fixture is compiled or run, so this is seconds, not
# minutes, and it needs no JIT engine (it never invokes $TUR).
set -u

cd "$(dirname "$0")/.." || exit 1

FAIL=0
note() { printf '%s\n' "$*"; }
fail() { printf 'FAIL run-shard-partition -- %s\n' "$*"; FAIL=$((FAIL + 1)); }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# $1 = output file, rest = environment assignments.
#
# stderr is KEPT, not dropped.  When the harness refuses to enumerate -- an
# engine gate it should have been exempt from, a missing binary, a syntax
# error -- the only symptom here is an empty list, and "0 fixtures" alone
# cannot tell that apart from a broken partition.  Discarding the reason cost
# a CI round exactly once: the non-JIT `test` job hit run-jit.sh's native
# smoke check, which exits 1, and the log said only "tur_shard_partition
# (Failed)".  diagnose() below prints what the harness actually said.
# The EXIT STATUS is the signal, not the line count.  run-jit.sh reports a
# refusal on stdout, so a harness that bailed hands back several lines of error
# prose -- which a count-based check happily treats as fixture names and then
# reports as a bogus partition ("6 fixtures in more than one shard").  That is
# a worse log than the real one.  Bail on the status instead, and say why.
#
# Not hypothetical: the non-JIT `test` job hit run-jit.sh's native-execution
# smoke check, which exits 1 long before enumerating, and CI said only
# "tur_shard_partition (Failed)".
list() {
    local out="$1"; shift
    if env "$@" TUR_TEST_LIST=1 bash tests/run-jit.sh > "$out" 2> "$out.err"; then
        return 0
    fi
    echo "FAIL run-shard-partition -- run-jit.sh exited non-zero while listing"
    echo "  env: $*"
    echo "  stdout (first 6 lines):"
    head -6 "$out" 2>/dev/null | sed 's/^/    /'
    echo "  stderr (first 6 lines):"
    head -6 "$out.err" 2>/dev/null | sed 's/^/    /'
    echo
    echo "  Most likely a gate in run-jit.sh that TUR_TEST_LIST should skip."
    echo "  List mode answers a question about tests/fixtures/ and must not"
    echo "  require a working JIT engine -- this test runs in the non-JIT job"
    echo "  too, where \`tur jit\` cannot run anything."
    exit 1
}

check_partition() {
    local n="$1" filter="${2:-}"
    local label="N=$n${filter:+ filter=$filter}"
    local full="$WORK/full-$n${filter:+-f}"

    if [ -n "$filter" ]; then
        list "$full" "TUR_TEST_FILTER=$filter"
    else
        list "$full"
    fi
    sort "$full" -o "$full"

    local total
    total=$(wc -l < "$full" | tr -d ' ')
    if [ "$total" -lt 2 ]; then
        fail "$label: the corpus enumerated $total fixtures; nothing to partition"
        return
    fi

    : > "$WORK/union"
    local i
    for i in $(seq 1 "$n"); do
        local part="$WORK/part-$n-$i"
        if [ -n "$filter" ]; then
            list "$part" "TUR_TEST_SHARD=$i/$n" "TUR_TEST_FILTER=$filter"
        else
            list "$part" "TUR_TEST_SHARD=$i/$n"
        fi
        cat "$part" >> "$WORK/union"
    done
    sort "$WORK/union" -o "$WORK/union"

    # Disjoint: a name in two shards would be run twice.
    local dupes
    dupes=$(uniq -d < "$WORK/union")
    if [ -n "$dupes" ]; then
        fail "$label: $(printf '%s\n' "$dupes" | wc -l | tr -d ' ') fixture(s) in more than one shard"
        printf '%s\n' "$dupes" | head -5 | sed 's/^/    - /'
    fi

    # Complete: a name in no shard would never run at all -- the dangerous half.
    local missing
    missing=$(comm -23 "$full" "$WORK/union")
    if [ -n "$missing" ]; then
        fail "$label: $(printf '%s\n' "$missing" | wc -l | tr -d ' ') fixture(s) in NO shard"
        printf '%s\n' "$missing" | head -5 | sed 's/^/    - /'
    fi

    # And nothing conjured that the full run does not have.
    local extra
    extra=$(comm -13 "$full" "$WORK/union")
    if [ -n "$extra" ]; then
        fail "$label: $(printf '%s\n' "$extra" | wc -l | tr -d ' ') fixture(s) in a shard but not in the full run"
    fi

    if [ -z "$dupes" ] && [ -z "$missing" ] && [ -z "$extra" ]; then
        note "  ok  $label -- $total fixtures partitioned across $n shard(s)"
    fi
}

note "shard partition (disjoint + complete):"
for n in 1 2 3 4 7; do check_partition "$n"; done

# Composing with a filter must not shift membership: ordinals are assigned over
# the full corpus, so a filtered shard is a SUBSET of the same unfiltered shard.
# Without that, `TUR_TEST_FILTER` could not be used to re-run one shard's
# failure -- the name would land in a different slice than the one that ran it.
note "filter composes without shifting membership:"
check_partition 3 '^saffron'
for i in 1 2 3; do
    list "$WORK/unf-$i" "TUR_TEST_SHARD=$i/3"
    list "$WORK/fil-$i" "TUR_TEST_SHARD=$i/3" "TUR_TEST_FILTER=^saffron"
    sort "$WORK/unf-$i" -o "$WORK/unf-$i"; sort "$WORK/fil-$i" -o "$WORK/fil-$i"
    strays=$(comm -23 "$WORK/fil-$i" "$WORK/unf-$i")
    if [ -n "$strays" ]; then
        fail "shard $i/3: filtering moved $(printf '%s\n' "$strays" | wc -l | tr -d ' ') fixture(s) into this shard"
    else
        note "  ok  shard $i/3 filtered is a subset of shard $i/3 unfiltered"
    fi
done

# Round-robin over each class independently is what makes the shards equal-cost:
# every shard gets ceil or floor of (class size / N), for BOTH classes.  A
# partition that sliced contiguously instead would still be disjoint and
# complete -- the checks above would pass -- while handing one shard all the
# error fixtures and another none.  Balance is the property that catches it,
# and non-emptiness is not enough: with ~550 negatives spread over 3 shards,
# even a badly skewed split leaves every shard holding some.
note "each class is balanced across shards (round-robin, not contiguous):"
happy_total=0; error_total=0
declare -a HAPPY_N=() ERROR_N=()
for i in 1 2 3; do
    e=$(grep -c '^errors/' "$WORK/unf-$i")
    h=$(( $(wc -l < "$WORK/unf-$i" | tr -d ' ') - e ))
    HAPPY_N+=("$h"); ERROR_N+=("$e")
    happy_total=$((happy_total + h)); error_total=$((error_total + e))
done
for class in happy error; do
    if [ "$class" = happy ]; then counts=("${HAPPY_N[@]}"); total=$happy_total
    else counts=("${ERROR_N[@]}"); total=$error_total; fi
    lo=$((total / 3)); hi=$(((total + 2) / 3))
    bad=0
    for c in "${counts[@]}"; do
        if [ "$c" -lt "$lo" ] || [ "$c" -gt "$hi" ]; then bad=1; fi
    done
    if [ "$bad" -eq 1 ]; then
        fail "$class fixtures are not balanced across 3 shards: ${counts[*]} (each must be $lo or $hi)"
    else
        note "  ok  $class -- ${counts[*]} across 3 shards (of $total, each $lo or $hi)"
    fi
done

# A nonsense spec must clamp the way tests/run.sh clamps it, and must never
# silently run nothing.  tools/ci/collect-suite-timings.py mirrors this table.
note "degenerate specs clamp rather than emptying the run:"
list "$WORK/s1of3" "TUR_TEST_SHARD=1/3"
list "$WORK/s3of3" "TUR_TEST_SHARD=3/3"
list "$WORK/whole"
declare -a CASES=(
    "0/3:$WORK/s1of3"    # index below 1 clamps up to the first shard
    "4/3:$WORK/s3of3"    # index past the total clamps down to the last
    "abc/3:$WORK/s1of3"  # non-numeric index is treated as 1
    "1/0:$WORK/whole"    # a total below 1 means "not sharded"
    "1/1:$WORK/whole"
    "2:$WORK/whole"      # no slash at all
)
for case in "${CASES[@]}"; do
    spec="${case%%:*}"; want="${case#*:}"
    list "$WORK/got" "TUR_TEST_SHARD=$spec"
    if ! diff -q <(sort "$WORK/got") <(sort "$want") >/dev/null; then
        fail "TUR_TEST_SHARD='$spec' selected $(wc -l < "$WORK/got" | tr -d ' ') fixtures; expected $(wc -l < "$want" | tr -d ' ')"
    else
        note "  ok  TUR_TEST_SHARD='$spec' -> $(wc -l < "$WORK/got" | tr -d ' ') fixtures"
    fi
done

# ---------------------------------------------------------------------------
# The same assertions for tests/run-turi.sh, which is sharded by TUR_TURI_SHARD.
#
# A second harness with its own variable needs its own proof: the two slices are
# computed over different corpora (run-turi.sh walks the same tree but counts
# `errors/` in one combined ordinal space, and it admits fixtures run-jit.sh does
# not), so "run-jit's partition holds" says nothing about this one.  Its own
# enumeration answers, via TURI_TEST_LIST, for the reason in this file's header.
turi_list() {
    local out="$1"; shift
    if env "$@" TURI_TEST_LIST=1 bash tests/run-turi.sh > "$out" 2> "$out.err"; then
        return 0
    fi
    echo "FAIL run-shard-partition -- run-turi.sh exited non-zero while listing"
    echo "  env: $*"
    head -6 "$out.err" 2>/dev/null | sed 's/^/    /'
    echo "  List mode must not need a built interpreter: it answers a question"
    echo "  about tests/fixtures/ and runs in the non-JIT job too."
    exit 1
}

turi_partition() {
    local n="$1"
    local full="$WORK/turi-full"
    turi_list "$full"
    sort "$full" -o "$full"
    local total; total=$(wc -l < "$full" | tr -d ' ')
    if [ "$total" -lt 2 ]; then
        fail "turi N=$n: enumerated $total fixtures; nothing to partition"
        return
    fi
    : > "$WORK/turi-union"
    local i
    for i in $(seq 1 "$n"); do
        turi_list "$WORK/turi-$n-$i" "TUR_TURI_SHARD=$i/$n"
        cat "$WORK/turi-$n-$i" >> "$WORK/turi-union"
    done
    sort "$WORK/turi-union" -o "$WORK/turi-union"
    local dupes missing extra
    dupes=$(uniq -d < "$WORK/turi-union")
    missing=$(comm -23 "$full" "$WORK/turi-union")
    extra=$(comm -13 "$full" "$WORK/turi-union")
    if [ -n "$dupes" ]; then
        fail "turi N=$n: $(printf '%s\n' "$dupes" | wc -l | tr -d ' ') fixture(s) in more than one shard"
        printf '%s\n' "$dupes" | head -5 | sed 's/^/    - /'
    fi
    if [ -n "$missing" ]; then
        fail "turi N=$n: $(printf '%s\n' "$missing" | wc -l | tr -d ' ') fixture(s) in NO shard"
        printf '%s\n' "$missing" | head -5 | sed 's/^/    - /'
    fi
    if [ -n "$extra" ]; then
        fail "turi N=$n: $(printf '%s\n' "$extra" | wc -l | tr -d ' ') fixture(s) in a shard but not in the full run"
    fi
    if [ -z "$dupes" ] && [ -z "$missing" ] && [ -z "$extra" ]; then
        note "  ok  turi N=$n -- $total fixtures partitioned across $n shard(s)"
    fi
}

echo
note "run-turi.sh shard partition (TUR_TURI_SHARD, disjoint + complete):"
for n in 1 2 3 4 7; do turi_partition "$n"; done

# TUR_TEST_SHARD must NOT shard this harness.  run-turi.sh reads its own variable
# precisely so the `aux` ctest part can sample it without touching the 160 other
# suites in the same job; if it ever honored TUR_TEST_SHARD too, setting that for
# one suite would silently slice this one as well.
note "run-turi.sh ignores TUR_TEST_SHARD (it has its own variable):"
turi_list "$WORK/turi-whole"
turi_list "$WORK/turi-foreign" "TUR_TEST_SHARD=1/4"
if diff -q <(sort "$WORK/turi-whole") <(sort "$WORK/turi-foreign") >/dev/null; then
    note "  ok  TUR_TEST_SHARD=1/4 left all $(wc -l < "$WORK/turi-whole" | tr -d ' ') fixtures in the run"
else
    fail "TUR_TEST_SHARD=1/4 changed run-turi.sh's selection; it must only honor TUR_TURI_SHARD"
fi

echo
if [ "$FAIL" -ne 0 ]; then
    echo "shard partition summary: $FAIL check(s) failed"
    exit 1
fi
echo "shard partition summary: all checks passed"
exit 0

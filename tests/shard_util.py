"""tests/shard_util.py -- parse an "i/N" shard spec and slice a work list.

Shared by tests/generic-spec-matrix.py and
tests/check-emitted-float-conversions.py.  Both are RUN_SERIAL ctest tests that
fan out internally, which on ctest's scheduler makes each one a barrier nothing
runs beside -- so splitting them across CI jobs is the only way to compress
them, and both need the same slice.

The spelling and the CLAMPING match tests/run.sh, tests/run-jit.sh and
tools/ci/collect-suite-timings.py:parse_shard, deliberately and to the letter:
1-based, a nonsense index snaps into range rather than erroring, and a total of
1 or less means "not sharded".  Diverging would let a timing row claim a slice
the harness did not run -- with "0/3" the harness runs shard 1/3, so every
reader of the spec must agree that it does.

Each caller passes its OWN environment variable (TUR_GSM_SHARD,
TUR_FCONV_SHARD), never $TUR_TEST_SHARD: collect-suite-timings.py tags rows
from the job's environment rather than per test, so $TUR_TEST_SHARD in a job
that also runs other suites mislabels all of them.
"""


def parse_shard(spec):
    """Parse an "i/N" shard spec into (index, total), 1-based.

    Returns (None, None) when the spec means "not sharded" -- empty, malformed,
    or a total of 1.
    """
    if not spec or "/" not in spec:
        return (None, None)
    left, _, right = spec.partition("/")
    total = int(right) if right.isdigit() else 1
    index = int(left) if left.isdigit() else 1
    if total < 1:
        total = 1
    if index < 1:
        index = 1
    if index > total:
        index = total
    if total <= 1:
        return (None, None)
    return (index, total)


def shard_slice(items, index, total):
    """Round-robin slice of `items` for 1-based shard `index` of `total`.

    Round-robin by ordinal, not contiguous: a contiguous split would still be
    disjoint and complete while handing one shard all of the expensive work.
    Here the caller's list is already ordered by its innermost loop, so a
    round-robin slice spreads each class of work evenly across shards.

    (None, None) -- the unsharded case -- returns the list unchanged, so a
    caller can slice unconditionally.
    """
    if not total or total <= 1:
        return list(items)
    return [x for n, x in enumerate(items) if n % total == index - 1]


def shard_label(index, total):
    """Render " shard i/N" for log lines, or "" when unsharded."""
    return " shard %d/%d" % (index, total) if total else ""

#!/usr/bin/env python3
"""tests/regions-fuzz-src.py -- SOURCE-LEVEL differential fuzzer for region
brackets (RM3, `with-region` / `bt-scope`).

Why this exists
---------------
A region rewinds a generation when the compiler can prove nothing outside it
still points in.  The proof is two locks -- a static walk over the bracket's
result type and a runtime escape note -- and the day after regions graduated,
three ordinary programs got past both (a node stored into an outer vec, a node
behind an erased `:int` field of an admitted record result, a `(Vec int)` of
erased nodes; docs/archive/regions-plan.md, "region-lock-hardening").  Each
was found by hand.  This harness generates that class of program by the
hundred and asks two questions the hand-written fixtures cannot ask at scale:

  1. SAFETY.  A value read back AFTER the bracket popped must be the value
     the generator predicted, on the default build AND under TUR_REGIONS=0.
     The default arm is compiled with `-fsanitize=address`, which turns the
     Debug arena's poison on in the pasted runtime: a straggler read of
     reclaimed region memory is a loud use-after-poison, not a stale byte
     that happens to still be right.

  2. SAVINGS.  A bracket with no escape MUST rewind.  The lock could be made
     trivially safe by refusing everything, and a safety-only fuzzer would
     never notice.  The generator knows, per bracket, whether anything it
     built escaped -- a store, an erasure, a node-bearing result -- and
     predicts the exact rewind / retire counts the runtime reports under
     TUR_REGION_STATS=1.  A mismatch either way is a failure: too few
     rewinds is a savings regression, too many is the lock failing open.

Both arms must also print exactly the predicted stdout, so the two arms
cannot agree by being wrong together.

Population
----------
Each program is a random sequence of CASES, each a function holding one
bracket (or two, nested).  A case picks the bracket form, the allocation
style (typed `TL` nodes, or `Link` nodes erased to `:int` through
`(:: (Link ..) :int)`), a small chain length, and a shape:

  results   int / float / cstr / record of ints / (Vec int) / (Pair int int)
            / (Option int) / (Map int int)     -- rewind when typed
            typed node                          -- retires (static walk)
            erased chain inside a record / vec  -- retires (erasure note)
  stores    vec-push! (typed and erased), a stored closure capturing a
            node, set! of a mutable global, mutmap-set!, bt-set! into a
            trail cell, a by-value aggregate holding a node pushed as a
            boxed element                       -- retire (store-side lock)
            a typed node handed to a hand-written inline-C cell store, which
            no store hook can see                -- retire (parameter note),
            and its mirror: inline-C called inside the bracket seeing only
            SCALARS                              -- must still rewind
  nested    inner + outer brackets with nothing escaping (both rewind), and
            the inner bracket storing the OUTER generation's node into an
            outer vec (inner rewinds, outer retires: owner flagging)

plus an optional extra chain built and consumed inside the bracket (typed:
no effect; erased: the erasure note retires the generation).

Proving it can fail
-------------------
Two one-line sabotages, both verified caught at n=20 seed=1 (A: 20/20
programs, B: 17/20 -- the other three held no typed no-escape bracket):

  A. Lock failing open -- in src/runtime/region.c, make
     `tur_region_note_escape` return immediately.  The store and erasure
     cases then rewind under their readers: ASan use-after-poison on the
     default arm and/or wrong values, and the savings model flags every
     "must retire" bracket as a rewind.
  B. Blanket refusal -- in emit_expr.c, make `region_type_reaches_node`
     return true at entry (both emit arms consult it).  Every output stays
     right and the SAVINGS check fails on every typed no-escape case:
     rewinds=0 where the model predicted them.

Usage
-----
    python3 tests/regions-fuzz-src.py --n 200 --seed 7
    python3 tests/regions-fuzz-src.py --n 40 --save-dir /tmp/rgn-fail

Exit status 0 when every case passed, 1 otherwise.  `--self-test` checks the
plumbing against the two pinned fixtures (region-escape-via-store,
region-escape-via-erasure) before any generated verdict counts.
"""

import argparse
import concurrent.futures
import os
import random
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzz_arm  # noqa: E402  (tests/fuzz_arm.py)
TIMEOUT = 120
STATS_RE = re.compile(r"region-stats: pushes=(\d+) rewinds=(\d+) retires=(\d+)")

# -fsanitize=address on the emitted program turns the arena poison on in the
# pasted region runtime (arena.c keys TUR_ARENA_ASAN on __SANITIZE_ADDRESS__).
# TUR_CC_FLAGS REPLACES the default flags, so the -L for libturi rides along.
ASAN_CC_FLAGS = "-O1 -g -std=c99 -fno-strict-aliasing -fsanitize=address -L{build}/src"

PRELUDE = """\
;; generated by tests/regions-fuzz-src.py -- seed {seed} case {idx}
(defdata Link :heap (Link [v : int nxt : int]))
(defdata TL :heap (TNil) (TCons :int :TL))
(defdata HoldsInt (HI :int :int))
(defdata HoldsLink (HL :int :Link))

(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))
(defn chain-sum [c : int] : int
  ```c
  struct {{ int64_t v; int64_t nxt; }} *p = (void *)(intptr_t)c;
  int64_t acc = 0; while (p) {{ acc += p->v; p = (void *)(intptr_t)p->nxt; }} return acc;
  ```)
(defn tbuild [n : int acc : TL] : TL
  (if (<= n 0) acc (tbuild (- n 1) (TCons n acc))))
(defn tsum [t : TL] : int
  (match t
    (TNil) 0
    (TCons v r) (+ v (tsum r))))
(defn link-v [l : Link] : int (match l (Link a b) a))
(defn cell-new [] : ptr<void>
  ```c
  int64_t *p = (int64_t *)malloc(sizeof(int64_t)); *p = 0; return (void *)p;
  ```)
(defn cell-set! [c : ptr<void> v : Link] : nil
  ```c
  *(int64_t *)c = (int64_t)(intptr_t)v;
  ```)
(defn cell-get-v [c : ptr<void>] : int
  ```c
  struct {{ int64_t v; int64_t nxt; }} *p = *(void **)c; return p->v;
  ```)
(defn twice [x : int] : int
  ```c
  return x * 2;
  ```)
"""


def tri(n):
    return n * (n + 1) // 2


class Case:
    """One generated function plus what main must do with it and what it must
    print.  `rewinds` / `retires` are the model's prediction for its brackets."""

    def __init__(self, name):
        self.name = name
        self.defn = ""        # the function text
        self.main = []        # lines for main (may bind a container first)
        self.expected = []    # stdout lines
        self.rewinds = 0
        self.retires = 0


def sum_expr(typed, n_var):
    """Build and consume a chain of length n inside a bracket; int-valued."""
    if typed:
        return f"(tsum (tbuild {n_var} (TNil)))"
    return f"(chain-sum (build {n_var} 0))"


def gen_case(rng, idx):
    c = Case(f"c{idx}")
    n = rng.randint(1, 6)
    typed = rng.random() < 0.6
    bt = rng.random() < 0.3            # bt-scope instead of with-region
    extra = rng.choice([None, "typed", "erased"])
    kind = rng.choice([
        "int", "float", "cstr", "record", "vec", "pair", "option", "map",
        "node", "erased-record", "erased-vec",
        "store-vec", "store-erased-vec", "store-closure", "store-global",
        "store-mutmap", "store-cell", "store-byval",
        "store-inline-c", "inline-c-scalar",
        "nested", "nested-store",
    ])
    # Shapes that only make sense in one style.
    if kind == "node":
        typed = True
    if kind in ("erased-record", "erased-vec", "store-erased-vec", "store-cell"):
        typed = False
    if kind in ("store-vec", "store-closure", "store-global", "store-mutmap",
                "store-byval", "nested-store", "store-inline-c",
                "inline-c-scalar"):
        typed = True     # the store hook is what is under test, not the erasure
    bracket = "bt-scope" if bt else "with-region"
    fx = " #fx{Bt}" if bt else ""
    s = sum_expr(typed, "n")
    pre = ""
    if extra == "typed":
        pre = "(tsum (tbuild n (TNil))) "
    elif extra == "erased":
        pre = "(chain-sum (build n 0)) "
    body_wrap = (lambda e: f"(do {pre}{e})") if pre else (lambda e: e)

    # An erased chain anywhere inside the bracket (the main chain or the
    # extra one) fires the erasure note: that generation retires.
    erased_inside = (not typed) or extra == "erased"
    escapes = erased_inside

    def one_bracket(body, ret):
        return f"(defn {c.name} [n : int]{fx} : {ret}\n  ({bracket} (fn [] {body_wrap(body)})))"

    T = tri(n)
    if kind == "int":
        c.defn = one_bracket(s, "int")
        c.main = [f"(println ({c.name} {n}))"]
        c.expected = [str(T)]
    elif kind == "float":
        c.defn = one_bracket(f"(* 1.5 (as float {s}))", "float")
        c.main = [f"(println ({c.name} {n}))"]
        c.expected = [fmt_float(1.5 * T)]
    elif kind == "cstr":
        c.defn = one_bracket(f'(do {s} "ok")', "cstr")
        c.main = [f"(println ({c.name} {n}))"]
        c.expected = ["ok"]
    elif kind == "record":
        c.defn = one_bracket(f"(HI n {s})", "HoldsInt")
        c.main = [f"(match ({c.name} {n}) (HI a b) (do (println a) (println b)))"]
        c.expected = [str(n), str(T)]
    elif kind == "vec":
        c.defn = one_bracket(f"(let [v (:: (vec-new) (Vec int))] (vec-push! v {s}) v)", "(Vec int)")
        c.main = [f"(println (vec-get ({c.name} {n}) 0))"]
        c.expected = [str(T)]
    elif kind == "pair":
        c.defn = one_bracket(f"(Pair {s} n)", "(Pair int int)")
        c.main = [f"(match ({c.name} {n}) (Pair a b) (do (println a) (println b)))"]
        c.expected = [str(T), str(n)]
    elif kind == "option":
        c.defn = one_bracket(f"(some {s})", "(Option int)")
        c.main = [f"(println (unwrap-or ({c.name} {n}) 0))"]
        c.expected = [str(T)]
    elif kind == "map":
        c.defn = one_bracket(f"(map-assoc (:: (map-new) (Map int int)) n {s})", "(Map int int)")
        c.main = [f"(println (map-get ({c.name} {n}) {n}))"]
        c.expected = [str(T)]
    elif kind == "node":
        c.defn = one_bracket("(tbuild n (TNil))", "TL")
        c.main = [f"(println (tsum ({c.name} {n})))"]
        c.expected = [str(T)]
        escapes = True
    elif kind == "erased-record":
        c.defn = one_bracket("(HI n (build n 0))", "HoldsInt")
        c.main = [f"(match ({c.name} {n}) (HI a h) (do (println a) (println (chain-sum h))))"]
        c.expected = [str(n), str(T)]
        escapes = True
    elif kind == "erased-vec":
        c.defn = one_bracket("(let [v (:: (vec-new) (Vec int))] (vec-push! v (build n 0)) v)", "(Vec int)")
        c.main = [f"(println (chain-sum (vec-get ({c.name} {n}) 0)))"]
        c.expected = [str(T)]
        escapes = True
    elif kind == "store-vec":
        c.defn = (f"(defn {c.name} [v : (Vec Link) n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] {body_wrap('(do (vec-push! v (Link n 0)) 1)')})))")
        c.main = [f"(let [v (:: (vec-new) (Vec Link))] (println ({c.name} v {n})) (println (link-v (vec-get v 0))))"]
        c.expected = ["1", str(n)]
        escapes = True
    elif kind == "store-erased-vec":
        c.defn = (f"(defn {c.name} [v : (Vec int) n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] {body_wrap('(do (vec-push! v (build n 0)) 1)')})))")
        c.main = [f"(let [v (:: (vec-new) (Vec int))] (println ({c.name} v {n})) (println (chain-sum (vec-get v 0))))"]
        c.expected = ["1", str(T)]
        escapes = True
    elif kind == "store-closure":
        c.defn = (f"(defn {c.name} [fv : (Vec (fn [] int)) n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] {body_wrap('(let [l (Link n 0)] (vec-push! fv (fn [] : int (link-v l))) 1)')})))")
        c.main = [f"(let [fv (:: (vec-new) (Vec (fn [] int)))] (println ({c.name} fv {n})) (println ((vec-get fv 0))))"]
        c.expected = ["1", str(n)]
        escapes = True
    elif kind == "store-global":
        # One global per case: a heap node is a unique value, and reading the
        # same global back twice in one program is a use-after-move.
        g = f"gl_{c.name}"
        c.defn = (f"(def ^mut {g} (Link 0 0))\n" +
                  one_bracket(f"(do (set! {g} (Link n 0)) 1)", "int"))
        c.main = [f"(println ({c.name} {n}))", f"(println (link-v {g}))"]
        c.expected = ["1", str(n)]
        escapes = True
    elif kind == "store-mutmap":
        c.defn = (f"(defn {c.name} [mm : (MutableMap int Link) n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] {body_wrap('(do (mutmap-set! mm n n (Link n 0)) 1)')})))")
        c.main = [f"(let [mm (:: (mutmap-new) (MutableMap int Link))] (println ({c.name} mm {n})) (println (link-v (mutmap-get mm {n} {n}))))"]
        c.expected = ["1", str(n)]
        escapes = True
    elif kind == "store-cell":
        c.defn = (f"(defn {c.name} [cell : BtCell n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] {body_wrap('(do (bt-set! cell (build n 0)) 1)')})))")
        c.main = [f"(let [cell (bt-cell-new 0)] (println ({c.name} cell {n})) (println (chain-sum (bt-get cell))))"]
        # bt-scope UNDOES the trailed write at its pop, so the reader sees the
        # cell's initial 0; the store still fired the note, so the generation
        # still retires.  with-region has no trail level and the write stays.
        c.expected = ["1", "0" if bt else str(T)]
        escapes = True
    elif kind == "store-byval":
        c.defn = (f"(defn {c.name} [hv : (Vec HoldsLink) n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] {body_wrap('(do (vec-push! hv (HL n (Link (* 2 n) 0))) 1)')})))")
        c.main = [f"(let [hv (:: (vec-new) (Vec HoldsLink))] (println ({c.name} hv {n})) (match (vec-get-byval hv 0) (HL a l) (println (link-v l))))"]
        c.expected = ["1", str(2 * n)]
        escapes = True
    elif kind == "store-inline-c":
        # A TYPED node into a hand-written C body: no ascription erases it, so
        # only the callee-side parameter note can see the escape.  This was a
        # silent wrong answer before that note existed.
        c.defn = (f"(defn {c.name} [cl : ptr<void> n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] {body_wrap('(do (cell-set! cl (Link n 0)) 1)')})))")
        c.main = [f"(let [cl (cell-new)] (println ({c.name} cl {n})) (println (cell-get-v cl)))"]
        c.expected = ["1", str(n)]
        escapes = True
    elif kind == "inline-c-scalar":
        # The other direction: inline-C is called inside the bracket but sees
        # only scalars, so the rewind must SURVIVE.  Without this the rule
        # could be "note every inline-C parameter" and nothing would notice.
        c.defn = one_bracket(f"(twice {s})", "int")
        c.main = [f"(println ({c.name} {n}))"]
        c.expected = [str(2 * T)]
    elif kind == "nested":
        inner = f"({bracket} (fn [] {s}))"
        c.defn = one_bracket(f"(+ {inner} {s})", "int")
        c.main = [f"(println ({c.name} {n}))"]
        c.expected = [str(2 * T)]
        # inner: escapes iff its own chain is erased; outer: erased inside too
        inner_escapes = not typed
        c.rewinds += 0 if inner_escapes else 1
        c.retires += 1 if inner_escapes else 0
    elif kind == "nested-store":
        c.defn = (f"(defn {c.name} [v : (Vec Link) n : int]{fx} : int\n"
                  f"  ({bracket} (fn [] (let [outer (Link (* 5 n) 0)] "
                  f"({bracket} (fn [] {body_wrap('(do (vec-push! v outer) 1)')}))))))")
        c.main = [f"(let [v (:: (vec-new) (Vec Link))] (println ({c.name} v {n})) (println (link-v (vec-get v 0))))"]
        c.expected = ["1", str(5 * n)]
        # the inner generation owns nothing that escaped (owner flagging);
        # the extra chain, if erased, sits in the inner generation and retires it
        inner_escapes = extra == "erased"
        c.rewinds += 0 if inner_escapes else 1
        c.retires += 1 if inner_escapes else 0
        escapes = True
    else:
        raise AssertionError(kind)

    if escapes:
        c.retires += 1
    else:
        c.rewinds += 1
    c.kind = kind
    c.typed = typed
    return c


def fmt_float(x):
    # Turmeric prints an integral float without a fractional part (`9`, not
    # `9.0`); 1.5 * T for a small integer T is otherwise exact and prints
    # like Python's repr.
    if x == int(x):
        return str(int(x))
    return repr(x)


def gen_program(seed, idx, n_cases):
    rng = random.Random(seed * 100003 + idx)
    cases = [gen_case(rng, i) for i in range(n_cases)]
    out = [PRELUDE.format(seed=seed, idx=idx)]
    for c in cases:
        out.append(c.defn)
    out.append("(defn main [] : int")
    for c in cases:
        for line in c.main:
            out.append("  " + line)
    out.append("  0)")
    expected = []
    for c in cases:
        expected.extend(c.expected)
    rewinds = sum(c.rewinds for c in cases)
    retires = sum(c.retires for c in cases)
    return "\n".join(out) + "\n", expected, rewinds, retires, cases


class Outcome:
    def __init__(self, status, stdout="", stderr="", stats=None):
        self.status = status
        self.stdout = stdout
        self.stderr = stderr
        self.stats = stats


def run_arm(tur, build, src, regions_on):
    env = dict(os.environ)
    env.pop("TUR_STDLIB_DIR", None)
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    if regions_on:
        env["TUR_REGION_STATS"] = "1"
        env["TUR_CC_FLAGS"] = ASAN_CC_FLAGS.format(build=build)
        env.pop("TUR_REGIONS", None)
    else:
        env["TUR_REGIONS"] = "0"
        # Arm clang's function-pointer detector on the reference arm
        # (tests/fuzz_arm.py).  The regions arm keeps its ASan flags: it links
        # the gcc-built sanitized runtime, and fnsan is clang-only.
        env, _ = fuzz_arm.armed_env(env)
    try:
        p = subprocess.run([tur, "run", src], capture_output=True, text=True,
                           timeout=TIMEOUT, cwd=REPO, env=env)
    except subprocess.TimeoutExpired:
        return Outcome("timeout")
    stats = None
    m = STATS_RE.search(p.stderr)
    if m:
        stats = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    if "AddressSanitizer" in p.stderr:
        return Outcome("asan", p.stdout, p.stderr, stats)
    if fuzz_arm.is_fnsan_trap(p.returncode):
        return Outcome("fnptr_trap", p.stdout, p.stderr, stats)
    if p.returncode != 0:
        return Outcome("fail", p.stdout, p.stderr, stats)
    return Outcome("clean", p.stdout, p.stderr, stats)


def check_program(tur, build, src_text, expected, rewinds, retires):
    """Returns a list of problem strings (empty == pass)."""
    problems = []
    with tempfile.NamedTemporaryFile("w", suffix=".tur", prefix="rgnfuzz-",
                                     delete=False) as f:
        f.write(src_text)
        path = f.name
    try:
        on = run_arm(tur, build, path, True)
        off = run_arm(tur, build, path, False)
    finally:
        os.unlink(path)
    exp = "\n".join(expected) + "\n"
    for arm, o in (("on", on), ("off", off)):
        if o.status == "timeout":
            problems.append(f"{arm}: timeout")
            continue
        if o.status == "asan":
            first = [l for l in o.stderr.splitlines() if "ERROR: AddressSanitizer" in l]
            problems.append(f"{arm}: {first[0] if first else 'AddressSanitizer report'}")
            continue
        if o.status == "fnptr_trap":
            msg = (f"{arm}: {fuzz_arm.TRAP_CLASS} -- an indirect call went "
                   "through a function pointer of the wrong type")
            if fuzz_arm.FNSAN_STRICT:
                problems.append(msg)
            else:
                print("  note: " + msg + " (report-only; "
                      "TUR_FUZZ_FNSAN_STRICT=0 set)", flush=True)
            continue
        if o.status == "fail":
            tail = o.stderr.strip().splitlines()[-1] if o.stderr.strip() else "(no stderr)"
            problems.append(f"{arm}: exit failure: {tail}")
            continue
        if o.stdout != exp:
            problems.append(f"{arm}: stdout mismatch\n  expected: {expected}\n  got:      {o.stdout.split()}")
    if on.status == "clean":
        if on.stats is None:
            problems.append("on: no region-stats line (TUR_REGION_STATS not honoured?)")
        else:
            pushes, rw, rt = on.stats
            if (rw, rt) != (rewinds, retires):
                problems.append(f"on: savings model mismatch: predicted rewinds={rewinds} retires={retires}, "
                                f"runtime reported rewinds={rw} retires={rt} (pushes={pushes})")
    return problems, on, off


def self_test(tur, build):
    """The plumbing: the two pinned fixtures must pass both arms and report
    the retire counts their comments promise (every bracket escapes)."""
    ok = True
    for fx, brackets in (("region-escape-via-store", 12), ("region-escape-via-erasure", 4)):
        d = os.path.join(REPO, "tests", "fixtures", fx)
        with open(os.path.join(d, "input.tur")) as f:
            src = f.read()
        with open(os.path.join(d, "expected.stdout")) as f:
            expected = f.read().splitlines()
        # store: eleven brackets escape and the nested case's inner rewinds
        # (case 10, the widened field store, arrived with r7rs-lang-plan R3,
        # and case 11, tvar/write in a bracket, with
        # stm-inside-closure-captured-tvar-undeclared -- CLAUDE.md's store-hook
        # rule puts every new store in that fixture, so a new case moves this
        # count too); erasure: all four retire.
        rw, rt = (1, 11) if fx == "region-escape-via-store" else (0, 4)
        problems, on, off = check_program(tur, build, src, expected, rw, rt)
        if problems:
            ok = False
            print(f"SELF-TEST FAIL {fx}:")
            for p in problems:
                print("  " + p)
        else:
            print(f"self-test ok: {fx} (rewinds={rw} retires={rt}, {brackets} brackets)")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=100, help="programs to generate")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cases", type=int, default=6, help="brackets per program")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--save-dir", default=None, help="write failing programs here")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    tur = os.path.abspath(args.tur)
    build = os.path.dirname(tur)
    if not os.access(tur, os.X_OK):
        print(f"no compiler at {tur}", file=sys.stderr)
        return 2

    if args.self_test:
        return 0 if self_test(tur, build) else 1

    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)
    print("regions-fuzz-src: reference arm " +
          fuzz_arm.armed_env(dict(os.environ))[1], flush=True)

    programs = [gen_program(args.seed, i, args.cases) for i in range(args.n)]
    kinds = {}
    for _, _, _, _, cases in programs:
        for c in cases:
            kinds[c.kind] = kinds.get(c.kind, 0) + 1

    failures = 0
    total_rw = total_rt = 0

    def work(item):
        i, (src, expected, rw, rt, cases) = item
        problems, on, off = check_program(tur, build, src, expected, rw, rt)
        return i, src, problems, on

    # Report each program the moment it finishes (as_completed, not map
    # order), flushed: under a pipe or ctest nothing showed until exit.
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(work, item) for item in enumerate(programs)]
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            i, src, problems, on = fut.result()
            done += 1
            if on.stats:
                total_rw += on.stats[1]
                total_rt += on.stats[2]
            if problems:
                failures += 1
                print(f"FAIL program {i} (seed {args.seed}):", flush=True)
                for p in problems:
                    print("  " + p, flush=True)
                if args.save_dir:
                    fn = os.path.join(args.save_dir, f"seed{args.seed}-{i}.tur")
                    with open(fn, "w") as f:
                        f.write(src)
                    print(f"  saved {fn}", flush=True)
            elif done % 10 == 0 or done == args.n:
                print(f"  {done}/{args.n} programs done", flush=True)

    print(f"regions-fuzz-src: {args.n} programs x {args.cases} cases, seed {args.seed}: "
          f"{args.n - failures} passed, {failures} failed; "
          f"runtime rewinds={total_rw} retires={total_rt}")
    print("  case kinds: " + ", ".join(f"{k}={v}" for k, v in sorted(kinds.items())))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

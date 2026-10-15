#!/usr/bin/env python3
"""WP2: where a compiled Turmeric program spends its cycles, by subsystem.

performance-audit-plan WP2's first bullet. For each program: `tur build` it
(default flags), sample it with `perf record` (user and kernel, no call
graphs: self time is what a partition needs), and bucket every sampled symbol
with profiling-plan section 4.3's prefix table, plus three buckets this audit
needed -- `region` (tur_region_, reg_, arena_), `tls` (the split preamble's
tur_tls_*_ptr accessors) and `kernel` (perf's [k] frames: page faults,
mmap/brk, exit teardown). Turmeric functions are split into `user` and
`stdlib` by name: symbols are demangled with `tur demangle`, and the program's
own TUR_CPS_CENSUS lines say which file defines each name.

Usage:
  python3 benchmarks/audit/subsys-profile.py --tur ./build-release/tur [--out DIR] prog.tur ...

Prints one row per program (percent of samples per subsystem) and writes
DIR/subsys.csv.
"""

import argparse
import collections
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# profiling-plan 4.3, in match order; first hit wins.
PREFIX = [
    (("tur_hamt_", "tur_map_", "node_insert", "node_has", "node_find"), "hamt"),
    (("tur_rc_", "tur_ref_", "__tur_rc_", "rc_cb_", "rc_dec", "rc_inc", "rc_free"), "rc"),
    (("gc_", "tur_gc_"), "gc"),
    (("tur_stm_", "tur_tvar_"), "stm"),
    (("tur_kont_", "tur_step_", "__tur_cps_", "dk_", "__dk_", "tur_cont", "tur_tramp"), "cps"),
    (("tur_ffi_call_",), "ffi"),
    (("tur_string_", "tur_sb_", "tur_slice_"), "string"),
    (("tur_trail_", "tur_uf_", "tur_bt_"), "trail"),
    (("tur_region_", "reg_", "arena_"), "region"),
    (("tur_tls_",), "tls"),
    (("tur_arena_",), "arena"),
]
LIBC = re.compile(r"^(malloc|free|cfree|calloc|realloc|_int_|__libc_|mem|str|tcache|"
                  r"unlink_chunk|sysmalloc|malloc_consolidate|_dl_|__memcpy|__memset|__strlen)")
BUCKETS = ["user", "stdlib", "hamt", "rc", "gc", "stm", "cps", "ffi", "string", "trail",
           "region", "tls", "arena", "libc", "kernel", "native"]


def classify(sym, user, stdlib, demangled):
    if sym.startswith("[k]"):
        return "kernel"
    name = sym[4:] if sym.startswith("[.] ") else sym
    for prefixes, bucket in PREFIX:
        if name.startswith(prefixes):
            return bucket
    if LIBC.match(name):
        return "libc"
    d = demangled.get(name, name)
    base = d.split("__spec__")[0]
    if d in user or base in user or name in user:
        return "user"
    if d in stdlib or base in stdlib or name in stdlib or name.startswith(("__inst_", "__spec")):
        return "stdlib"
    return "native"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tur", default=os.environ.get("TUR", "./build-release/tur"))
    ap.add_argument("--perf", default="")
    ap.add_argument("--freq", type=int, default=4999)
    ap.add_argument("--out", default="")
    ap.add_argument("programs", nargs="+")
    args = ap.parse_args()
    tur = os.path.abspath(args.tur)
    perf = args.perf or next(iter(sorted(
        p for p in (os.path.join("/usr/lib/linux-tools", d, "perf")
                    for d in (os.listdir("/usr/lib/linux-tools") if os.path.isdir("/usr/lib/linux-tools") else []))
        if os.path.exists(p))), shutil.which("perf"))
    rows = []
    with tempfile.TemporaryDirectory() as w:
        for prog in args.programs:
            exe = os.path.join(w, "prog")
            if subprocess.run([tur, "build", prog, "-o", exe], cwd=ROOT,
                              capture_output=True).returncode != 0:
                print(f"{prog}: build failed", file=sys.stderr)
                continue
            census = subprocess.run([tur, "emit-c", prog], cwd=ROOT, capture_output=True,
                                    env=dict(os.environ, TUR_CPS_CENSUS="1")).stderr.decode()
            user, stdlib = set(), set()
            for line in census.splitlines():
                p = line.split(" ", 6)
                if len(p) == 7 and p[0] == "cps-census":
                    (stdlib if "stdlib/" in p[5] else user).add(p[6])
            data = os.path.join(w, "perf.data")
            subprocess.run([perf, "record", "-q", "-F", str(args.freq), "-o", data, exe],
                           cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            rep = subprocess.run([perf, "report", "-i", data, "--stdio", "--no-children",
                                  "--sort", "symbol", "-F", "overhead,sym"],
                                 capture_output=True, text=True).stdout
            syms = []
            for line in rep.splitlines():
                m = re.match(r"\s*([\d.]+)%\s+(\[[k.]\]\s+\S+)", line)
                if m:
                    syms.append((float(m.group(1)), m.group(2).replace("  ", " ")))
            names = sorted({s[4:] for _, s in syms if s.startswith("[.]")})
            dem = subprocess.run([tur, "demangle"], input="\n".join(names), capture_output=True,
                                 text=True).stdout.splitlines()
            demangled = dict(zip(names, dem)) if len(dem) == len(names) else {}
            share = collections.Counter()
            top = collections.defaultdict(list)
            for pct, s in syms:
                b = classify(s.replace("[.]  ", "[.] "), user, stdlib, demangled)
                share[b] += pct
                top[b].append((pct, s))
            row = {"program": prog, **{b: round(share[b], 1) for b in BUCKETS}}
            row["top_native"] = "; ".join(f"{s} {p:.1f}" for p, s in sorted(top["native"], reverse=True)[:3])
            rows.append(row)
            print(f"{prog:58} " + " ".join(f"{b} {share[b]:.0f}" for b in BUCKETS if share[b] >= 1),
                  flush=True)
    if args.out and rows:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "subsys.csv"), "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
            wr.writeheader()
            wr.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())

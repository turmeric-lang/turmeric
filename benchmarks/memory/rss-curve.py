#!/usr/bin/env python3
"""WP3: how a program's resident set grows while it runs.

Runs a command, samples its VmRSS from /proc/<pid>/status every --interval
seconds until it exits, and writes the series as CSV. The summary line is what
a table needs: wall time, RSS at the first sample, peak (VmHWM at exit), and
the growth per iteration when --iterations says how many the program ran.

Usage:
  python3 benchmarks/memory/rss-curve.py --out curve.csv [--iterations N] -- ./prog args...

Linux only (/proc).
"""

import argparse
import csv
import os
import subprocess
import sys
import time


def rss_kib(pid, field="VmRSS"):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith(field + ":"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=float, default=0.1)
    ap.add_argument("--iterations", type=int, default=0)
    ap.add_argument("--label", default="")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd:
        sys.exit("rss-curve: no command")
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    samples, hwm = [], 0
    while p.poll() is None:
        r = rss_kib(p.pid)
        h = rss_kib(p.pid, "VmHWM")
        if r is not None:
            samples.append((round(time.monotonic() - t0, 3), r))
        if h:
            hwm = max(hwm, h)
        time.sleep(args.interval)
    wall = time.monotonic() - t0
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["t_s", "rss_kib"])
        w.writerows(samples)
    first = samples[0][1] if samples else 0
    last = samples[-1][1] if samples else 0
    peak = max([hwm] + [s[1] for s in samples])
    per = ""
    if args.iterations and samples:
        per = f" growth/iter={((last - first) * 1024) / args.iterations:.1f} B"
    print(f"{args.label or ' '.join(cmd)}: rc={p.returncode} wall={wall:.2f}s samples={len(samples)} "
          f"first={first / 1024:.1f}MiB last={last / 1024:.1f}MiB peak={peak / 1024:.1f}MiB{per}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

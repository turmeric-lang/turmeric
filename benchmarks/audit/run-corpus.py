#!/usr/bin/env python3
"""Run the performance-audit corpus and record one row per (program, engine, phase).

WP0 of docs/upcoming/performance-audit-plan.md.  Reads benchmarks/audit/corpus.txt
(the committed corpus list) and, for every program on every engine it names,
measures what the plan's baseline asks for:

  wall time    best and median of --reps runs (perf_counter around wait4)
  user / sys   CPU of the best-wall run, from the child's rusage -- which on
               Linux folds in every descendant the child reaped, so a
               `tur build` row includes the cc it ran
  peak RSS     max ru_maxrss over the reps (KiB; macOS bytes are converted)
  bytes        emitted C size for the `emit` phase, binary size for `build`

Phases per engine:

  cc      emit  (`tur emit-c`)   build (`tur build -o`)   run (the binary)
  jit     run   (`tur jit <file>`, compile + run together)
  interp  run   (`tur --interpret <file>`)

Every run's stdout is checked against the program's pinned answer before its
number counts, the way benchmarks/run-triangle.sh refuses a program whose
engines disagree: an experiment must not look fast by doing less.  The pin for
a fixture is its own expected.stdout; for everything else it lives under
benchmarks/audit/expected/.  `--pin` writes missing pins (two runs, which must
agree), and never overwrites one unless `--repin`.

Self-timing programs print nanosecond columns that cannot be pinned verbatim.
Their corpus line says `cols=1,2,5`: only those comma-separated columns of each
output line are compared (lines without a comma are compared whole).

It refuses a sanitized `tur` (the Debug build): every number would be ASan's
shadow-memory bookkeeping.  The probe reads symbol tables with `nm`, the way
the driver's own libturi autodetect does (src/main.c) -- a plain grep of the
binary finds the string "__asan_init" in a Release tur too, because the driver
carries that nm command line as a literal.

Usage:
  python3 benchmarks/audit/run-corpus.py --tur ./build-release/tur
  python3 benchmarks/audit/run-corpus.py --only triangle --engines cc --reps 5
  python3 benchmarks/audit/run-corpus.py --pin --reps 1      # write missing pins

Writes <out>/rows.csv, <out>/meta.json and <out>/summary.md (default out:
benchmarks/output/audit/<UTC timestamp>/, which is gitignored).
"""

import argparse
import csv
import datetime
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CORPUS = os.path.join(ROOT, "benchmarks", "audit", "corpus.txt")
PIN_DIR = os.path.join(ROOT, "benchmarks", "audit", "expected")
ENGINES = ("cc", "jit", "interp")
SANITIZER_SYMBOLS = ("__asan_init", "__tsan_init", "__ubsan_handle_")


# --------------------------------------------------------------------------
# Corpus
# --------------------------------------------------------------------------

class Program:
    def __init__(self, path, engines, check, timeout, note, lineno):
        self.path = path
        self.engines = engines
        self.check = check
        self.timeout = timeout
        self.note = note
        self.lineno = lineno

    @property
    def pin_path(self):
        d = os.path.dirname(self.path)
        if d.startswith("tests/fixtures/"):
            return os.path.join(ROOT, d, "expected.stdout")
        key = re.sub(r"\.tur(\.sweet)?$", "", self.path).replace("/", "__")
        return os.path.join(PIN_DIR, key + ".stdout")


def read_corpus(path):
    """One program per line: <path> <engines> <check> [timeout=<s>] [# note]."""
    progs = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line, _, note = raw.partition("#")
            fields = line.split()
            if not fields:
                continue
            if len(fields) < 3:
                sys.exit(f"{path}:{lineno}: want <path> <engines> <check>, got {raw.strip()!r}")
            prog_path, engines, check = fields[:3]
            timeout = 120.0
            for extra in fields[3:]:
                k, _, v = extra.partition("=")
                if k == "timeout":
                    timeout = float(v)
                else:
                    sys.exit(f"{path}:{lineno}: unknown option {extra!r}")
            engs = tuple(e for e in engines.split(",") if e)
            bad = [e for e in engs if e not in ENGINES]
            if bad:
                sys.exit(f"{path}:{lineno}: unknown engine(s) {bad}")
            if check != "exact" and not re.fullmatch(r"cols=\d+(,\d+)*", check):
                sys.exit(f"{path}:{lineno}: check must be `exact` or `cols=N,M,...`")
            if not os.path.exists(os.path.join(ROOT, prog_path)):
                sys.exit(f"{path}:{lineno}: no such program {prog_path}")
            progs.append(Program(prog_path, engs, check, timeout, note.strip(), lineno))
    return progs


def masked(text, check):
    if check == "exact":
        return text
    keep = [int(c) - 1 for c in check[len("cols="):].split(",")]
    out = []
    for line in text.splitlines():
        if "," in line:
            cells = line.split(",")
            line = ",".join(cells[i] if i < len(cells) else "" for i in keep)
        out.append(line)
    return "\n".join(out) + ("\n" if text.endswith("\n") else "")


# --------------------------------------------------------------------------
# Measurement
# --------------------------------------------------------------------------

def maxrss_kib(ru):
    # ru_maxrss is KiB on Linux and bytes on macOS.
    return ru.ru_maxrss // 1024 if sys.platform == "darwin" else ru.ru_maxrss


def run_once(argv, timeout, stdout_path=None):
    """Run argv from the repo root; return (rc, wall_s, rusage, stdout_bytes).

    rc is None on timeout.  stdout goes to a file when stdout_path is given (so
    a large output does not sit in a pipe buffer while the clock runs)."""
    # A blocking wait4 plus a kill timer, not a WNOHANG poll: a poll's sleep is
    # wall-clock error, and the triangle programs run in single milliseconds.
    out = open(stdout_path, "wb") if stdout_path else subprocess.DEVNULL
    timed_out = threading.Event()
    try:
        t0 = time.perf_counter()
        p = subprocess.Popen(argv, cwd=ROOT, stdin=subprocess.DEVNULL,
                             stdout=out, stderr=subprocess.DEVNULL)

        def kill():
            timed_out.set()
            p.kill()
        timer = threading.Timer(timeout, kill)
        timer.start()
        _, status, ru = os.wait4(p.pid, 0)
        wall = time.perf_counter() - t0
        timer.cancel()
        p.returncode = os.waitstatus_to_exitcode(status)
    finally:
        if stdout_path:
            out.close()
    if timed_out.is_set():
        return None, wall, ru, b""
    data = open(stdout_path, "rb").read() if stdout_path else b""
    return p.returncode, wall, ru, data


def measure(argv, reps, timeout, scratch, check=None, pin=None):
    """Best-of-reps.  Returns a dict of metrics plus status and the stdout of
    the first run (used for pinning)."""
    walls, best = [], None
    peak = 0
    first_out = None
    status = "ok"
    for i in range(reps):
        rc, wall, ru, data = run_once(argv, timeout, os.path.join(scratch, "stdout"))
        peak = max(peak, maxrss_kib(ru))
        if rc is None:
            status = f"timeout>{timeout:g}s"
            walls.append(wall)
            break
        if rc != 0:
            status = f"rc={rc}"
            walls.append(wall)
            break
        text = data.decode("utf-8", "replace")
        if first_out is None:
            first_out = text
        if pin is not None and masked(text, check) != masked(pin, check):
            status = "MISMATCH"
            walls.append(wall)
            break
        walls.append(wall)
        if best is None or wall < best[0]:
            best = (wall, ru)
    row = {
        "reps": len(walls),
        "wall_ms_min": round(min(walls) * 1000, 2),
        "wall_ms_median": round(statistics.median(walls) * 1000, 2),
        "user_ms": round(best[1].ru_utime * 1000, 2) if best else "",
        "sys_ms": round(best[1].ru_stime * 1000, 2) if best else "",
        "maxrss_kib": peak,
        "status": status,
    }
    return row, first_out


# --------------------------------------------------------------------------
# Probes and header
# --------------------------------------------------------------------------

def sanitizer_symbols(tur):
    nm = shutil.which("nm")
    if not nm:
        return None
    found = set()
    for flag in ([], ["-D"]):
        r = subprocess.run([nm] + flag + [tur], capture_output=True, text=True)
        for sym in SANITIZER_SYMBOLS:
            if sym in r.stdout:
                found.add(sym)
    return sorted(found)


def has_jit(tur):
    r = subprocess.run([tur, "jit"], capture_output=True, text=True)
    return "usage: tur jit" in (r.stdout + r.stderr)


def first_line(argv):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=30)
        return (r.stdout or r.stderr).strip().splitlines()[0]
    except Exception:
        return ""


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def stamp(path):
    st = os.stat(path)
    return (st.st_size, st.st_mtime_ns)


def header(tur, args):
    git = lambda *a: subprocess.run(["git", "-C", ROOT] + list(a),
                                    capture_output=True, text=True).stdout.strip()
    return {
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "commit": git("rev-parse", "HEAD"),
        "dirty": bool(git("status", "--porcelain", "--untracked-files=no")),
        "tur": os.path.relpath(tur, ROOT),
        "tur_version": first_line([tur, "--version"]),
        "cc": first_line([os.environ.get("CC", "cc"), "--version"]),
        "host": platform.node(),
        "kernel": platform.release(),
        "cpu": cpu_model(),
        "nproc": os.cpu_count(),
        "reps": args.reps,
        "cooldown_s": args.cooldown,
        "env": {k: os.environ[k] for k in ("CC", "TUR_CC_FLAGS", "TUR_BUILD_DIR")
                if k in os.environ},
    }


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tur", default=os.environ.get("TUR", "./build-release/tur"))
    ap.add_argument("--corpus", default=CORPUS)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--engines", default=",".join(ENGINES))
    ap.add_argument("--only", default="", help="regex over program paths")
    ap.add_argument("--out", default="")
    ap.add_argument("--cooldown", type=float, default=0.0,
                    help="seconds to sleep between programs (thermal)")
    ap.add_argument("--pin", action="store_true", help="write missing pins")
    ap.add_argument("--repin", action="store_true", help="overwrite existing pins")
    ap.add_argument("--allow-sanitized", action="store_true")
    args = ap.parse_args()

    tur = os.path.abspath(os.path.join(ROOT, args.tur) if not os.path.isabs(args.tur) else args.tur)
    if not os.access(tur, os.X_OK):
        sys.exit(f"run-corpus: no tur at {tur}")
    san = sanitizer_symbols(tur)
    if san is None:
        print("run-corpus: WARNING -- no `nm`, cannot check for a sanitized tur", file=sys.stderr)
    elif san and not args.allow_sanitized:
        sys.exit(f"run-corpus: {args.tur} is sanitized ({', '.join(san)}); its numbers would be\n"
                 "  sanitizer overhead.  Build a Release tur:\n"
                 "    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON\n"
                 "    cmake --build build-release -j\n"
                 "  or pass --allow-sanitized for rows you will not check in.")
    if "TUR_CC_FLAGS" in os.environ:
        print("run-corpus: WARNING -- TUR_CC_FLAGS is set; cc rows measure it, not the "
              "driver's defaults", file=sys.stderr)

    want = set(args.engines.split(","))
    jit_ok = has_jit(tur)
    if "jit" in want and not jit_ok:
        print("run-corpus: this tur has no JIT engine (configure -DTUR_JIT=ON); "
              "jit rows are skipped", file=sys.stderr)

    progs = [p for p in read_corpus(args.corpus) if re.search(args.only, p.path)]
    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = os.path.abspath(args.out or os.path.join(ROOT, "benchmarks", "output", "audit", ts))
    scratch = os.path.join(out, "scratch")
    os.makedirs(os.path.join(scratch, "bin"), exist_ok=True)
    meta = header(tur, args)
    tur_stamp = stamp(tur)

    fields = ["program", "engine", "phase", "status", "reps", "wall_ms_min",
              "wall_ms_median", "user_ms", "sys_ms", "maxrss_kib", "bytes"]
    rows = []
    csv_path = os.path.join(out, "rows.csv")
    with open(csv_path, "w", newline="") as fcsv:
        w = csv.DictWriter(fcsv, fieldnames=fields)
        w.writeheader()

        def emit(prog, engine, phase, row, nbytes=""):
            r = dict(program=prog.path, engine=engine, phase=phase, bytes=nbytes, **row)
            rows.append(r)
            w.writerow(r)
            fcsv.flush()
            print(f"  {engine:6} {phase:5} {r['status']:10} min {r['wall_ms_min']:>9} ms"
                  f"  rss {r['maxrss_kib']:>7} KiB  {nbytes}", flush=True)

        for n, prog in enumerate(progs):
            if n and args.cooldown:
                time.sleep(args.cooldown)
            print(f"[{n + 1}/{len(progs)}] {prog.path}", flush=True)
            pin = None
            if os.path.exists(prog.pin_path) and not args.repin:
                pin = open(prog.pin_path, encoding="utf-8").read()
            elif not args.pin:
                print(f"  no pin at {os.path.relpath(prog.pin_path, ROOT)} (run with --pin)")
            exe = os.path.join(scratch, "bin", re.sub(r"[^A-Za-z0-9_.-]", "_", prog.path))
            src = os.path.join(ROOT, prog.path)
            outs = {}
            for engine in prog.engines:
                if engine not in want or (engine == "jit" and not jit_ok):
                    continue
                if engine == "cc":
                    row, _ = measure([tur, "emit-c", src], 1, prog.timeout, scratch)
                    emit(prog, "cc", "emit", row,
                         os.path.getsize(os.path.join(scratch, "stdout"))
                         if row["status"] == "ok" else "")
                    row, _ = measure([tur, "build", src, "-o", exe], args.reps, prog.timeout, scratch)
                    emit(prog, "cc", "build", row,
                         os.path.getsize(exe) if row["status"] == "ok" else "")
                    if row["status"] != "ok":
                        continue
                    argv = [exe]
                elif engine == "jit":
                    argv = [tur, "jit", src]
                else:
                    argv = [tur, "--interpret", src]
                row, first = measure(argv, args.reps, prog.timeout, scratch, prog.check, pin)
                emit(prog, engine, "run", row)
                if row["status"] == "ok" and first is not None:
                    outs[engine] = first
            # Pinning: every engine that ran must agree, and with a second run.
            if (args.pin or args.repin) and outs and (pin is None or args.repin):
                answers = {masked(o, prog.check) for o in outs.values()}
                ref_engine = "cc" if "cc" in outs else next(iter(outs))
                ref = outs[ref_engine]
                if len(answers) != 1:
                    print(f"  NOT PINNED: engines disagree ({', '.join(outs)})")
                elif prog.pin_path.startswith(os.path.join(ROOT, "tests", "fixtures")):
                    print("  NOT PINNED: fixture pins are the fixture's own expected.stdout")
                else:
                    again_argv = [exe] if ref_engine == "cc" else (
                        [tur, "jit", src] if ref_engine == "jit" else [tur, "--interpret", src])
                    rc, _, _, data = run_once(again_argv, prog.timeout,
                                              os.path.join(scratch, "stdout"))
                    if rc != 0 or masked(data.decode("utf-8", "replace"), prog.check) != masked(ref, prog.check):
                        print("  NOT PINNED: a second run disagreed with the first (use cols=)")
                    else:
                        os.makedirs(PIN_DIR, exist_ok=True)
                        with open(prog.pin_path, "w", encoding="utf-8", newline="\n") as f:
                            f.write(ref)
                        print(f"  pinned {os.path.relpath(prog.pin_path, ROOT)}")

    if stamp(tur) != tur_stamp:
        print(f"run-corpus: WARNING -- {args.tur} changed while this run was in progress; "
              "these rows mix two compilers and are not a result", file=sys.stderr)
        meta["tur_changed_mid_run"] = True
    with open(os.path.join(out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
    write_summary(os.path.join(out, "summary.md"), meta, rows)
    shutil.rmtree(scratch, ignore_errors=True)
    bad = [r for r in rows if r["status"] != "ok"]
    print(f"run-corpus: {len(rows)} rows, {len(bad)} not ok -> {os.path.relpath(out, ROOT)}")
    return 1 if bad else 0


def write_summary(path, meta, rows):
    """One markdown line per program: the cc build/run split beside jit and interp."""
    by = {}
    for r in rows:
        by.setdefault(r["program"], {})[(r["engine"], r["phase"])] = r

    def cell(r, key="wall_ms_min"):
        if r is None:
            return ""
        return f"{r[key]}" if r["status"] == "ok" else r["status"]

    with open(path, "w") as f:
        f.write(f"commit `{meta['commit'][:12]}`{' (dirty)' if meta['dirty'] else ''}, "
                f"{meta['tur_version']}, {meta['cpu']}, nproc {meta['nproc']}, "
                f"{meta['cc']}, best of {meta['reps']}, {meta['date_utc']}\n\n")
        f.write("| program | emit-c ms | C bytes | cc build ms | binary bytes | cc run ms "
                "| run RSS KiB | jit ms | interp ms |\n")
        f.write("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for prog, ph in by.items():
            e, b, c = ph.get(("cc", "emit")), ph.get(("cc", "build")), ph.get(("cc", "run"))
            f.write(f"| {prog} | {cell(e)} | {e['bytes'] if e else ''} | {cell(b)} | "
                    f"{b['bytes'] if b else ''} | {cell(c)} | {cell(c, 'maxrss_kib')} | "
                    f"{cell(ph.get(('jit', 'run')))} | {cell(ph.get(('interp', 'run')))} |\n")


if __name__ == "__main__":
    sys.exit(main())

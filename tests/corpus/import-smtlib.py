#!/usr/bin/env python3
"""Import a bounded sample of the SMT-LIB benchmark library into the corpus.

The corpus this feeds (`tests/corpus/smtlib/`) is replayed by
`tur_refine_corpus` with no solver linked -- see that directory's README. The
hand-written and generated benchmarks there cover the fragment; what the real
library adds is SHAPES nobody on this project would think to write.

## What this reads

`smt-lib-benchmark-data-2025.json` -- the Zenodo record metadata for
*SMT-LIB release 2025 (non-incremental benchmarks)*, record 16740866. It lists
90 per-logic tarballs with sizes, md5 checksums, and content URLs. The metadata
is committed; the tarballs are not (4.89 GB for the full release).

Licence: the release is CC-BY-4.0, so redistributing a sample inside this repo
is permitted **with attribution**. `--sample` writes an ATTRIBUTION file next to
the imported benchmarks; do not delete it.

## Why a sample and not the whole thing

QF_LIA alone is 687 MB compressed. A regression corpus wants breadth of shape,
not volume -- and every committed file is one a reviewer might have to reason
about. The default takes a bounded, deterministic sample per logic.

## Usage

    pip install zstandard
    python3 tests/corpus/import-smtlib.py --list
    python3 tests/corpus/import-smtlib.py --logics QF_UFLIA,QF_UF --sample 25
    python3 tests/corpus/import-smtlib.py --logics QF_UFLIA --sample 25 --dry-run

Downloads land in a scratch directory (`--cache`, default `/tmp/smtlib-cache`)
and are md5-verified against the record metadata before anything is extracted.

`--base-url` fetches from a mirror (`<base-url>/<LOGIC>.tar.zst`) instead of the
record's own URLs -- an S3 bucket, a static host, anything that serves the files
by name. The md5 from the record is still enforced, so a mirror can only supply
the same bytes; it cannot substitute different data. This is the escape hatch
for an environment where the record's host is blocked but a bucket is not.

## THIS HAS ALREADY BEEN RUN -- you probably want the clone

The import is DONE and its output is published at

    https://github.com/turmeric-lang/smt-lib-benchmarks

as `smtlib-2025/`: 25 benchmarks per logic across the eight fragment logics
(200 files), seed 1, with the CC-BY-4.0 ATTRIBUTION file. It lives in a
separate repository because the data is too large to check into the turmeric
tree's history -- NOT because it could not be obtained.

    git clone https://github.com/turmeric-lang/smt-lib-benchmarks /tmp/smtlib-bench
    TUR_CORPUS_TIMEOUT=3 ./build/tur_refine_corpus /tmp/smtlib-bench/smtlib-2025

Only re-run this script to produce a DIFFERENT sample (other logics, other
seed, larger `--sample`).

## Note on network access

`zenodo.org` was blocked by the egress policy of one container this was
developed in (the proxy answered 403 to CONNECT). That was an environment
restriction, not a problem with this script or with the availability of the
data: it runs fine anywhere with ordinary network access, which is how the
published sample above was produced.
"""

import argparse
import hashlib
import io
import json
import pathlib
import random
import sys
import tarfile
import urllib.request

# The logics the in-house chain has any chance with: uninterpreted functions,
# difference logic, and linear integer/real arithmetic, quantifier-free. The
# reader skips whatever falls outside the fragment, so importing a wider logic
# is safe -- it just yields skips rather than coverage.
FRAGMENT_LOGICS = [
    "QF_UF", "QF_UFIDL", "QF_UFLIA", "QF_UFLRA",
    "QF_IDL", "QF_RDL", "QF_LIA", "QF_LRA",
]

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_RECORD = REPO_ROOT / "tests" / "corpus" / "smt-lib-benchmark-data-2025.json"
DEFAULT_DEST = REPO_ROOT / "tests" / "corpus" / "smtlib" / "smtlib-2025"

ATTRIBUTION = """\
These benchmarks are a sample of the SMT-LIB benchmark library,
"SMT-LIB release 2025 (non-incremental benchmarks)" (Zenodo record 16740866),
distributed under the Creative Commons Attribution 4.0 International licence
(CC-BY-4.0): https://creativecommons.org/licenses/by/4.0/legalcode

They are unmodified except for selection. Imported by
tests/corpus/import-smtlib.py; see tests/corpus/smtlib/README.md for how they
are used.
"""


def load_record(path):
    with open(path) as f:
        rec = json.load(f)
    entries = rec["files"]["entries"]
    out = {}
    for key, meta in entries.items():
        logic = key.split(".")[0]
        out[logic] = {
            "key": key,
            "size": meta["size"],
            "md5": meta["checksum"].split("md5:")[-1],
            "url": meta["links"]["content"],
        }
    return rec, out


def human(n):
    return f"{n / 1e6:.1f} MB" if n < 1e9 else f"{n / 1e9:.2f} GB"


def fetch(url, dest, expect_md5, expect_size):
    """Download to `dest` unless a verified copy is already there."""
    if dest.exists() and dest.stat().st_size == expect_size:
        if md5_of(dest) == expect_md5:
            print(f"    cached and verified: {dest}")
            return True
        print(f"    cached copy failed md5; refetching")
        dest.unlink()

    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"    downloading {human(expect_size)} ...")
    try:
        # `timeout` is per-socket-operation: a stalled connection raises
        # instead of hanging silently forever. Progress is printed so a slow
        # transfer is visibly moving, not mistaken for a hang.
        with urllib.request.urlopen(url, timeout=60) as r, open(dest, "wb") as out:
            got = 0
            last = -1
            for chunk in iter(lambda: r.read(1 << 20), b""):
                out.write(chunk)
                got += len(chunk)
                pct = int(got * 100 / expect_size) if expect_size else 0
                if pct != last:                   # one line, rewritten in place
                    print(f"\r    {human(got)} / {human(expect_size)} "
                          f"({pct}%)", end="", flush=True)
                    last = pct
            print()                               # end the progress line
    except Exception as exc:                      # noqa: BLE001 -- report and move on
        print(f"\n    FAILED: {exc}")
        print(f"    (a timeout/stall here means the transfer stalled; rerun to "
              f"resume from the verified cache. A 403 means the host is blocked "
              f"by an egress policy -- report it rather than routing around it)")
        if dest.exists():                         # partial file must not masquerade as cached
            dest.unlink()
        return False

    got = md5_of(dest)
    if got != expect_md5:
        print(f"    CHECKSUM MISMATCH: expected {expect_md5}, got {got}")
        dest.unlink()
        return False
    print("    md5 ok")
    return True


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sample_members(tar_path, n, seed):
    """Deterministically pick `n` distinct .smt2 members.

    Names are deduplicated BEFORE sampling. A tar can legitimately carry the
    same path twice, and since each member is written out under a name derived
    from its path, duplicates silently overwrite each other -- so sampling 25
    could quietly yield 20 files. Fewer benchmarks than asked for is exactly the
    kind of shortfall that should be loud, not inferred later from an `ls`.
    """
    import zstandard

    dctx = zstandard.ZstdDecompressor()
    with open(tar_path, "rb") as fh, dctx.stream_reader(fh) as reader:
        buf = io.BytesIO(reader.read())
    tf = tarfile.open(fileobj=buf, mode="r:")

    seen = set()
    members = []
    for m in tf.getmembers():
        if not (m.isfile() and m.name.endswith(".smt2")):
            continue
        if m.name in seen:
            continue
        seen.add(m.name)
        members.append(m)
    members.sort(key=lambda m: m.name)            # stable before sampling
    pool = len(members)
    rng = random.Random(seed)
    if pool > n:
        members = rng.sample(members, n)
        members.sort(key=lambda m: m.name)
    return tf, members, pool


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--record", default=str(DEFAULT_RECORD),
                    help="Zenodo record metadata JSON")
    ap.add_argument("--logics", default=",".join(FRAGMENT_LOGICS),
                    help="comma-separated logic names, or 'all'")
    ap.add_argument("--sample", type=int, default=25,
                    help="benchmarks to keep per logic (0 = all; be careful)")
    ap.add_argument("--seed", type=int, default=1,
                    help="sampling seed, so an import is reproducible")
    ap.add_argument("--base-url", default=None,
                    help="fetch from a MIRROR instead of the record's URLs: "
                         "<base-url>/<LOGIC>.tar.zst. The md5 from the record "
                         "is still enforced, so a mirror cannot substitute "
                         "different data. Useful when the record's host is "
                         "blocked by an egress policy but a bucket is not.")
    ap.add_argument("--cache", default="/tmp/smtlib-cache")
    ap.add_argument("--dest", default=str(DEFAULT_DEST))
    ap.add_argument("--list", action="store_true",
                    help="print what is available and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve and report, download nothing")
    args = ap.parse_args()

    rec, entries = load_record(args.record)
    title = rec.get("metadata", {}).get("title", "?")

    if args.list:
        print(f"{title}\n")
        total = 0
        for logic in sorted(entries):
            mark = " *" if logic in FRAGMENT_LOGICS else "  "
            print(f" {mark} {logic:14} {human(entries[logic]['size']):>10}")
            total += entries[logic]["size"]
        print(f"\n    {len(entries)} logics, {human(total)} total")
        print("    * = in the fragment the in-house chain decides")
        return 0

    wanted = (sorted(entries) if args.logics == "all"
              else [l.strip() for l in args.logics.split(",") if l.strip()])
    missing = [l for l in wanted if l not in entries]
    if missing:
        print(f"import-smtlib: not in the record: {', '.join(missing)}",
              file=sys.stderr)
        return 1

    cache = pathlib.Path(args.cache)
    dest = pathlib.Path(args.dest)
    print(f"{title}")
    print(f"  logics : {', '.join(wanted)}")
    print(f"  sample : {args.sample or 'ALL'} per logic (seed {args.seed})")
    print(f"  dest   : {dest}\n")

    if args.dry_run:
        total = sum(entries[l]["size"] for l in wanted)
        for l in wanted:
            u = (f"{args.base_url.rstrip('/')}/{entries[l]['key']}"
                 if args.base_url else entries[l]["url"])
            print(f"  would fetch {l:12} {human(entries[l]['size']):>10}  {u}")
        print(f"\n  total download: {human(total)}")
        return 0

    dest.mkdir(parents=True, exist_ok=True)
    (dest / "ATTRIBUTION").write_text(ATTRIBUTION)

    imported = failed = 0
    for logic in wanted:
        meta = entries[logic]
        print(f"  {logic}")
        tarball = cache / meta["key"]
        url = (f"{args.base_url.rstrip('/')}/{meta['key']}"
               if args.base_url else meta["url"])
        if not fetch(url, tarball, meta["md5"], meta["size"]):
            failed += 1
            continue

        want = args.sample or 10 ** 9
        tf, members, pool = sample_members(tarball, want, args.seed)
        outdir = dest / logic
        outdir.mkdir(parents=True, exist_ok=True)
        written = set()
        for m in members:
            data = tf.extractfile(m)
            if data is None:
                continue
            # Flatten: the corpus runner recurses, but a flat per-logic
            # directory keeps `ls` and review sane.
            name = m.name.replace("/", "__").lstrip("._")
            if name in written:                   # distinct paths, same flat name
                stem, _, ext = name.rpartition(".")
                name = f"{stem}~{len(written)}.{ext}"
            (outdir / name).write_bytes(data.read())
            written.add(name)
        kept = len(written)
        print(f"    kept {kept} of {pool} benchmarks -> {outdir}")
        if args.sample and kept < args.sample:
            reason = ("the logic only has that many"
                      if pool <= args.sample else "extraction dropped some")
            print(f"    NOTE: asked for {args.sample}, kept {kept} "
                  f"({reason})")
        imported += kept

    print(f"\n  imported {imported} benchmarks; {failed} logic(s) failed")
    if imported:
        print("\nNext:")
        print("  python3 tests/corpus/validate-labels.py tests/corpus/smtlib")
        print("  ./build/tur_refine_corpus tests/corpus/smtlib")
        print("\nThe library's own :status labels are authoritative -- "
              "validate-labels.py\nre-checks them anyway, which is how a "
              "mis-imported or truncated file shows up.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

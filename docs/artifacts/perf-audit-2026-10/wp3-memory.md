# WP3 -- memory and allocation

[performance-audit-plan](../../upcoming/performance-audit-plan.md) WP3: turn the
memory-usage guide's cost-table snapshot into a harness, find the
per-iteration allocations a user would not expect, and record the long-run,
build-time and leak-residue numbers (M-1 .. M-5). Box and method are WP0's
([README.md](README.md)): NUC 9 Pro, WSL2 Ubuntu 24.04, gcc 13.3, a Release
`tur` built from `main` @ `83f7857ae`, valgrind 3.22. Everything here counts
bytes, not time, so none of it needed an idle box.

<!-- PROBES -->

## Long-run RSS curves

`benchmarks/memory/rss-curve.py` samples a running program's `VmRSS` every
100 ms (the plan asked for once a second; these programs finish in 1-12 s).
Four shapes at two sizes each, from fixtures with one loop count raised. Raw
series: [wp3-curves/](wp3-curves/).

| program | iterations | wall | peak RSS | RSS at 0 / 25 / 50 / 75 / 100% of the run (MiB) | growth |
| --- | ---: | ---: | ---: | --- | --- |
| `cps-tramp-resume-deep-1m` (tail resume; the control) | 100M | 12.3 s | **1.0 MiB** | 0.1 / 1.0 / 1.0 / 1.0 / 1.0 | none |
| `effect-nontail-resume-under-outer` ticks (M-2) | 2M | 0.9 s | **307 MiB** | 0.1 / 129 / 254 / 215 / 42 | ~160 B per non-tail resume, held until the outer call returns, then released |
| `region-with-region` rounds (R-3) | 100k | 1.1 s | **413 MiB** | 0.0 / 297 / 414 / 414 / 414 | ~4.3 KiB per round, never released (every bracket retires) |
| `tailcall-carrier-let-deep`, 6 loops | 60M | 4.0 s | **2.59 GiB** | 0.1 / 725 / 1529 / 1985 / 2589 | ~45 B per iteration, linear, never released |

- **The tail-resume loop is flat** at 100M iterations, as the guide says a
  tail `resume` should be.
- **M-2 is real and bounded by the outer call**: a 2M-tick non-tail resume
  loop holds ~300 MiB of continuation copies until the handle's outermost
  direct-style call returns, then drops to 42 MiB -- what
  [effect-nontail-resume-copies-held-until-outer-entry](../../reported/effect-nontail-resume-copies-held-until-outer-entry.md)
  describes, at ~160 B per resume (the guide's cost table says ~630 B of
  traffic; most of it is freed as it runs).
- **R-3**: every one of the region brackets retires (WP2), so each keeps its
  slab: 4.3 KiB a round, for the life of the process.
- **`tailcall-carrier-let-deep` grows linearly to 2.6 GiB -- and that is the
  guide, not a leak.** Five of its six constant-stack loops allocate something
  per iteration that the cost model says nothing frees: a `Vec` (`count-vec`,
  `count-two`: containers are not freed at scope exit) or a `:heap` cell
  (`count-list`'s stdlib `Cons`, `count-box`, `count-hp`: `:heap` nodes live
  until a region ends or the process does). The by-value recursive `count-lst`
  is the control and costs nothing. The fixture tests constant *stack*; at
  scale it is a clean demonstration of the guide's two "never freed" rows.

## M-3: leak residue

Same sweep and procedure as
[carrier-sum-option-boxes-have-no-owner](../../reported/carrier-sum-option-boxes-have-no-owner.md)
(every fixture in `docs/artifacts/rm1-erased-base-callers.txt`, built with
`TUR_RUNTIME=source` and `-fsanitize=address`, here also with the preamble
inline so every byte is instrumented, run with `detect_leaks=1`). Raw:
[wp3-leak-residue.txt](wp3-leak-residue.txt).

Two numbers, because the report's own 2026-10-08 investigation found that
LeakSanitizer's default roots under-count this sweep (a stale copy of a
pointer in a dead stack frame keeps a leaked block "reachable"), and switched
to `LSAN_OPTIONS=use_stacks=0:use_registers=0`:

| roots | 2026-09-28 | 2026-10-08 | now |
| --- | ---: | ---: | ---: |
| default | 1766 B | 1857 B | 1919 B |
| **stack / register roots off** (the comparable method) | -- | **1984 B** | **1968 B** |

**Under the comparable method the residue shrank by 16 B**, and that is one
row: `hkt-stdlib-result-ok-biased` (the `:int` inline-C reader), 64 -> 48 B.
Every other row is byte-identical to 2026-10-08's strict column
(`re-string` 548, `constrained-defn-cons-return-monomorphize` 432,
`option-niche-crossings` 245, `httpd-req-string-opt` 126,
`hkt-constrained-byvalue-bind-pure` 112, `refined-nonempty` 80,
`typed/zipper-basic` 64, `hkt-constrained-hole-headed-instance-head` 64,
`hkt-constrained-spec-reresolves-instance` 48, `option-niche-string` 41,
`conv-defstruct-option-hkt-instance-bodies` 40,
`hkt-stdlib-option-result-instances` 40, `hkt-constrained-byvalue-carrier` 32,
`typed-slots/coerce-carrier-to-struct` 32, `option-map-capturing-closure` 16;
the rest 0). The default-roots total *rose* by 62 B only because three rows
that the stale-stack effect used to hide now read their strict values -- which
is the report's point about default roots, seen from the other side. Anyone
re-measuring should quote the strict number.

**The opted-in corpus is clean.** `tests/run-leak-check.sh` on the Debug
`tur` (every `requires.leak-check` fixture, LSan on): **212 passed, 0 failed,
4 known-open**, the four carrying their `known-leak` markers --
`await-in-generic-capturing-lambda-struct` 16 B, `byval-recursive-adt-shared-copy-not-freed`
176 B, `colored-generic-erased-carrier-param` 16 B, `saffron-higher-order`
880 B.

## M-5: `tur build` peak RSS

`/usr/bin/time -f %M` (the process and the children it reaped, so the cc it
ran) over WP0's 70-program corpus, once each, Release and Debug (ASan) `tur`.
Raw: [wp3-build-rss.csv](wp3-build-rss.csv).

| `tur` | median | range | outlier |
| --- | ---: | --- | --- |
| Release | **33 MiB** | 30-61 MiB | `examples/reflected-measures` 345 MiB |
| Debug (ASan + UBSan) | **96 MiB** | 94-105 MiB | `examples/reflected-measures` 496 MiB |

Debug costs a median 2.9x Release. The one outlier is WP1's: built with
`tur build <file>`, that example's measures stay opaque (its manifest's
`:experiments` is not read) and the refinement solver works through them for
~390 ms. Plain Turmeric builds sit well under the 175-254 MB the r7rs audit
recorded for r7rs fixtures under ASan, and are nearly flat across the corpus
(30-61 MiB Release): the prelude every build carries, not the program, sets
most of it.

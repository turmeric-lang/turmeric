# Performance audit 2026-10 -- artifacts

Measurements for [performance-audit-plan](../../archive/performance-audit-plan.md).
**Start at [summary.md](summary.md)**: the audit's result on one page -- what
to build next across every surface, ranked, and the definition of done closed
item by item.

One file per work package (`wp0-*.md`, `wp1-*.md`, ...), raw CSV/JSONL beside
it. Every table in those files carries the header below or names a different
one; a number without a header is not a result.

## The baseline box (WP0, confirmed 2026-10-14)

The plan's section 4 named an Intel NUC8i7HVK. **The machine is a NUC 9 Pro**
-- the checklist below was worked on the box itself, and the plan's table is
corrected to match.

| | |
| --- | --- |
| Machine | Intel NUC 9 Pro, `NUC9V7QNX` ("Quartz Canyon"), the author's own |
| CPU | Intel Core i7-9850H @ 2.60 GHz (Coffee Lake-H): 6 cores / 12 threads, AVX2, no AVX-512; L1d 32 KiB x6, L2 256 KiB x6, L3 12 MiB |
| Host memory | 16 GB (2 x 8 GB DDR4-2400) |
| Storage | 256 GB internal NVMe (`TEAM TM8PS7256G`, C:) -- the WSL disk lives here. A 1 TB external USB SSD (D:) is attached but **not used** by the audit |
| Host OS | Windows 11 Home 10.0.26200.9457 |
| WSL | 2.6.1.0, default version 2; kernel `6.6.87.2-microsoft-standard-WSL2`; `systemd-detect-virt` = `wsl` |
| Distro | Ubuntu 24.04.5 LTS (what `ubuntu-latest` runs), installed 2026-10-14 for this audit |
| VM shape | `%UserProfile%\.wslconfig`: `memory=12GB`, `processors=12`, `swap=4GB`, `[experimental] autoMemoryReclaim=disabled`. Inside: `nproc` 12, MemTotal 11.7 GiB, swap 4 GiB |
| Toolchain | gcc 13.3.0, clang 18.1.3, cmake 3.28.3, glibc 2.39, Python 3.12.3, valgrind 3.22.0 |
| perf | Ubuntu `linux-tools-6.8.0-147-generic` (perf 6.8.12), called directly at `/usr/lib/linux-tools/6.8.0-147-generic/perf`; `kernel.perf_event_paranoid` = 2 |
| PMU | **exposed.** `perf stat -e cycles,instructions,cache-misses,branch-misses` returns real counts inside the VM, and `perf record -g` samples with call graphs |
| Checkout | `/srv/perf/turmeric` on the distro's own ext4 disk (never `/mnt/c`); remote `win` = the Windows clone, `origin` = GitHub |
| tur | Release, `-DTUR_JIT=ON`, built at `build-release/` (40 s wall at `-j12`, 262 MB peak RSS); `tur --version` = v0.65.1 |

### Checklist answers (plan section 4, "The baseline machine")

1. **WSL2 on a working hypervisor -- yes.** `systeminfo`: "A hypervisor has
   been detected." `Win32_ComputerSystem.HypervisorPresent` = True.
   (`VirtualizationFirmwareEnabled` reads False, which is what Windows reports
   once Hyper-V owns VT-x; it is not a firmware problem.) WSL was installed
   but had **no distribution**; Ubuntu 24.04 was installed for the audit.
2. **What the VM is given.** There was no `.wslconfig` (the VM would have got
   ~8 GB). One was written with the shape above. Side effect on the host:
   reserving 12 of 16 GB pushes Windows' commit charge up, and the
   system-managed `pagefile.sys` sits at 10.5 GB.
3. **Hardware counters -- yes** (above). The plan's fallback (`cpu-clock`
   sampling plus callgrind) is not needed; WP2 keeps its IPC and cache
   columns. Ubuntu's 6.8 `perf` runs against Microsoft's 6.6 kernel when
   called by its full path.
4. **Where the tree lives.** ext4, `/srv/perf/turmeric`. The VHDX
   (`%LOCALAPPDATA%\wsl\{55c2fc8f-...}\ext4.vhdx`) is on the internal NVMe;
   C: had 30 GB free after the install and a Release build, short of the
   plan's 80 GiB estimate. If space runs out, the overflow is a **new** ext4
   VHDX on D: attached with `wsl --mount --vhd` (block device, not 9P) --
   never `/mnt/d`, and nothing already on D: is touched.
5. **Clock speed and noise.** The active power plan was **Balanced**. For
   the WP0 baseline the author switched to the **High performance** scheme
   (`powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c`; back with
   `381b4222-f694-41f0-9685-ff5bb260df2e`), with the Windows 11 Power-mode
   overlay on *Best performance* (`ded574b5-...`), and quit Steam and Razer
   Synapse.

   **Thermal: no throttling under a serial load.** Two minutes of one
   single-threaded program back to back (`sized/cps-tramp-resume-8m`, 130
   runs), with Windows' `\Processor Information(_Total)\% Processor
   Performance` sampled each second -- the VM cannot see the real clock, the
   host can. The clock held 157.7-166.4% of the 2.6 GHz base (4.10-4.33 GHz,
   mean 4.21 GHz) with no dip. Run time drifted ~5% over the first minute
   (~890 ms -> ~940 ms as the clock eased from ~163% to ~158%) and then held;
   quarter means 917 / 927 / 934 / 927 ms, min 878, median 925, max 1131
   (two early outliers). So: best-of-N with a short cool-down between
   programs (the baseline uses 3 s) is enough; nothing needs long rests.
   This says nothing about an all-core load (a parallel build or suite),
   which was not measured. Defender real-time
   protection is on and tamper-protected; the ext4 VHDX is one file to it,
   so it does not scan the tree file by file. The host's standing background
   load is Steam (and its `steamwebhelper`s), Razer Synapse and the NVIDIA
   overlay -- the top CPU consumers on 2026-10-14 at ~12% total load; quit
   them for a sweep. Thermal check: see `wp0-baseline.md`.
6. **Pinning is to vCPUs.** Not used for WP0; any later `taskset` row says
   "vCPU-pinned".

### Not the CI runners' shape

12 logical CPUs of a 2019 laptop-class part inside a VM. These numbers are
compared with each other and with nothing on `/ci`.

## Re-measuring on the baseline box

The fixes in [summary.md](summary.md) can be written and tested anywhere, and
the reports the audit filed reproduce from counts, output or leaked bytes on
any machine. What only this box can say is **what a fix bought**: every "buys"
figure is relative to this box, and its numbers compare only with each other
(decision 1). The nightly benchmarks on `/ci` catch a drift on hosted runners;
they cannot check a prediction made here. So a PR that implements a ranked
change, or a suspected regression, gets a before/after from this box.

**The box, as WP0 set it up** (checklist above):

1. Host on the **High performance** plan
   (`powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c`; Balanced is
   `381b4222-f694-41f0-9685-ff5bb260df2e`), Steam, Razer Synapse and the NVIDIA
   overlay closed.
2. Nothing else building or testing in the VM.
3. Two checkouts, each a Release `-DTUR_JIT=ON` build of **all targets** (`tur
   build` links the runtime archives `--target tur` does not produce). A `tur`
   reads the stdlib beside it, so a stdlib change needs its own tree.

**The run** (inside the WSL distro; refs reach `/srv/perf/turmeric` from the
Windows clone through its `win` remote):

```sh
cd /srv/perf/turmeric
git fetch -q win "+refs/remotes/origin/main:refs/remotes/win/origin-main" \
                 "+claude/<fix-branch>:refs/remotes/win/<fix-branch>"
git worktree add --detach /srv/perf/before win/origin-main
git worktree add --detach /srv/perf/after  win/<fix-branch>
for d in before after; do
  cmake -S /srv/perf/$d -B /srv/perf/$d/build-release -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON
  cmake --build /srv/perf/$d/build-release -j12
done
cd /srv/perf/after
bash benchmarks/audit/remeasure.sh /srv/perf/before/build-release/tur \
     /srv/perf/after/build-release/tur /srv/perf/runs/<fix> \
     [--sweeps 3] [-- --only '<regex>' | --engines cc,jit]
```

`benchmarks/audit/remeasure.sh` warns when the plan is not High performance or
the box is busy (it waits up to two minutes for a just-finished build's load to
settle), runs the WP0 sweep (`run-corpus.py --reps 5 --cooldown 3`) **alternating
before and after** so the host's slow drift lands on both sides, merges each
side, and writes `compare.md` / `compare.csv` with
`benchmarks/audit/compare-runs.py`. The whole corpus is about ten minutes a
sweep, so the default six sweeps are an hour; `--only` narrows it to the
programs the change is about.

**Reading `compare.md`.** A row is `faster` or `slower` only when it moved
more than 5% (10% under 50 ms) and more than its own noise -- the spread
between its two best sweeps -- and under 5 ms it is left to the timer
(`floor`). The geometric mean per engine and phase is over the rows that are
neither noisy nor at the floor. An A/A run on 2026-10-15 (the same `tur` on
both sides, 27 rows) set those numbers: at a flat 5% it called one 17 ms
`emit` "faster"; with 10% under 50 ms it reads 23 same, 3 floor, 1 noisy, 0
moved. Size (`emit` C bytes, `build` binary bytes) and peak-RSS changes are
listed separately.

**Quoting it in a PR:** the geomean table, the "Rows that moved" table and the
footer (box, both `tur`s, the corpus commit, sweeps, date). A same-day A/B is
the result. A comparison against [wp0-baseline.csv](wp0-baseline.csv)
(`compare-runs.py wp0-baseline.csv <run>`) is context, not a result: the box
and the tree have both moved since.

**A worked example, and two traps it found** (2026-10-15, the triangle and
three sized programs, 27 rows; the outputs are
[remeasure-2026-10-15-ab.md](remeasure-2026-10-15-ab.md)):

- **The A/B** -- WP0's own commit (`1a2052bf7`) against `main` @ `1d419dd97`,
  three sweeps a side: one row moved, `region-rounds-100k` under `tur jit`,
  **1.59 s -> 1.04 s (0.651)**; everything else `same`, `noisy` or `floor`.
- **Trap 1, the old CSV.** Compared against `wp0-baseline.csv` instead, the
  same `main` read `mut-map-reassign-1m`'s JIT run as **+29%** and four cc
  runs as +5-6% -- none of it in the code: WP0's commit, rebuilt and run the
  same day, read the same as `main`, and its own cc run of `mut-map` 6.6%
  above the CSV's. Hence "same-day A/B is the result".
- **Trap 2, a hot CPU.** That +29% came from the A/A run, started 30 s after
  a 12-thread build: `mut-map`'s JIT run read 2.08 s on both sides, against
  1.71 s an hour later. An all-core build leaves the part heat-soaked and
  boosting lower for minutes, which the load average does not show once the
  build exits. `remeasure.sh` now waits for the load to settle **and** two
  minutes more; after building by hand, wait before measuring.

**Other surfaces**, by hand with the same rule (both sides, same session):
memory, `TUR=<tur> bash benchmarks/memory/measure.sh` for each build and diff
the tables; the triangle, `TUR=<tur> REPS=11 bash benchmarks/run-triangle.sh`
twice per build; the web, `WEB_PERF=1 WEB_PERF_LOADS=5 npx playwright test
--project=desktop tests/web-perf.spec.js` against each side's production
build. The web toolchain (Node 22, emsdk 6.0.10, Playwright Chromium; ~4 GB) is
not installed on the box at present: put it on C:, or on a **new** VHDX on D:
attached with `wsl --mount --vhd` from an elevated shell -- not an image file
under `/mnt/d`, which loses its writes (D: is exFAT).

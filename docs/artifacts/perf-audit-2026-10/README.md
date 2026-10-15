# Performance audit 2026-10 -- artifacts

Measurements for [performance-audit-plan](../../upcoming/performance-audit-plan.md).
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

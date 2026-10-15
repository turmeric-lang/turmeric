# benchmarks/memory -- the memory-usage guide's cost table, regenerated

`measure.sh` is the harness that
[docs/guides/memory-usage-guide.md](../../docs/guides/memory-usage-guide.md)
describes in "How the numbers were measured", checked in and generalized from
one probe to a directory of them. `probes/` holds one probe per row of the
guide's cost table, plus the rows performance-audit-plan WP3 asked for. The
guide's table was a 2026-10-05 snapshot with no harness behind it (M-1); this
directory is what regenerates it.

## Running it

From the repository root, with a Release `tur` (a sanitized one is refused,
as by `benchmarks/run-benchmarks.sh`):

```sh
TUR=./build-release/tur bash benchmarks/memory/measure.sh               # every probe
TUR=./build-release/tur bash benchmarks/memory/measure.sh --out /tmp/mem # + CSV and .md there
TUR=./build-release/tur bash benchmarks/memory/measure.sh benchmarks/memory/probes/10-map-int-entry.tur
```

Needs `valgrind` (memcheck and massif) and `nm` on `PATH`; Linux/glibc only.
The markdown table goes to stdout, progress and failures to stderr. Options:
`--out DIR` (writes `memory-costs.csv` and `memory-costs.md`), `--n N` (one N
for every probe; the expect-N check is then skipped), `-j J` (probes in
parallel, default 2, at most 4), `--keep` (keep the scratch directory). All
builds, binaries and valgrind logs go in a `mktemp -d` directory under
`$TMPDIR`; nothing is written into the repository. The whole set takes about
20 seconds at `-j4`.

The script exits 1 if any probe failed: it did not build, exited non-zero, or
printed a first line of stdout (or a `TUR_REGION_STATS=1` line) other than the
one its comment pins. A failed probe is not measured -- a probe that does less
than it says must not produce a small number.

## Method

The guide's, unchanged. Each probe has `__N__` where the element count goes.
It is built twice, with `__N__` = 0 and = N (10000 unless the probe says
otherwise), and each binary is run under memcheck and under massif. Every
column is (value at N - value at 0) / N, so start-up, the stdio buffer, the
runtime's one-time tables and a probe's warm-up element all cancel. Probes
whose feature has a first-use cost (an rc, an effect handler, a region arena,
the async scheduler, a fiber) run one warm-up element in both builds; the
first-rc probe instead measures N=1 against N=0, so its row IS the one-time
cost.

| Column | Meaning |
| --- | --- |
| allocs/elt | memcheck "total heap usage" allocations |
| bytes/elt | memcheck "total heap usage" bytes allocated: traffic, counting blocks freed at once |
| peak/elt | the largest `mem_heap_B + mem_heap_extra_B` massif snapshot (`--peak-inaccuracy=0.0`): how big the process gets, malloc's 8-byte header and 16-byte rounding included |
| peak req/elt | the `mem_heap_B` part of that snapshot: the bytes the program asked for |
| lost/elt | memcheck LEAK SUMMARY "definitely lost" |
| +ind/elt | memcheck "indirectly lost": blocks only a lost block points to. A leaked list of N cells is one definitely lost head and N-1 indirectly lost cells, so lost + ind is what leaked |
| regions | the `TUR_REGION_STATS=1` line at N, when the probe opened a region |

A peak difference can be up to ~0.4 B/elt low at N=10000: the N=0 run's own
peak is the 4 KiB stdout buffer, which in an N run is often allocated only
after the structure that sets the peak has been freed (probe 05 reads 39.6,
not 40, for that reason).

Probe annotations (comment lines): `;; row:` the table label, `;; guide:` the
guide's 2026-10-05 cell, `;; measure: N=<n>`, `;; expect-0:` / `;; expect-N:`
the first stdout line at each N (each probe prints a checksum that depends on
N), `;; expect-regions-0:` / `;; expect-regions-N:` the region-stats line.

## Results, 2026-10-14

- `tur` v0.65.1, Release, built from `main` @
  `83f7857ae732cf61509209abc3bab6d143dd4263` (the merge of #1188).
- The WP0 baseline box: Intel NUC 9 Pro (Core i7-9850H), WSL2 Ubuntu 24.04.5,
  kernel 6.6.87.2-microsoft-standard-WSL2, gcc 13.3.0, glibc 2.39,
  valgrind 3.22.0.
- Two full runs gave identical per-element columns.

### The guide's rows

Traffic is bytes / allocations per element; peak is with malloc overhead,
requested bytes in brackets; lost is definitely + indirectly lost.

| Guide row | Guide, 2026-10-05 | Probe | Traffic | Peak (req) | Lost | |
| --- | --- | --- | --: | --: | --: | --- |
| `:int`/`:float`/`:bool`/`:cstr` literal | 0, 0 | 01 | 0 / 0 | 0 | 0 | same |
| by-value `defstruct` P, passed and returned | 0, 0 | 02 | 0 / 0 | 0 | 0 | same |
| `Option`/`Result` of a scalar | 0, 0 | 03 | 0 / 0 | 0 | 0 | same |
| `:heap` node (3 ints) | 24 B (40 B peak), 1, never freed | 04 | 24 / 1 | 40 (24) | 24 | same |
| by-value recursive `ICons` link | 24 B, 1, scope exit | 05 | 24 / 1 | 39.6 (23.6) | 0 | same |
| stdlib `(Cons int)` cell | 16 B (24 B peak), 1, never freed | 06 | 16 / 1 | 24 (16) | 16 | same |
| `Vec int` slot | ~19 B peak, ~26 B traffic, amortised 0 | 07 | 26.2 / 0.0014 | 19.3 | 0 | same |
| `Vec P` slot, 24 B P | ~53 B peak, 1 | 08 | 50.2 / 1 | 53.1 (37.1) | 0 | same |
| `MutableMap int int` entry | ~78 B peak, ~105 B traffic, amortised 0 | 09 | 104.8 / 0.0011 | 78.2 | 0 | same |
| `Map int int` entry, old versions freed | ~66 B live; ~720 B, ~7 allocs per insert | 10 | 715.5 / 6.89 | **95.8** (66.5) | 0 | **moved** (see 1) |
| `Map int int`, old versions NOT freed ("Containers") | 1.7 KB per insert, all leaked | 11 | 715.5 / 6.89 | **790.3** (715.5) | **715.4** | **moved** (see 2) |
| `String` | 16 B + len + 1, 1 | 12 | 22.0 / 1 | 0 (released) | 0 | same ("hello": 16 + 5 + 1) |
| `rc<T>` | 72 B block + value block, 2 | 13 | 80 / 2 | 0 (dropped) | 0 | same (72 + 8) |
| first `rc<T>` in a program | one-time 544 KiB, 2 | 14 | 557,136 / 4 | 557,072 (557,056) | 0 | same (544 KiB exactly, plus the rc's own 2 blocks) |
| capturing closure (1 capture) | 8 + 8 + 8/capture, 1 | 15 | 24 / 1 | 0 | 0 | same |
| named `defn` used as a value | 0, 0 | 16 | 0 / 0 | 0 | 0 | same |
| a call through a function value | ~200 B, 2 | 17 | **304** / 2 | 0 | 0 | **moved** (see 3) |
| | | 17b | **600** / **5** | 0 | 0 | non-tail call: see 3 |
| `perform` + tail `resume` | the perform's frame and env, 2 | 18 | 168 / 2 | 0 | 0 | same, now a number |
| `perform` + a non-tail or repeated `resume` | ~630 B, 6, kept until the outermost call returns | 19 | **792** / **7** | **0** | 0 | **moved** (see 4) |
| | | 19b | 2,142 / 17 | **794.7** (698.7) | 0 | repeated: still kept |
| region generation | 64 KiB slabs, node size, 1 per 64 KiB | 20 | 15.7 / 0.00024 | 15.7 | 0 | same; `pushes=2 rewinds=2 retires=0` |

What moved:

1. **`Map int int` live entry: ~66 B -> 95.8 B peak.** Not a regression: the
   guide's 66 B is requested bytes (the probe's `peak req` reads 66.5), while
   its own method -- and every other peak in its table -- counts malloc's
   overhead, which for a HAMT of small blocks is another 29 B an entry. On the
   guide's definition a live `Map` entry is ~96 B, so it is LARGER than a
   `MutableMap` entry (~78 B), and the guide's "Containers" table ("smallest to
   largest: Vec ~19, Map ~66, MutableMap ~78") and strategy 2 have the order
   wrong. The traffic (~716 B, ~6.9 allocations an insert) is as the guide says.
2. **Rebinding a map without `map-free`: 1.7 KB -> ~790 B per insert** (peak;
   715 B leaked, 8 B of it definitely and 707 B indirectly lost). The guide's
   1.7 KB predates the 2026-10-07 fix that stopped allocating every HAMT node
   at full 32-slot size, which the guide records for the live-entry number but
   not for this one.
3. **A call through a function value: ~200 B -> 304 B, 2 allocations** for
   `use1` (`(f i)`, a tail call) called from main's direct-style `while` loop:
   each call enters `use1`'s `__cps` twin through its direct entry wrapper, a
   `dk_prompt` root plus `dk_done` (2 x 152 B `calloc`s) and a `setjmp`. When
   the call through the value is NOT in tail position (`plus1`,
   `(+ 1 (f i))`, probe 17b) it is **600 B and 5 allocations**: a third 152 B
   DK node for the heap join (`dk_frame_join`), and the reap list's array,
   which every direct entry allocates afresh (`realloc` to 16 B, then to
   128 B) and frees when it returns. Inside a CPS-colored loop (probe 21,
   below) a turn costs 176 B and 2 allocations (a 152 B join node and a 24 B
   env). All of it is freed as it runs; peak is flat in every shape.
4. **`perform` + one non-tail `resume`, a `handle` a turn: no longer kept.**
   792 B and 7 allocations of traffic a turn, peak flat -- the 2026-10-12
   in-place resume (effect-nontail-resume-copies-held-until-outer-entry). The
   other half of the row, a `k` resumed more than once (`^multishot`, probe
   19b), still keeps **~795 B a turn** until the outermost call returns
   (2,142 B and 17 allocations of traffic). The guide does not say which shape
   its 630 B came from.

### The new rows (performance-audit-plan WP3)

| Row | Probe | Traffic | Peak (req) | Lost | Finding |
| --- | --- | --: | --: | --: | --- |
| (a) CPS-colored loop (R-1): a self-tail-recursive loop whose turn calls `use1` | 21 | 176 / 2 | **0** | 0 | peak constant, as the guide's "Calls through function values" table says; `loop` and `use1` both have `__cps` twins |
| (a') a loop that calls its OWN fn parameter, `(+ acc (f i))` | 21b | 0 / 0 | 0 | 0 | not emitted CPS at all: a plain C loop with one indirect call |
| (b) non-tail resume LOOP (M-2): N performs under one `handle`, case `(+ 0 (resume k 1))` | 22 | 320 / 3 | **167.7** (151.7) | 0 | peak grows ~168 B a turn: one 152 B DK frame a turn (`calloc` in `dk_frame_resume_borrow`, under `dk_perform`), 168 B with malloc's overhead, kept until the `handle` returns -- the report's "one 144 B node a turn", which is 152 B on this tree. Traffic: 2 x 152 B frames + one 16 B block |
| (c) `Vec` of a wide struct (M-4): 64 B, eight ints | 23 | 90.2 / 1 | 85.1 (77.1) | 0 | the box is the element's size plus malloc's 8 B (72 B), on top of the 8 B slot and its doubling slack; it scales linearly with the element (probe 08: 24 B element, 53.1 B) |
| (d) async spawn + await (R-10): `(await (async (fn [] ...)))` a turn | 24 | 256 / 4 | 0 | 0 | **no fiber stack**: `async` runs a body that never suspends inline on the caller's stack, and `tur_fiber_block_new` has no caller in the emitted program. A task is a 152 B DK node, the 24 B closure env, a 64 B `TurFuture` and a 16 B block, all freed by the await |
| (d') a raw fiber (`fiber-new`, default stack) made, run to its yield and end, freed | 24b | 1,050,936 / 2 | 0 | 0 | this is the 1 MB: a 1 MiB stack plus a 2,360 B FiberBlock per fiber; stdlib/taskgroup.tur's spawns take this path |
| (e) region bracket rewound in a loop (R-3 steady state): N rounds of an 8-cell bracket | 25 | **472 / 5** | **0** | 0 | peak constant and every round rewinds (`pushes=10001 rewinds=10001 retires=0`), but each bracket entry costs 5 allocations / 472 B -- see below |

## Findings worth a follow-up

- **A `with-region` bracket in a defn costs ~5 mallocs (472 B) per entry,**
  even though it rewinds and its nodes cost no malloc. `with-region` is
  `(defn with-region [A] [^fat body : (fn [] A)] : A (body))`
  (stdlib/region.tur), a tail call through a function value, so the
  bracket's caller (`round` in probe 25) gets a `__cps` twin, and each entry
  from direct-style code builds a DK root (2 x 152 B, freed at
  `__dk_entry_leave`) plus three 56 B frames (freed by `__dk_reap_run`). For
  probe 25's 8-cell (128 B) round that is 3.7x the memory the bracket
  reclaims. Peak is flat, so it is traffic, not growth. Repro: probe 25; the
  same shape as the guide's strategy-4 example.
- **Each direct-style entry into a CPS function that reaps a frame
  re-creates the reap list's array**: `realloc(NULL, 16)` then
  `realloc(p, 128)`, freed when the entry returns -- 2 of probe 17b's 5
  allocations a call (144 of its 600 B). Keeping the array across entries
  would take a non-tail fn-value call made from direct-style code from 5
  allocations to 3.
- **R-1 is narrower than stated.** `use1` (tail call through `f`) and `plus1`
  (non-tail) get `__cps` twins; a self-tail-recursive loop that calls its own
  `f` (probe 21b) does not. Separately, `tur emit-c --dump-cps-coloring`
  reports `use1`, `plus1` and probe 21's `loop` as `uncolored` although all
  three are emitted with `__cps` twins, so a census of R-1's blast radius
  taken from that dump would undercount.
- **R-10's "each task mallocs a 1 MB stack" holds for fibers, not for
  `async`.** An `async` body that does not suspend never reaches the fiber
  allocator (probe 24). What an `async` body that DOES suspend costs is not
  measured here.

## Not measured here

- A suspending `async` body (one that parks on a pending future): the probe
  would need a source of not-yet-ready futures; the shapes above cover the
  inline path and the raw-fiber path.
- Memory the runtime takes with `mmap` rather than `malloc`: massif runs
  without `--pages-as-heap`. The fiber stacks, region slabs and every
  container above are `malloc`ed, so they are counted.
- The interpreter and `#lang r7rs` / `#lang saffron` (a different allocator;
  see the guide's last section), long-run RSS curves, and `tur build`'s own
  peak RSS -- the other WP3 items, which are not per-element costs.

# WP5 -- web / WASM

[performance-audit-plan](../../upcoming/performance-audit-plan.md) WP5: put a
number on what Try Turmeric costs the visitor -- the download, the time to a
first result, the language server's latency -- and cost W-1 (the embedded
stdlib, `docstrings.tur` included) and W-2 (the startup preload).

**Where the numbers come from.** Not the WP0 box: it has no Node or emsdk.
They are from CI's web-smoke job on a GitHub-hosted `ubuntu-latest` runner
(run 38690387398 on #1193, emsdk 6.0.10, Playwright's Chromium, headless),
plus three direct reads of production. The runner is a shared machine, so
read the millisecond figures as one run's shape; the trend is WP6's job. The
bytes are exact.

## The instrument

This change adds, all informational:

- **Boot marks.** `turi_wasm_init` records each phase's wall time and
  returns the table from `turi_wasm_boot_timings()` (pinned by
  `tests/wasm_glue_session_unit.c`). The eval worker times `turmeric.js`,
  the wasm module and `turi_wasm_init` and posts them on `ready`; `main.js`
  publishes them with its own marks as `window._turiBoot`.
- **`web/tests/web-perf.spec.js`** (only with `WEB_PERF=1`): serves the
  production `dist/client` on a free port and measures three cold loads in
  fresh browser contexts, the first evaluation, the bytes downloaded, a
  repeat visit and the LSP. It asserts only that the page works.
- **`tools/ci/web-bundle-size.py`**: raw / gzip -9 / brotli -q 11 sizes and
  the wasm's section split.
- Both run in the web-smoke job, non-blocking and unretried; the metrics
  become `web_perf:*` rows on ci-metrics (`kind: "web-perf"`, kept out of
  the suite views). Raw: [wp5-web-perf.json](wp5-web-perf.json),
  [wp5-web-bundle.json](wp5-web-bundle.json).

## A cold load, end to end

Median of three cold loads, served from localhost (so no transfer time --
see the next section for what a real network adds):

| mark | ms since navigation | share of Ready |
| --- | ---: | ---: |
| `DOMContentLoaded` | 152 | |
| Monaco imported and the editor up; `initWasm` starts | **533** | 58% |
| eval worker `ready` | 782 | 27% (worker spawn ~50, `turmeric.js` 14, wasm module 81, `turi_wasm_init` 98) |
| "Ready" shown (after the `doc-names.json` fetch) | **918** | 15% |
| first result, if Run is clicked at Ready | **945** | click -> result 29 ms |

The first of the three loads took 1557 ms to Ready (a cold browser and
server); the other two 918 and 903.

- **The WASM boot waits for Monaco.** `init()` awaits the Monaco import and
  the editor before `initWasm` creates the worker, so the worker's ~250 ms
  and the doc-names fetch's ~140 ms run after it, not beside it.
- **The startup preload is 98 ms** (W-2, below), the wasm compile and
  instantiate 81 ms, `turmeric.js` 14 ms.
- **"Ready" also waits for `doc-names.json`** (350 KB, the doc-panel index),
  ~140 ms after the worker is up.

## What a first visit downloads

23 requests, **12.69 MB uncompressed** before Ready. What they weigh on the
wire, using production's own figures where measured (production serves
`Content-Encoding: br` from Cloudflare's edge; nothing in the repo
configures it) and gzip -9 elsewhere:

| item | raw | on the wire | |
| --- | ---: | ---: | --- |
| `turmeric.wasm` | 5.48 MB | **1.60 MB** (production, br) | 1.23 MB at brotli -q 11 |
| two Iosevka woff2 (400, 500) | 1.98 MB | **1.98 MB** | woff2 does not compress further |
| Monaco (`editor.api`, `vs`) | 4.03 MB | ~1.02 MB (gzip) | |
| `doc-names.json` | 0.35 MB | 0.17 MB (production, br) | |
| the other 17 (page, CSS, app JS, `turmeric.js`) | 0.86 MB | ~0.23 MB | `turmeric.js` 27 KB in production |
| **total before Ready** | **12.69 MB** | **~5.0 MB** | |

At 10 Mbit/s that is ~4 s of download before any of the ~0.9 s of CPU above;
at 50 Mbit/s, ~0.8 s. **On a real first visit the download, not the boot,
is the cost**, and two items are 72% of it: the wasm (1.6 MB) and the fonts
(2.0 MB).

- **The fonts.** `site.css` and `styles.css` import
  `@fontsource/iosevka/400.css` and `500.css`; each weight's "latin" subset
  is a ~1 MB woff2, because Iosevka ships thousands of glyph variants. The
  page needs ASCII and a few symbols.
- **The wasm's split** (bytes): code 1.91 MB, data 3.56 MB -- the embedded
  stdlib (3.15 MB) is most of the data section. Under brotli -q 11 the
  stdlib alone is 0.55 MB of the wasm's 1.23 MB (45%).
- **Production compresses on the fly**, at a lower brotli level than -q 11:
  1.60 MB served against 1.23 MB possible, 0.37 MB (23%) left on the table.

## W-1: the embedded stdlib

`--embed-file stdlib@/stdlib` copies all 204 files (3.15 MB) into the wasm.
**Startup reads 36 of them: 344 KB, 10.9% of the tree** (the direct list in
`src/turi/preload.c` plus what those files `load`: `string`, `str-build`,
`typeclass`). The other 168 files -- `httpd.tur` (270 KB), the r7rs prelude
(207 KB), SRFIs, `.scm` sources -- are read only when a program loads them,
yet every first visit downloads them (~0.48 MB of brotli) and both workers
(eval and LSP) materialize all 3.15 MB in their in-memory file systems.

`stdlib/docstrings.tur` -- the case W-1 named -- is 622 KB raw, **118 KB
brotli (9.6% of the wasm's brotli bytes)**, and is read only on the first
doc-panel lookup (`turi_doc_lookup`). Serving it lazily saves those 118 KB;
serving the whole non-startup set lazily saves ~0.48 MB, four times as much.

## W-2: the startup preload

`turi_wasm_init`'s phases, median of three cold loads:

| phase | ms |
| --- | ---: |
| `env_new` | 1.0 |
| `preload_macros` (`macros.tur`, `contract.tur`) | **33.2** |
| `preload_native_stubs` | 1.6 |
| `preload_collections` (31 files, one `turi_eval`) | **53.3** |
| `preload_typeclasses` (`typeclass-show.tur`) | 7.7 |
| `pin_and_natives` | 0.4 |
| `snapshot_prelude` | 1.4 |
| **total** | **98** |

A prelude parsed and elaborated at build time (or the PS5 snapshot
serialized into the bundle) would remove most of the 95 ms of preload from
every eval-worker start. That is ~10% of a cold Ready here, behind Monaco and
far behind a real network's download, but it is **~20% of a repeat visit's
Ready**: the repeat visit below compiles the wasm from cache in 59 ms and
still pays the full 100 ms preload.

## Repeat visit and the language server

| | |
| --- | ---: |
| repeat visit, HTTP cache: Ready | **504 ms** (Monaco 175, worker ready 429, of which module 59 and preload 100) |
| LSP boot (first editor focus; a second wasm instance) | **137 ms** |
| keystroke -> diagnostics, median of 10 edits | **166 ms** (165-173) |
| of which the client's debounce | 150 ms (`lsp-client.js` `DEFAULT_DEBOUNCE_MS`) |
| analysis + transport | **~16 ms** |

That CI run's repeat visit was served by the HTTP cache alone: on a loopback
host the app skips the service worker unless the URL carries `?sw=1`
(`web/README.md`). The spec now loads `/try/?sw=1`; the section below has the
service-worker numbers, from the WP0 box.

## On the WP0 box, with the service worker

Added at the audit's close-out, once Node 22, emsdk 6.0.10 and Playwright's
Chromium were installed on the baseline box (a work disk on D:; built the way
the web-smoke job builds, from `main` @ `58b985ca8`; the same 5,478,786-byte
`turmeric.wasm`). Two sweeps of five cold loads each, `?sw=1`. Raw:
[wp5-web-perf-box-1.json](wp5-web-perf-box-1.json),
[wp5-web-perf-box-2.json](wp5-web-perf-box-2.json).

| | sweep 1 | sweep 2 | CI runner (above) |
| --- | ---: | ---: | ---: |
| cold: Ready | 1004 ms | 852 ms | 918 ms |
| cold: Monaco up (`initWasm` starts) | 583 ms | 509 ms | 533 ms |
| cold: `turi_wasm_init` | 149 ms | 146 ms | 98 ms |
| first result after Run | 46 ms | 51 ms | 29 ms |
| **repeat visit through the service worker: Ready** | **517 ms** | **506 ms** | (504 ms, HTTP cache) |
| LSP keystroke -> diagnostics (analysis) | 166 ms (16) | 167 ms (17) | 166 ms (16) |

The box is slower per core than the hosted runner on the preload (collections
80 ms, macros 45 ms against 53 and 33), and otherwise agrees with it.

- **The service worker does not make a repeat visit faster than the HTTP
  cache:** 506-517 ms against 504. The repeat visit is Monaco (~280 ms from the
  worker's cache), the wasm module (~78 ms) and the preload (~106 ms), whoever
  serves the bytes -- which is why W-2's build-time prelude is the repeat
  visit's lever, not caching.
- **What the worker downloads on a first visit**: its install precaches the
  shell, the docs pack (6.73 MB) and every `url(/assets/...)` the bundled CSS
  names -- which, through the `@fontsource` packages' `@font-face` rules, is
  **all 98 font files: 49 `.woff2` (2.63 MB) and 49 `.woff` (4.71 MB)**, every
  unicode-range subset of every family and weight the CSS imports, in both
  formats. A browser that has a service worker reads woff2, and a page only
  fetches the subsets its text needs, so the `.woff` half is downloaded and
  never used. With it, the spec counts ~27.5 MB of response bodies over a first
  visit, against 12.7 MB before Ready without the worker (that figure counts
  a file twice when the page and the precache both fetch it).

## Not a problem (measured, fine)

- **The first evaluation**: 27-30 ms from Run to the printed result.
- **LSP analysis**: ~16 ms per edit on a small buffer; the 150 ms debounce is
  nine tenths of the keystroke latency, a choice rather than a cost.
- **`turmeric.js`**: 98 KB raw, 27 KB on the wire, 14 ms to load in the
  worker.
- **`env_new`, the native stubs, pinning and the snapshot**: under 5 ms
  together.

## Ranked: change / buys / effort

| # | change | buys | effort |
| --- | --- | --- | --- |
| 1 | **Subset the Iosevka fonts** (self-hosted ASCII + the symbols the UI uses, one weight if 500 is cosmetic), **and precache only woff2** (`precacheShellAssets` in `web/public/sw.js` takes every `url()` the CSS names) | most of ~1.98 MB a first visit needs before Ready, and the 4.71 MB of `.woff` the service worker fetches and never uses | small |
| 2 | **Take the 168 non-startup stdlib files out of the wasm**; fetch one on first `load` (a lazy FS node or a second data file) | ~0.48 MB brotli (~30% of the wasm transfer) and 2.8 MB of file system in each of two workers; `docstrings.tur` (W-1, 118 KB) comes with it | medium |
| 3 | **Pre-compress `turmeric.wasm` at brotli -q 11** at build and serve it with `Content-Encoding: br` from `worker.js` | 0.37 MB, 23% of the wasm transfer | small-medium |
| 4 | **Start the eval worker and the `doc-names.json` fetch beside the Monaco import**, not after it | ~0.25-0.4 s of a 0.9 s local Ready off the critical path; on a network, the wasm download overlaps Monaco's | small |
| 5 | **A build-time prelude (W-2)**: ship the post-preload state instead of re-running the preload | ~95 ms per eval-worker start, ~20% of a repeat visit | medium-large |

## Not measured

- A real network: the transfer figures are computed from bytes, not
  timed. A throttled Playwright run would time them.
- Mobile, other browsers, a large editor buffer under the LSP.
- The precache's bytes per file, deduplicated: the spec sums response bodies
  by path, so a file the page and the worker both fetch counts twice.

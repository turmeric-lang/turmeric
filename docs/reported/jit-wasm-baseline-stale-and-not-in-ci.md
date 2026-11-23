# No CI job runs `tests/run-jit-wasm.sh`, so its baseline drifts unseen

**Severity:** low (test infrastructure). Nothing in `.github/`,
`CMakeLists.txt` or the Justfile runs the browser-JIT corpus. On 2026-11-23
`tests/jit-wasm-baseline.txt`, last rewritten on 2026-10-30, was 11 fixtures
behind, and nine of those were real defects that only this harness shows.

Filed 2026-11-23, found while checking turmeric-lang/mir#21
([c2mir-wasm-vararg-buffer-allocas-per-call](../archive/c2mir-wasm-vararg-buffer-allocas-per-call.md))
against the corpus.

## What the stale baseline was hiding (measured, emcc 6.0.10, node 22)

`bash tests/run-jit-wasm.sh` on `main` at e9372656 exited 1 with 11 fixtures
"worse than the baseline". All 11 were added between 2026-10-28 and 2026-11-07
and had no baseline row.

- **Nine were wasm32 defects in the CPS code**, fixed the same day:
  [wasm32-cps-pointer-slots-read-as-64-bit-words](../archive/wasm32-cps-pointer-slots-read-as-64-bit-words.md).
  Two of them printed wrong numbers with no diagnostic.
- **Two cannot run on the page** and got baseline rows:
  `scheduler-spawn-detached-frees-done` (fibers: `getcontext`) and
  `stdlib-c-fn-callbacks` (threads: `tur_timer_wheel_new`).

So the baseline holds again, but only until the next fixture lands.

## Root cause

The harness is run by hand. A full run is 3309 fixtures and took about 40
minutes on 4 cores here, in four shards of under 12 minutes each.

## Fix directions

- Run `tests/run-jit-wasm.sh` in CI, sharded with `TUR_TEST_SHARD` and
  `TUR_JIT_WASM_RESULTS`, in the job that already builds `tur_wasm_jit`
  (`.github/workflows/ci.yml`), or nightly if the per-PR cost is too high.

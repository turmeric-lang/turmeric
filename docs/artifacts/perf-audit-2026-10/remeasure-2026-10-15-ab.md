# Re-measure worked example, 2026-10-15

Produced by `benchmarks/audit/remeasure.sh` and `compare-runs.py` on the WP0
box; see [README.md](README.md#re-measuring-on-the-baseline-box). Triangle
plus `sized/{cps-tramp-resume-8m,mut-map-reassign-1m,region-rounds-100k}`.

# 1. A/B: WP0's commit 1a2052bf7 (before) against main 1d419dd97 (after), 3 sweeps a side

## Before / after

before: `runs/remeasure-ab/before`  
after: `runs/remeasure-ab/after`  
rows compared: 27; only before: 0; only after: 0. Verdicts: faster 1, floor 3, noisy 2, same 21.
A row is faster or slower only when it moved more than 5% (10% under 50 ms) and more than its own noise; under 5 ms it is left to the timer.

### Geometric mean ratio (after / before), rows neither noisy nor at the floor

| engine | phase | rows | geomean | faster | slower |
| --- | --- | --: | --: | --: | --: |
| cc | build | 6 | 1.000 | 0 | 0 |
| cc | emit | 4 | 0.983 | 0 | 0 |
| cc | run | 3 | 1.019 | 0 | 0 |
| interp | run | 3 | 1.000 | 0 | 0 |
| jit | run | 6 | 0.938 | 1 | 0 |

### Every row

| program | engine | phase | before ms | after ms | ratio | noise | verdict |
| --- | --- | --- | --: | --: | --: | --: | --- |
| `region-rounds-100k` | jit | run | 1.59 s | 1.04 s | 0.651 | 1.0% | faster |
| `region-rounds-100k` | cc | emit | 17.5 | 17.0 | 0.974 | 0.9% | same |
| `loop-sum` | cc | run | 0.6 | 0.6 | 0.984 | 3.3% | floor |
| `mandel` | cc | emit | 16.8 | 16.6 | 0.985 | 5.4% | same |
| `mandel` | interp | run | 656.9 | 646.9 | 0.985 | 0.1% | same |
| `loop-sum` | cc | emit | 16.7 | 16.5 | 0.987 | 4.1% | same |
| `mut-map-reassign-1m` | cc | emit | 17.3 | 17.1 | 0.987 | 6.3% | same |
| `mut-map-reassign-1m` | cc | build | 170.6 | 169.6 | 0.994 | 1.8% | same |
| `loop-sum` | cc | build | 164.2 | 163.7 | 0.996 | 2.6% | same |
| `mandel` | cc | build | 182.4 | 181.8 | 0.997 | 0.4% | same |
| `cps-tramp-resume-8m` | cc | build | 175.4 | 174.8 | 0.997 | 1.4% | same |
| `mandel` | cc | run | 3.3 | 3.3 | 0.997 | 1.5% | floor |
| `fib` | jit | run | 105.9 | 105.6 | 0.998 | 1.5% | same |
| `loop-sum` | interp | run | 1.48 s | 1.48 s | 0.999 | 1.2% | same |
| `mandel` | jit | run | 109.0 | 109.1 | 1.001 | 0.5% | same |
| `fib` | cc | emit | 16.9 | 17.0 | 1.005 | 11.0% | noisy |
| `mut-map-reassign-1m` | cc | run | 1.33 s | 1.33 s | 1.007 | 2.5% | same |
| `region-rounds-100k` | cc | build | 178.1 | 179.4 | 1.007 | 3.3% | same |
| `region-rounds-100k` | cc | run | 856.4 | 862.9 | 1.008 | 1.1% | same |
| `fib` | cc | build | 165.5 | 167.2 | 1.010 | 1.0% | same |
| `loop-sum` | jit | run | 107.1 | 108.4 | 1.012 | 1.6% | same |
| `fib` | cc | run | 2.0 | 2.0 | 1.015 | 3.5% | floor |
| `cps-tramp-resume-8m` | jit | run | 1.07 s | 1.09 s | 1.015 | 0.8% | same |
| `fib` | interp | run | 161.4 | 164.1 | 1.016 | 1.0% | same |
| `mut-map-reassign-1m` | jit | run | 1.68 s | 1.71 s | 1.018 | 6.5% | same |
| `cps-tramp-resume-8m` | cc | emit | 16.7 | 17.3 | 1.035 | 49.6% | noisy |
| `cps-tramp-resume-8m` | cc | run | 835.6 | 872.9 | 1.045 | 4.9% | same |

Box: Intel(R) Core(TM) i7-9850H CPU @ 2.60GHz, 12 vCPUs; Power Scheme GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (High performance)
before: tur: the Turmeric compiler (v0.65.1) (/srv/perf/wp0c/build-release/tur)
after:  tur: the Turmeric compiler (v0.65.1) (/srv/perf/main/build-release/tur)
corpus commit: 1d419dd97; 3 sweeps a side, alternating; 2026-10-15T18:14Z

# 2. A/A: the same main tur on both sides, 2 sweeps a side, started 30 s after a 12-thread build

## Before / after

before: `runs/remeasure-aa/before`  
after: `runs/remeasure-aa/after`  
rows compared: 27; only before: 0; only after: 0. Verdicts: floor 3, noisy 1, same 23.
A row is faster or slower only when it moved more than 5% (10% under 50 ms) and more than its own noise; under 5 ms it is left to the timer.

### Geometric mean ratio (after / before), rows neither noisy nor at the floor

| engine | phase | rows | geomean | faster | slower |
| --- | --- | --: | --: | --: | --: |
| cc | build | 6 | 1.011 | 0 | 0 |
| cc | emit | 5 | 0.994 | 0 | 0 |
| cc | run | 3 | 0.987 | 0 | 0 |
| interp | run | 3 | 0.999 | 0 | 0 |
| jit | run | 6 | 1.003 | 0 | 0 |

### Rows that moved

Nothing moved beyond the threshold and its noise.


# 3. Context only: main (the A/A's after side) against wp0-baseline.csv

## Before / after

before: `docs/artifacts/perf-audit-2026-10/wp0-baseline.csv`  
after: `runs/remeasure-aa/after`  
rows compared: 27; only before: 269; only after: 0. Verdicts: faster 1, floor 3, same 19, slower 4.
A row is faster or slower only when it moved more than 5% (10% under 50 ms) and more than its own noise; under 5 ms it is left to the timer.

### Geometric mean ratio (after / before), rows neither noisy nor at the floor

| engine | phase | rows | geomean | faster | slower |
| --- | --- | --: | --: | --: | --: |
| cc | build | 6 | 1.023 | 0 | 0 |
| cc | emit | 6 | 1.042 | 0 | 0 |
| cc | run | 3 | 1.055 | 0 | 2 |
| interp | run | 3 | 1.008 | 0 | 0 |
| jit | run | 6 | 0.987 | 1 | 2 |

### Rows that moved

| program | engine | phase | before ms | after ms | ratio | noise | verdict |
| --- | --- | --- | --: | --: | --: | --: | --- |
| `region-rounds-100k` | jit | run | 1.51 s | 1.04 s | 0.688 | 0.4% | faster |
| `region-rounds-100k` | cc | run | 822.0 | 869.3 | 1.058 | 0.5% | slower |
| `cps-tramp-resume-8m` | jit | run | 1.03 s | 1.09 s | 1.063 | 0.3% | slower |
| `mut-map-reassign-1m` | cc | run | 1.24 s | 1.32 s | 1.064 | 1.1% | slower |
| `mut-map-reassign-1m` | jit | run | 1.61 s | 2.08 s | 1.290 | 0.6% | slower |

### Size changes (`emit`: C bytes, `build`: binary bytes)

| program | engine | phase | before | after | change |
| --- | --- | --- | --: | --: | --: |
| `cps-tramp-resume-8m` | cc | build | 45112 | 45104 | -0.0% |
| `mut-map-reassign-1m` | cc | build | 48152 | 48144 | -0.0% |
| `fib` | cc | build | 26536 | 26528 | -0.0% |
| `loop-sum` | cc | build | 23328 | 23320 | -0.0% |
| `mandel` | cc | build | 26736 | 26728 | -0.0% |


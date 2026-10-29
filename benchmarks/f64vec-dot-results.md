# f64vec-dot -- the N1 dot product kernel against `Vec` and C

Measured on a 4-core x86-64 box (gcc 13.3, clang 18.1, SSE2 baseline, no
`-march`). Best of three runs, wall clock.

The workload is simd-and-vectorization-plan section 1.3's: two arrays of 4096
doubles dotted 200,000 times -- 819M multiply-adds, all in L1/L2, so
compute-bound. Every program printed the same total, `2.68684e+09`.

| program | `tur build` (`-O2`) | `tur build --release` (`-O3`) |
|---|---|---|
| [bench-vec-dot.tur](bench-vec-dot.tur) -- `(Vec float)`, `vec-get` per element | 2.17s | 2.18s |
| [bench-f64vec-dot.tur](bench-f64vec-dot.tur) -- `f64vec-dot` (N1), gcc | 0.18s (12x) | 0.18s (12x) |
| the same, `CC=clang` | 0.18s | 0.18s |
| [baseline](bench-f64vec-dot-baseline.c) `lanes` -- the kernel's eight-lane loop in C | 0.31s | 0.16s |
| [baseline](bench-f64vec-dot-baseline.c) `plain` -- a left-to-right C loop | 0.51s | 0.53s |

What it says:

- **A default build is as fast as `--release`.** The kernel's eight lanes are
  written out, so gcc's `-O2` vectorizes its main loop; the baseline's
  `lanes` loop, written with an inner `for k < 8`, is still twice as slow at
  `-O2`. (The first spelling of the kernel was that loop: 0.30s at `-O2`,
  measured 2026-10-28 -- see the plan's N1 "As built".)
- **It matches the same loop in C** and beats the plain C loop, which a
  strict left-to-right float sum cannot vectorize without `-ffast-math`; the
  kernel's fixed eight-lane order is that reassociation, chosen by the
  library, so it gives the same bits on every engine.
- **clang is as fast** since 2026-10-29 (it was 0.32s). With the eight
  accumulators in an array and a `s[i & 7]` tail, clang paired lane 0 with
  lane 4 and built every vector from two half loads; the kernel now keeps
  them as eight scalars and writes the tail out, the same arithmetic, so the
  total is unchanged. `f32vec-dot` gained more: 0.27s -> 0.075s under clang.
- AVX2 widths need `-march`, which tur never sets (`TUR_CC_FLAGS` opts in).

Reproduce:

```sh
tur build benchmarks/bench-f64vec-dot.tur -o k --release && time ./k
tur build benchmarks/bench-vec-dot.tur -o v --release && time ./v
cc -O3 -std=c99 benchmarks/bench-f64vec-dot-baseline.c -o c && time ./c lanes && time ./c plain
```

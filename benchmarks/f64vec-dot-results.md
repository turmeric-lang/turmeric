# f64vec-dot -- the N1 dot product kernel against `Vec` and C

Measured 2026-10-28 on a 4-core x86-64 box (gcc 13.3, SSE2 baseline, no
`-march`), Debug `tur` (the compiler's build type does not affect the program
it emits). Best of two runs, wall clock.

The workload is simd-and-vectorization-plan section 1.3's: two arrays of 4096
doubles dotted 200,000 times -- 819M multiply-adds, all in L1/L2, so
compute-bound. Every program printed the same total, `2.68684e+09`.

| program | `tur build` (`-O2`) | `tur build --release` (`-O3`) |
|---|---|---|
| [bench-vec-dot.tur](bench-vec-dot.tur) -- `(Vec float)`, `vec-get` per element | 2.09s | 2.19s |
| [bench-f64vec-dot.tur](bench-f64vec-dot.tur) -- `f64vec-dot` (N1) | 0.30s (7.0x) | 0.20s (11x) |
| [baseline](bench-f64vec-dot-baseline.c) `lanes` -- the kernel's eight-lane loop in C | 0.31s | 0.16s |
| [baseline](bench-f64vec-dot-baseline.c) `plain` -- a left-to-right C loop | 0.51s | 0.53s |

What it says:

- **The kernel is within 25% of the same loop written in C** at `-O3`, and the
  same at `-O2`; the gap is the call per repetition and the header load, not
  the loop.
- **It beats the plain C loop.** A strict left-to-right float sum cannot be
  vectorized without `-ffast-math`; the kernel's fixed eight-lane order is the
  reassociation `-ffast-math` would make, chosen by the library, so it is
  fast with the default flags and gives the same bits on every engine.
- **`-O2` already vectorizes it partly** (gcc's SLP pass on the eight-lane
  body); `-O3` adds the loop vectorizer.
- AVX2 widths need `-march`, which tur never sets (`TUR_CC_FLAGS` opts in);
  section 1.3 measured ~2x more there for the vector-extension shape.

Reproduce:

```sh
tur build benchmarks/bench-f64vec-dot.tur -o k --release && time ./k
tur build benchmarks/bench-vec-dot.tur -o v --release && time ./v
cc -O3 -std=c99 benchmarks/bench-f64vec-dot-baseline.c -o c && time ./c lanes && time ./c plain
```

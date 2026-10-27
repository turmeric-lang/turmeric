# Fix: monte_carlo_pi Turmeric Benchmark Output Format

## Problem

`python3 scripts/validate_correctness.py` reports:

```
[!!] numerical/monte_carlo_pi [small]  c:pass  turmeric:FAIL(got='3132000' expected='3.132000')
```

The C implementation prints the result as a decimal float:

```c
printf("%.6f\n", 4.0 * inside / iters);   // => "3.132000"
```

The Turmeric implementation intentionally scales to an integer to avoid float printing:

```turmeric
return (int64_t)(4.0 * inside / iters * 1e6);   // => 3132000
```

The validator's `"float"` comparison mode parses both outputs as `float()`, so it compares
`3132000.0` against `3.132000`, a factor-of-1e6 mismatch that always fails.

## Root Cause

The comment in `benchmarks/numerical/turmeric/monte_carlo_pi.tur` reads:

> Return pi * 1e6 as integer to avoid float printing

This was written when Turmeric lacked a convenient way to format floats.
The inline C block can call `printf` directly, so the workaround is unnecessary.

## Fix

### 1. Change `estimate-pi` to `:void`, print inside the C block

In `benchmarks/numerical/turmeric/monte_carlo_pi.tur`, replace the current
`estimate-pi` function (return type `:int`, integer-scaled result) with a
`:void` function that calls `printf("%.6f\n", ...)` directly:

```turmeric
(defn estimate-pi [iters : int] : void
  ```c
  uint64_t state = 6364136223846793005ULL;
  #define LCG_NEXT() ({ state = state * 6364136223846793005ULL + 1442695040888963407ULL; \
                        (double)(state >> 11) / (double)(1ULL << 53); })
  int64_t inside = 0;
  for (int64_t i = 0; i < iters; i++) {
    double x = LCG_NEXT(), y = LCG_NEXT();
    if (x * x + y * y <= 1.0) inside++;
  }
  printf("%.6f\n", 4.0 * inside / iters);
  #undef LCG_NEXT
  ```)
```

Replace the call site to drop `println` (the C block already prints):

```turmeric
(estimate-pi (parse-first-arg 1000))
```

### 2. Rebuild the Turmeric binary

```sh
cd performance-comparison/benchmarks/numerical/turmeric
../../../../build-rel/tur build monte_carlo_pi.tur -o monte_carlo_pi
```

Or delete the stale binary so `validate_correctness.py` rebuilds it automatically:

```sh
rm performance-comparison/benchmarks/numerical/turmeric/monte_carlo_pi
```

### 3. Regenerate the golden file

```sh
cd performance-comparison
python3 scripts/validate_correctness.py --golden --category numerical
```

This overwrites `results/golden/numerical_monte_carlo_pi_small.txt` with the
new `%.6f` output.

### 4. Verify

```sh
python3 scripts/validate_correctness.py
```

Expected result: `21/21 passed, 0 failed`.

## Acceptance Criteria

- `monte_carlo_pi` Turmeric output matches the format `3.NNNNNN` (six decimal places).
- Validator reports `[OK] numerical/monte_carlo_pi`.
- No regression in any other benchmark.

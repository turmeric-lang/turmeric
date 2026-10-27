# Performance Comparison Improvements Plan

Two orthogonal goals:

1. **Idiomatic Turmeric** -- replace inline-C bodies with real Turmeric/stdlib
   constructs wherever a native equivalent exists.
2. **Add turi and Rust** -- two new comparison targets alongside C, Clojure,
   Racket, and Python.

---

## Part 1 -- Idiomatic Turmeric Refactor

### Background

Most benchmarks under `benchmarks/*/turmeric/` are essentially C programs
wrapped in a single inline-C block.  The only exceptions are the recursive
benchmarks (`fibonacci.tur`, `factorial.tur`, `fib_recursive.tur`) and the
tail-call benchmark (`function_call.tur`), which are already fully idiomatic.
The goal is to push as many of the remaining benchmarks toward real Turmeric
so the comparison reflects the language's actual characteristics rather than
C-via-FFI.

### Benchmark-by-benchmark decisions

#### Already idiomatic -- no change needed

| File | Why it is already fine |
|---|---|
| `micro/turmeric/function_call.tur` | Uses `if`/tail recursion throughout |
| `numerical/turmeric/fibonacci.tur` | Tail-recursive `fib-iter`, no inline-C |
| `recursion/turmeric/factorial.tur` | Tail-recursive `fact`, no inline-C |
| `recursion/turmeric/fib_recursive.tur` | Doubly-recursive `fib`, no inline-C |

The only inline-C in those four files is `parse-first-arg`, which is a shared
helper discussed separately below.

#### `parse-first-arg` / `parse-arg` -- shared CLI parsing helper

Every benchmark contains the same ~6-line inline-C block to read `g_tur_args`.
Replace it with a single shared file `benchmarks/shared/turmeric/args.tur` that
exposes `parse-first-arg` and `parse-arg`, and `(include ...)` it from each
benchmark.  If Turmeric does not yet have a first-class args API in stdlib,
keep one canonical inline-C helper and reference it; do not duplicate it 20
times.

#### `micro/turmeric/int_arith.tur`

Current: the entire multiply-add loop is inline-C.

Replace with a tail-recursive loop using native Turmeric arithmetic:

```turmeric
(defn int-arith-loop [i : int a : int b : int n : int] : int
  (if (>= i n)
    (bit-xor a b)
    (int-arith-loop (+ i 1)
                    (+ (* a 1000003) b)
                    (+ (* b 999983) a)
                    n)))
```

This only works if `bit-xor` is available.  Check `src/compiler/` for the
builtin list; add `bit-xor` / `bitwise-xor` if missing (it is a trivial
one-liner compiler pass or extern-c stub).

#### `micro/turmeric/float_arith.tur`

Current: the FP loop is inline-C; `sqrt` is called via a forward declaration.

`sqrt` has no stdlib wrapper yet.  Add `(extern-c sqrt [^float] :float)` to
`stdlib/math.tur` (create if absent) and call it from turmeric code.  The loop
itself becomes a tail-recursive accumulator:

```turmeric
(extern-c sqrt [x :float] :float)

(defn float-arith-loop [i : int a : float b : float n : int] : float
  (if (>= i n)
    (+ a b)
    (float-arith-loop (+ i 1)
                      (+ (* a 1.0000001) (sqrt b))
                      (+ (* b 0.9999999) (sqrt a))
                      n)))
```

Print the result with `(println (float-arith-loop 0 1.0 1.0 n))` and format to
6 decimals if Turmeric has a printf-style formatter; otherwise use inline-C
only for the final `printf("%.6f\n", result)`.

#### `data_structures/turmeric/list_ops.tur`

Current: manually mallocs a C linked list, iterates it, frees it.

Replace entirely using `stdlib/list.tur`:

```turmeric
(include "stdlib/list.tur")

(defn build-list [i : int n : int acc] : int
  (if (>= i n)
    acc
    (build-list (+ i 1) n (cons i acc))))

(defn sum-list [lst acc : int] : int
  (if (list-nil? lst)
    acc
    (sum-list (tail lst) (+ acc (head lst)))))

(let [lst (build-list 0 n (nil-value))]
  (println (sum-list lst 0))
  (list-free lst))
```

This is a direct demonstration of the `cons`/`nil-value`/`head`/`tail` API
that exists today.  The benchmark measures Turmeric's list allocation speed
rather than raw C malloc.

#### `data_structures/turmeric/hash_map.tur`

Current: open-addressing C hash table, all inline.

Replace with `stdlib/hamt.tur` (`hamt/new`, `hamt/set`, `hamt/get`,
`hamt/free`).  Because `hamt/set` takes a key as `:ptr<void>`, integer keys
need to be boxed; use a small inline-C helper `box-int` / `unbox-int` if no
native boxing exists yet.  Alternatively, use `hamt/hash-str` with
`int->cstr`.

```turmeric
(include "stdlib/hamt.tur")

;; Insert i -> i*2 for i in [0, n)
(defn insert-loop [m i : int n : int] : ptr<void>
  (if (>= i n)
    m
    (insert-loop (hamt/set m (hamt/hash-int i) (box-int i) (box-int (* i 2)))
                 (+ i 1) n)))

;; Sum all values
(defn lookup-loop [m i : int n : int acc : int] : int
  (if (>= i n)
    acc
    (let [v (hamt/get m (hamt/hash-int i) (box-int i))]
      (lookup-loop m (+ i 1) n (+ acc (unbox-int v))))))
```

Note: this changes the benchmark semantics slightly (persistent HAMT vs.
mutable open-addressing table) -- document this clearly in the methodology.
If an integer-keyed mutable hash table is needed for a fair comparison, that
is a feature gap to note rather than paper over with inline-C.

#### `data_structures/turmeric/sort.tur`

Current: allocates a raw C array, fills it with an LCG PRNG, calls `qsort`.

`stdlib/vec.tur` exists and provides `vec-new`, `vec-push!`, `vec-get`,
`vec-len`.  There is no native sort on `vec` yet.

Plan:
- Fill the vec with native Turmeric loops.
- For the sort itself, add `vec-sort!` to `stdlib/vec.tur` that wraps `qsort`
  (one small inline-C function), then call it from turmeric code.
- The PRNG loop can be a tail-recursive LCG using native integer arithmetic.

```turmeric
(defn fill-vec [v i : int n : int state : int] : int
  (if (>= i n)
    state
    (let [next-state (+ (* state 6364136223846793005) 1442695040888963407)]
      (vec-push! v (bit-shr next-state 1))
      (fill-vec v (+ i 1) n next-state))))
```

Then print `(vec-get v 0)` and `(vec-get v (- n 1))` after sorting.

#### `numerical/turmeric/primes.tur`

Current: allocates a C `char` array sieve, all inline.

`stdlib/vec.tur` could store the sieve as a vec of ints (one int per slot).
The outer and inner loops become Turmeric tail-recursive loops using the `for`
macro (from `stdlib/macros.tur`):

```turmeric
(defn mark-multiples [sieve i : int j : int limit : int] : nil
  (when (<= j limit)
    (vec-set! sieve j 1)
    (mark-multiples sieve i (+ j i) limit)))

(defn count-loop [sieve i : int limit : int count : int] : int
  (if (> i limit)
    count
    (if (= (vec-get sieve i) 0)
      (do
        (mark-multiples sieve i (* 2 i) limit)
        (count-loop sieve (+ i 1) limit (+ count 1)))
      (count-loop sieve (+ i 1) limit count))))
```

This replaces all inline-C.  Note it uses ints not chars per slot, so memory
usage will be higher -- acceptable for a language benchmark.

#### `numerical/turmeric/monte_carlo_pi.tur`

Current: LCG macro and double arithmetic, all inline-C.

Replace with a tail-recursive LCG + native arithmetic loop:

```turmeric
(defn lcg-next [state : int] : int
  (+ (* state 6364136223846793005) 1442695040888963407))

(defn int->unit-float [bits : int] : float
  ;; shift right 11 bits, divide by 2^53
  (/ (float (bit-shr bits 11)) 9007199254740992.0))

(defn mc-loop [i : int n : int state : int inside : int] : int
  (if (>= i n)
    inside
    (let [s1 (lcg-next state)
          x  (int->unit-float s1)
          s2 (lcg-next s1)
          y  (int->unit-float s2)]
      (mc-loop (+ i 1) n s2
               (if (<= (+ (* x x) (* y y)) 1.0)
                 (+ inside 1)
                 inside)))))
```

Print the result as `(* 4.0 (/ inside n))`.  Use inline-C only for the final
`printf("%.6f\n", ...)` if there is no float-format stdlib function yet.

#### `numerical/turmeric/matrix_multiply.tur`

Current: three-loop DGEMM over raw C `double` arrays, all inline.

This is fundamentally a compute-intensive numeric kernel.  `stdlib/vec.tur`
stores `int64_t`, not `double`, so it cannot represent the matrix directly.
Options:

1. **Keep inline-C** for the inner kernel but call it from Turmeric loops that
   manage the matrix layout.  This is a minimal improvement.
2. **Add a `fvec.tur`** (float vec, backed by a `double *`) to stdlib, then
   implement the DGEMM in turmeric using three nested tail-recursive loops.
   This is the more correct approach but requires a new stdlib module.

Recommendation: note this as a **feature gap** in the plan; add `fvec.tur` as
a tracked stdlib task, and leave the benchmark as inline-C for now with a
comment explaining why.

#### `real_world/turmeric/nbody.tur`

Same situation as `matrix_multiply.tur` -- requires `double` arrays and FP
math.  Keep inline-C for the physics kernel but expose it as a clean Turmeric
function boundary.  Flag `fvec.tur` / native float vec as a prerequisite for
full idiomaticity.

#### `real_world/turmeric/ray_tracing.tur`

Heavily uses `double` structs (Vec3), macro-defined vector math, and inner
loops.  Keep inline-C; this benchmark intentionally stresses numeric
throughput.  A comment at the top of the file should explain this.

#### `io/turmeric/file_read.tur`, `file_write.tur`, `random_access.tur`

Current: raw `fopen`/`fwrite`/`fread`/`fclose` calls, all inline.

`stdlib/io.tur` already provides `file-open`, `file-read`, `file-close`,
`write-file`, `read-file`.  Replace the inline-C with these functions.
Example for `file_read.tur`:

```turmeric
(include "stdlib/io.tur")

(defn write-n-bytes [path : cstr n : int] : nil
  ;; build an n-byte buffer inline-C (only the buffer fill needs C)
  ...)

(defn read-and-count [path : cstr] : int
  (let [fh (file-open path "rb")]
    (if (not (file-handle-ok? fh))
      -1
      (let [buf (malloc 4096)]
        (let [total (read-loop fh buf 0)]
          (file-close fh)
          (free buf)
          total)))))
```

The write side still needs inline-C for the buffer fill; the read side can use
`file-read` from stdlib entirely.

#### `memory/turmeric/alloc_churn.tur`

Current: `malloc`/`free` of `int64_t *` in a loop.

There is no native Turmeric abstraction for bare allocation churn.  Two
options:

1. Keep inline-C and note it as intentionally testing the allocator.
2. Replace with cons-cell churn: build and immediately discard a list of N
   elements using `cons`/`list-free` from `stdlib/list.tur`.  This tests
   Turmeric's allocator path through its own GC/ownership system rather than
   raw `malloc`/`free`.

Recommendation: keep the inline-C version as `alloc_churn_c.tur` and add a
second variant `alloc_churn_tur.tur` that uses cons cells.  Run both and
compare; this is more informative than replacing one with the other.

#### `concurrency/turmeric/thread_ring.tur`

Current: raw pthreads + mutexes + condition variables, all inline-C.

`stdlib/thread.tur` provides `thread-spawn-fn` / `thread-join`.
`stdlib/chan.tur` provides `chan-new` / `chan-send` / `chan-recv` /
`chan-free`.

Replace with a channel-based ring:

```turmeric
(include "stdlib/thread.tur")
(include "stdlib/chan.tur")

;; Each worker reads from its input channel, decrements the token,
;; writes to the next channel.  When token reaches 0, stops.
(defn ring-worker-fn [args : ptr<void>] : ptr<void>
  ...)
```

The worker struct (input-chan, output-chan) must be heap-allocated and passed
as `args`.  This is the idiomatic pattern for `thread-spawn-fn`.  It fully
removes the pthread/mutex/cond-var inline-C.

#### `string_processing/turmeric/string_concat.tur`

Current: `malloc`/`realloc` buffer for concatenation, all inline.

`stdlib/str.tur` provides a borrowed view type, not an owned growable string.
There is no `string-builder` in stdlib yet.  Options:

1. Keep inline-C and note the missing stdlib feature.
2. Use a `vec` of int (each int storing one ASCII byte) as a byte buffer.
   This is awkward; do not do it just to avoid inline-C.

Recommendation: keep inline-C, add a stdlib issue note for `strbuf.tur`
(a growable owned string builder).

#### `string_processing/turmeric/text_search.tur`

Current: `malloc` for haystack, `strstr` in a loop, all inline.

`stdlib/str.tur` has no `str-find` / substring search yet.  Keep inline-C
until `str-find` is added to stdlib.  Note it as a feature gap.

---

### Summary: inline-C disposition

| Benchmark | Inline-C after refactor | Reason kept |
|---|---|---|
| `micro/int_arith` | none | fully native arithmetic |
| `micro/float_arith` | printf only | no float formatter in stdlib |
| `micro/function_call` | none (already native) | -- |
| `data_structures/list_ops` | none | stdlib/list.tur covers it |
| `data_structures/hash_map` | box-int helper only | HAMT key boxing |
| `data_structures/sort` | vec-sort! wrapper only | qsort call |
| `numerical/fibonacci` | none (already native) | -- |
| `numerical/primes` | none | vec + loops cover it |
| `numerical/monte_carlo_pi` | printf only | no float formatter |
| `numerical/matrix_multiply` | all (fvec.tur missing) | feature gap |
| `real_world/nbody` | all (fvec.tur missing) | feature gap |
| `real_world/ray_tracing` | all | intentional compute kernel |
| `io/file_read` | buffer fill only | stdlib/io.tur covers read side |
| `io/file_write` | none | stdlib/io.tur covers write side |
| `io/random_access` | seek/offset only | no seek in stdlib/io.tur |
| `memory/alloc_churn` | kept as _c variant | intentional allocator test |
| `concurrency/thread_ring` | none | stdlib/thread+chan covers it |
| `recursion/factorial` | none (already native) | -- |
| `recursion/fib_recursive` | none (already native) | -- |
| `string_processing/string_concat` | all (strbuf.tur missing) | feature gap |
| `string_processing/text_search` | all (str-find missing) | feature gap |

Feature gaps to track:
- `stdlib/math.tur` -- `sqrt`, `fabs`, `pow`, `floor`, `ceil`
- `stdlib/fvec.tur` -- growable `double` array
- `stdlib/strbuf.tur` -- owned growable string builder
- `stdlib/str.tur` additions -- `str-find`, `str-contains?`
- `stdlib/vec.tur` addition -- `vec-sort!`
- builtin `bit-xor`, `bit-shr`, `bit-and` operators (check if already present)
- builtin `float` cast and `float->int`

---

## Part 2 -- Add turi (interpreted mode)

### What turi is

`turi` is the tree-walking interpreter embedded in libturi (`src/turi/`).
Running `tur --interpret file.tur` (verify exact flag with `tur --help`) or
embedding via `turi_eval` executes Turmeric source without compilation to C.
This is meaningfully different from the compiled path and worth measuring
because:
- It shows the cost of interpretation overhead.
- Some users will use the REPL / embedded eval path for scripting.
- It gives a comparison point: compiled Turmeric vs interpreted Turmeric vs
  compiled C vs other languages.

### File structure

Do **not** duplicate the `.tur` source files.  The turi benchmarks use the
same source but a different runner.  Structure:

```
benchmarks/
  numerical/
    turmeric/          <-- existing compiled-mode files
      fibonacci.tur
    turi/              <-- new: symlinks or hardlinks to turmeric/ files
      fibonacci.tur -> ../turmeric/fibonacci.tur  (symlink)
```

The runner script (`scripts/run_benchmarks.sh`) distinguishes the two by
using `tur file.tur` for the compiled path and `tur --interpret file.tur`
for the turi path.

If the flag is different, record the exact invocation after verifying with
`tur --help`.

### Caveats

- Not all benchmarks will work under interpretation: inline-C blocks are
  JIT-compiled or rejected in interpreted mode.  Any benchmark that still
  has inline-C after the idiomatic refactor (see Part 1) will need a
  separate turi-specific version that calls an `extern-c` function instead.
- The concurrency benchmarks (`thread_ring`) may not work under the
  interpreter; note this and skip them for the turi column.
- Expected result: turi will be 5-50x slower than compiled turmeric on
  compute-bound benchmarks, closer on I/O-bound ones.

### README update

Add a turi row to the language table in `performance-comparison/README.md`:

```
| turi      | latest (same source) | Turmeric tree-walking interpreter |
```

Add a build/run command row:

```
| turi      | tur --interpret file.tur              |
```

---

## Part 3 -- Add Rust

### Directory structure

```
benchmarks/
  numerical/
    rust/
      fibonacci/
        Cargo.toml
        src/
          main.rs
      primes/
        ...
  data_structures/
    rust/
      hash_map/
        ...
  ...
```

Each benchmark is a minimal `cargo new --bin` project.  Use `--release` builds
for fair comparison.  Add a top-level `benchmarks/rust-workspace/Cargo.toml`
that lists all benchmarks as members so a single `cargo build --workspace
--release` compiles everything.

### Rust implementation notes per benchmark

| Benchmark | Key Rust approach |
|---|---|
| `micro/int_arith` | plain loop, `i64` arithmetic, `wrapping_mul` / `wrapping_add` |
| `micro/float_arith` | loop with `f64`, `f64::sqrt()` |
| `micro/function_call` | tail-call loop (Rust TCO not guaranteed; use `loop {}`) |
| `numerical/fibonacci` | iterative with `u64` |
| `numerical/primes` | `Vec<bool>` sieve, same algorithm as C |
| `numerical/monte_carlo_pi` | LCG PRNG as `u64`, same constants |
| `numerical/matrix_multiply` | `Vec<f64>` flat layout, triple nested loop |
| `data_structures/list_ops` | `Vec<i64>` push + sum (idiomatic Rust, not linked list) |
| `data_structures/hash_map` | `std::collections::HashMap<i64, i64>` |
| `data_structures/sort` | `Vec<i64>`, `v.sort_unstable()` |
| `real_world/nbody` | struct array, same physics as C |
| `real_world/ray_tracing` | inline Vec3 struct, same algorithm |
| `memory/alloc_churn` | `Box<i64>` alloc + drop in loop |
| `concurrency/thread_ring` | `std::sync::mpsc` channels, one thread per ring node |
| `io/file_read` | `std::fs::File`, `BufReader`, read to Vec |
| `io/file_write` | `std::fs::File`, `BufWriter` |
| `string_processing/string_concat` | `String::push_str` in loop |
| `string_processing/text_search` | `str::matches()` count or manual `find` loop |
| `recursion/factorial` | `loop {}` (not recursion; Rust will not TCO a recursive call) |
| `recursion/fib_recursive` | `fn fib(n: u64) -> u64` doubly-recursive |

### Build command

```sh
cargo build --release --manifest-path benchmarks/rust-workspace/Cargo.toml
```

Run each benchmark via:
```sh
./benchmarks/rust-workspace/target/release/<benchmark-name> <arg>
```

### README update

Add Rust to the language table:

```
| Rust      | 1.78.0 (stable) | rustc / cargo             |
```

Add build command row:

```
| Rust      | cargo build --release && ./target/release/bench <arg> |
```

### Methodology update

The `performance-comparison/docs/methodology.md` needs a Rust and turi row
in the compiler/interpreter versions table.  No other methodology changes are
needed: the same warm-up + 10-iteration protocol applies.

---

## Implementation order

1. Shared `args.tur` helper (unblocks all idiomatic rewrites).
2. Benchmark rewrites that need no new stdlib:
   `int_arith`, `float_arith` (extern-c sqrt), `list_ops`, `primes`,
   `monte_carlo_pi`, `io/file_read`, `io/file_write`, `concurrency/thread_ring`.
3. Stdlib additions: `vec-sort!`, `bit-xor`/`bit-shr`, `extern-c sqrt` in
   `math.tur`.
4. Remaining rewrites that depend on step 3: `sort`, `hash_map`.
5. Add turi symlinks + runner script changes.
6. Add Rust workspace + one benchmark category at a time.
7. Update README and methodology docs.
8. Run full benchmark suite and collect baseline results.

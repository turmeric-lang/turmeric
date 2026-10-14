# `tur --interpret`: a Scheme string's code points and a string port's buffer stay until the env is freed

**Severity:** low-medium (interpreter memory). Since 2026-10-14 the pool sweep
(turi-dropped-closures-kept-until-teardown) hands back what a Scheme loop
drops: its frames, bindings, closures and small structs. It does not hand back:

- the code-point vector inside a dropped string;
- a dropped string port's buffer.

So a loop that makes strings still grows under `--interpret`. A compiled
program does not grow. Found 2026-10-14: what an eval'd procedure still grew
by, per call, after the `(scheme eval)` bridge's env started sweeping. The
bridge carries each argument and result as `write` text through string ports.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (run n)
  (let loop ((i 0) (acc 0))
    (if (= i n) acc
        (loop (+ i 1) (+ acc (string-length (string-append "ab" (number->string i))))))))
(write (run 80000))
(newline)
```

Peak RSS, Release `tur`, measured 2026-10-14:

| loop body | 20,000 iterations | 80,000 iterations | per iteration |
| --- | --- | --- | --- |
| `(string-append "ab" (number->string i))` | 46.8 MB | 98.8 MB | 0.87 KB |
| `(write-char #\a (open-output-string))` | 34.0 MB | 42.6 MB | 0.14 KB |
| `write` to a string port, `read` it back | 39.5 MB | 59.5 MB | 0.33 KB |
| an eval'd `(lambda (x) ...)` called once | 74.9 MB | 117.6 MB | 0.7 KB |
| the `write`/`read` loop, compiled | 10.9 MB | 10.9 MB | -- |

## Root cause

- **Strings.** A Scheme string is an `R7rsString` around a `(Vec int)` of
  code points (stdlib/r7rs/prelude.tur:1423). The struct is a small pool
  object, and the sweep hands it back. The vector's buffer is a collection box
  (`TuriCollBuf`, src/turi/env.h). The sweep's marker marks the boxes it
  reaches (`gc->boxes`, `turi_cont_gc_begin`, eval.c) but never frees an
  unmarked one. Only `collsweep_after_rewind` frees boxes, and it runs at a
  top-level boundary under scratch promotion, which a `#lang r7rs` program
  never reaches.
- **String ports.** An `r7rs_io` (src/turi/interpreter_natives.c,
  `native_r7rs_io_new`) and its buffer are calloc'd and handed to the prelude
  as a `TURI_INT` carrying the pointer. The prelude frees only its own
  scratch string builders (`r7rs-io-free__`). Under the interpreter nothing
  tracks a port's buffer, so it is not freed even at teardown. A compiled
  program's `malloc` is the collector's, which is why it does not grow.

## Fix directions

1. **Free unmarked collection boxes in the pool sweep.** After
   `turi_cont_gc_reclaim`, destroy each box the marking pass did not reach,
   as `collsweep_after_rewind` does. Keep its rule: a reached box whose scan
   is incomplete (a Set or Map) frees nothing that pass.
2. **Make an `r7rs_io` a collection box** (`destroy` frees the buffer and
   the struct, `scan` is empty and complete), so (1) covers string ports. The
   three standard ports are process-global and stay unboxed.

---
title: Performance Guide
category: Performance
description: Writing fast Turmeric programs -- numerical computation, data structures, string processing, concurrency, memory, recursion, I/O, and benchmarking methodology
---

# Turmeric Performance Guide

Turmeric compiles to optimised C99 (release builds use `-O2` by default). This
means its performance ceiling is close to hand-written C, but the patterns you
choose matter. This guide covers the major performance dimensions -- numerical
computation, data structures, string processing, concurrency, memory and GC,
recursion, and I/O -- and finishes with a methodology section for writing and
interpreting benchmarks.

---

## Build flags

Always benchmark release builds:

```sh
just release          # cmake --build build -j --config Release
```

The debug build (`just build`) inserts contract checks and disables
optimisations; its timing numbers are not meaningful for comparison.

---

## Numerical computation

### Integer and floating-point arithmetic

Arithmetic on `int` and `float` compiles to the corresponding C types.
There is no boxing overhead for scalars declared with concrete types:

```turmeric
(defn square [x] : int (* x x))
(defn hyp [a b] : float
  (sqrt (+ (* a a) (* b b))))
```

```sweet-exp
defn square [x] :int
  {x * x}
defn hyp [a b] :float
  sqrt((+ (* a a) (* b b)))
```

Avoid leaving numeric expressions untyped in hot loops -- the elaborator may
widen to a tagged value when it cannot infer a concrete numeric type.

### Fibonacci (iterative vs recursive)

Iterative is faster for large N because it avoids stack growth:

```turmeric
; iterative -- O(n) time, O(1) space.  The named-let `loop` call is a
; self-tail-call, so it is lowered to an iterative backedge (see
; "Self-tail-call optimization" below): the stack does not grow with n.
(defn fib-iter [n] : int
  (let loop [i n a 0 b 1]
    (if (= i 0)
      a
      (loop (- i 1) b (+ a b)))))

; recursive -- O(2^n) time, avoid for n > ~30
(defn fib-rec [n] : int
  (if (< n 2)
    n
    (+ (fib-rec (- n 1)) (fib-rec (- n 2)))))
```

```sweet-exp
; iterative -- O(n) time, O(1) space (self-tail-call -> loop; see below)
defn fib-iter [n] :int
  let loop [i n a 0 b 1]
    if ={i 0}
      a
      loop({i - 1} b {a + b})

; recursive -- O(2^n) time, avoid for n > ~30
defn fib-rec [n] :int
  if {n < 2}
    n
    {fib-rec({n - 1}) + fib-rec({n - 2})}
```

### Self-tail-call optimization

A **self-tail-call** -- a call to the enclosing function (or named-let `loop`
binding) in *tail position* -- is lowered to an iterative loop rather than a C
function call.  The compiler reassigns the parameter variables and jumps back to
the top of the function body, so iteration count no longer drives C-stack depth:
a self-recursive countdown of 10,000,000 iterations completes instead of
overflowing the stack.

The guarantee applies to:

- a self-recursive `defn` whose recursive call is in tail position, and
- the named-let / loop idiom `(let loop [...] ... (loop ...))`,

with tail position computed through `if`, `match` arms (guarded ones included),
`cond`/`when` (which macro-expand to `if`), `do`, `let`/`letrec`, and the LAST
operand of `and` / `or` -- `(and (>= n 0) (all-ok? (- n 1)))` is `if (!(n >=
0)) return false;` and a backedge (`tests/fixtures/tailcall-and-or-deep`).  For
example, both of these are lowered to a loop:

```turmeric no-check
; self-recursive defn -- tail call in the `if` else-branch
(defn count-down [n :int acc :int] :int
  (if (= n 0)
    acc
    (count-down (- n 1) (+ acc 1))))

; named-let -- the (loop ...) call is the self-tail-call
(defn sum-to [n :int] :int
  (let loop [i   :int 0
             acc :int 0]
    (if (>= i n)
      acc
      (loop (+ i 1) (+ acc i)))))
```
```sweet-exp
; self-recursive defn -- tail call in the `if` else-branch
defn count-down [n :int acc :int] :int
  if {n = 0}
    acc
    count-down({n - 1} {acc + 1})

; named-let -- the (loop ...) call is the self-tail-call
defn sum-to [n :int] :int
  let loop [i   :int 0
             acc :int 0]
    if {i >= n}
      acc
      loop({i + 1} {acc + i})
```

A named let lowers one of two ways -- to a lifted closure when the loop body
reads an enclosing variable, and to a plain lifted function when it does not
-- and both are optimized.  In the closure case the backedge reassigns the
loop parameters and leaves the captured environment alone (it does not change
across a self-call).  Pinned by `tests/fixtures/tco-named-let-capture-deep`
and `tests/fixtures/tco-named-let-nocapture-deep` at 5,000,000 iterations
each.

**Mutual tail calls.** Functions that tail-call each other in a cycle are
fused into one C function with a dispatch loop, so the cycle runs in constant
stack at any optimization level:

```turmeric no-check
(defn is-even? [n :int] :bool (if (= n 0) true  ^tailcall (is-odd?  (- n 1))))
(defn is-odd?  [n :int] :bool (if (= n 0) false ^tailcall (is-even? (- n 1))))
```

Each member keeps its own C function as a thin wrapper, so calls from outside
the cycle -- and non-tail calls from inside it -- are unchanged.  A group forms
when every member is a plain top-level `defn` (no closure, dictionary, inline-C,
effectful or `catch-unwind` body; no variadic, `fn`-typed or pass-by-pointer
parameter), all share one C return type, and the group stays within 8 members
and 16 parameters in all.  Pinned by `tests/fixtures/tailcall-mutual-deep` at
10,000,000 steps at `-O0`.  Before this, a small cycle passed at `-O2` only
because clang inlined it into a loop, and overflowed at `-O0`.

A cycle T5 does not fuse -- more than 8 members or 16 parameters -- still has
its calls in C tail position (`return f(args);`), and where the C compiler can
guarantee a tail call they are marked `musttail` too, so under **clang on
x86-64/aarch64** such a cycle also runs in constant stack at `-O0`
(`tests/fixtures/tailcall-musttail-deep`).  gcc and the JIT have no such
guarantee, so on those the calls are ordinary and `^tailcall` still refuses
them: the annotation promises a tail call on every toolchain.

**Boundary (1.0).** Self and mutual tail calls are optimized.  The following
are left as ordinary recursive calls -- correct, but not stack-optimized:

- **non-tail recursion** (e.g. `(+ n (sum-to (- n 1)))`, where work remains
  after the call returns) -- never eligible, by definition;
- a `match` whose arms cannot be put in tail position at all: one that yields
  no value, one with no arms, or an `offer` over a session channel.  An
  ordinary `match` -- ADT constructors, type-narrowing an `any`, literals,
  with or without `when`-guards -- **is** in the tail grammar;
- **mutual tail calls outside a group** -- a cycle with a member that is not a
  plain top-level function, a return type that differs, or more than 8 members
  / 16 parameters -- and **indirect** tail calls through a typed `fn` value
  (Saffron's dynamic calls through an `any` are handled; see below);
- self-recursive functions with pass-by-pointer struct, function-typed,
  poly-fn, or carrier-ABI (a by-value recursive ADT such as a `(Tree float)`)
  parameters -- a backedge cannot reassign them;
- a call under a `let` that binds a function value -- the whole `let` is off
  the tail path, whether or not the binding is live at the call.  A `let` that
  binds a carrier-ABI value (a `Vec`, a list, a `:heap` struct, a by-value
  recursive ADT) **is** in the tail grammar;
- a call under a `let` whose binding is released when the `let` ends (a
  by-value recursive spine it owns, a `^mut` cell a closure captured, a caught
  `Result` box, a fresh `Option`/`Result` box) and is used as more than a plain
  number while the backedge carries a non-number into the next iteration.  The
  release moves to the backedge; it can do that only once nothing that leaves
  the iteration could still point into what it frees;
- a self-recursive function with an explicit `defer` in the block around the
  call: a `defer` you wrote runs after the call, innermost first, and that
  order is observable, so it cannot move ahead of a backedge;
- a self-recursive function whose owned local (an `rc<T>`/`ref<T>`, a move-only
  Drop value, a by-value ADT with an owning field) is still **live** at the
  call -- passed as an argument, captured by a closure, borrowed, or read as
  anything but a plain number.  Its drop has to wait for the call to return.
  An owned local that is **dead** at the call is fine: its drop glue runs just
  before the backedge instead of just after the call, which nothing can
  observe.  `(let [b (ref 1)] (if (= n 0) acc (loop (- n 1) (+ acc @b))))` is a
  backedge; `(loop (- n 1) (count-refs x))` with `x` an `rc` is not.  This is
  the permanent half of the boundary, for the reason Rust has no guaranteed
  TCO: a value the callee may still use cannot be dropped before it runs;
- a self-recursive function that genuinely uses a control operator
  (`perform`/`handle`/`shift`/`await`) -- it is CPS-lowered, and the loop
  runs on the delimited-control path rather than as a C backedge.

#### Asking to be told: `^tailcall`

The boundary above is a list you have to hold in your head, and nothing in the
source says which side a given call landed on.  `^tailcall` annotates a call
with the assertion *this must become a real tail call*; a call the compiler
cannot place in tail position is then `TUR-E0716` at compile time, naming the
reason, instead of a stack overflow at an unpredictable depth later.

```turmeric no-check
(defn count-down [n :int acc :int] :int
  (if (= n 0)
    acc
    ^tailcall (count-down (- n 1) (+ acc 1))))   ; ok -- becomes a backedge

(defn not-tail [n :int] :int
  (if (= n 0)
    0
    (+ 1 ^tailcall (not-tail (- n 1)))))          ; TUR-E0716: argument position
```

The annotation is a prefix -- it binds to the call that follows it -- so it fits
in a `match` or `handle` arm, where an extra list element would silently re-pair
every clause after it.  The explicit `(^tailcall (f x))` spelling means exactly
the same thing, and is what `tur fmt` writes.

It changes nothing about how the call runs.  Removing it always silences the
error; it does not make the call a tail call, it only stops asking.

Every reason the boundary list gives has its own message.  `tur explain
TUR-E0716` prints them all, with what to do about each:

```
$ tur run --debug loop.tur
loop.tur:9:24: error [TUR-E0716]: `^tailcall` call is not in tail position: an
owned local of the enclosing `let` (a `ref<T>`, `rc<T>` or other value with drop
glue) is still live at the call -- it is passed, captured, borrowed or read as
more than a plain number -- so its drop cannot move ahead of the call
```

Two things worth knowing about the check itself:

- **It runs during C emission**, so `tur build`, `tur run` and `tur emit-c`
  perform it and `tur check` does not.
- **`--interpret` never reports it.**  The tree-walking evaluator trampolines
  direct, mutual and indirect tail calls alike, so under the interpreter every
  tail call already is one and the annotation is vacuously satisfied.  This is
  the one place the two engines genuinely differ rather than one lagging the
  other.

The annotation is also the test instrument for the rest of the tail-call work:
each later stage is verified by a fixture that annotates a call and asserts the
annotation holds at `-O0`.  A tail-call fixture built at `-O2` asserts nothing
-- clang will inline a small recursive cycle into a loop and the fixture then
measures the C compiler.  See
[proper-tail-calls-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/proper-tail-calls-plan.md).

**Dynamic tail calls in Saffron.** A call through an `any` in tail position --
`(f f (- n 1))` where `f` is a function value -- runs in constant stack too.
There is no callee to branch to, so it bounces instead: the call is recorded
and handed back to a trampoline loop one frame below, which makes it.  Every
Saffron function takes `any` arguments and returns an `any`, which is what
lets one loop make any such call.  Measured 2.7x *faster* than the nested
calls it replaced (`benchmarks/saffron-dyn-tail-results.md`), because each
nested call used to open and tear down a delimited-control entry.  Pinned at
10,000,000 deep at `-O0` by `tests/fixtures/tailcall-dyn-deep`; `^tailcall`
accepts a dynamic call and refuses one only for its position.

Indirect tail calls in **typed** Turmeric (through a `fn`-typed value) are not
built and are not planned: without one calling convention for every function
there is nothing uniform for a trampoline to call.  Routing them through the
existing CPS backend is not a way out either: that backend emits a tail call
as an ordinary call, a panic check, and a continuation invocation, which was
measured rather than assumed.
See
[proper-tail-calls-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/proper-tail-calls-plan.md)
for the measurements and the staging, and
[control-flow-completeness-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/control-flow-completeness-plan.md)
(Phase CF1) for the self-tail-call work that shipped.

### Prime sieve

The Sieve of Eratosthenes benefits from a `vec` (a growable array over a
`malloc`ed block) rather than a linked list.

> Corrected on 2026-08-20: this example previously used `vec/make`,
> `vec/get`, `vec/set!` and `(import "stdlib/vec.tur")`, none of which exist.
> The version below was run: it prints `25`, the number of primes below 100.

```turmeric
(load "stdlib/vec.tur")

(defn sieve-count [limit : int] : int
  (let [flags   (:: (vec-new) (Vec int))
        ^mut k  0
        ^mut i  2
        ^mut n  0]
    (while (<= k limit) (vec-push! flags 1) (set! k (+ k 1)))
    (vec-set! flags 0 0)
    (vec-set! flags 1 0)
    (while (<= (* i i) limit)
      (when (= (vec-get flags i) 1)
        (let [^mut j (* i i)]
          (while (<= j limit)
            (vec-set! flags j 0)
            (set! j (+ j i)))))
      (set! i (+ i 1)))
    (set! k 2)
    (while (<= k limit)
      (when (= (vec-get flags k) 1) (set! n (+ n 1)))
      (set! k (+ k 1)))
    n))
```

```sweet-exp
load("stdlib/vec.tur")

defn sieve-count [limit : int] : int
  let [flags   (:: (vec-new) (Vec int))
       ^mut k  0
       ^mut i  2
       ^mut n  0]
    while <=(k limit)
      vec-push!(flags 1)
      set!(k {k + 1})
    vec-set!(flags 0 0)
    vec-set!(flags 1 0)
    while <=(*(i i) limit)
      when =(vec-get(flags i) 1)
        let [^mut j (* i i)]
          while <=(j limit)
            vec-set!(flags j 0)
            set!(j {j + i})
      set!(i {i + 1})
    set!(k 2)
    while <=(k limit)
      when =(vec-get(flags k) 1)
        set!(n {n + 1})
      set!(k {k + 1})
    n
```

Note there is no numeric-range `for`: `for` is the monadic comprehension, so a
counted loop is a `while` over a `^mut` binding. A binding that `set!` writes
to must be declared `^mut` at its binding site.

### Monte Carlo pi estimation

Use the stdlib RNG rather than reaching into `<stdlib.h>` from inline-C, so the
compiler can see through the calls.

> Corrected on 2026-08-20: this example previously loaded `stdlib/rand.tur`
> and called `rand/float`. The module is **`stdlib/random.tur`** and the
> function is `rand-float`, which returns an **int in [0, 9999]** -- divide by
> 10000.0 for a float in [0, 1). `int->float` lives in `stdlib/math.tur` and is
> not auto-loaded. The version below was run and lands in range.

```turmeric
(load "stdlib/math.tur")
(load "stdlib/random.tur")

(defn estimate-pi [samples : int] : float
  (let [^mut i      samples
        ^mut inside 0]
    (while (> i 0)
      (let [x (/ (int->float (rand-float)) 10000.0)
            y (/ (int->float (rand-float)) 10000.0)]
        (when (<= (+ (* x x) (* y y)) 1.0) (set! inside (+ inside 1))))
      (set! i (- i 1)))
    (* 4.0 (/ (int->float inside) (int->float samples)))))
```

```sweet-exp
load("stdlib/math.tur")
load("stdlib/random.tur")

defn estimate-pi [samples : int] : float
  let [^mut i      samples
       ^mut inside 0]
    while >(i 0)
      let [x {int->float(rand-float()) / 10000.0}
           y {int->float(rand-float()) / 10000.0}]
        when <=({*(x x) + *(y y)} 1.0)
          set!(inside {inside + 1})
      set!(i {i - 1})
    {4.0 * {int->float(inside) / int->float(samples)}}
```

Note `rand-float` is seeded from `time(NULL)` on first use, so successive runs
differ -- do not pin an exact value in a test.

## Data structures

> The API names in this section were corrected on 2026-08-20. It previously
> documented `vec/make`, `vec/sort!`, `vec/fill!`, `hamt/insert` and
> `hamt/get-or`, none of which exist.

### Lists vs vecs

Use `list` / `cons` for functional transformations where sharing matters; use
`vec` for index-heavy access and mutation in place.

| Operation | `list` | `vec` |
|-----------|--------|-------|
| Prepend | O(1) | O(n) |
| Random read | O(n) | O(1) |
| Append (single) | O(n) | O(1) amortised |

The real `vec` surface is `vec-new`, `vec-push!`, `vec-get`, `vec-set!`,
`vec-len` (`stdlib/vec.tur`), plus the `[...]` literal, which lowers to
`vec-of` in expression position. See
[data-literals-guide.md](data-literals-guide.md).

### Hash maps

`stdlib/hamt.tur` (a persistent hash-array-mapped trie) is the standard map,
wrapped by `stdlib/map.tur`. The operations are `hamt/set` and `hamt/get` --
persistent, so each `set` returns a new map sharing structure with the old.
`#map{...}` literals construct one directly.

For a key that must outlive its source buffer, use `String` rather than a
computed `cstr`: `MapKey` for `String` copies and owns the key bytes, where a
`cstr` key can dangle. See [strings-guide.md](strings-guide.md).

### Sorting

No `vec/sort!` ships today. Sort by moving through a `list` or writing the
comparison loop directly over the `vec`; if you add a sort to `stdlib/vec.tur`,
this section should name it.

## String and text processing

> Corrected on 2026-08-20. This section previously documented `str/concat`,
> `str/builder`, `str/view`, `str/format` and `stdlib/regex.tur`. None of them
> exist.

Turmeric has three string-shaped types and the choice between them is the
performance decision that matters. `cstr` is a borrowed `const char *` with no
length; `str` (`stdlib/str.tur`) is a borrowed pointer+length view, so
substring-without-copy over a buffer you already own; `String`
(`stdlib/string.tur`) owns refcounted immutable bytes, so `Clone` is an O(1)
retain and structural sharing in a persistent map is free.

Repeated concatenation is the usual hot spot: each concat allocates and copies,
so building a string in a loop is quadratic. Prefer accumulating pieces and
joining once, and prefer a `str` view over copying a substring out.

There is no regex engine in the tree. `stdlib/cstr.tur` provides the byte
primitives (`cstr-len`, `cstr-nth`, `cstr-sub`, `cstr-eq?`) that scanning code
is written against.

See [strings-guide.md](strings-guide.md) for the full comparison.

## Concurrency and parallelism

> Corrected on 2026-08-20. This section previously documented
> `stdlib/concurrency.tur` and `stdlib/dynamic-vars.tur`; neither exists. The
> dynamic-variable module is `stdlib/dynvar.tur`.

Threads and channels live in `stdlib/concurrent.tur` (mutexes, rwlocks,
condvars, and the `mutex-guard-*` pair behind the `with-lock` /
`with-read-lock` / `with-write-lock` macros). `stdlib/schan.tur` provides
session-typed channels, where the protocol is a phantom threaded through each
operation, so a send that must precede a receive is checked at compile time.
`stdlib/stm.tur` and `stdlib/stm-sync.tur` cover transactional variables and
TMVar/TChan.

Thread-local and dynamically-scoped state is `stdlib/dynvar.tur`
(`defdynamic` / `let-dyn`).

The costs worth knowing: a thread is an OS thread, so creation is not free and
a work-queue over a fixed pool beats spawning per item; an uncontended
`with-lock` is cheap while a contended one parks; and STM retries the whole
transaction on conflict, so keeping transactions short matters more than
keeping them few.

See [threading-guide.md](threading-guide.md) and [stm-guide.md](stm-guide.md).

## Memory and allocation

> Corrected on 2026-08-20. The GC-pressure benchmark scripts this section
> referenced (`scripts/run_all.sh`, `analyze_results.py`) do not exist; see
> [Benchmarking methodology](#benchmarking-methodology) for the real harness.

Turmeric is refcounted, not tracing, so the cost model is retain/release
traffic and drop glue rather than collection pauses. Practical consequences:

- `Clone` on a `String` or an `Arc` is a refcount bump, not a copy -- sharing
  is cheap and does not need avoiding.
- A cycle is never reclaimed by refcounting alone. `tests/` carries a
  `tur_stdlib_no_rc_cycles` check for exactly this in the stdlib.
- `(gc-auto!)` exists and is strictly opt-in; it is not becoming the default.
- A by-value struct parameter is copied on bind, so a wide struct passed
  through a hot loop is worth passing by pointer or borrowing.

See [gc-guide.md](gc-guide.md) for how RC, arenas, and the cycle collector fit
together, and [ownership-guide.md](ownership-guide.md) for deciding who owns
what in the first place.

## Recursion and stack usage

> Corrected on 2026-08-20. This section previously pointed at
> `stdlib/trampoline.tur`, which does not exist.

Self-tail-calls are optimised into a loop -- see
[Self-tail-call optimization](#self-tail-call-optimization) above, which is
measured and accurate. A tail-recursive accumulator therefore runs in constant
stack and is the idiomatic way to write a loop over a list.

Mutual recursion is **not** turned into a loop: `even?` calling `odd?` calling
`even?` grows the stack, so deep mutual recursion needs restructuring into a
single self-recursive function with an explicit state parameter, or into an
explicit worklist. There is no trampoline module to reach for.

Generators (`gen` / `yield`) and the CPS-lowered paths have their own cost
model; the tree-walking interpreter retains roughly 4 KiB per trampolined step,
which is a memory multiplier under `--interpret` and nothing at all compiled.

## I/O operations

> Corrected on 2026-08-20. This section previously documented `io/open` and
> `io/read-all`, neither of which exists.

`stdlib/io.tur` provides `read-file` (whole file into a malloc'd,
NUL-terminated buffer; NULL on error; **caller frees**) and `write-file`, plus
the lower-level file handle operations. `stdlib/fs.tur` covers path and
directory work.

The performance points that hold: one `read-file` beats a per-line read loop
for a file that fits in memory; writes should be batched rather than issued
per record; and the returned buffer is owned by the caller, so a read in a loop
that never frees is a leak, not merely garbage.

Asynchronous file and socket I/O is a separate surface -- see
[async-await-guide.md](async-await-guide.md).

## Real-world algorithms

### N-body simulation

Float-heavy simulations benefit from struct-of-arrays layout when possible.
Turmeric structs are currently records (array-of-structs), so prefer
separating coordinate vectors into dedicated `vec`s if profiling reveals cache
pressure:

```turmeric
(defstruct body [x :float y :float z :float
                 vx :float vy :float vz :float
                 mass :float])
```

```sweet-exp
defstruct body [x :float y :float z :float
                vx :float vy :float vz :float
                mass :float]
```

### Ray tracing

Ray-box intersection and dot products are the hot paths. Annotate return types
concretely (`float`) so the elaborator does not insert tag checks in the inner
loop:

```turmeric
(defn dot [ax ay az bx by bz] :float
  (+ (* ax bx) (+ (* ay by) (* az bz))))
```

```sweet-exp
defn dot [ax ay az bx by bz] :float
  {{ax * bx} + {{ay * by} + {az * bz}}}
```

---

## Execution engines: interpreter vs `tur jit` vs `cc -O2`

Turmeric has three execution engines, and which one is fastest depends on
what you are optimizing for -- startup latency or steady-state throughput:

| engine | invocation | compile step | best for |
|---|---|---|---|
| interpreter | `tur --interpret f.tur` | none | tiny scripts, REPL turns, debugging |
| MIR JIT | `tur jit f.tur` | in-process (c2mir) | run-edit-run loops, spice REPL reloads |
| cc | `tur build f.tur` + run | subprocess cc -O2 | long-running programs, deployment |

A project can select its default engine for `tur run` declaratively:
`:engine "cc" | "jit" | "interp"` in `build.tur`, overridden by `TUR_ENGINE`
in the environment, overridden by `--engine` on the command line (the same
ladder shape as `:build-dir`).  An unknown value is a hard error
(TUR-E0311), and a `"jit"` selection on a build without the engine, or
without the `jit` experiment enabled, fails loudly rather than silently
substituting -- the engines differ in SEMANTICS (`#?(:tur ... :turi ...)`,
inline-C carve-outs, c2mir divergences), not just speed, so pair a
load-bearing `:engine` with a `:tur-version` floor (older binaries silently
ignore unknown manifest keys and run under cc).

Measured triangle (x86-64 Linux, Release `tur`, best of 5, end-to-end wall
time; `bash benchmarks/run-triangle.sh` regenerates this from
`benchmarks/triangle/`):

| program | interpreter | tur jit | cc build | cc run | cc total |
|---|---|---|---|---|---|
| fib (fib 27, call-heavy) | 137ms | 127ms | 178ms | 2ms | 180ms |
| loop-sum (5M-iteration loop) | 1567ms | 137ms | 180ms | 3ms | 183ms |
| mandel (float inner loop) | 639ms | 142ms | 187ms | 5ms | 192ms |

All three legs are re-measured together on each run, so the columns are
comparable to each other. They are NOT comparable to a snapshot taken on a
different machine.

How to read it:

- **The front end dominates one-shot latency on every engine.** Roughly
  200ms of each cell is elaboration and codegen shared by all three legs;
  the engines differ in what happens after. For a program this small the
  interpreter's zero-compile leg makes it competitive end to end even
  while its loop throughput is 9x behind (loop-sum).
- **`tur jit` beats the cc round trip end to end**, by ~25-30% on these
  programs. Code generation is serialized and LAZY -- only the functions a
  run actually calls get compiled (worth 23-36% off the JIT leg alone
  versus eager generation). Its other advantage is structural, being IN PROCESS: no
  subprocess, no disk artifacts, and the spice REPL reload path is ~3.2x
  faster than the `tur build --shared` round trip it replaces (see the
  repl guide). `TUR_JIT_GEN=eager` restores whole-program generation, which
  is slower but compiles every function up front.
- **Compiled native runtime is 4-7ms** for these workloads -- for any
  long-running or repeatedly-invoked program, `tur build` once and run
  the binary; nothing else is close in steady state.
- The MIR tier generates good-but-not-gcc code: expect JIT'd loop bodies
  within ~1-2x of cc -O2, not parity, and note the JIT runs the program
  on a sized entry stack (`TUR_JIT_STACK_MB`, default 1024 on 64-bit) because
  MIR does not perform gcc's sibling-call optimization -- so a deep
  recursion the cc path survives only because gcc turned the self-call into
  a jump will overflow here.  That is a real difference in what the two
  engines forgive, and it is worth knowing which of your loops are actually
  lowered to loops (see the self-tail-call section above).

The engine triangle is exact on OUTPUT: `benchmarks/run-triangle.sh`
refuses to time a program whose three engines disagree, and the fixture
corpus runs under all three harnesses (`tests/run.sh`, `tests/run-turi.sh`,
`tests/run-jit.sh`).

## Cross-language comparison

A separate suite, `performance-comparison/` at the project root, runs the
same 21 benchmarks (numerical, data structures, string processing,
concurrency, memory, recursion, I/O, real-world, micro) across up to nine
columns: C, Turmeric (`tur build`), turi (`tur --interpret`), `tur jit`,
Rust, Haskell, Clojure, Racket, and Python. This is the `scripts/run_all.sh`
/ `aggregate_results.py` / `check_environment.sh` harness -- it lives under
`performance-comparison/`, not the top-level `benchmarks/` this guide's
methodology section otherwise describes, and answers a different question:
not "did this Turmeric change regress," but "how does Turmeric's output
compare to a native-compiled, a JVM-hosted, and a scripting-language peer
doing the same work." See
[performance-comparison/README.md](https://github.com/turmeric-lang/turmeric/blob/main/performance-comparison/README.md)
and its `docs/methodology.md` for the full setup, same-algorithm ground
rules, and how to run it.

Two things any reading of that suite's numbers should keep in view:

- **Every column runs the same algorithm**, not each language's idiomatic
  rewrite -- a linked cons chain in Rust for `list_ops`, not a `Vec`; a real
  `std::thread` + `pthread`-equivalent ring for `thread_ring`, not a
  green-thread substitute. Where a language's natural analogue genuinely
  differs (`Data.IntMap.Strict` / `std::collections::HashMap` / Turmeric's
  HAMT for `hash_map`), that's stated in the methodology doc, not picked
  silently for whichever looks fastest.
- **A column can be absent for toolchain reasons that have nothing to do
  with the language's performance** -- Clojure needs a JVM; not having one
  installed makes every Clojure row `absent`, not `0ms`. `check_environment.sh`
  prints this matrix before a sweep starts specifically so an incomplete
  toolchain reads as an incomplete report, not a suspiciously fast column.

## Benchmarking methodology

### The real harness

Benchmarks live in `benchmarks/` and are run by
`benchmarks/run-benchmarks.sh` -- all of them, or one by name:

```sh
./benchmarks/run-benchmarks.sh            # every benchmark
./benchmarks/run-benchmarks.sh bench-logic-query
```

The layout it expects:

| File | Role |
|---|---|
| `benchmarks/<name>.tur` | the benchmark source |
| `benchmarks/<name>-baseline.c` | optional monomorphic C baseline to compare against |
| `benchmarks/<name>.time` | optional upper bound in milliseconds |

Results are written to `benchmarks/benchmark-results.md`.

### A benchmark does not time itself

This is the part the old template got backwards. The **runner** measures wall
time around the built executable (`measure_time` in `run-benchmarks.sh`); the
benchmark itself just does the work. So a benchmark needs no clock, no
argument parsing, and no `elapsed_ns=` output -- it is an ordinary program with
a `main`, and `benchmarks/bench-logic-query.tur` and friends are the models to
copy.

If a benchmark does need to size its work from the command line, read `*args*`
or use `args/parse` from `stdlib/args.tur`. Reading `g_tur_args` directly, or
via a hand-rolled `parse-first-arg`, is forbidden -- see CLAUDE.md.

### Build flags

The runner compiles with `-O2` by default and honours `TUR_CC_FLAGS` and `CC`:

```sh
CC=clang TUR_CC_FLAGS="-O3 -march=native" ./benchmarks/run-benchmarks.sh
```

### Reading a result honestly

- Compare against the `-baseline.c` where one exists; an absolute number on an
  unspecified machine is not a result.
- A `.time` bound is a regression tripwire, not a target -- it is an upper
  bound chosen for the slowest machine expected to run it.
- Run one benchmark at a time. A concurrent build or test suite competing for
  cores makes the number meaningless, and this repo has been bitten by exactly
  that in its test suite (see
  [test-suite-portability-guide.md](test-suite-portability-guide.md)).
- The three-engine comparison has its own runner,
  `benchmarks/run-triangle.sh`; see
  [Execution engines](#execution-engines-interpreter-vs-tur-jit-vs-cc--o2)
  above.

## Performance checklist

Use this list before calling a hot path done:

- [ ] Built with `just release` (not `just build`)
- [ ] Numeric types annotated concretely (`int`, `float`, not inferred `any`)
- [ ] Hot loops use `vec` instead of list where random access or mutation is
      needed
- [ ] No repeated string concatenation inside loops -- accumulate the
      pieces and join once; prefer a `str` view over copying a substring
- [ ] Recursive functions in tail position (verified by running with a large
      input without stack overflow)
- [ ] Profiled with `time` and at least 5 iterations; CV < 10%
- [ ] Input size swept from small to large to confirm O-complexity matches
      expectation

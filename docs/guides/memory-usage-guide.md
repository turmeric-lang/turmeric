---
title: Memory Usage Guide
category: Performance
description: What each language feature costs in heap memory, measured -- scalars, structs, sums, lists, Vec, MutableMap, Map, String, rc, closures, function-value calls, effects and regions -- what frees it, and strategies for using less
---

# Memory Usage Guide

This guide puts a number on what each Turmeric feature costs in heap memory,
says what frees that memory, and lists strategies for using less. Use it when
a program is bigger than you expect, or when you are choosing a data
structure and want to know what it costs.

It covers compiled programs (`tur build`, `tur run`). The interpreter and the
two dynamic dialects work differently and get their own short section at the
end. For *why* memory is managed the way it is, read
[gc-guide.md](gc-guide.md). For *who should own* a value, read
[ownership-guide.md](ownership-guide.md).

---

## The model in one paragraph

Turmeric has no tracing collector in the default build. A value is either
**free** (it lives in registers, on the C stack, or inline in its parent), or
it is a **heap block** with exactly one of these ways to be reclaimed:

1. **Scope exit.** The compiler emits a drop when the owner goes out of
   scope. This covers `rc<T>`, capturing closures, a by-value struct with an
   owning field, and a by-value recursive `defdata` whose spine it can prove
   is owned.
2. **An explicit free** that you write: `vec-free`, `mutmap-free`,
   `map-free`, `string/release`, and the `free` of a `cstr` that a stdlib
   function returned.
3. **A region.** `with-region` (or `bt-scope`) rewinds every `:heap` node
   allocated inside it in one step, if the compiler can prove nothing escapes.
4. **Never, by design.** A `:heap` node allocated outside a region and a
   `:copy` recursive value live until the process exits.

Most memory problems come from a value you assumed was in group 1 being in
group 2 or 4.

---

## How the numbers were measured

Every number below was measured, not estimated. The method:

- A small probe program builds N values (N = 0 and N = 10 000) with a
  Release build of `tur` on x86-64 Linux with glibc.
- **Traffic** is valgrind memcheck's `total heap usage` (bytes and
  allocations), N minus 0, divided by N. It counts every allocation, even
  ones that are freed straight away.
- **Peak** is the largest `mem_heap_B + mem_heap_extra_B` sample from
  `valgrind --tool=massif`, N minus 0, divided by N. It includes malloc's own
  bookkeeping (on glibc, about 8 to 16 bytes a block, with sizes rounded up
  to 16).
- **Leaked** is what memcheck reports as lost at exit.

Traffic tells you how hard the allocator is working. Peak tells you how big
the process gets. Small, freed-at-once allocations show up in traffic but not
in peak.

Here is the harness, so you can measure your own shapes. Write the probe with
`__N__` where the element count goes:

```sh
#!/bin/bash
# measure.sh probe.tur -- per-element heap traffic and peak, N=10000 vs N=0
f=$1; N=${2:-10000}
run() {
  sed "s/__N__/$1/g" "$f" > m_$1.tur && tur build m_$1.tur -o m_$1.bin >/dev/null || exit 1
  valgrind ./m_$1.bin 2>&1 >/dev/null |
    awk '/total heap/ {gsub(",",""); print $5, $9}'
  valgrind --tool=massif --massif-out-file=ms_$1.out ./m_$1.bin >/dev/null 2>&1
  awk -F= '/mem_heap_B/ {h=$2} /mem_heap_extra_B/ {t=h+$2; if (t>m) m=t} END {print m}' ms_$1.out
}
{ read a0 b0; read p0; } < <(run 0)
{ read a1 b1; read p1; } < <(run $N)
echo "allocs/elt=$(( (a1-a0)/N ))  bytes/elt=$(( (b1-b0)/N ))  peak/elt=$(( (p1-p0)/N ))"
```

The numbers are a snapshot of one build (2026-10-05). They move when the
runtime or the codegen changes. Re-measure before you lean on one.

---

## The cost table

"Per element" means per value, cell, entry or call, depending on the row.

| Feature | Heap per element | Allocations | Reclaimed by |
| --- | --- | --- | --- |
| `:int`, `:float`, `:bool`, `:cstr` literal | 0 | 0 | -- (stack or static) |
| By-value `defstruct` (`(defstruct P [x : int y : int z : int])`), passed and returned | 0 | 0 | -- (copied by value) |
| `Option`/`Result` of a scalar, constructed and matched | 0 | 0 | -- (by value) |
| `:heap` `defstruct` / `defdata` node (3 ints) | 24 B (40 B peak with malloc overhead) | 1 | a region, otherwise never |
| By-value recursive `defdata` link (`(ICons [hd : int tl : IL])`) | 24 B | 1 | scope exit, in the shapes [gc-guide.md](gc-guide.md#known-gaps) lists; a region |
| stdlib `(Cons int)` cell (`tcons-of`) | 16 B (24 B peak) | 1 | a region, otherwise never |
| `Vec int` slot | 8 B, doubling from 4 (~19 B peak, ~26 B traffic incl. growth copies) | amortised 0 | `vec-free` |
| `Vec P` slot, `P` wider than 8 B | 8 B slot + a boxed copy of `P` (~53 B peak for a 24 B `P`) | 1 | `vec-free` (frees the boxes too) |
| `MutableMap int int` entry | 32 B slot at load factor <= 0.75 (~78 B peak, ~105 B traffic) | amortised 0 | `mutmap-free` |
| `Map int int` entry (persistent HAMT) | ~66 B live; ~720 B and ~7 allocations of traffic per insert | ~7 per insert | `map-free` of each version you drop |
| `String` | 16 B header + length + 1 | 1 | `string/release` (refcounted) |
| `rc<T>` | 72 B control block + a separate block for the value | 2 | last drop |
| first `rc<T>` in a program | one-time 544 KiB (512 KiB free queue + 32 KiB GC registry) | 2 | process exit |
| capturing closure | 8 B drop-glue pointer + 8 B code pointer + 8 B per capture | 1 | scope exit (closure drop glue) |
| named `defn` used as a value | 0 (its fat box is a static) | 0 | -- |
| a call through a function value | ~200 B of continuation frames | 2 | when the outermost direct-style call returns (see below) |
| `perform` + `resume` under `handle` | ~630 B | 6 | when the `handle`'s outermost direct-style call returns |
| region generation | 64 KiB slabs; nodes inside cost their size and no malloc each | 1 per 64 KiB | rewind at bracket exit, if nothing escapes |

A few rows need more than a cell.

### By-value data is free

A `defstruct` without `:heap`, an `Option`, a `Result` and a non-recursive
`defdata` are C structs. They are passed, returned and matched by value, and
they never touch the heap:

```turmeric
(defstruct P [x : int y : int z : int])

(defn mk [i : int] : P (P i i i))
(defn sum [p : P] : int (+ (.x p) (+ (.y p) (.z p))))

(defn half [i : int] : (Option int)
  (if (= i (* 2 (/ i 2))) (Some (/ i 2)) (None)))

(defn main [] : int
  (println (sum (mk 3)))                         ; 9, no heap
  (println (match (half 8) (Some h) h (None) 0)) ; 4, no heap
  0)
```

Measured: 0 allocations per call for both shapes. A wide struct still costs a
copy every time it is passed, which matters for speed (see
[performance-guide.md](performance-guide.md#memory-and-allocation)) but not
for memory.

### `:heap` nodes live until a region ends or the process does

`:heap` on a `defstruct` or `defdata` makes each value a malloc'd node shared
by pointer. Nothing frees such a node one at a time: a persistent structure's
nodes have no single owner, so neither a refcount nor a scope-exit drop can
know when the last reference goes. The stdlib's `(Cons A)` list is `:heap`.

Outside a region, every node you allocate stays allocated until exit
(measured: 24 B per 3-int node, all of it reported lost). Inside a
`with-region` that rewinds, the same nodes cost no individual mallocs and are
reclaimed together. That is what regions are for -- see
[strategy 4](#4-bracket-allocation-heavy-phases-in-with-region).

### Containers

Per entry, from smallest to largest:

| Container | Peak per entry | Mutation | Frees |
| --- | --- | --- | --- |
| `Vec int` | ~19 B | in place | `vec-free` |
| `Map int int` | ~66 B | a new version per insert | `map-free` per version |
| `MutableMap int int` | ~78 B | in place | `mutmap-free` |

`Vec` holds a 24-byte header and a buffer of 8-byte slots that doubles when
full, so a `Vec` can be up to twice as large as its contents. An element
wider than 8 bytes -- a by-value struct of two or more fields, a wide `defdata`
-- does not fit in a slot, so each `vec-push!` mallocs a box for it. A
`Vec` of 24-byte structs costs about 53 bytes per element, not 24.

`MutableMap` is open-addressed: 32-byte slots, resized to double when the
load factor reaches 0.75.

`Map` (and `Set`, which is built on the same HAMT) is persistent: every
`map-assoc` returns a new map that shares structure with the old one. That
makes old versions cheap to keep, but **each version is a separate object you
must `map-free`.** A loop that rebinds a map without freeing the previous
version keeps every version: measured 1.7 KB per insert, all of it leaked.
When you free each old version, a live entry costs about 66 bytes: a
24-byte leaf node, a 32-byte entry, and its share of the bitmap nodes above
it. (Before 2026-10-07 every node was allocated at the size of a full
32-slot node, and an entry cost ~360 bytes --
[hamt-nodes-allocated-at-full-array-size](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/hamt-nodes-allocated-at-full-array-size.md).)
What a persistent map still costs is traffic: each insert copies the path
from the root, about 7 allocations and 720 bytes.

None of the three containers is freed at scope exit. A compiled `Vec`,
`MutableMap` or `Map` local that you do not free lives until the process
exits.

### Strings

| Type | What it is | Heap |
| --- | --- | --- |
| `cstr` literal | a pointer to static bytes | 0 |
| `cstr` returned by a stdlib function | a malloc'd buffer | yours to free |
| `str` | a pointer and length into a buffer you already have | 0 |
| `String` | one block: 16-byte header, the bytes, a NUL | 1 malloc; refcounted |

`string/concat` copies both inputs into a new `String`. Building a string by
concatenating in a loop therefore copies everything built so far, every time:
10 000 one-character appends cost about 5 KB of traffic **per character**.
A `StringBuilder` grows its buffer by doubling, so the same build costs about
4 bytes of traffic per character. See [strategy 6](#6-build-strings-with-a-builder).

### `rc<T>`

`(rc/of v)` makes two allocations: a 72-byte control block (counts, a drop
function, a cycle-collector walker, GC bookkeeping) and a separate block for
the value. Measured: 2 allocations per `rc/of`, of an `int` or of a struct,
freed at the last drop.

The first `rc/of` in a program also allocates the deferred-free queue
(512 KiB) and the cycle collector's block registry (32 KiB). Those live until
exit. In a small program that uses one `rc`, they are most of the heap.

### Closures

A capturing lambda is one malloc of `8 + 8 + 8 * captures` bytes: a pointer
to its drop glue, the code pointer, and the captured values. A closure bound
in a `let` is freed when the `let` ends. A named `defn` passed as a value
costs nothing: its fat box is allocated once, statically.

In a loop written as recursion, a closure `let`-bound in the body and only
ever called is freed at the jump back to the top of the loop. One that the
body passes on as an argument -- to the loop's own next call, or to a function
that only calls it -- is kept until the outermost call into the loop returns,
then freed. (Until 2026-10-07 a closure returned by a call, like `(mk i)`
below, was never freed in that position:
[closure-let-in-self-tail-loop-leaks](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/closure-let-in-self-tail-loop-leaks.md).)

### Calls through function values, and effects

A function that calls a function-typed value -- a parameter `f`, a closure
in a `let`, a field holding a function -- is emitted in continuation-passing
style, and each such call allocates continuation frames: measured 2
allocations and about 200 bytes per call. A `perform` handled by `handle`
costs about 6 allocations and 630 bytes.

These frames are freed in batches, when the **outermost** direct-style call
returns. What that means in practice depends on how the loop around them is
written:

| Loop shape | Peak memory |
| --- | --- |
| `while` loop that calls the function each iteration | constant |
| self-tail-recursive loop that calls it each iteration | grows ~200 B per iteration until the loop returns |

So a long-running loop that calls through function values, or performs
effects, should be a `while` loop
([fn-value-call-cps-frames-held-until-outer-entry](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/fn-value-call-cps-frames-held-until-outer-entry.md)).
An `#fx{}` annotation on the function type does not change this today.

---

## What frees what

| You allocated | It is freed when | If you do nothing |
| --- | --- | --- |
| `rc<T>` | the last strong handle drops | freed (unless it is in a cycle and the collector is off) |
| a capturing closure in a `let` | the `let` ends | freed |
| a capturing closure as the payload of a by-value `Option`/`Result` local | the `let` ends, when every use is a `match` that only calls it or a `some?`-style tag predicate | otherwise leaked ([sum-closure-payload-never-dropped](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/sum-closure-payload-never-dropped.md)) |
| a by-value struct with an `rc` or `ref` field | the owning local ends | freed |
| a by-value recursive `defdata` local | the local ends, when the compiler proves it owns the spine | freed in the proven shapes; otherwise leaked |
| `Vec`, `MutableMap`, `Map`, `Set` | you call `vec-free` / `mutmap-free` / `map-free` | leaked |
| `String` | its count reaches 0 via `string/release` | leaked |
| a `cstr` returned by a stdlib function | you `free` it (or adopt it into a `String`) | leaked |
| a `:heap` node | its region rewinds | leaked |
| a `:copy` recursive `defdata` value | its region rewinds | leaked |
| anything a region retires | process exit | -- |

"Leaked" here means "kept until the process exits". For a short-lived
program that is often fine. For a server, a game loop or anything that runs a
loop many times, it is the bug.

---

## Strategies

### 1. Prefer by-value data

The cheapest heap block is the one you never allocate. A `defstruct` without
`:heap`, an `Option`, a `Result` and a non-recursive `defdata` are free.
Reach for `:heap` only when you need sharing by pointer or a recursive
shape.

### 2. Pick the container by its per-entry cost

If you need an index or a stack, use a `Vec` (~19 B per `int`). If you need
lookups and mutate in place, use a `MutableMap` (~78 B per entry). Use a
persistent `Map` (~66 B per live entry) when you need its persistence: several
versions alive at once, or a value you pass around and must not see change.
A `Map` used as a mutable table is no larger than a `MutableMap`, but every
insert allocates a new path (~7 allocations) and each old version needs its
own `map-free`.

For a `Vec` of wide structs, two cheaper layouts avoid the per-element box:

- **Structure of arrays.** Keep one `Vec` per field (`xs`, `ys`, `zs`)
  instead of one `Vec` of `P`. Each element is then 8 bytes in a slot, with
  no box.
- **`:heap` elements inside a region.** If the `Vec` and its elements all die
  together, allocate the elements as `:heap` nodes inside a `with-region` and
  let the region reclaim them.

For many small flags, `stdlib/sized-bits.tur`'s `SizedBitVec` stores one bit
per flag instead of 8 bytes per slot.

### 3. Free every container, and every version of a persistent one

Pair every `vec-new`, `mutmap-new` and `map-new`/`#map{}` with its free.
For a persistent map you rebind in a loop, free the version you are dropping:

```turmeric
(load "stdlib/map.tur")

(defn fill [m : (Map int int) i : int n : int] : (Map int int)
  (if (= i n)
    m
    (let [m2 (map-assoc m i (* i i))]
      (map-free m)              ; drop the old version, keep m2
      (fill m2 (+ i 1) n))))

(defn main [] : int
  (let [m (fill (:: #map{} (Map int int)) 0 1000)]
    (println (map-count m))     ; 1000
    (map-free m)
    0))
```

`map-free` is safe under structural sharing: it decrements each node's count
and frees only the nodes no other version uses. Without the `(map-free m)`
line, this loop keeps every intermediate version. A `#map{...}` literal
already frees its own intermediate versions.

For a bulk build of a raw HAMT, a transient avoids the intermediate versions
altogether (`hamt/transient`, `hamt/transient-set!`, `hamt/persistent!`; see
[hamt-guide.md](hamt-guide.md#transient-mode)).

### 4. Bracket allocation-heavy phases in `with-region`

A region is the only way to reclaim `:heap` nodes, and the cheapest way to
reclaim any batch of them: the nodes cost no malloc each, and the whole
generation is reclaimed in one step. Put work that builds a temporary
structure and returns a small answer inside `with-region`:

```turmeric
(defdata Tree :heap (Leaf) (Node [l : Tree v : int r : Tree]))

(defn build [lo : int hi : int] : Tree
  (if (> lo hi)
    (Leaf)
    (let [mid (/ (+ lo hi) 2)]
      (Node (build lo (- mid 1)) mid (build (+ mid 1) hi)))))

(defn total [t : Tree] : int
  (match t
    (Leaf) 0
    (Node l v r) (+ v (+ (total l) (total r)))))

(defn rounds [k : int acc : int] : int
  (if (= k 0)
    acc
    (rounds (- k 1)
            (+ acc (with-region (fn [] : int (total (build 1 10000))))))))

(defn main [] : int
  (println (rounds 50 0))   ; 2500250000, in the memory of one round
  0)
```

The region rewinds at the end of each round, and the next round reuses its
slabs, so 50 rounds peak at the memory of one.

A region rewinds only if the compiler can prove that nothing allocated
inside is still reachable after the bracket. When it cannot, it **retires**
the generation instead: the memory is kept until process exit, exactly as if
there were no region. Retiring is always safe; it just saves nothing. A
generation retires when, inside the bracket:

- the result can reach a node (return an `int`, a `cstr` or a by-value
  record of scalars, not a node, closure or pointer);
- a node is stored somewhere that outlives the bracket (`vec-push!` into an
  outer `Vec`, `set!` on a global, `map-assoc` into an outer map);
- a node is erased to `:int`, `ptr<void>` or `Any` (`(:: node :int)`),
  unless the erased word is only compared with `0` (`(= (:: l :int) 0)`,
  the null test);
- a node is passed to a function whose body is inline C.

Walking a stdlib `(Cons A)` with `tnil?`, `thead`, `ttail` and `tlength`
inside a region rewinds. Building one does too with `tcons-of`, but not with
`tcons`: its tail parameter is the carrier-level `:int`, so passing a typed
list to it erases the node and the generation retires. Inside regions, build
with `tcons-of`.

**Check that your regions rewind.** Run the program with
`TUR_REGION_STATS=1`; it prints one line at exit:

```
region-stats: pushes=50 rewinds=50 retires=0
```

Every retire is a region that saved nothing. If you see retires you did not
expect, look for one of the four escapes above.

### 5. Use `rc<T>` for sharing, not by habit

`rc<T>` costs two allocations and about 100 bytes per value, and the first
one in a program costs 544 KiB. If the value has one owner, pass it by value
or `^borrow` it instead
([ownership-guide.md](ownership-guide.md#the-smell-an-rct-that-is-not-pulling-its-weight)).
When you do share, put one `rc` around a struct rather than an `rc` around
each of its fields.

An `rc` (or any value with drop glue) that is still live at a self tail call
also stops that call from becoming a loop: its drop has to run after the
call, so each iteration keeps a stack frame and its `rc` until the recursion
unwinds. Measured: about 144 bytes of peak memory per iteration. Write
`^tailcall` on the call to be told when this happens; it reports TUR-E0716
and names the live local. Then move the `rc` into a helper that returns a
plain value, so it is dead before the call.

### 6. Build strings with a builder

Concatenation in a loop is quadratic. A builder is linear:

```turmeric
(load "stdlib/string.tur")

(defn add-xs [b : StringBuilder i : int n : int] : int
  (if (= i n)
    0
    (do (builder/push-cstr! b "x")
        (add-xs b (+ i 1) n))))

(defn main [] : int
  (let [b (builder/new)]
    (add-xs b 0 10000)
    (let [s (builder/finish b)]
      (println (string/len s))   ; 10000
      (string/release s)))
  0)
```

Also:

- Use a `str` view, not a copied `String`, to look at part of a buffer you
  already own.
- When a stdlib function hands you a malloc'd `cstr`, either `free` it or
  adopt it with `string/adopt-cstr`, which takes ownership.
- Release every `String` you create. See
  [strings-guide.md](strings-guide.md#memory-management--common-pitfalls).

### 7. Write long-running loops as `while` loops

A `while` loop and a self-tail-recursive function compile to the same jump,
but they are not the same for memory today:

- When the loop body calls a function value or performs an effect, a `while`
  loop frees the continuation frames each iteration; a recursive loop keeps
  all of them until it returns.
- A capturing closure `let`-bound in a `while` body is freed each iteration.
  In a self-tail-recursive body it is freed each iteration only when the body
  just calls it; one passed on as an argument is kept until the loop returns.

```turmeric
(defn mk [k : int] : (fn [int] int) (fn [x : int] : int (+ x k)))

(defn main [] : int
  (let [^mut i   0
        ^mut acc 0]
    (while (< i 100000)
      (let [f (mk i)]               ; freed at the end of each iteration
        (set! acc (+ acc (f 1))))
      (set! i (+ i 1)))
    (println acc))
  0)
```

For a loop that runs a handful of times, either form is fine. For an event
loop, a server's accept loop or a simulation's main loop, use `while`.

### 8. Turn the collector on only for cycles

The cycle collector is off by default and only matters if you build cycles
out of `rc<T>` values. Prefer to avoid the cycle with `weak<T>` for the back
edge. If you need the collector, `(gc-auto!)` collects at allocation
checkpoints; `(gc-live-blocks)` and `(gc-candidate-high-water)` tell you
whether it is keeping up. A cycle that runs through a plain `Vec` or `Map`
slot is invisible to it; use `RcVec` or `RcChain` for those. See
[gc-guide.md](gc-guide.md#seeing-what-the-collector-did).

---

## Finding where the memory went

1. **Measure the whole program.** `valgrind --tool=massif ./prog` and
   `ms_print massif.out.<pid>` show the peak and the call stacks that
   allocated it. A large block from `rc_free_queue_init` is the one-time `rc`
   cost, not a leak.
2. **Find leaks.** `valgrind --leak-check=full ./prog` names the allocation
   site of every lost block. In Turmeric's emitted C, the site names the
   feature: `ctor_*` is a `:heap` constructor, `rc_cb_alloc*` is an `rc`, a
   `__env_N` block is a closure, `dk_frame*` is a continuation frame,
   `tur_hamt*` is a map.
3. **Check regions.** `TUR_REGION_STATS=1 ./prog` prints pushes, rewinds and
   retires.
4. **Check the collector.** `TUR_GC_TRACE=1 ./prog` prints one line per
   collection.
5. **Read the C.** `tur emit-c prog.tur` and search the function you care
   about for `malloc`, `tur_region_alloc_or_malloc`, `rc_cb_alloc` and
   `__cps`. A `__cps` twin of a function means its calls through function
   values allocate frames.
6. **Leak-check a fixture.** In this repository, a fixture with a
   `requires.leak-check` marker is run under LeakSanitizer by
   `tests/run-leak-check.sh`; `bash tests/run.sh` does not check emitted
   programs for leaks. See
   [test-suite-portability-guide.md](test-suite-portability-guide.md#7a-leak-checking----what-is-covered-and-what-is-not).

---

## The interpreter and the dynamic dialects

**`tur --interpret` and the REPL** use a different allocator. The
tree-walking interpreter allocates frames, bindings and closures from a pool
and never frees them one at a time; a trampolined loop retains about 4 KiB
per step, so a 1e6-step loop peaks around 3.5 GiB under `--interpret` and
uses nothing extra compiled. The REPL copies each turn's surviving values out
and rewinds the rest. If interpreted memory is the problem, compile the
program. See [gc-guide.md](gc-guide.md#the-interpreter-is-different).

**`#lang r7rs` and `#lang saffron`** programs replace the whole allocator
with a conservative mark-sweep collector, so nothing above about explicit
frees applies to them. `TUR_GC_STATS=1` prints what that collector did. See
[gc-guide.md](gc-guide.md#the-dialect-collector-lang-r7rs-lang-saffron).

---

## Checklist

- [ ] Data that has one owner is by value, not `:heap` and not `rc`.
- [ ] Every `Vec`, `MutableMap` and `Map` has a matching free, including
      each dropped version of a persistent map.
- [ ] A `Vec` of wide structs is either structure-of-arrays or lives in a
      region.
- [ ] Temporary `:heap` structures are built inside `with-region`, and
      `TUR_REGION_STATS=1` shows them rewinding.
- [ ] Strings built in a loop use a `StringBuilder`.
- [ ] Long-running loops that call function values or perform effects are
      `while` loops.
- [ ] Recursive loops that hold an `rc` are checked with `^tailcall`.
- [ ] valgrind's leak summary is empty, or every remaining block is one you
      chose to keep until exit.

---

## Known issues

These are open findings that change the numbers above. Each report has a
repro and the measurements.

- [fn-value-call-cps-frames-held-until-outer-entry](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/fn-value-call-cps-frames-held-until-outer-entry.md)
  -- calls through function values hold their frames until the outermost
  call returns.
- [byvalue-recursive-shared-copies-leak](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/byvalue-recursive-shared-copies-leak.md)
  -- by-value recursive values copied out of a borrow or a container leak.

## Related

- [gc-guide.md](gc-guide.md) -- reference counting, regions, the cycle
  collector, and what is not managed.
- [ownership-guide.md](ownership-guide.md) -- which ownership strategy to use.
- [performance-guide.md](performance-guide.md) -- speed, including tail calls
  and `^tailcall`.
- [strings-guide.md](strings-guide.md) -- `cstr`, `str` and `String`.
- [hamt-guide.md](hamt-guide.md) -- the persistent map and transients.

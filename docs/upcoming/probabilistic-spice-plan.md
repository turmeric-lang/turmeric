# `spices/prob`: probabilistic data structures

> **Status:** nothing exists. Greps for `bloom`, `filter`, `sketch`, `prob`,
> `hyper`, `hll`, `count-min`, `cuckoo`, `tdigest`, and `top-k` over
> `stdlib/`, `src/`, `docs/guides/`, and all 48 spices return nothing
> relevant. This is a green-field spice.
> **Open:** all phases. **Track:** post-v1 -- nothing on the v1 line depends
> on this; it is written down so the design survives.
> **Type:** spice (in `../turmeric-spices/`), no stdlib additions proposed.
> **Sequencing:** independent. May reuse `JoinSemilattice` /
> `BoundedJoin` from `stdlib/typeclass-lattice.tur` (landed by the lattice
> vocabulary plan), which is the one cross-spice dependency worth naming.

## 0. Summary

Turmeric has **no probabilistic data structures at all.** A grep for the
seven families Redis groups under "probabilistic" -- Bloom filters, Cuckoo
filters, HyperLogLog, Count-Min Sketch, Top-K, t-digest, and IBLT -- returns
nothing across the stdlib, the compiler, the guides, and every spice. There
is no approximate membership, no cardinality estimator, no frequency sketch,
and no streaming quantile.

That is a larger hole than it looks, because Turmeric already has nearly
every substrate a probabilistic library normally has to build for itself:

- **A bit array.** `stdlib/sized-bits.tur` ships `SizedBitVec` -- `new`,
  `free`, `get`, `set!`, `clear!`, `toggle!`, `count` (popcount), `fill!`.
  That is a Bloom filter's bit vector with the name on it, and a Cuckoo
  filter's fingerprint store once you widen the cell.
- **Bitwise primitives as builtins.** `bit-and`, `bit-or`, `bit-xor`,
  `bit-shl` are compiler builtins (`src/compiler/builtins.c`, emitting `&`,
  `|`, `^`, `<<`); `bit-shr` is a `defn` in `stdlib/bits.tur`. The
  double-hashing arithmetic a Bloom filter, a Cuckoo filter, and a
  Count-Min Sketch all need is expressible in plain Turmeric, not inline C.
- **A hash finalizer.** `stdlib/hash.tur` has `hash-int` (splitmix64
  finalizer, bijective, avalanche-complete) plus typed variants
  (`hash-int8` ... `hash-uint64`). That is the indexing hash for every
  structure here.
- **A real hash function.** `stdlib/digest.tur` ships `digest/sha256` and
  `digest/md5`, self-contained, no external crypto lib. Double-hashing
  (`h1 + i * h2 mod m`) from one SHA-256 gives `k` independent hashes for
  Bloom and Cuckoo without `k` hash functions.
- **A growable array.** `stdlib/vec.tur`'s `Vec[A]` (`vec-new`, `vec-len`,
  `vec-get`, `vec-push!`, `vec-set!`) is the counter array for a Count-Min
  Sketch and the register array for a HyperLogLog.
- **A lattice vocabulary.** `stdlib/typeclass-lattice.tur` ships
  `JoinSemilattice` (`join`, with associativity / commutativity /
  idempotence laws) and `BoundedJoin` (`bottom`). Four of the seven
  structures are mergeable, and their merge is a join-semilattice -- so
  they get law-checking and `join-all` for free by declaring an instance,
  exactly as `spices/crdt` does.

This plan proposes `spices/prob`: the four structures that are both broadly
useful and a clean fit for the substrate (Bloom filter, HyperLogLog,
Count-Min Sketch, Cuckoo filter), plus Top-K and t-digest as later phases,
and IBLT explicitly deferred. The selection criterion is stated in section
1.2 and applied honestly: IBLT is declined for now, not buried.

The reason to write this in Turmeric rather than port a C library is the
same one the CRDT plan names: the merge laws are checkable, not
aspirational. A Bloom filter's `join` is bit-OR -- associative, commutative,
idempotent -- and Turmeric can attach that to `#refine{...}` contracts and
a seeded fuzzer that joins filters in random orders and asserts the result
is order-independent. A port leaves that in a doc comment.

## 1. What exists today, and the selection

### 1.1 Substrate that already works

| Need | What we have | File |
| --- | --- | --- |
| Bit array (Bloom, Cuckoo) | `SizedBitVec`: new/free/get/set/clear/toggle/count/fill | `stdlib/sized-bits.tur:29,61,84,123,148,174,200,227,255` |
| Bitwise AND/OR/XOR/shift | `bit-and`, `bit-or`, `bit-xor`, `bit-shl` (builtins), `bit-shr` | `src/compiler/builtins.c:104-107`, `stdlib/bits.tur:20` |
| Indexing hash | `hash-int` (splitmix64), typed variants | `stdlib/hash.tur:24,47-148` |
| Strong hash for double-hashing | `digest/sha256`, `digest/md5` | `stdlib/digest.tur:108,186` |
| Counter / register array | `Vec[A]`: new/len/get/push/set | `stdlib/vec.tur:55,78,99,120` |
| Merge laws + identity | `JoinSemilattice`, `BoundedJoin` | `stdlib/typeclass-lattice.tur:286,311` |
| Deterministic test generation | `Seeded-Random`, `seeded-next-int!` | `stdlib/random.tur:138,153` |
| Wire format | `derive-json` (defstruct), `derive-json-sum` (defdata) | `spices/json` |
| Opaque handle type | `defopaque` over `:ptr<void>` | `spices/secret`, `stdlib/random.tur` |

Two facts about the bitwise builtins are load-bearing and worth stating
plainly, because they are the difference between this spice being plain
Turmeric and being a wall of inline C:

- `bit-and`/`bit-or`/`bit-xor`/`bit-shl` are **not** `defn`s in a stdlib
  file. They are registered in `src/compiler/builtins.c` as `BS_BIN_INFIX`
  forms emitting the C operators directly. They are always available; no
  import is needed. (`stdlib/r7rs/prelude.tur:3215` and `spices/ecs` already
  call them bare.)
- `bit-not` is **not** a builtin. `bit-xor x -1` is the NOT, and is how the
  r7rs prelude spells it (`stdlib/r7rs/prelude.tur:3225`). The spice should
  ship a `bit-not` wrapper for readability, or just use the xor form
  consistently.

### 1.2 Selection: what is sensible, and what is not

The user's seven suggestions come from the Redis probabilistic docs. Not
all of them are equally sensible to implement, and the plan says so per
structure rather than implementing all seven by default:

| Structure | Verdict | Why |
| --- | --- | --- |
| Bloom filter | **C1** | Membership approximation, no false negatives. `SizedBitVec` + double-hashing is the whole structure. Mergeable (bit-OR). The clearest win. |
| HyperLogLog | **C1** | Cardinality estimation in ~1.5 KB for a billion elements. `Vec` of registers + a `clz` helper. Mergeable (pointwise `max`). Broadly useful. |
| Count-Min Sketch | **C1** | Frequency estimation with a provable over-counting bound. `Vec` of `Vec` (or flat `Vec`) of counters + `k` hashes. Mergeable (pointwise `+`). The third clearest win. |
| Cuckoo filter | **C2** | Bloom alternative that supports **deletion** (Bloom cannot, without a counting variant). Fingerprint + cuckoo hashing over a bucket array. More complex but the deletion story is the reason to have it alongside Bloom, not instead of it. |
| Top-K | **C2** | Heavy-hitters / most-frequent elements. Min-heap of (count, item) pairs, or a Count-Min Sketch + heap. Useful, but it is the one structure whose "merge" is messy (see 2.6). |
| t-digest | **C3** | Streaming approximate quantiles. Mergeable by combining centroid sets -- a genuine join-semilattice. Algorithmically the hardest; needs a sort (see 1.3). Worth it for percentile-on-a-stream, which nothing else here covers. |
| IBLT | **deferred** | Invertible Bloom Lookup Table: set reconciliation, "tell me the difference between two large sets." Niche -- the use case is sync/diff, not query. It overlaps with what `spices/crdt`'s delta layer already addresses for replicated state. Decline for now; revisit if a concrete sync consumer appears. |

The criterion the table applies: a structure is in if it is (a) broadly
useful as a query primitive, (b) a clean fit for the existing substrate, and
(c) either mergeable or self-contained. IBLT fails (a) -- it is a
reconciliation primitive, not a query -- and overlaps with existing work.
That is a judgment call and it is stated, not hidden.

### 1.3 Gap 1 -- no count-leading-zeros

HyperLogLog's register value is the position of the leftmost set bit in a
hash, which is `64 - clz(hash)` (or `W - clz` for a window of `W` bits).
There is no `clz`, `ctz`, `ffs`, or `__builtin_clz` wrapper anywhere in
stdlib or the builtins (grepped). `SizedBitVec-count` is popcount, not
leading-zeros.

Resolution: the spice ships one inline-C helper, `hash-clz`, in
`prob/hll`:

```turmeric
;;; Position of the leftmost set bit in the low `w` bits of `h`,
;;; 1-indexed (0 if none).  The HLL register value.
(defn hash-clz [h : int w : int] : int
  ```c
  uint64_t x = (uint64_t)h & (w >= 64 ? ~0ULL : ((1ULL << w) - 1));
  if (x == 0) return 0;
  return w - (int)__builtin_clzll(x);
  ```)
```

This is the **one** inline-C body in C1, and it costs the HLL module its
interpreter coverage (see section 4). The alternative -- a Turmeric loop
shifting and testing each bit -- is O(64) per insert and acceptable for
correctness but not for a structure whose entire point is speed. The plan
prefers the one-line builtin and documents the interpreter cost.

### 1.4 Gap 2 -- no sort

t-digest merge combines two centroid lists and must re-sort by mean before
re-clustering. There is no `sort`, `vec-sort`, or `qsort` wrapper in stdlib
(grepped). This is the single reason t-digest is C3 and not earlier: it
needs a sort primitive that does not exist yet.

Resolution: the spice ships a `centroid-sort` over `Vec` in plain
Turmeric (insertion sort is fine -- t-digest centroids are nearly sorted
after merge, so insertion sort is O(n) in the common case and the
constant is what matters). If that measures badly, a `qsort` call in
inline C is the fallback. Either way the gap is in the spice, not stdlib:
tur-signal does not sort, so a stdlib `sort` is not justified by an
existing caller (the same gate the CRDT plan applies to `map-merge-with`
in its section 5).

### 1.5 Gap 3 -- `random.tur` is `rand()`, and that is fine here

`stdlib/random.tur` is libc `rand()` seeded with `time(NULL)` -- not
crypto-secure, already documented as a defect for the secret spice. For
probabilistic structures this is **not** a defect: a Bloom filter needs
uniformly distributed hash values, not unpredictability, and `hash-int`'s
splitmix64 finalizer gives avalanche at the quality these structures
require. The spice should use `hash-int` / `digest/sha256` for all
production hashing and `Seeded-Random` only for test generation, and the
doc page should say plainly that the structures are not
adversarially secure -- a hostile input can be crafted to collide in any
non-keyed Bloom filter, and the fix (a keyed hash / salted filter) is a
later follow-up, not a C1 concern.

## 2. Design

### 2.1 Module layout

```
spices/prob/
  build.tur
  src/prob/
    hash.tur       ;; double-hashing helpers shared by bloom/cuckoo/cms
    bloom.tur      ;; Bloom filter
    hll.tur        ;; HyperLogLog
    cms.tur        ;; Count-Min Sketch
    cuckoo.tur     ;; Cuckoo filter            (C2)
    topk.tur       ;; Top-K                    (C2)
    tdigest.tur    ;; t-digest                  (C3)
  tests/prob/
    test_bloom.tur
    test_hll.tur
    test_cms.tur
    test_cuckoo.tur
    test_topk.tur
    test_tdigest.tur
    test_merge.tur ;; lattice-law harness across all mergeable types
```

`prob/hash` is the one intra-spice module that is not exported: it holds
the double-hashing helper (`double-hash` -> `k` indices into `m` slots)
shared by Bloom, Cuckoo, and CMS, so the three do not re-implement it. It
is deliberately not a stdlib addition -- tur-signal does not double-hash.

### 2.2 The hash layer

```turmeric
;;; Given a byte sequence, produce two 64-bit hashes via SHA-256
;;; (first 16 bytes -> two int64s).  These are the seeds for double-hashing.
(defn prob-hash-pair [data : int len : int] : int
  ;; returns a packed pair; see digest/sha256.  Inline-C only to unpack
  ;; the two int64s out of the 32-byte digest; the hash itself is stdlib.
  ...)

;;; The i-th of k hashes into a universe of m slots, by double-hashing:
;;;   idx_i = (h1 + i * h2) mod m,   with h2 forced odd to guarantee a
;;; full-period walk when m is prime.  This is the Kirsch-Mitzenmacher
;;; construction: one hash pair, k independent-looking indices.
(defn prob-double-hash-index [h1 : int h2 : int i : int m : int] : int
  (bit-and (+ h1 (* i (bit-or h2 1))) (- m 1)))
```

`prob-double-hash-index` is plain Turmeric over the bitwise builtins --
no inline C. The `bit-or h2 1` forces `h2` odd; the `bit-and ... (- m 1)`
is a fast `mod` for power-of-two `m` (the structures should size `m` to a
power of two for this, and the doc page should say so). For non-power-of-two
`m` (Cuckoo, which wants a prime bucket count), use `% m`.

### 2.3 Bloom filter

```turmeric
;;; A Bloom filter: a bit array of m bits and k hash functions.
;;; Membership is approximate (false positives possible, no false negatives).
;;; Mergeable: the join of two filters of equal (m, k) is bit-OR.
(defopaque Bloom :ptr<void>)

(defn bloom-new     [m : int k : int] : Bloom ...)        ;; SizedBitVec of m bits
(defn bloom-free     [bf : Bloom] : void ...)
(defn bloom-insert!  [bf : Bloom data : int len : int] : nil ...)  ;; set k bits
(defn bloom-maybe?   [bf : Bloom data : int len : int] : bool ...) ;; all k bits set?
(defn bloom-count     [bf : Bloom] : int ...)             ;; popcount of the bit array
(defn bloom-merge     [a : Bloom b : Bloom] : Bloom ...)  ;; bit-OR into a new filter

(definstance JoinSemilattice [Bloom]
  (join [x y] (bloom-merge x y)))
(definstance BoundedJoin [Bloom]
  (bottom [] (bloom-new (default-m) (default-k))))
```

`bloom-insert!` is `k` calls to `sized-bitvec-set!` at the `k`
`prob-double-hash-index` positions. `bloom-maybe?` is `k` calls to
`sized-bitvec-get`. `bloom-merge` is a word-wise OR over the two
`SizedBitVec` backing arrays -- this is the one place a direct backing-array
walk beats per-bit `get`/`set`, and it is a small inline-C loop OR it is
`SizedBitVec`-level if a `sized-bitvec-or!` is added (see 2.7). The
`JoinSemilattice` instance is what makes `join-all` and the law harness
work; `BoundedJoin`'s `bottom` is an all-zero filter of the default size.

Sizing: `m = -(n * ln p) / (ln 2)^2` bits, `k = (m / n) * ln 2`, for `n`
expected elements and target false-positive rate `p`. The constructor takes
`n` and `p` and computes `m`/`k`, OR takes `m`/`k` directly for the caller
who knows what they want. Both spellings ship; the `n`/`p` one is the
convenience, the `m`/`k` one is what `bottom` and `merge` require to match.

### 2.4 HyperLogLog

```turmeric
;;; HyperLogLog: cardinality estimation in m registers (m = 2^p, p in [4..16]).
;;; Mergeable: the join is pointwise max of registers.
(defopaque Hll :ptr<void>)

(defn hll-new      [p : int] : Hll ...)            ;; m = 2^p registers, init 0
(defn hll-free      [h : Hll] : void ...)
(defn hll-add!      [h : Hll data : int len : int] : nil ...) ;; hash, split, update register by max
(defn hll-cardinality [h : Hll] : float ...)       ;; the estimate, with bias correction
(defn hll-merge     [a : Hll b : Hll] : Hll ...)   ;; pointwise max

(definstance JoinSemilattice [Hll]
  (join [x y] (hll-merge x y)))
(definstance BoundedJoin [Hll]
  (bottom [] (hll-new (default-p))))
```

`hll-add!` hashes the input, uses the low `p` bits as the register index,
and sets that register to `max(old, hash-clz(high-bits, W))` -- the one
inline-C helper from 1.3. `hll-cardinality` is the standard raw-estimate
harmonic mean plus the small/large-range bias corrections (the
Flajolet/Martinez corrections, or the simpler Heule/Nunkesser correction
that Redis uses). The correction table is data, not code; ship it as a
`Vec` lookup for the small-range case and the `2^32`-threshold switch for
the large-range case.

`hll-merge` is pointwise `max` over the two register `Vec`s -- a plain
Turmeric loop, no inline C. The `JoinSemilattice` instance is exact: max
is associative, commutative, idempotent, and the all-zero register set is
`bottom`.

### 2.5 Count-Min Sketch

```turmeric
;;; Count-Min Sketch: frequency estimation with depth d, width w.
;;; Over-counts never, under-counts never by more than the error bound.
;;; Mergeable: the join is pointwise addition of counters.
(defopaque CountMin :ptr<void>)

(defn cms-new       [d : int w : int] : CountMin ...)  ;; d rows, w cols, all zero
(defn cms-free       [c : CountMin] : void ...)
(defn cms-add!       [c : CountMin data : int len : int] : nil ...) ;; ++d cells
(defn cms-estimate   [c : CountMin data : int len : int] : int ...) ;; min over d rows
(defn cms-merge      [a : CountMin b : CountMin] : CountMin ...)    ;; pointwise +
(defn cms-inner-product [a : CountMin b : CountMin] : float ...)   ;; self-join size

(definstance JoinSemilattice [CountMin]
  (join [x y] (cms-merge x y)))
(definstance BoundedJoin [CountMin]
  (bottom [] (cms-new (default-d) (default-w))))
```

The sketch is a `d * w` counter matrix. Back it with a single flat
`Vec[uint32]` of length `d * w` (row `i`, col `j` at `i*w + j`) rather than
a `Vec` of `Vec`s -- one allocation, one free, cache-friendly, and the
index arithmetic is `(* i w) j` which the bitwise builtins do not help with
(unless `w` is a power of two, in which case `<<` replaces `* w`; the
constructor should round `w` to a power of two and document it).

`cms-add!` is `d` increments at `prob-double-hash-index(h1, h2, i, w)` per
row. `cms-estimate` is the `min` over the `d` rows -- the over-counting
bound is `min`, never `max`. `cms-merge` is pointwise `+`, which is
associative and commutative but **not idempotent** -- joining a sketch
with itself doubles every count. That means `CountMin` is a
`JoinSemilattice` only if "join" means "merge two distinct streams"; the
law harness in section 3 must test merge of disjoint streams, not
idempotence, and the instance docstring must say so. This is the one
structure where the lattice fit is imperfect, and the plan says so rather
than pretending `+` is idempotent.

`cms-inner-product` (the size of the join of two streams) is the query
that justifies a sketch over a plain counter; it is `min` over rows of the
dot product, and it is what Redis ships as `CMS.INITBYPROB`.

### 2.6 Cuckoo filter, Top-K, and the merge question

**Cuckoo filter** (C2) stores fingerprints in a bucket array with cuckoo
hashing (two candidate buckets per item, kick on collision). It supports
`delete` -- the feature Bloom lacks -- at the cost of a bounded
relocation loop on insert. The structure is a `Vec` of buckets, each
bucket a small fixed array of fingerprints (4 by convention). Insert and
lookup are plain Turmeric over the bitwise builtins (fingerprint =
low 8 bits of a hash; bucket index = `hash mod num-buckets`). The
relocation loop has a max-kick count (500 by convention) after which the
filter reports full. **Cuckoo filters are mergeable only when
non-overlapping** -- two filters built from the same item set can merge by
re-inserting, but two arbitrary filters cannot bit-OR like a Bloom. The
spice ships `cuckoo-merge` as "insert all of B into A" and does **not**
declare a `JoinSemilattice` instance. The doc page says why: cuckoo merge
is not idempotent and not commutative under load. This is the honest
answer; do not force a lattice instance that lies.

**Top-K** (C2) is a min-heap of `(count, item)` pairs capped at `K`.
`topk-add!` increments the count if the item is present, else inserts;
if the heap exceeds `K`, evict the min. `topk-list` returns the `K`
pairs sorted descending. The merge question is the same as Cuckoo:
`topk-merge` is "add all of B's items into A", which is not idempotent
and not commutative if both sides share items (the merged count is the
sum, which is correct for two streams, but joining a Top-K with itself
doubles counts). No `JoinSemilattice` instance; the doc page says so.
Top-K can be backed by a Count-Min Sketch for the counts plus a heap for
the leaders, or by an exact `Map` of counts -- the sketch-backed variant
is approximate but bounded-memory, and is the one Redis ships. The plan
prefers the exact `Map`-backed variant for C2 (simpler, correct) and
notes the sketch-backed variant as a memory-bounded option.

### 2.7 t-digest (C3)

t-digest is a streaming approximate quantile structure: a set of
centroids `(mean, count)` whose scale function `k(q)` controls compression
near the tails. `tdigest-add!` inserts a value; `tdigest-quantile` returns
the approximate value at quantile `q` by interpolating between centroids.
Merge combines two centroid sets, sorts by mean (gap 1.4), and re-clusters
to the scale bound. The merge is a genuine join-semilattice -- associative,
commutative, idempotent -- and the plan declares the instance:

```turmeric
(definstance JoinSemilattice [TDigest]
  (join [x y] (tdigest-merge x y)))
(definstance BoundedJoin [TDigest]
  (bottom [] (tdigest-new (default-compression))))
```

t-digest is the most algorithmically involved structure here and the
only one that needs a sort. It is C3 because the sort gap (1.4) must close
first and because the centroid clustering loop is fiddly enough that a
correctness fuzzer (section 3) is non-negotiable, not nice-to-have.

### 2.8 Purity, ownership, and the inline-C budget

The CRDT plan's section 2.5 sets a discipline this plan inherits: keep
inline C out of the primitives so the core runs under `tur --interpret`,
because `tests/run-turi.sh` PASS-skips any fixture with a user inline-C
block. The budget here is:

| Module | Inline C | Why |
| --- | --- | --- |
| `prob/hash` | none | double-hashing is bitwise builtins |
| `prob/bloom` | one loop in `bloom-merge` (or none, if `sized-bitvec-or!` lands) | word-wise OR beats per-bit |
| `prob/hll` | one line, `hash-clz` (1.3) | `__builtin_clzll` has no Turmeric spelling |
| `prob/cms` | none | `Vec` arithmetic |
| `prob/cuckoo` | none | bucket array is `Vec`, fingerprint is `bit-and` |
| `prob/topk` | none | `Map` + heap |
| `prob/tdigest` | none (insertion sort in Turmeric) | unless sort measures badly (1.4) |

That is **one or two** inline-C bodies across the whole spice. The HLL
`hash-clz` is unavoidable; the Bloom `bloom-merge` OR-loop is avoidable if
a `sized-bitvec-or!` (and `-and!`, `-xor!`) is added to
`stdlib/sized-bits.tur` -- but that is a stdlib addition justified by this
spice's caller, which is not an existing tur-signal caller, so the plan
ships the inline-C merge in the spice and leaves the stdlib widening as a
noted follow-up. If the interpreter coverage cost turns out to matter for
HLL, the fallback is a 64-iteration Turmeric `hash-clz` loop behind a
`#refine` contract, gated by a benchmark.

Ownership: every structure is a `defopaque :ptr<void>` holding a heap
allocation (the `SizedBitVec` or `Vec`). Each ships a `*-free`. The merge
functions return a **new** allocation and leave both inputs valid, matching
the CRDT convention -- but unlike CRDTs these are mutable (`*-add!`), so
the handle is a single-owner resource, not a shareable CvRDT state. No
`:linear` for C1 (the structures are small and copied freely); a later
phase may revisit if a `:linear` handle proves to catch use-after-free in
practice, the way the secret spice uses it.

### 2.9 Serialization

`derive-json` (defstruct) and `derive-json-sum` (defdata) from
`spices/json` cover the wire format with no new encoder, matching the CRDT
spice. The `:spices` dep is `:optional true` in the manifest, so the core
builds without a transport. A Bloom filter serializes as
`{m, k, bits: <base64 of the SizedBitVec backing array>}`; HLL as
`{p, registers: [...]}`; CMS as `{d, w, counters: [...]}`. The
`defopaque` handles are not directly `derive-json`-able (they are
pointers), so each structure ships a `*-to-json` / `*-from-json` pair that
walks the backing array, the same shape the secret spice uses for `Secret`.
Binary framing belongs to the msgpack spice when it lands; these
structures are a good forcing case for a compact binary format and a bad
reason to block on it.

## 3. Correctness checking -- the part that is not a port

A probabilistic structure is correct if (a) its error bound holds
empirically and (b) its merge is law-abiding. Every library asserts (a) in
prose and ignores (b). Turmeric can check both, in three layers matching
the CRDT plan's section 3:

**Layer 1 -- merge laws as contracts.** The `JoinSemilattice` instances
(Bloom, HLL, t-digest; CMS with the idempotence caveat from 2.5) carry
`#refine{...}` post-conditions on `join` in debug builds. The Release
caveat is the same one the CRDT plan records: a Release `tur` compiles
contracts out under `NDEBUG`, so law checking is a Debug-build activity and
the suite must say so.

**Layer 2 -- a law harness.** One function per law, generic over the
class, exactly as the CRDT plan writes them:

```turmeric
(defn law-associative? [^JoinSemilattice A ^Eq A] [x : A y : A z : A] : bool
  (eq? (join (join x y) z) (join x (join y z))))
(defn law-commutative? [^JoinSemilattice A ^Eq A] [x : A y : A] : bool
  (eq? (join x y) (join y x)))
(defn law-idempotent? [^JoinSemilattice A ^Eq A] [x : A] : bool
  (eq? (join x x) x))
```

These are written once and instantiate for Bloom, HLL, and t-digest. CMS
is tested with `law-associative?` and `law-commutative?` only -- the
idempotence law does not hold for `+`, and the test must say so by
**not** running it, not by running it and accepting failure. Cuckoo and
Top-K have no instance and no law harness; their correctness is the
empirical layer below.

**Layer 3 -- empirical error-bound validation.** This is the layer that
has no analogue in the CRDT plan, because probabilistic correctness is
statistical, not algebraic. For each structure, a seeded test inserts a
known set and asserts the measured error is within the proven bound:

| Structure | Property tested | Bound |
| --- | --- | --- |
| Bloom | false-positive rate over `n` absent elements | measured FP rate within 2x of `p` (chi-square, 1000 seeds) |
| Bloom | no false negatives | every inserted element reports `maybe?` = true, all seeds |
| HLL | cardinality estimate | relative error < 2% for `n` in `[10^3, 10^7]`, standard HLL bound `1.04/sqrt(m)` |
| CMS | frequency estimate | `estimate >= true` always; `estimate <= true + eps * N` with `eps = e/w` over `N` total |
| Cuckoo | false-positive rate + no false negatives | as Bloom; plus `delete` then `lookup` = false |
| Top-K | the `K` reported are the `K` true most-frequent | exact for the `Map`-backed variant |
| t-digest | quantile error | `|approx - true| / range < delta` for `delta` set by compression |

The Bloom FP-rate test is the one that catches a subtle bug a law harness
cannot: if `k` is wrong for `m`/`n`, the filter still joins correctly
(laws pass) but the FP rate is off by a factor. Only the empirical test
sees that. This is the probabilistic analogue of the CRDT plan's finding
that a convergence fuzzer catches order-dependence the laws miss -- and
the complement is the same: the laws catch a broken merge the empirical
test might not, so both layers ship.

`Seeded-Random` drives every Layer 3 test so a failure is a reproducible
seed, not a flake. The model is `tests/saffron-fuzz-src.py`, the same
in-repo precedent the CRDT plan cites.

## 4. Back-end and dialect caveats

- **Interpreter:** the core is designed to run there (section 2.8). The
  HLL `hash-clz` inline-C body costs HLL its interpreter coverage; the
  Bloom `bloom-merge` inline-C body (if kept) costs Bloom's merge. Both
  have Turmeric fallbacks documented in 2.8; whether to ship the fallback
  or the builtin is a benchmark decision for C1, not a design one.
- **JIT:** nothing here is JIT-specific, but the structures are a good
  stress case -- tight loops over `Vec` with parametric element types are
  the shape that has surfaced monomorphization and carrier-repr defects
  before (the CRDT plan filed nine in C3 alone). Expect to file at least
  one; the flat `Vec[uint32]` backing for CMS is the most likely trigger.
- **Saffron:** probabilistic structures through `any` are out of scope.
  The types are the point; a Bloom filter behind `any` loses the
  `JoinSemilattice` instance that is the reason to have the type.
- **`uint32` counters in CMS:** `uint32` exists as a type (used in
  `stdlib/hash.tur`'s typed variants). CMS counters should be `uint32`
  not `int` to halve memory and to make the overflow semantics explicit
  (wrapping add, not signed). Confirm `vec-set!` / `vec-get` work over
  `uint32` in C1 before committing; the `Vec[A]` parameterization is
  generic but the wide-element / RC decision functions
  (`tur-vec-elem-wide?`, `tur-vec-elem-rc?` in `stdlib/vec.tur:310,327`)
  return constant 0 and may need an instance path checked.

## 5. Why a spice, and why nothing goes in stdlib

The stdlib-growth gate is that additions are justified by tur-signal's
actual call surface. tur-signal does not estimate cardinality, sketch
frequencies, or test approximate membership, so every structure here is
spice material without argument.

The one arguable stdlib addition is `sized-bitvec-or!` / `-and!` / `-xor!`
(word-wise bitwise ops on `SizedBitVec`), which would let `bloom-merge`
avoid inline C (2.8). The plan still says **spice**, for the same reason
the CRDT plan says `map-merge-with` stays in the spice: putting it in
stdlib means committing to its semantics (in-place vs. new allocation,
aliasing) at a moment when no stdlib caller exists to pin them down. Ship
the inline-C merge in `prob/bloom`; promote the `SizedBitVec` ops to
stdlib once the Bloom filter is their first real user and the aliasing
question is answered by something other than a guess.

The other arguable one is `clz` / a `bit-clz` builtin. The plan does
**not** propose it: HLL is the only caller, one inline-C line is cheaper
than a compiler builtin, and a builtin is a commitment to a codegen path
on every back-end. If a second caller appears (e.g. a bitset spice), that
changes.

## 6. Phases

- **C1 -- Bloom + HLL + Count-Min Sketch.** `spices/prob/` exists:
  manifest, `prob/hash` (double-hashing), `prob/bloom`, `prob/hll`,
  `prob/cms`, and `tests/prob/test_bloom.tur`, `test_hll.tur`,
  `test_cms.tur`, `test_merge.tur`. The `JoinSemilattice` / `BoundedJoin`
  instances for Bloom and HLL land here; CMS gets its
  `JoinSemilattice` instance with the idempotence caveat documented in
  2.5 and tested in 3. `test_merge.tur` is the law harness (layer 2) across
  all three; the per-structure tests carry the empirical bounds (layer 3).

  One or two inline-C bodies total (2.8): `hash-clz` in HLL, and
  `bloom-merge`'s OR-loop unless the Turmeric per-bit fallback is
  acceptable by benchmark. CMS is zero inline C.

  The C1 acceptance criterion is not "the tests pass" but "the Bloom
  false-positive rate test fails when `k` is deliberately mis-set by one"
  -- the same mutation-tested discipline the CRDT plan applies to its
  OR-Set scenarios. If the FP test still passes with `k` wrong, the test is
  not measuring what it claims.

- **C2 -- Cuckoo filter + Top-K.** `prob/cuckoo`, `prob/topk`, and their
  tests. Neither declares a `JoinSemilattice` instance (2.6); both ship a
  `*-merge` that is "insert all of B into A" with a docstring saying it is
  not idempotent and not commutative under shared items. The Cuckoo test
  must cover the filter-full case (max kicks exceeded) and the
  delete-then-lookup case; the Top-K test must cover eviction (the
  `K+1`-th distinct item replaces the min).

- **C3 -- t-digest.** `prob/tdigest` and `test_tdigest.tur`. Gated on the
  sort gap (1.4) closing -- the spice ships its own insertion sort over
  `Vec` if stdlib still has none. The `JoinSemilattice` / `BoundedJoin`
  instances land here; the law harness extends to t-digest. The empirical
  test is quantile error against a known distribution (uniform and
  heavy-tailed), at several compression settings. This is the phase most
  likely to surface a compiler defect, because the centroid clustering
  loop is the most complex generic code in the spice.

- **Deferred -- IBLT.** Not started. Revisit if a concrete set-reconciliation
  consumer appears that is not already served by `spices/crdt`'s delta
  layer. The plan declines it now (1.2) rather than leaving it as an
  implied C4.

C1 is the useful unit; someone can build with the spice at the end of C1.
C2 adds the deletion-capable membership filter and heavy-hitters. C3 adds
streaming quantiles. There is no sync phase -- unlike CRDTs, these
structures are query primitives, not replicated state, and the merge is a
local combine, not a network operation.

## 7. Risks and open questions

- **`uint32` in `Vec` is unverified for this use.** `Vec[A]` is generic
  but the wide-element and RC decision functions
  (`stdlib/vec.tur:310,327`) return constant 0 and may not have been
  exercised over `uint32` in a hot loop. Probe `vec-set!` / `vec-get` over
  `uint32` in C1 before committing CMS to it; the fallback is `int`
  counters (double the memory, signed overflow). File a report if the
  path is broken, do not work around it silently.
- **CMS idempotence is a real lie if unqualified.** `+` is not idempotent.
  The `JoinSemilattice` instance for `CountMin` is correct for merging
  two **distinct** streams and wrong for re-joining a sketch with itself.
  The instance docstring and the law harness must both say this, and the
  harness must skip `law-idempotent?` for CMS by name, not by accident.
  This is the one place the lattice vocabulary fits imperfectly; the plan
  prefers an honest caveat over dropping the instance (which loses
  `join-all`) or pretending the law holds.
- **Cuckoo and Top-K merge is not a lattice.** Forcing a
  `JoinSemilattice` instance on either would let `join-all` produce wrong
  answers under duplicate delivery. The plan declines the instance and
  ships a plain `*-merge`; the risk is that a caller reaches for `join`
  expecting lattice semantics and gets stream-sum semantics. Mitigate
  with a doc page section titled "merge is not idempotent" on both, and
  do not export a `join` alias for them.
- **Hash quality is load-bearing and not measured.** `hash-int`'s
  splitmix64 has avalanche but the double-hashing construction
  (`h1 + i*h2`) is only as good as `h2`'s distribution. If `h2` has a
  pattern modulo `m`, the `k` indices correlate and the FP rate drifts.
  The Bloom FP-rate test (layer 3) is what catches this; if the measured
  rate exceeds 2x the target across 1000 seeds, the hash layer is the
  suspect, not the filter sizing. Ship a small avalanche test on
  `prob-hash-pair` in C1.
- **t-digest correctness is hard to fuzz.** The quantile error bound is
  statistical and the clustering loop has off-by-one and scale-function
  edge cases at the tails. The C3 empirical test must cover the tails
  (q near 0 and 1) explicitly, not just the median, because the scale
  function compresses the tails hardest and that is where a naive
  implementation drifts. A convergence-style fuzzer is not enough; the
  test needs a known-distribution oracle.
- **No benchmark exists for any of this.** The structures are sold on
  speed and memory; the plan ships none measured. C1 should land a small
  benchmark (insert throughput, query latency, merge cost) alongside the
  correctness suite, so C3's t-digest design has a baseline. The CRDT
  plan names the same gap for HAMT join cost and is right to.
- **`defopaque` over `:ptr<void>` is verified** (the secret spice uses it
  extensively), so unlike the CRDT plan's `defopaque` over `:Sym` risk,
  the handle shape here is not speculative. The risk is the other
  direction: a `defopaque` is not `derive-json`-able, so each structure
  needs a hand-written `*-to-json` / `*-from-json` pair (2.9). That is
  five small pairs of functions, not a design risk, but it is cost the
  CRDT plan does not pay because its types are `defstruct`s.

## 8. References

- Bloom, *Space/Time Trade-offs in Hash Coding with Allowable Errors*
  (1970) -- the Bloom filter in 2.3.
- Flajolet, Fusy, Gandouet, Meunier, *HyperLogLog: the analysis of a
  near-optimal cardinality estimation algorithm* (2007) -- HLL in 2.4;
  Heule, Nunkesser, Hall, *HyperLogLog in Practice* (2013) -- the bias
  corrections Redis uses.
- Cormode, Muthukrishnan, *An Improved Data Stream Summary: The
  Count-Min Sketch and its Applications* (2003) -- CMS in 2.5.
- Fan, Andersen, Kaminsky, Mitzenmacher, *Cuckoo Filter: Practically
  Better Than Bloom* (2014) -- the Cuckoo filter in 2.6.
- Dunning, *t-Digest: Efficient Computation of a Very Large, High-
  Cardinality Data Set's Distribution* (2019) -- t-digest in 2.7.
- Kirsch, Mitzenmacher, *Less Hashing, Same Performance: Building a
  Better Bloom Filter* (2006) -- the double-hashing construction in 2.2.
- Redis, *Probabilistic data types*
  (https://redis.io/docs/latest/develop/data-types/probabilistic/) --
  the seven-structure list this plan selects from (1.2).
- In-tree: [crdt-spice-plan.md](crdt-spice-plan.md) (the merge-law
  harness, the inline-C budget discipline, and the format this plan
  follows), `docs/guides/developing-spices-guide.md` (manifest, exports,
  vendored C, testing), `stdlib/typeclass-lattice.tur`
  (`JoinSemilattice`, `BoundedJoin`), `stdlib/sized-bits.tur`
  (`SizedBitVec`), `stdlib/hash.tur` (`hash-int`), `stdlib/digest.tur`
  (`digest/sha256`).

# Each unreferenced coloured function costs a scan of the program's whole inline C

**Severity:** low (compile time). Costs about 0.5 ms of `emit-c` per coloured
user function that nothing calls directly, on a Release `tur`. Real programs
rarely have many such functions, but a generated or test program can: one with
1,800 of them spent 42% of `emit-c` here.

## Repro

A generated program: N higher-order functions, each calling through
function-typed parameters, so CPS-coloured; the first 200 called from `main`;
plus N coloured functions nothing calls. p250 is N = 250 and p1000 is
N = 1000:

```python
import sys
n = int(sys.argv[1]); T = '(fn [int] int)'
out = ['(defn pos-of [x : int] : int x)']
for i in range(n):
    out.append(f'(defn h{i} [f : {T} g : {T} k : {T} x : int] : int\n'
               f'  (let [a (f x) b (g a) c (k b)] (+ (f a) (+ (g b) (k c)))))')
body = '0'
for i in range(min(n, 200)):
    body = f'(+ (h{i} pos-of pos-of pos-of {i + 1}) {body})'
out.append(f'(defn main [] : int (println {body}) 0)')
out += [f'(defn c{j} [] : int (h{j} pos-of pos-of pos-of 1))' for j in range(n)]
print('\n'.join(out))
```

Release `tur`, `emit-c`, best of 3:

| program | with the scan | scan short-circuited | emitted C |
|---|---|---|---|
| p250 (300 unreferenced) | 254 ms | 115 ms | identical |
| p1000 (1,800 unreferenced) | 1261 ms | 735 ms | identical |
| the same 1,000 functions as direct calls (nothing coloured) | 147 ms | 148 ms | identical |
| corpus: `cps-callback-registry-past-256`, `r7rs-numbers`, `r7rs-control`, `effect-tags-past-128`, `parsec-guide-patterns` | -- | within noise | identical |

callgrind on p1000: 4.7 G of 7.0 G instructions are in this function -- 3.15 G
in `memcmp`, 1.09 G on its call line, 0.41 G in the loop around it. That
`memcmp` is 45% of the whole compile.

A side note for whoever measures this next: a `tur` binary outside the build
tree resolves no stdlib beside itself (`find_stdlib_beside_exe`,
`src/driver/paths.c`) and emits a smaller program, which hides this cost.
Measure with the in-tree binary.

## Root cause

`fn_unreached` (`src/compiler/emit_cps_ir.c:6699`) decides whether a coloured
user function is unreachable, called from CPS classification (`ensure_S`) once
per such function. It first scans the program's list of called bindings
linearly. When the function is not in it, it searches EVERY inline-C body in
the program -- the stdlib's included, about 34 KB here -- byte by byte, running
a `memcmp` at each offset for each of two spellings of the name (lines
6728-6740). That is roughly 2 x (inline-C bytes) compares per function: about
68,000 `memcmp` calls each.

## Fix directions

- Tokenize the inline-C text once per program (identifiers only) into a hash
  set, alongside `g_reach_refs`. Each lookup is then one or two hash probes,
  and the per-function cost is O(1). The "longer identifier" boundary rule
  (`app_x` is not `app`) falls out of tokenizing.
- Make `g_reach_refs.called` a set too. It is a linear scan per function, a
  smaller cost (43 M instructions at p1000) but the same shape.
- Gate the result: the emitted C must stay byte-identical (the table above was
  checked that way).

## Resolution (2026-10-29)

Both fix directions landed in `src/compiler/emit_cps_ir.c`, with the scan's
exact match rule kept:

- `g_reach_refs.called` is mirrored into a pointer hash set.
- The inline C is indexed once per program (`reach_index_build`, built on the
  first `fn_unreached` call and freed by `reach_refs_reset`). The old test
  matched a spelling wherever the next byte is not alphanumeric and is not a
  lone `_`, and it required nothing before the match. So the set holds every
  suffix of every identifier run that ends at such a byte, a run followed by
  `__` included. An identifier-only spelling is then one hash probe. A
  spelling with any other byte (a source name such as `foo-bar`) still takes
  the byte scan, so the answer is the same in every case.

Checked 2026-10-29 on the Debug (ASan) `tur`, `emit-c`, two runs each, with the
emitted C byte-identical before and after:

| program | before | after |
|---|---|---|
| p250 | 1.14 / 1.20 s | 0.60 / 0.56 s |
| p1000 | 4.63 / 4.56 s | 1.95 / 2.50 s |

`tests/run.sh`'s `expected.c` snapshot comparisons pass unchanged, which is
the corpus half of the byte-identity gate.

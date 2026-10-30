# clang splits a loop-carried vector into two halves every iteration

**Severity:** low (performance; no wrong answer). Under clang, a 16-byte
vector value (`F32x4`, `F64x2`, ... from `stdlib/simd.tur`) that is carried
from one loop iteration to the next is kept as two 8-byte halves: each
iteration joins them (`movlhps`) before using the value and splits the new
value again (`unpckhpd`). When the loop is a latency-bound recurrence the
join sits on the critical path. tur-simd's biquad on `F32x4`
(`s1-biquad4-f32!`, turmeric-spices#131) runs 1.35x slower than its
hand-written vector C under clang (1.32 vs 0.97 ns/elem); gcc keeps the
value in one register and matches (0.985 vs 0.969). Kernels whose loops carry
independent accumulators (the dot product, the blend) hide it and match on
both compilers. Found graduating simd-and-vectorization-plan S1.

## Repro

```turmeric
(load "stdlib/simd.tur")

;; a first-order recurrence over four interleaved channels: z carries
(defn smooth! [^borrow buf : F32Vec k : float32] : nil
  (let [kv     (f32x4-splat k)
        ^mut z (f32x4-splat (as float32 0.0))
        ^mut i 0]
    (while (< i (f32vec-len buf))
      (set! z {{kv * z} + (unsafe (f32x4-load-unchecked buf i))})
      (unsafe (f32x4-store-unchecked! buf i z))
      (set! i {i + 4}))))

(defn main [] : int
  (let [b (f32vec-new 4096 (as float32 0.25))]
    (smooth! b (as float32 0.5))
    (println (as float (f32vec-ref b 4095)))
    0))
```

`CC=clang tur build rec.tur -o rec && objdump -d --no-show-raw-insn rec`
shows the inner loop as:

```
movlhps  %xmm1,%xmm2              ; join z's halves
mulps    %xmm0,%xmm2              ; kv * z
addps    0x10(%r15,%rax,4),%xmm2  ; + the loaded lanes
movaps   %xmm2,%xmm1
unpckhpd %xmm2,%xmm1              ; split the new z again
movaps   %xmm2,0x10(%r15,%rax,4)
```

With `CC=gcc` the same loop has neither shuffle.

## Root cause

Each vector type is a `^copy` defstruct of lane fields
(`typedef struct { float l0, l1, l2, l3; } tur_adt_F32x4;`), and each
operation is an inline-C function taking and returning that struct by value.
The x86-64 SysV ABI classifies a 16-byte struct of floats as two SSE
eightbytes, so clang lowers every such parameter and result to a pair of
`<2 x float>` values in IR before inlining. After inlining, instcombine
cancels the split/join between two operations in straight-line code, but not
across the loop's phi: the phi is formed on the pair, so the join and split
stay in the loop. gcc does not lower by-value aggregates that way before
inlining and is unaffected.

Things that do **not** change it (checked in plain C against clang -O2):
`__attribute__((aligned(16)))` on the struct; writing the operations with
`__builtin_memcpy` instead of a union pun; and making the type a union of
the lane struct and a `vector_size(16)` member -- the ABI merges the two
members' classes back to SSE + SSE, so it is still passed as two halves.

## Fix directions

- **Declare the vector types with a vector-typed C representation.** A struct
  or union whose only member is `float __attribute__((vector_size(16)))` is
  classified SSE + SSEUP and passed in one register. The lane fields (`l0`
  ...) would then need to be accessor expressions in the emitter rather than
  C struct members, for the `stdlib/simd.tur` types only -- a change to how
  the emitter declares and reads those defstructs, and to c2mir's lowering
  (no vector types there; it keeps the lane struct under `__MIRC__`).
- **Mark the operations `always_inline` and pass by pointer internally.**
  Avoids the by-value ABI lowering at the operation boundaries, at the cost
  of rewriting every generated body in `tools/gen-simd.py`; the phi would
  then be on the struct alloca, which SROA can turn into one vector. Needs
  measuring -- the lowering may reappear at the defn boundary of the
  enclosing Turmeric function.
- **Workaround today:** carry loop state in a numeric array (store and reload
  through `f32vec`), or keep a latency-bound recurrence in inline C, as
  tur-simd's `simd-biquad4-f32!` does.

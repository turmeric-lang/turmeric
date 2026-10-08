# c2mir computes `long long OP unsigned int` in `unsigned int` on win64

**Severity: medium.** A silent wrong answer, not a diagnostic. It affects
every `tur jit` program on Windows whose C (emitted or inline) mixes a signed
64-bit operand with an `unsigned int` one. Found 2026-09-26 while fixing the
Windows JIT's whole-preamble fallback
([jit-windows-support-spike](../archive/jit-windows-support-spike.md)).

**Status: RESOLVED 2026-09-28.**
[rjungemann/mir#4](https://github.com/turmeric-lang/mir/pull/4) merged into the
fork's master as `79cb2905`, and `TUR_MIR_GIT_TAG` in `cmake/mir.cmake` now
pins that commit (with a line in the pin notes above it). One in-tree user of
the pattern, the r7rs bignum subtraction, had already been rewritten so it no
longer depends on the conversion; that cast stays, since it is correct C
either way.

## Repro

````turmeric
(defn probe [] : int
  ```c
  volatile uint32_t a = 5, b = 7;
  int64_t d = (int64_t)a - b;       /* C says -2 */
  printf("%lld\n", (long long)d);
  return 0;
  ```)
(defn main [] : int (probe))
````

`tur run` prints `-2`. `tur jit` on Windows prints `4294967294`. Linux and
macOS JITs print `-2`.

## Root cause

`arithmetic_conversion` (`c2mir/c2mir.c`, about line 6004 at pin `b7e72a95`)
handles "unsigned int vs a higher-ranked signed type" with

```c
} else if ((t1.u.basic_type == TP_UINT && t2.u.basic_type >= TP_LONG
            && MIR_LONG_MAX >= MIR_UINT_MAX) ...
```

That asks `MIR_LONG_MAX` about **every** signed type of rank `long` or above.
On LP64 it holds. On LLP64 (win64, where `long` is 32-bit) it fails even when
the signed type is `long long`, and the fallthrough picks the *unsigned*
operand's type. So `long long - unsigned int` is typed, and computed, as
`unsigned int`.

## How it surfaced

`r7rs-bignums`: `(exact-integer-sqrt (expt 10 39))` returned the right root
`31622776601683793319` and a wrong remainder,
`1294967297294967359840736555185931535` instead of `62545769258890964239`. The
cause was `r7bn_sub_mag`'s `int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i]
: 0) - borrow`: the conditional is `uint32_t`, so the whole subtraction ran in
32 bits and every borrow was 4294967294.

It had been invisible because Scheme programs never engage the S2 split, and
the whole-preamble path they take instead could not link on Windows until
2026-09-26. Every Scheme program on the Windows JIT therefore fell back to cc,
which gets it right. Programs on the split path were already exposed, but
through user inline C only.

In-tree mitigation (2026-09-26): `stdlib/r7rs/bignum.tur` casts both limbs
there. `r7rs-bignums` matches under `tur jit` on Windows with the pinned MIR.
The other `int64_t ... R7BN_BASE` sites in that file are all-`int64_t`, or
reach a `(uint32_t)` cast whose low 32 bits are the same either way. Nothing
else in the tree was audited for the pattern.

## Fix ([rjungemann/mir#4](https://github.com/turmeric-lang/mir/pull/4))

Branch `fix/llp64-uint-llong-conversion` off the fork's master (`b7e72a95`, the
current pin), one commit. It follows C11 6.3.1.8 directly:

1. If the unsigned type's rank is at least the signed one's, the result is the
   unsigned type.
2. Otherwise, if the signed type can represent every value of the unsigned
   one, the result is the signed type.
3. Otherwise, the result is the signed type's unsigned counterpart.

Every LP64 result keeps its width. On LLP64, `unsigned int` vs `long` becomes
`unsigned long` instead of `unsigned int`, which has the same width. It adds
`c-tests/new/llp64-uint-llong-conv.c`.

Verified on Windows 11 / MSYS2 UCRT64 with the patched c2mir in
`build-win/_deps/mir-src`: the repro prints `-2`; `r7rs-bignums` matches under
`tur jit` *without* the bignum.tur cast; the new c-test passes under gcc.

To land it:

1. Merge [rjungemann/mir#4](https://github.com/turmeric-lang/mir/pull/4). The
   patch is reproduced below for reading without leaving this repo.
2. Bump `TUR_MIR_GIT_TAG` in `cmake/mir.cmake` to the merge commit, and add a
   line to the pin notes above it.
3. Re-run `TUR_TEST_FILTER='^r7rs' bash tests/run-jit.sh` on Windows.

The bignum.tur cast can stay: it is correct C either way.

```diff
diff --git a/c-tests/new/llp64-uint-llong-conv.c b/c-tests/new/llp64-uint-llong-conv.c
new file mode 100644
index 00000000..3c9fbbff
--- /dev/null
+++ b/c-tests/new/llp64-uint-llong-conv.c
@@ -0,0 +1,21 @@
+/* C11 6.3.1.8: `long long OP unsigned int` has type long long whenever long
+   long can represent every unsigned int value -- which includes LLP64
+   targets (win64), where long is 32 bits.  arithmetic_conversion used to ask
+   LONG_MAX about every signed type of rank long or above, so on win64 this
+   operation was done in unsigned int: (long long) 5u - 7u came out as
+   4294967294, and a base-1e9 bignum subtraction borrowed wrongly.  */
+int main (void) {
+  volatile unsigned int a = 5, b = 7;
+  volatile long long borrow = 0;
+  long long d = (long long) a - b;
+  if (d != -2) return 1;
+  if (!((long long) a - b < 0)) return 2;
+  long long t = (long long) a - b - borrow;
+  if ((t < 0 ? t + 1000000000u : t) != 999999998) return 3;
+  if (sizeof ((long long) 1 - 1u) != sizeof (long long)) return 4;
+  /* unsigned long vs long long: long long when it can hold every unsigned
+     long (LLP64), unsigned long long when it cannot (LP64).  Negative either
+     way is the discriminating bit only on LLP64, so check the size.  */
+  if (sizeof ((long long) 1 - 1ul) != sizeof (long long)) return 5;
+  return 0;
+}
diff --git a/c2mir/c2mir.c b/c2mir/c2mir.c
index 07fb461a..e5f0fcba 100644
--- a/c2mir/c2mir.c
+++ b/c2mir/c2mir.c
@@ -5998,16 +5998,27 @@ static struct type arithmetic_conversion (const struct type *type1, const struct
 
     if (signed_integer_type_p (&t1)) SWAP (t1, t2, t);
     assert (!signed_integer_type_p (&t1) && signed_integer_type_p (&t2));
-    if ((t1.u.basic_type == TP_ULONG && t2.u.basic_type < TP_LONG)
-        || (t1.u.basic_type == TP_ULLONG && t2.u.basic_type < TP_LLONG)) {
+    /* C11 6.3.1.8, with both operands promoted: the unsigned type wins when
+       its rank is at least the signed one's; otherwise the signed type wins
+       when it can represent every value of the unsigned one, and its unsigned
+       counterpart wins when it cannot.  This used to ask MIR_LONG_MAX about
+       every signed type of rank long or above, which on an LLP64 target
+       (win64: 32-bit long) sent `long long OP unsigned int` to unsigned int --
+       so `(int64_t) 5u - 7u` was 4294967294, not -2.  */
+    int urank = t1.u.basic_type == TP_UINT ? 1 : t1.u.basic_type == TP_ULONG ? 2 : 3;
+    int srank = t2.u.basic_type == TP_LONG ? 2 : t2.u.basic_type == TP_LLONG ? 3 : 1;
+    mir_ullong umax = (t1.u.basic_type == TP_UINT    ? (mir_ullong) MIR_UINT_MAX
+                       : t1.u.basic_type == TP_ULONG ? (mir_ullong) MIR_ULONG_MAX
+                                                     : (mir_ullong) MIR_ULLONG_MAX);
+    mir_ullong smax = (t2.u.basic_type == TP_LONG    ? (mir_ullong) MIR_LONG_MAX
+                       : t2.u.basic_type == TP_LLONG ? (mir_ullong) MIR_LLONG_MAX
+                                                     : (mir_ullong) MIR_INT_MAX);
+    if (urank >= srank) {
       res.u.basic_type = t1.u.basic_type;
-    } else if ((t1.u.basic_type == TP_UINT && t2.u.basic_type >= TP_LONG
-                && MIR_LONG_MAX >= MIR_UINT_MAX)
-               || (t1.u.basic_type == TP_ULONG && t2.u.basic_type >= TP_LLONG
-                   && MIR_LLONG_MAX >= MIR_ULONG_MAX)) {
+    } else if (smax >= umax) {
       res.u.basic_type = t2.u.basic_type;
     } else {
-      res.u.basic_type = t1.u.basic_type;
+      res.u.basic_type = t2.u.basic_type == TP_LONG ? TP_ULONG : TP_ULLONG;
     }
   }
   return res;
```

## Resolution (2026-09-28)

Landed exactly as the three steps above say:

1. [rjungemann/mir#4](https://github.com/turmeric-lang/mir/pull/4) merged with a
   merge commit, `79cb29058e8c`, on the fork's master. Its own CI (the
   ubuntu, macOS and Windows `test` jobs) was green. The gcc-farm and qemu
   jobs were cancelled because the fork has no self-hosted runners for them,
   which is also true of the earlier PRs.
2. `TUR_MIR_GIT_TAG` moved from `b7e72a95` to `79cb2905`, with a pin-note entry.
3. Re-verified on **Linux** (x86-64, gcc 13) only. There was no Windows box
   for this pass. A fresh `-DTUR_JIT=ON` build fetched `79cb2905`
   (`git -C build-jit/_deps/mir-src log` confirms it), and
   `TUR_TEST_FILTER='^r7rs' bash tests/run-jit.sh` gives 97 passed, 0 failed,
   1 skipped. With the patched MIR, `c2m` runs the new
   `c-tests/new/llp64-uint-llong-conv.c` cleanly (exit 0) under both `-eg`
   and `-ei`. The Windows result is the one recorded above against the same
   patch, applied in `build-win/_deps/mir-src` before it merged. The first
   Windows `run-jit.sh` on the new pin will confirm it end to end.

# poly-to-fat: a wide by-value aggregate argument crossed on three disagreeing ABIs

**Severity: medium** -- one half is a hard build failure with a diagnostic that
names nothing in the user's program; the other half is undefined behaviour that
happened to produce right answers on x86-64 at -O0. Found 2026-08-29 while
chasing the case left open at the bottom of
[arrow-struct-typed-arrow-abi](arrow-struct-typed-arrow-abi.md).

## The seam

A typeclass method that forwards its closure argument to a `^fat` sink boxes a
`tur_poly_fn_t {env, fn}` into a fat handle (`EX_POLY_TO_FAT`). Three parties
then have to agree on how one argument crosses:

| Party | Spelling of a wide by-value aggregate parameter |
| --- | --- |
| the sink's call site, casting slot 0 through the typed-thunk typedef | `int64_t` -- a heap-box pointer (`thunk_param_slot_c_name`) |
| the slot-0 shim (`ensure_typed_poly_to_fat`) | **the aggregate, by value** |
| the poly wrapper in slot 1 (`make_poly_wrapper`) | `int64_t` carrier, deref'd in the wrapper body |

The shim agreed with neither of the other two.

## Repro A -- hard build failure (> 16 bytes)

`Tuple3 int int int` is 24 bytes, so `sum3` takes `const T *`.

```turmeric
(defn call-t3 [^fat f : (fn [(Tuple3 int int int)] #fx{} int)
               x : (Tuple3 int int int)] : int
  (f x))

(defclass Box3 [^f] (box3 [container [fn :fn]] : int))
(definstance Box3 [BoxW]
  (box3 [container fn] (call-t3 fn (tuple3 11 22 33))))

(defn sum3 [t : (Tuple3 int int int)] : int
  (+ (tuple3-1st t) (+ (tuple3-2nd t) (tuple3-3rd t))))
```

```
error: aggregate value used where an integer was expected
```

`make_poly_wrapper` marks the argument in `poly_agg_arg_mask`, and the emit
side "derefs it back to the aggregate the callee parameter expects"
(`emit_expr.c`, Slice 3). That is right only *below* the pass-by-pointer
threshold. At 24 bytes the callee wants the pointer and the carrier word
already is one, so the word wanted retyping, not dereferencing -- and doing
both emitted `sum3((const T *)(intptr_t)(*(T *)(intptr_t)(x)))`.

The by-value **sum** site a few hundred lines down already draws exactly this
fork ("The param is pass-by-pointer, and the carrier word already IS a pointer
to the aggregate... just retype the word"). The poly-wrapper's inner call did
not.

## Repro B -- undefined behaviour that returned the right answer

```turmeric
(defn call-tf [^fat f : (fn [(Tuple2 float float)] #fx{} float)
               x : (Tuple2 float float)] : float
  (f x))
```

Printed `3.75`, correctly, from this:

```c
/* call site: passes a POINTER through an int64 slot */
(*(tur_thunk_double_int64_t_t *)f)((void *)f, (int64_t)(intptr_t)(&__t170))

/* slot 0: declares the aggregate BY VALUE (SysV class SSE -> xmm0:xmm1) */
static double __tur_poly_to_fat1_double_tur_adt_Tuple2__float__float(
        void *__e, tur_adt_Tuple2__float__float a0) { ... }

/* slot 1: declares the int64 carrier and derefs it itself */
static double __poly_1377(void *env, int64_t x) {
    return sumf(*(tur_adt_Tuple2__float__float *)(intptr_t)(x));
}
```

The shim's declared parameter is not in the same register *class* as what the
caller passes or what the callee reads. The answer arrived because the shim,
reading the SSE registers, left the integer register holding the pointer
untouched for the wrapper to find -- incidental to the ABI, not guaranteed by
it. `(Tuple2 int int)` survived the same way one register over. Repro A is the
same defect where no such overlap exists.

## Resolution (2026-08-29)

Two independent fixes, one per half.

**`ensure_typed_poly_to_fat`** (`src/compiler/emit_module.c`) now spells each
parameter with `thunk_param_slot_c_name` -- the same routine the sink's typed
thunk typedef uses -- on both the shim signature and the slot-1 cast, so all
three parties agree by construction. Unlike the bare-fn typed fatshim, this
shim never bridges an argument: slot 1 is the poly wrapper, which takes the
carrier and derefs it itself, so the slot word is forwarded as-is. The
`all_int64` early-out compares on the same slot spelling, so
`(fn [(Tuple2 int int)] int)` is now recognised as the all-int64 signature it
really is and rides the preamble `__tur_poly_to_fat<N>` shim -- which was
always the honest one for it.

**The `poly_agg_arg_mask` unbox** (`src/compiler/emit_expr.c`) forks on the
pass-by-pointer threshold: past it, retype the carrier word as `const T *`
instead of dereferencing. The later wide-byval pbp arg site may retype again,
so the emitted C can carry the cast twice; that is deliberate rather than
tidied away, because a pointer-to-pointer cast is idempotent and applying it
here is correct whether or not the later site fires, whereas leaving the word
bare would depend on that site firing for every shape.

- Fixtures: `tests/fixtures/poly-to-fat-pbp-aggregate-arg` (repro A),
  `tests/fixtures/poly-to-fat-wide-byval-arg` (repro B, plus the 16-byte
  integer pair). The float pair carries a non-zero fractional part on purpose:
  an integral pair cannot show a register-class mix-up, because the wrong lane
  still prints as a round number.
- Verification: `bash tests/run.sh` 2732 passed, 0 failed -- no fixture
  snapshot moved; `tests/run-jit.sh` (c2mir) green. The four pre-existing
  `poly-to-fat-*` fixtures (`-bare-fat-sink`, `-float-named-fn`,
  `-float-roundtrip`, `-multiarg-roundtrip`) all still pass, which is what
  pins the scalar-float shim this change sits next to.

## Note on how this was reached

The parent report's closing note guessed there was "no known miscompile" here
because a poly-carrier value is erased to int64 by construction. That reasoning
was sound for the *carrier* and wrong about the *sink*: an untyped `^fat` sink
is indeed all-int64 and safe, but a sink with a declared concrete signature
routes through `ensure_typed_poly_to_fat`, which is where the three spellings
diverged. Worth remembering as a shape: "the value is erased" does not imply
"every shim that carries it is".

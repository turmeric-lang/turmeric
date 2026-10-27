# Calling a `^fat` parameter whose fn type carries a non-empty effect row segfaults

> **RESOLVED 2026-07-30.** The E2a registry lookup keyed on the parameter's own
> value; a `^fat` parameter holds a boxed `{ shim, direct-entry }` record, not
> the direct entry the registry is keyed on, so the lookup missed every time and
> the miss was called unguarded. Details in *Resolution* at the end.

**Severity:** high (silent miscompile -> crash). It type-checked clean, emitted C
that compiled clean, and died at runtime with no diagnostic. Any API declaring a
callback as effectful -- middleware, a logging hook, a visitor that may
`perform` -- was unusable through a `^fat` parameter.

Verified against `./build/tur` at **v0.32.2** (Debug), on branch
`claude/composite-type-alias-gap-yd70bn`.

## Summary

A **`^fat`** parameter typed `(fn [T] #fx{E} R)` with a **non-empty** effect row
crashed the moment it was called. Neither a `handle` nor an actual `perform` was
involved -- the callee never performed anything and there was no handler in the
program. Both conditions were required:

| Parameter | Result |
|---|---|
| `[lg : (fn [int] #fx{Log} nil) ...]` (no `^fat`) | works |
| `[^fat lg : (fn [int] nil) ...]` | works |
| `[^fat lg : (fn [int] #fx{} nil) ...]` (empty row) | works |
| `[^fat lg : (fn [int] #fx{Log} nil) ...]` | **SIGSEGV** |
| `[^fat lg : (fn [int] #fx{Log} int) ...]` | **SIGSEGV** |

The passed value's shape did not matter: an inline lambda and a named top-level
`defn` both crashed.

`^fat` mattering is what made this easy to misread on first contact. The initial
probes all carried `^fat` because they were lifted from the Backtrack Monad in
`logic-programming-guide.md`, which uses it throughout -- so the first
characterisation ("any non-empty effect row on a fn-typed param") was too broad.
Dropping `^fat` makes the same program run correctly end to end, handler traffic
and all.

## Repro

```turmeric
(defeffect Log [n : int] : nil)

(defn use-fn [^fat lg : (fn [int] #fx{Log} nil) n : int] #fx{Log} : int
  (do (lg n) 0))

(defn main [] : int
  (use-fn (fn [x : int] nil) 42))
```
```
$ tur check q1.tur      # clean, exit 0
$ tur run q1.tur
Segmentation fault
```

Control -- identical but with an empty row -- ran fine:

```turmeric
(defn use-fn [^fat lg : (fn [int] #fx{} nil) n : int] : int
  (do (lg n) 0))

(defn main [] : int
  (use-fn (fn [x : int] nil) 42))
```

## Root cause

The non-empty row makes the emitter CPS-convert the call and route it through
the E2a direct-entry -> CPS-entry registry (`via_registry` in
`src/passes/cps_ir.c`, emitted at `src/compiler/emit_cps_ir.c`). From
`tur emit-c` on the repro:

```c
static int64_t use_hyfn__cps(int64_t lg, int64_t n, DK *__kont) {
    return ((int64_t (*)(int64_t, DK *))__tur_cps_lookup((intptr_t)lg))(
        n, __dk_reap_node(dk_frame(use_hyfn_j0, 0, __kont)));
    /* E2a threaded fn-value heap join */
}
```

versus the empty-row control, which uses the ordinary fat-shim call:

```c
(*(tur_thunk_void_int64_t_t *)((void *)(intptr_t)(lg)))((void *)(intptr_t)(lg), n);
```

Two things went wrong in the first form:

1. **The key was wrong.** `__tur_cps_lookup` is keyed on a function's *direct
   entry address*, registered at startup by `__tur_cps_register`. A plain
   fn-value param holds that address. A `^fat` param does not: its calling
   convention guarantees an already-boxed `{ shim, direct-entry }` fat record
   (the arg loop auto-shims a thin fn into one -- see the comment at
   `elab_call.c:5476`), carried as the int64 pointer carrier. So the key was a
   heap address that was never registered.

   The registration was sitting right there. For the named-`defn` case the
   emitted C contains both halves, and they simply never meet:

   ```c
   __tur_cps_register((intptr_t)logit, (__tur_cps_fn)logit__cps);  /* keyed on logit   */
   __t155[1] = (int64_t)(intptr_t)logit;                           /* slot 1 IS logit  */
   ... __tur_cps_lookup((intptr_t)lg) ...                          /* keyed on the box */
   ```

2. **The miss was unguarded.** `__tur_cps_lookup` returns `(__tur_cps_fn)0` when
   nothing matches, and the emitted code called the result immediately with no
   NULL check -- so a registry miss was a call through NULL rather than a
   diagnostic.

## Resolution -- 2026-07-30

Both halves fixed.

**The key** (`e2a_lookup_key`, `src/compiler/emit_cps_ir.c`). When the
`via_registry` callee is an `is_fat` binding, the lookup reads the fat record's
direct-entry slot -- `((int64_t *)(intptr_t)(lg))[1]` -- which is exactly what
`__tur_fatshim_*` dispatches through. A non-`^fat` callee keeps `(intptr_t)lg`
unchanged. Applied at both `via_registry` emit sites, tail and heap-join.

**The miss** (`__tur_cps_lookup_checked`, `src/compiler/emit_dk_runtime.c`). A
new checked wrapper aborts with a named message instead of calling NULL:

```
tur: internal error: no CPS entry registered for effectful fn-value 'lg'
  -- it cannot thread the effect handler chain
```

Both `via_registry` sites now go through it. Only a value whose CPS entry was
registered at startup can thread the handler chain, so a miss is a compiler bug
-- worth an abort that names itself rather than a SIGSEGV that does not.

This also un-blocks `(defalias Logger (fn [cstr] #fx{Log} nil))`, the alias that
surfaced this in the first place.

### Tests

Neither `via_registry` path had any fixture coverage with a `^fat` callee --
which is why this survived. Regenerating all 140 codegen snapshots produced
**only** the new preamble block and zero call-site changes, confirming it.

- `tests/fixtures/effectful-fat-fn-param/` -- inline-lambda shapes: performing
  and not performing, tail and non-tail (heap-join) position, `nil` and `int`
  results, with the empty-row and no-row spellings as controls.
- `tests/fixtures/effectful-fat-fn-param-named/` -- the named-`defn` shape, kept
  separate for the reason below.

`bash tests/run.sh`: **2442 passed, 0 failed**.

### A second defect on the same shape -- also fixed

While writing `effectful-fat-fn-param-named` it turned out that passing a
**named** effectful `defn` as a `^fat` fn-value only reached codegen at all when
the call was the sole `handle` body; sequencing it inside a `do`, or nesting its
result in a builtin call, aborted the compiler with
`effect form (EX kind 57) reached the direct/fiber emitter`. Confirmed
orthogonal at the time (it aborted identically on a compiler rebuilt with this
fix stashed), filed separately, and since fixed:
[named-effectful-defn-as-fat-fn-value-ices.md](named-effectful-defn-as-fat-fn-value-ices.md).

Root cause there was the mirror image of this one -- boxing a function into a
fat closure was counted as *performing* its effect, where here the box was not
recognised as *holding* the function. Both come from the fat record sitting
between a named fn and the code that uses it. `effectful-fat-fn-param-named` now
exercises all three call positions rather than avoiding two of them.

## Found while

Validating that `defalias` composite targets are transparent for
[composite-type-alias-gap.md](composite-type-alias-gap.md).
`(defalias Logger (fn [cstr] #fx{Log} nil))` crashed, and the control with the
type spelled inline crashed identically -- so aliases were never involved.

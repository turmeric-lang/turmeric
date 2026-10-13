# CPS wrapper stores a by-value ADT into an `int64_t` temp with no bridging cast

**Severity: high -- a v0.48.0 -> v0.49.0 regression that fails the build on
macOS.** A CPS-lowered function that ascribes a by-value-ADT-returning call
to `:int` emits `__t0 = __t252;` where `__t0` is `int64_t` and `__t252` is a
`tur_adt_Vec__int *`. Under AppleClang 21 `-Wint-conversion` is an error by
default, so the C compile fails outright; under Linux gcc it is a warning and
the build proceeds, which is why this is a macOS-only red.

Found 2026-09-16 when `plot (macos-latest)` went red on an unrelated
turmeric-spices PR. **`plot` is not at fault and the PR is not at fault** --
the same `plot` tree, unchanged, builds clean on v0.48.0 and fails on
v0.49.0.

## Repro

`turmeric-spices` at `main` (5b6f308), spice `plot`, no source changes:

```sh
cd spices/plot && tur fetch

# v0.49.0 -- all 11 suites fail to compile
.../turmeric-v0.49.0-macos-arm64/bin/tur test tests
# 11 tests, 0 passed, 11 failed

# v0.48.0 -- same tree, all green
.../turmeric-v0.48.0-macos-arm64/bin/tur test tests
# 11 tests, 11 passed, 0 failed
```

Every one of the 11 failures is the same single error, differing only in
line number:

```
tests_plot_bbox_test_tur.c:6988:10: error: incompatible pointer to integer
  conversion assigning to 'int64_t' (aka 'long long') from 'tur_adt_Vec__int *'
  (aka 'struct tur_adt_Vec__int *') [-Wint-conversion]
 6988 |     __t0 = __t252;
```

## The source construct

`spices/plot/src/plot/core.tur:910`:

```turmeric
(defn plot-anyrenderers [rs : (Vec (exists [a] [(Renderer a)] a)) opts : int] : ptr<void>
  (plot (:: (anyrenderers->legacy rs) :int) opts))
```

`anyrenderers->legacy` is declared `: (Vec int)` (`core.tur:894`) and its
result is ascribed to `:int` as the argument to a tail call.

## The emitted C

The CPS wrapper for that function:

```c
static int64_t plot__core__plot_hyanyrenderers__cps(int64_t rs, int64_t opts, DK *__kont) {
    int64_t __t0;
    void * __t1;
    tur_adt_Vec__int * __t252 = plot__core__anyrenderers_hy_gtlegacy((int64_t)(intptr_t)rs); /* cps->direct */
    if (tur_panicking) return ((int64_t)0);
    __t0 = __t252;          /* <-- int64_t = tur_adt_Vec__int *, no cast */
    goto L2;
    L2:;
    __t1 = plot__core__plot(__t0, opts); /* cps->direct */
    ...
}
```

`__t0` is declared `int64_t`, which is right -- it is the `(:: ... :int)`
ascription's temp. The defect is the **store**: the typed ADT pointer is
assigned straight into it. Note the same function's `rs` parameter IS
bridged correctly one line above (`(int64_t)(intptr_t)rs`), and the direct
(non-CPS) emission of the same module bridges this class of store properly:

```c
void *__t250 = __t248;
int64_t go_1847 = (int64_t)(intptr_t)(__t250);
```

So the direct emitter has the bridge and the CPS wrapper does not.

## Relationship to already-fixed reports

This is the same family as `global-def-store-misses-int-ptr-bridge`
(resolved 2026-09-11), which was the module-level-`def` store missing the
same cast, and it has the same macOS-only signature for the same reason.
That fix evidently did not reach the CPS wrapper's temp store. Worth
checking whether the two share a store-emission helper that only one caller
was routed through.

## Minimization -- what did NOT reproduce

Two hand-written probes failed to trigger it, which narrows where to look:

1. A `perform`-colored `defn` whose body contains
   `(sink (:: (mk) :int))`, with `mk : (Vec int)` -- **builds clean**. So a
   colored function plus an ascribed by-value-ADT call is not sufficient on
   its own.
2. The same with the ascribed call as the argument of a *tail* call to a
   colored callee -- did not get far enough to judge; that shape evicted the
   CPS backend ("this effect operation has no lowering here"), which is a
   separate limitation.

The distinguishing features of the real case, still unprobed: the argument
is `(Vec (exists [a] [(Renderer a)] a))` (an existential element type), the
producer uses `letrec` + `open`, and the whole thing sits in tail position
of a function returning `ptr<void>`. A minimizer should start from
`plot-anyrenderers` and remove one of those at a time rather than building
up from a bare effect.

## Why CI did not catch it earlier

- turmeric-spices CI builds `tur` from **tip-of-main turmeric** on every
  run, so it picks up turmeric regressions with no version bump on this
  side. The last green spices `main` run was 2026-09-15 (5b6f308); v0.49.0's
  codegen change landed in turmeric main after that.
- The Linux leg passes. `plot (ubuntu-latest)` is green on the very run
  where `plot (macos-latest)` fails, because gcc treats `-Wint-conversion`
  as a warning. Any int/ptr straddle is therefore invisible on Linux and
  only ever surfaces as a macOS-only red -- see the same pattern in
  `global-def-store-misses-int-ptr-bridge`.

## Fix directions

1. Route the CPS wrapper's ascription-temp store through whatever emits the
   bridging cast on the direct path (`(int64_t)(intptr_t)`), rather than
   assigning the callee's declared C type straight in. The temp's declared
   type is already correct; only the store is wrong.
2. Add a fixture pinning a CPS-lowered `(:: <call returning a by-value ADT>
   :int)`. There is currently no fixture in this shape, which is why a
   codegen change could move it silently.
3. Consider building the Linux CI leg with `-Werror=int-conversion` so this
   class stops being macOS-only. Every instance so far has been found by a
   macOS red after a Linux green, which makes the feedback loop as slow as
   it can be.

## Guides to update when fixed

None identified -- this is a codegen defect with no documented behavior
attached to it.

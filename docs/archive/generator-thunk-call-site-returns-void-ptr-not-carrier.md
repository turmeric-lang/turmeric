---
title: A generator's thunk call site returns void * where the closure returns int64_t
category: Archive
description: A closure literal applied inside a generic's generator body is lifted into the generator state and dispatched fat; the call site, and at a narrow type the slot-0 widen wrapper, called the carrier-returning base thunk at a spec-resolved return type. Return-type-only, so only -fsanitize=function saw it. Both sites now follow the thunk's recorded return spelling.
---

# A generator's thunk call site returns `void *` where the closure returns `int64_t`

> **RESOLVED 2026-10-03.** Fix direction 1, and a second site the reduction
> had not reached.  The ingredient table below is narrower than the defect:
> the generic `thunk` is not load-bearing -- `(gen-unwrap (gen-next (gen []
> (yield ((fn [] x))))))` in a generic traps on its own at A := cstr or
> bool.  What matters is that the head temp `__call_head_N` is lifted into
> the generator's STATE STRUCT, which sends the call down the fat-dispatch
> path (`emit_expr.c`, the `TY_FN` non-global branch) instead of the direct
> thunk call an ordinary `let` gets.  The lambda is emitted once, at the
> generic's int64 carrier; two sites then disagreed with it:
>
> 1. **The call site.**  The closure-head block
>    (`closure-head-dispatch-follows-emitted-signature`) already read the
>    thunk's recorded return spelling, `int64_t`, and set the result to
>    `int`.  The later `word_back` block (from
>    `fnsan-parametric-fn-field-read-by-spec`) then saw an `int` result and a
>    pointer-resolving declared type, and re-derived the pointer -- casting
>    slot 0 to `const char *(*)(void *)` / `void *(*)(void *)`.  It now
>    stands down when the head's signature is known (`head_sig_known`).
> 2. **The construction site**, at A := bool / int8 / uint8 (not in the
>    filing; found widening the probe).  `narrow-closure-result-read-through-
>    int64-carrier` wraps slot 0 in `__tur_widen_<thunk>`, which called the
>    BASE thunk as `bool (*)(void *)` because the result type was resolved
>    through the spec.  With no inner-closure clone, slot 0 is the base thunk,
>    so the wrapper now follows its recorded spelling: a thunk that returns
>    `int64_t` needs no widening.
>
> Pinned by `tests/fixtures/generator-thunk-call-site-carrier` (16 lines: the
> fuzzer's case, cstr, bool, int, int8, uint8, float32, a by-value struct, an
> Option, a fn value, and a one-argument lambda; it traps under the fnsan gate
> on the pre-fix compiler and matches `--interpret` after).  `tests/run-fnsan.sh`
> armed with clang 18: 3534 passed, 0 failed; `bash tests/run.sh` 3534/0.
> `type-fuzz-src.py --seed 20261003 --n 400` armed: ok 370, SEAM_REJECT 29,
> GEN_REJECT 1, 0 BUG -- case 376 is `ok` rather than `KNOWN`, every other
> bucket as filed; `--known-probes` reports the row FIXED.  The `known_bug_slug` row in
> `tests/type-fuzz-src.py` is retired, so a regression is a `BUG_fnptr_trap`
> again; the pinned probe stays as a FIXED row.


**Severity: low-medium.** Undefined behavior, not a wrong answer. The mismatch
is ABI-benign on LP64 -- both types are 8 bytes returned in the same register
-- so the program prints the correct result and only
`-fsanitize=function` sees it. It is a latent hazard: nothing guarantees a
future target, or a sufficiently aggressive optimizer, keeps treating the two
as interchangeable.

Found by the nightly `Fuzz` workflow, `type-fuzz-src` seed **20261003**,
case 376, as `BUG_fnptr_trap`
(tags `closure_ret,gbody,gid,scalar_bool,thin_hof,thunk`).

## Minimal repro

Four forms, reduced from the generator's 11:

```turmeric
(defn mk [x : bool] : (fn [] bool) (fn [] x))
(defn thunk [B] [v : B] : (fn [] B) (fn [] v))
(defn gbody [A] [x : A] : A
  ((thunk (gen-unwrap (gen-next (gen [] (yield ((fn [] x)))))))))
(defn main [] : int
  (println ((gbody (mk true))))
  0)
```

```sh
# correct answer, no diagnostic
tur build repro.tur -o repro && ./repro            # prints: true

# the same program under the function-pointer check
CC=<a clang with -fsanitize=function> \
TUR_CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -fsanitize=function -fsanitize-trap=function" \
  tur build repro.tur -o repro && ./repro          # dies: SIGTRAP, no output
```

## Root cause

With `-fsanitize=function` and no `-fsanitize-trap`, clang names it exactly:

```
runtime error: call to function __fn_19 through pointer to incorrect
               function type 'void *(*)(void *)'
note: __fn_19 defined here
```

In the emitted C the closure is defined returning the carrier:

```c
static int64_t __fn_19(void * __env_p_22) {
        struct __env_21 *__env___env_21 = (struct __env_21 *)__env_p_22;
        return __env___env_21->x;
}
```

and the generator's resume body calls it through a `void *`-returning thunk:

```c
typedef void * (*tur_thunk_void___t)(void *);
...
int64_t __ps_200 = ((int64_t)(intptr_t)(
    (*( tur_thunk_void___t *)((void *)(intptr_t)(__g->__call_head_24)))
        ((void *)(intptr_t)(__g->__call_head_24))));
```

So it is purely a **return-type** disagreement -- `void *` at the call site
versus `int64_t` at the definition. The parameter lists match, and the result
is immediately cast back through `(int64_t)(intptr_t)`, which is why the value
survives. The call site picked the generic `void *` thunk shape while the
closure body was emitted against the type variable's `int64_t` carrier;
whichever of the two is authoritative, they are chosen independently here.

## Which ingredients are load-bearing

Measured by reduction; `plain` is the default build, `fnsan` adds the check.
Every row prints `true` under `plain`.

| variant | fnsan |
| --- | --- |
| the four forms above | **TRAP** |
| ... with the generator removed (`(thunk x)`) | no trap |
| ... yielding `x` instead of `((fn [] x))` | no trap |
| ... yielding `(let [l x] l)` | no trap |
| ... with `thunk` removed (yield's value returned directly) | no trap |
| ... with a thin HOF added back | TRAP (as found) |
| ... with a generic identity pass-through added back | TRAP (as found) |

So three things must coincide: a **generator yield**, an **immediately-applied
closure** as the yielded expression, and a **generic thunk** wrapping the
unwrapped result. The `gid` and `thin_hof` crossings in the original tags are
incidental -- the finding reproduces without them.

## Fix directions

1. **Emit the thunk call site at the closure's own return type.** The call
   already casts the result back through `(int64_t)(intptr_t)`, so the narrow
   change is to select the `int64_t`-returning thunk typedef here rather than
   `tur_thunk_void___t`. Cheapest if the generator lowering has the callee's
   emitted return type to hand at that point.
2. **Emit the closure returning `void *`** so it matches the generic thunk.
   Consistent with the call site, but it moves the cast rather than removing
   it, and the closure's return type is the tyvar's carrier everywhere else.
3. **Make the thunk typedef family carrier-parameterized** so both sides
   derive the same type from one place, which is what would stop the next
   instance of this rather than this one.

## A caveat about the pinned probe

This defect is invisible without `-fsanitize=function`: the program builds,
runs, and prints the right answer. Its `--known-probes` row therefore reports
`FIXED` on any box where fnsan is unavailable -- notably stock macOS, where
Apple clang does not provide it and the harness prints
`fnsan: UNAVAILABLE`. **Do not retire this row on a `FIXED` from a run whose
banner does not say `fnsan: ARMED`.** Reproduce with a clang that has the
check (Homebrew LLVM works: `CC=$(brew --prefix llvm)/bin/clang`).

Related: on arm64 the trap arrives as SIGTRAP rather than SIGILL, which the
harnesses misclassified as `BUG_toolchain_other` until
`tests/fuzz_arm.py` learned both statuses.

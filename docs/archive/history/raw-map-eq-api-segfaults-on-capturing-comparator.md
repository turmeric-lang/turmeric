# Raw `map-*-eq` API segfaults when handed a capturing closure comparator (compiled path)

> **RESOLVED 2026-06-17 (direction 1, via the compiler enabler).** The public
> raw `map-*-eq` `keyeq` parameter is now typed `(c-fn [K K] bool)`, so the
> existing C-function-pointer checker (`elab_call.c`) rejects a capturing closure
> at **compile time** with a clear diagnostic instead of letting it segfault at
> runtime. The enabling sub-task identified in the 2026-06-17 investigation
> below -- "a type variable inside `(c-fn ...)` does not unify with the outer
> tyvar" -- was fixed first: the c-fn argument/result signature check now treats
> a tyvar position in the *expected* `c-fn` as a wildcard (it still enforces the
> no-environment and arity contracts), so `(c-fn [K K] bool)` works on the
> generic `map-assoc-eq [K V]` for `int`, `cstr`, and `float` keys alike.
> `mk-cmp`'s carrier-ABI `:int` address was left as-is (it feeds the internal,
> untyped `-eq-o` variants the typeclass macros use); the one fixture that passed
> `(mk-cmp 0)` to the *public* API (`typed/map-collision-forced`) now passes the
> captureless `tur-int-carrier-eq?` that `mk-cmp[int]` returns the address of --
> same comparator, honest type. Verified: the capturing-closure repro is a clean
> compile error; `cstr` / `int` captureless comparators and the forced-collision
> test pass on both `--interpret` and `tur run`; `tests/run.sh` and
> `tests/run-turi.sh` stay green with **zero snapshot churn** (no affected
> fixture has an `expected.c`). Moving to `docs/archive/` per the resolved-report
> rule.

**Summary:** The raw content-keyed map API (`map-assoc-eq` / `map-get-eq` /
`map-has-eq?` / `map-dissoc-eq`, stdlib/map.tur) declares its `keyeq` parameter
*untyped* and its inline-C body casts it straight to a bare C function pointer
(`(void *)(intptr_t)keyeq`). Passing a **capturing** closure there compiles
without complaint and then **segfaults at runtime** on the compiled path -- the
fat-closure box pointer is called as if it were `bool(*)(int64_t,int64_t)`. The
interpreter handles the same source correctly (TI10 Tier B routes a
`TURI_CLOSURE` comparator through the `tur_hamt_*_eq_ctx` trampoline), so this is
also a `tur` vs `turi` divergence.

**Severity:** Medium. Hard crash (SIGSEGV), not a silent miscompile, and only on
*misuse* of the raw API -- but the type checker gives no help: an untyped `keyeq`
accepts a capturing closure that the inline-C contract cannot honor. The
type-erasing untyped parameter is exactly the kind of hole CLAUDE.md's "No Lazy
`:int` Stand-Ins" rule warns about, one level removed (here it is an *untyped*
param rather than `:int`, but the effect is the same: the compiler cannot reject
a value the callee cannot use).

## Minimal repro

```turmeric
(defn main [] : int
  (let [k   42
        cmp (fn [a : int b : int] : bool (= (+ a k) (+ b k)))   ;; captures k
        m   (map-assoc-eq (map-assoc-eq (:: (map-new) (Map int int)) 0 1 100 cmp)
                          0 1 200 cmp)]
    (println (map-get-eq m 0 1 cmp)))
  0)
```

| Path | Result |
| --- | --- |
| `tur run` (compiled) | **Segmentation fault** (exit 139) |
| `tur --interpret`    | `200` (correct) |

A *captureless* lambda or a top-level `defn` works on both paths, because the
elaborator lowers it to a plain C function pointer -- which is what the inline-C
expects.

## Root cause

`stdlib/map.tur`:

```turmeric
(defn map-assoc-eq [K V]
  [m (Map K V) h : int key : K val : V keyeq]      ; <-- keyeq is UNTYPED
  : (Map K V)
  ```c
  ...
  void *new_hamt = tur_hamt_set_eq(map->hamt, (uint64_t)h, (void *)(intptr_t)key,
                                   (void *)(intptr_t)val,
                                   (void *)(intptr_t)keyeq);   ; <-- cast to C fn ptr
  ...
  ```)
```

`tur_hamt_set_eq` (src/runtime/hamt.c) takes
`tur_hamt_keyeq_fn = bool(*)(int64_t,int64_t)` and, on a hash collision, calls
`keyeq(a, b)`. When `keyeq` is a capturing closure, the value passed is a
pointer to the closure's fat box, not code; calling it jumps into data -> SIGSEGV.
The contract the inline-C assumes ("`keyeq` is an int64 carrying a C function
pointer address" -- what a `MapKey` `mk-cmp` instance returns) is undocumented at
the type level, so nothing rejects a closure.

This is the compiled-path mirror of what TI10 Tier B fixed for the interpreter:
the interpreter's `native_map_assoc_eq` (src/main.c) detects
`a[4].tag == TURI_CLOSURE` and routes through `tur_hamt_set_eq_ctx` +
`map_turi_eq_tramp`, so a closure works there.

## Expected behavior

One of:

1. **Reject at elaboration.** Give `keyeq` a function-pointer type the checker
   can enforce (a non-`^fat`, captureless `(fn [K K] bool)`), so a capturing
   closure is a compile error pointing the user at either a top-level `defn` /
   captureless lambda, or the typeclass `Map[K V]` surface.
2. **Support it.** Declare `keyeq` `^fat` and fat-dispatch it in the inline-C +
   thread a ctx through `tur_hamt_*_eq` on the compiled path too (the `_eq_ctx`
   family already exists in the runtime for the interpreter). This is the larger
   fix and carries fixture-snapshot churn for the new ABI.

Direction (1) is the smaller, safer change and matches the API's actual intended
use (the typed `map-assoc`/`map-get`/... macros always feed `mk-cmp`'s C address).

## Validation of a fix

- The minimal repro above should either compile-error (direction 1) or print
  `200` on both paths (direction 2).
- `tests/fixtures/tib-map-reentrant-comparator/` (added alongside this report)
  exercises the *supported* shape -- top-level captureless comparators reading a
  global map -- and passes on both paths; it should stay green.
- `bash tests/run.sh` and `bash tests/run-turi.sh` stay green.

## Investigation 2026-06-17 -- both directions are blocked on deeper work

An attempt at direction (1) (type `keyeq` so the checker rejects capturing
closures) found the clean fix is **not** focused -- it is entangled with the
carrier-ABI type erasure and a compiler limitation. Three concrete blockers,
each reproduced:

1. **`mk-cmp` returns `:int`, not a function type.** The public raw API
   legitimately accepts *both* a captureless fn (e.g. `tur-cstr-key-eq?`) **and**
   `mk-cmp`'s C-address `:int` (see `tests/fixtures/typed/map-collision-forced`,
   which passes `(mk-cmp 0)`). Typing `keyeq` as any function type rejects the
   `:int` address path, so `mk-cmp` would have to be retyped across the whole
   `MapKey` typeclass (method + int/bool/cstr/float32/float instances + the
   `native_mk_cmp_*` overrides in `src/main.c` + user struct-key instances in
   fixtures).

2. **Carrier comparators are int-typed except `cstr`.** `tur-int-carrier-eq?` /
   `tur-f32-carrier-eq?` / `tur-f64-carrier-eq?` are all `(fn [int int] bool)`
   (they bit-reinterpret the int64 carrier word), while `tur-cstr-key-eq?` is
   `(fn [cstr cstr] bool)`. So the comparator's true type is **not** uniformly
   `(... [K K] ...)` -- a `float` key's comparator is `[int int]`, not
   `[float float]`. A single typed `mk-cmp` signature cannot describe all
   instances honestly.

3. **A type variable inside `(c-fn ...)` does not unify with the outer tyvar.**
   Typing `keyeq : (c-fn [K K] bool)` on the generic `map-assoc-eq [K V]`
   resolved to `(c-fn [int int] bool)` regardless of the map's `K` (it ignored
   `K = cstr` for `tce4-map-cstr-key`). `c-fn` is for concrete C-ABI types; it is
   untested with type variables (zero `c-fn` uses in stdlib). And a plain
   non-`^fat` `(fn [K K] bool)` param boxes the comparator into a *fat aggregate*
   that the inline-C `(void *)(intptr_t)keyeq` cannot cast -- breaking **all**
   callers, not just capturing ones.

**Consequence.** The c-fn rejection path (`elab_call.c:2896`, the existing
"argument N is a capturing closure, but the parameter is a C function pointer"
diagnostic) *does* fire correctly when `keyeq` is a concrete `(c-fn [int int]
bool)` -- the capturing-closure repro becomes a clean compile error. But making
that typing correct for *all* key types requires either:

- **(1')** compiler support for type variables inside `(c-fn [...] ...)` (so
  `(c-fn [K K] bool)` binds `K` to the map's key type), **plus** retyping
  `mk-cmp` to return `(c-fn [a a] bool)` (benign for `float`, whose body returns
  an int-carrier comparator the HAMT only ever calls int-level); or
- **(2)** the full fat-dispatch rework: make `keyeq` `^fat`, have `mk-cmp` return
  the comparator *function* (not its `:int` address), fat-dispatch it in the
  inline-C / thread a ctx through `tur_hamt_*_eq` on the compiled path (the
  `_eq_ctx` family already exists for the interpreter).

Both are sizable Track A / typeclass-ABI changes with broad fixture-snapshot
churn -- **not** the "smaller, safer" change the original directions implied. The
enabling sub-task is **(1')'s compiler half**: support tyvars in `c-fn`. With
that, direction (1) becomes a contained stdlib change. Recommend sequencing the
compiler support first.

## Notes

Found while adding interpreter-reentrancy coverage for TI10 Tier B
(`docs/upcoming/turi-parity-post-v1-plan.md`). Tier B itself (the interpreter
side) is complete and correct; this report is strictly about the *compiled*
raw-API footgun and is Track A / map.tur surface, not interpreter work.

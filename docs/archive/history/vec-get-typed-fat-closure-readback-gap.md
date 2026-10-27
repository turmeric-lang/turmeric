---
title: vec-get of a Vec<typed fat closure> cannot be re-called as the typed closure
category: Reported
severity: medium
description: `vec-get` is declared `[A] [v :int i :int] :A`, but reading back an element that was pushed as a typed fat closure (e.g. an `SF Sample Sample` of type `(fn [^fat sig :(fn [:float] :float)] :ptr<void>)`) gives the elaborator no way to recover the closure type at the call site. The returned value elaborates as `:int` / `:ptr<void>`, and applying it (`(sf sig)`) is rejected with "expected (fn [] : ?), got int" -- even when the surrounding `let` is annotated with the expected closure type, and even when the readback flows through a fixed-arity typed helper. Binding into `^fat` in a `let` is not accepted either. Practical effect: a fold over `Vec<SF Sample Sample>` -- the natural shape for `effects-chain` per tur-signal-rebuild-plan -- cannot currently be expressed without per-element inline-C dispatch, which is the banned pattern the rebuild explicitly forbids.
---

# `vec-get` of a `Vec<typed fat closure>` cannot be re-called as the typed closure

> **Status: RESOLVED.** The `Vec<SF>` fold now elaborates, builds, and runs
> leak-clean. The readback uses the documented `::` ascription idiom
> (`(:: (vec-get effects i) :ptr<void>)`); the gaps that actually blocked the
> shape were five interacting closure/fat-dispatch bugs, all fixed:
>
> 1. **Recursion `arg_fat` propagation** (`elab_fns.c`, Phase HRT5 early-update):
>    a self-recursive call passing a `:ptr<void>` fat box into the function's own
>    `^fat` fn param saw the stale pass-1 `arg_fat=false` and rejected the arg
>    with a spurious `expected (fn [] : ?), got ptr<void>`. The forward binding
>    now carries `arg_fat`/`result_fat` before the body is elaborated.
> 2. **`if`-branch fat/`:ptr<void>` unification** (`elab_forms.c`, `elab_if`):
>    the fold base case `(if done sig (loop ... (apply ...)))` mixes a `^fat` fn
>    value (`sig`, `TY_FN`) with the threaded `:ptr<void>` recursive arm. Both
>    are fat boxes, so the `if` now widens to `:ptr<void>` instead of reporting a
>    `then=(fn ...) else=ptr<void>` mismatch.
> 3. **`^fat` in `let`-binding position** (`elab_forms.c`, `elab_let`): the
>    `^fat out :(fn ...)` form the diagnostic help text already recommended is
>    now accepted -- it re-types a `:ptr<void>` fat-box result into a
>    directly-callable fat closure.
> 4. **Closure thunk `arg_fat` shift** (`elab_fns.c`, capturing-lambda env
>    prepend): a closure that captures BOTH a plain value and a `^fat` param
>    gained a hidden env param, but `arg_fat` was not shifted with
>    `arg_full_types`, so a captureless lambda passed into one of the closure's
>    `^fat` params skipped the `EX_FN_TO_FAT` shim and a bare fn pointer reached
>    a fat-dispatch consumer -> slot-0 SEGV.
> 5. **Fat-dispatch thunk ABI** (`emit_expr.c`, typed `^fat`/boxed `TY_FN` call):
>    `(sf sig)` cast slot 0 using the *actual argument*'s static type (`sig`'s
>    `:float` thunk -> `R(*)(void*, double)`) instead of `sf`'s *declared*
>    `:ptr<void>` param ABI, passing the fat-box pointer in an FP register. Now
>    keyed off the callee's declared param types.
>
> A LeakSanitizer leak in the `if`-branch-mismatch diagnostic (`type_name` on a
> composite `fn` type) was fixed in passing. Regression coverage:
> `tests/fixtures/vec-typed-fat-closure-readback/` (prints `6`).

## Summary

Reading a fat-closure value out of a `Vec[A]` via `vec-get` and then
re-applying it as a typed closure is not expressible today. The
elaborator types the readback as `:int` / `:ptr<void>` and rejects the
subsequent call, even when type information is supplied at every
plausible location (the surrounding `let`, a `^fat`-annotated binding,
an explicit fixed-arity wrapper). The element pushed in is a typed
fat closure; the element coming out is not.

The signal-spice rebuild's Phase 5 (`compose.tur` / `effects-chain`)
depends on this shape, so this is the closest live consumer.

## Severity

Medium. The shape is the natural one for any combinator that takes a
homogeneous container of arrows / SFs / handlers and applies them in
sequence (effects chains, middleware stacks, signal pipelines). The
workarounds either (a) re-introduce raw int64 thunk dispatch via
inline-C -- the exact pattern `tur-signal-rebuild-plan` and the
"What is explicitly NOT carried over" ban list forbid, or
(b) special-case fixed N-element pipelines, which defeats the point.

## Observed vs. expected

### Observed

A minimal repro (run with `build/tur` 0.18.0):

```turmeric
(defn apply-sf
  [^fat sf  : (fn [:ptr<void>] #{} : ptr<void>)
   ^fat sig : (fn [:float]     #{} : float)] : ptr<void>
  (sf sig))

(defn chain-loop
  [effects : int
   ^fat sig : (fn [:float] #{} : float)
   i : int n : int] : ptr<void>
  (if (>= i n)
    sig
    (chain-loop effects (apply-sf (vec-get effects i) sig) (+ i 1) n)))

(defn effects-chain
  [effects : int ^fat input : (fn [:float] #{} : float)] : ptr<void>
  (chain-loop effects input 0 (vec-len effects)))
```

Elaboration error:

```
error [TUR-E0001]: function 'apply-sf' arg 1:
  expected (fn [] : ?), got int
    (chain-loop effects (apply-sf (vec-get effects i) sig) (+ i 1) n)
                                  ^^^^^^^^^^^^^^^^^^^
```

`vec-get` is `[A] [v :int i :int] :A`. With no instantiation hint
reachable from the call site, `A` cannot be resolved to the typed fat
closure shape, and the readback is rejected as `:int` even though
`apply-sf`'s parameter is explicitly `^fat ... :(fn [...] :ptr<void>)`.

Adding a let-binding with `^fat`:

```turmeric
(let [^fat out :(fn [:float] #{} : float) (effects-chain v sig)]
  (out 1.0))
```

is rejected with `unbound symbol 'out'` -- the `^fat` modifier is not
recognized in let-binding position.

Calling `(out 1.0)` *without* the `^fat` binding:

```
error: 'out' has type :ptr<void> (a raw pointer), which is not
directly callable; declare it as a fat closure parameter
(^fat out, or ^fat out :(fn [...] :T)) to call it
```

so the help text directs the user toward a syntax (`^fat` in `let`)
that the elaborator does not currently accept.

### Expected

Either:

1. **`vec-get` accepts a type instantiation** that propagates to the
   readback, so `(vec-get [SF-Sample-Sample] effects i)` types as the
   declared closure shape and can be passed where a typed fat closure
   is expected; *and/or*
2. **`^fat ... :(fn [...] :T)` is accepted in `let` binding position**
   so the result of a `:ptr<void>`-returning helper (the standard
   shape for fat-closure-returning defns) can be locally re-typed for
   call.

Once one of those works, the fold shape

```turmeric
(defn chain-loop [effects sig i n]
  (if (>= i n) sig
    (chain-loop effects ((vec-get effects i) sig) (+ i 1) n)))
```

should elaborate end-to-end.

## Repro

The minimal file is reproduced above. Steps:

```sh
# from turmeric repo root, with build/tur built (Debug or Release)
cat > /tmp/test_chain.tur <<'EOF'
(defn apply-sf
  [^fat sf  :(fn [:ptr<void>] #{} :ptr<void>)
   ^fat sig :(fn [:float]     #{} :float)] :ptr<void>
  (sf sig))

(defn chain-loop
  [effects :int
   ^fat sig :(fn [:float] #{} :float)
   i :int n :int] :ptr<void>
  (if (>= i n)
    sig
    (chain-loop effects (apply-sf (vec-get effects i) sig) (+ i 1) n)))

(defn gain [g :float]
  (let [gv g]
    (fn [^fat sig : (fn [float] float)]
      (fn [t :float] :float (* gv (sig t))))))

(defn main [] : int
  (let [v (vec-new)]
    (vec-push! v (gain 2.0))
    (vec-push! v (gain 3.0))
    (let [sig (fn [t : float] : float t)
          out (chain-loop v sig 0 (vec-len v))]
      (println (out 1.0))))
  0)
EOF
./build/tur check /tmp/test_chain.tur
```

## Root-cause hypothesis

Two intertwined gaps:

1. **`vec-get` element-type inference**: with element type `A` left
   unresolved at the call site, the readback collapses to the storage
   shape (`int64_t` slot), losing the typed-fat-closure shape the
   pushed value originally had. No syntax currently surfaces the
   instantiation to the elaborator from the call site -- there is no
   accepted `(vec-get [T] v i)` form analogous to other type-param
   instantiations.
2. **`let`-bound fat-closure re-typing**: a `:ptr<void>`-returning
   defn (the standard shape for fat-closure-returning helpers) cannot
   be re-typed into a callable closure at the binding site. The error
   message recommends `^fat ...` in `let`, but the parser does not
   accept it.

Either fix alone unblocks the fold; both would be ideal.

## Proposed fix directions

- Accept `(vec-get [T] v i)` (or equivalent type-arg form) at the
  call site, threading `T` through readback's return type.
- Or: implement a typed `Vec[A]` carrier whose element type is
  remembered at construction time, so `(vec-get v i)` infers `A`
  from `v`'s carrier shape rather than the `[A]` param alone.
- And/or: extend `let`-binding to accept the `^fat name :(fn ...)`
  form the error help text suggests, so callers can locally re-type a
  `:ptr<void>` result without rewriting the producer.

## Validation of a fix

- The minimal repro above elaborates cleanly under `tur check` and
  `tur build`, prints the expected `6.0` at runtime, and runs
  leak-clean under ASan.
- The signal spice's deferred `effects-chain` (rebuild-plan Phase 5)
  can be implemented as the natural `vec-get`-fold without inline-C
  thunk dispatch.
- A regression fixture under `tests/fixtures/vec-typed-fat-closure-readback/`
  exercises both `vec-get` instantiation and `let`-bound `^fat`
  re-typing, and is added with the fix.

## Related

- [[tur-signal-rebuild-plan]] -- Phase 5 (`compose.tur`,
  `effects-chain`) needs this shape.
- `let-bound-sf-loses-outer-arg-type-when-inner-captures.md`
  (now archived) -- PR #281 fixed the let-binding side for the
  *single-value* case; this report is the container-readback
  generalization.
- `arrow-compose-float-closure-int64-thunk-mismatch.md` -- related
  case where typed `:float -> :float` closures got dispatched through
  int64 thunks; resolved at the level of `compose-float` only.

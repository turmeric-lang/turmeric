# Fat-Closure Return-Position Shim Plan

A follow-up to the `A#1` fat-closure auto-shim (see
[test-suite-idioms-plan.md](test-suite-idioms-plan.md), "Implementation
status"). The `^fat` parameter marker retired every capture-forcing dummy that
fed a fat consumer **at a call-argument site**. Two genuine dummies survive
because the fat closure is produced at a **return position**, which the
call-site marker does not reach:

- `tests/fixtures/logic-fresh/input.tur:94`
  -- `(fn [x] (let [_ dummy] (goal-for-term x)))`
- `tests/fixtures/logic-reify/input.tur:135`
  -- `(fn [x] (let [_ dummy] (outer-body x)))`

This plan describes the gap and proposes how to close it so those last two
dummies can be deleted.

---

## 1. The pattern

A factory function declares a fat-closure return type (`:ptr<void>`) and returns
a `fn` literal whose body does **not** reference any free variable -- so the
lambda is non-capturing and lowers to a bare C function pointer:

```turmeric
;; tests/fixtures/logic-fresh/input.tur:92
(defn make-fresh-fn [] : ptr<void>
  (let [dummy 0]
    (fn [x] (let [_ dummy] (goal-for-term x)))))   ;; <-- capture-forcing dummy
```

The consumer fat-calls the returned value through the `{ thunk, env }`
protocol:

```turmeric
;; tests/fixtures/logic-fresh/input.tur:50,66-69,97
(defn apply-fat [f arg] : int
  ```c int64_t *fat = (int64_t*)(intptr_t)f;
  return TUR_APPLY1(fat, arg); ```)

(defn fresh-impl [lf state] : int
  (let [nv  (term-var (lvar-next))
        lf2 lf]
    (apply-goal (apply-fat lf2 nv) state)))         ;; TUR_APPLY1 reads slot 0

(defn main []
  (let [goal (fresh (make-fresh-fn))                ;; arg is a CALL result
        ...]
    ...))
```

`fresh-impl` reads `lf[0]` as a thunk and calls `thunk(lf, arg)`. If
`make-fresh-fn` returned a bare function pointer, slot 0 would be the first
eight bytes of the function's machine code -- a segfault. The
`(let [dummy 0] ... (let [_ dummy] ...))` wrapper forces the lambda to capture
`dummy`, producing a real fat closure.

`logic-reify` is the same shape one level deeper (`make-outer-fn` returns a
lambda that calls `outer-body`, which itself calls `(fresh (make-inner-fn x))`).

---

## 2. Why `^fat` does not reach it

The `^fat` marker (the `A#1` mechanism) only fires at a **call-argument** site:
when a bare `fn` value (`TY_FN`) is passed to a parameter whose
`arg_fat[idx]` bit is set, `elab_call.c` wraps it in `EX_FN_TO_FAT`
(`src/compiler/elab_call.c:2110`). Two facts defeat it here:

1. **The fat closure is produced at a return, not an argument.**
   `make-fresh-fn`'s tail expression is the bare `fn`. There is no call site
   between the lambda and the function boundary for the marker to attach to.

2. **By the time it reaches a call, it is already `TY_PTR_VOID`.**
   The argument to `fresh` is `(make-fresh-fn)` -- a call whose static type is
   the declared return type `:ptr<void>` (`TY_PTR_VOID`). Even if `fresh`'s
   parameter `f` were marked `^fat`, the shim's `ak == TY_FN` test fails and the
   value is treated as "already fat" and passed through (it would be a bare
   pointer, and crash). The shim cannot see that the `TY_PTR_VOID` value
   originated from a non-capturing lambda.

So the missing coercion is **representation selection at the point a bare `fn`
becomes a fat-closure-typed value** -- i.e. a *return* (and, by the same logic,
a `let`-binding or struct field declared `:ptr<void>`/fat). That is a
producer-side decision the current call-site marker never makes.

---

## 3. Root cause

A non-capturing `fn` has exactly one runtime representation today (a bare
function pointer), chosen at elaboration with no regard for the context it flows
into. `EX_FN_TO_FAT` already knows how to manufacture the alternative
representation (`{ __tur_fatshim<arity>, orig_fn }`, see
`src/compiler/emit_expr.c:3253`); what is missing is a second *insertion site*
for it -- the return position -- and a signal that says "a fat closure is
expected here."

---

## 4. Fix options

### Option A -- `^fat` on the return type (symmetric with the param marker)

Allow `^fat` on a function's return annotation:

```turmeric
(defn make-fresh-fn [] ^fat :ptr<void>
  (fn [x] (goal-for-term x)))     ;; non-capturing; auto-shimmed on return
```

Mechanics:

- Parse `^fat` before the return type in `defn`/`fn`, store a `result_fat`
  bit on the fn `Type` (mirrors `arg_fat[]` in
  `src/compiler/types.h`).
- At return/tail emission, when `result_fat` is set and the returned expression
  is a bare `TY_FN`, wrap it in `EX_FN_TO_FAT`. The relevant sites are the
  non-TCO body tail (`src/compiler/emit_fns.c`, the "Function with return value"
  branch -- the same area as the `A#1` latent-bug fix), `emit_tail`'s leaf for
  TCO bodies, and explicit `EX_RETURN`. A capturing closure (`TY_PTR_VOID`)
  passes through; a non-fn return is a typed error (the diagnostic half).

Pros: explicit, local, consistent with the existing `^fat` parameter story; no
type inference required. Cons: must be inserted at every tail/return path; the
annotation lives on the producer, which the reader must remember to add.

### Option B -- expected-type-driven shim at `fn`-literal sites (bidirectional)

Generalize: whenever the elaborator places a bare non-capturing `fn` literal in
a position whose **expected type** is a fat-closure type (`TY_PTR_VOID`), wrap
it in `EX_FN_TO_FAT`. Expected types are already available at:

- return position (the declared/inferred result type),
- `let`/`def` bindings with a declared `:ptr<void>` type,
- struct-literal fields typed `:ptr<void>`.

Pros: no new syntax; covers returns, bindings, and fields in one rule; closes
the gap for code that never wrote a marker. Cons: broader change with more
surface area; `:ptr<void>` is overloaded (it is also a genuine raw pointer), so
the rule must be scoped to "the value is literally a non-capturing `fn`" to
avoid wrapping real pointers -- which limits it to syntactic `fn` literals in
those positions (acceptable: that is exactly the dummy pattern).

### Option C -- carry the bare fn type through and shim at the eventual call

Let `make-fresh-fn` keep a function return type (`TY_FN`) instead of
`:ptr<void>`, mark `fresh`'s parameter `^fat`, and let the existing call-site
shim fire on `(fresh (make-fresh-fn))` because the argument would then be
`TY_FN`.

Pros: reuses the shipped mechanism with zero new codegen. Cons: requires a
first-class "function returning a function" return-type spelling that survives
through `(make-fresh-fn)`; Turmeric's bare `:fn` keyword does not currently
resolve to `TY_FN` (it falls through to an unresolved struct -- see the `A#1`
investigation notes), so this option is blocked on return-type plumbing for
function types and is the least self-contained.

**Recommendation:** Option A as the primary fix (smallest, explicit, symmetric
with `^fat` parameters), optionally subsuming Option B's binding/field cases
later if the same dummy shows up outside return position.

---

## 5. Edge cases to cover

- **Tail through control flow.** The returned `fn` may sit in the tail of an
  `if`/`let`/`do`/`cond` (`fresh`'s own body returns a *capturing* closure, so
  it is unaffected, but a future factory might branch). The shim must apply at
  every tail leaf, matching how `emit_tail` recurses.
- **Already-fat returns.** A capturing lambda or a forwarded `TY_PTR_VOID` value
  returned from a `^fat` function passes through unchanged (mirror the
  `ak == TY_PTR_VOID || ak == TY_NIL` arm of the arg-site shim).
- **Arity bound.** `EX_FN_TO_FAT` only ships `__tur_fatshim0..5`; reject
  arity > 5 with the same diagnostic the arg-site path uses.
- **turi interpreter.** `EX_FN_TO_FAT` is already transparent in `eval.c`
  (returns `eval(inner)`); no interpreter change needed.

---

## 6. Cleanup after the fix

1. Add `^fat` to the return type of `make-fresh-fn` (logic-fresh) and
   `make-outer-fn` (logic-reify) -- or, under Option B, rely on the inferred
   `:ptr<void>` return.
2. Delete the `(let [dummy 0] ...)` outer binding and the inner
   `(let [_ dummy] ...)` wrapper, leaving the plain non-capturing lambda.
3. Run `bash tests/run.sh`; both fixtures are runtime-only (no `expected.c`
   snapshot), so no snapshot regeneration is needed -- only the printed output
   must still match (`logic-fresh` prints `1`; `logic-reify` prints `1`, then
   the two walked term values).

---

## 7. Acceptance signal

```sh
grep -rn 'let \[_ dummy\]' tests/fixtures/logic-fresh/input.tur \
                           tests/fixtures/logic-reify/input.tur
```

returns nothing, both fixtures PASS, and the whole suite stays green. After
this lands, the `A#1` "Not covered" note in
[test-suite-idioms-plan.md](test-suite-idioms-plan.md) can be cleared.

---

## 8. Out of scope

- Unifying the two non-capturing-fn representations (bare pointer vs fat) at a
  single chokepoint. The `A#1` analysis showed that is broad and risks raw-C
  callback FFI (`tur_hamt_map` takes a raw `void *(*)(void*, void*)`); the
  per-site `^fat` discipline is deliberately kept.
- Return-position handling for `tur_poly_fn_t` (rank-2 `EX_POLY_WRAP`) values --
  a separate representation with its own wrapper path.

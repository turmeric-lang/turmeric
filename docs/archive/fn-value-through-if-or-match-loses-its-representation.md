# A function value chosen by an `if` or `match` loses its representation

> **RESOLVED 2026-11-28.** Fix direction 1. The join picks one representation
> and converts every arm to it: `elab_fn_join_to_fat` (`src/compiler/elab_call.c`)
> classifies each value leaf, through `do` / `let` tails and nested `if` /
> `match`, as carrier, fat or thin, the way `elab_normalize_fn_tail_leaves`
> does for a defn's result. When the arms share one signature and are not all
> thin, it boxes a carrier leaf (EX_POLY_TO_FAT), gives a thin one a static
> box (`elab_fn_value_to_fat`), and leaves a fat one as it is. It then types
> the join `boxed`, re-typing each wrapper on the way out. The `if` join
> (`elab_forms.c`) calls it, and so do the union, literal and ADT `match`
> constructions (`match_fn_join`, `elab_structs.c`). It replaces the cell-2
> branch for every arm it can classify. Cell 2 still covers a carrier beside
> a non-variable fat-normalized arm, the recursion case it was written for.
>
> Three neighbours of the same seam are covered too:
>
> - A `do` / `let` yielding a carrier is boxed the same way
>   (`elab_fn_tail_carrier_to_fat`). It was refused as a call head, as a let
>   init, and as a constructor argument, and it was a cc error as a `(fn ...)`
>   argument.
> - An `if` whose other arm diverges, `(if c f (panic ...))`, is boxed too.
> - The ADT `match`'s per-arm type check consults the join
>   (`match_arms_fn_joinable`). It used to reject a carrier arm beside a defn
>   as "arm types are incompatible".
>
> Arms whose signatures differ are still a type error. The message now names a
> carrier arm by its declared type, not `ptr<void>`. Two carriers of different
> signatures, which `type_eq` matched as `ptr<void>`, now get that error and
> are no longer refused later as "not callable".
>
> Ownership (fix direction 2): no stack box. A join's arms are emitted inside
> the `if`'s C block, so a stack box would die before the join is read.
> Instead, every box the join makes is fresh (NULL header) or static (the
> keep header), so whoever holds the join owns the box it yields:
>
> - **Call head, or a `let` that is only called:** `let_binding_env_freeable`
>   (`emit_expr.c`, `fn_join_leaves_fresh`) drops the box at scope exit.
> - **Argument to a non-retaining parameter:** hoisted to a `__borrowc` temp
>   (`arg_is_freeable_closure_source`), then freed the same way.
> - **Constructor fn field:** owned (`fnfield_closure_owned`).
>
> The escape walk (`binding_escapes_impl_x`, `emit_core.c`) learned that a
> parameter boxed at, or passed through, a join escapes only if the `let`
> binding holding the join does. Without that, every caller of such a function
> kept the closure it passed (24 B a call).
>
> Also covered, in a follow-up the same day:
>
> - **A returned join.** A function whose result is a box it makes on every
>   call is inferred `returns_fresh_closure`, as a closure factory is
>   (`fn_join_returns_fresh_box`, `elab_fns.c`, run after stage 2). That covers
>   a join, a `let` alias of one, and a bare carrier stage 2 boxed. A carrier
>   parameter's bit goes in `fresh_closure_captured_params`, but only when the
>   parameter appears nowhere else in the body. The caller then drops the box
>   it was handed. This also stopped a leak `main` had for a function returning
>   a captureless lambda (`let-tail-noncapturing-lambda-fat-temp`'s `direct`;
>   its snapshot gained the drop).
> - **A `(Some <join>)` local matched in place.** It is dropped like a lambda
>   literal payload (`sum_payload_join_owned`, `emit_expr.c`). That includes
>   a capturing lambda beside a defn, which leaked on `main` too.
> - **A function that performs.** It was refused before, and on `main`.
>   Resolved separately as
>   [carrier-fn-param-in-join-evicts-effectful-fn](carrier-fn-param-in-join-evicts-effectful-fn.md).
>
> What still leaks is not specific to joins:
>
> - A caller keeps the closure it passed when the callee stores the parameter
>   in a struct or a `Some`, or hands it back in a result. The same holds for a
>   closure factory that captures the parameter, on `main`.
> - A function returning `(Some <fn value>)` leaks the payload, lambda or join
>   alike. That is the `sum-closure-payload-never-dropped` family.
>
> Pinned by `fn-value-through-if-or-match` (every row below, both arms, plus
> float, cstr, two-argument, nested, diverging and ADT-`match` shapes),
> `fn-value-join-box-reclaimed` (`requires.leak-check`, including the returned
> and `Some` shapes) and `errors/fn-value-join-signature-mismatch`.

**Severity: high (miscompile: several shapes compile and then segfault).** The
other shapes are refused with a misleading diagnostic, or fail in the C
compiler.

The problem is an `if` or `match` whose arms are function values of one type,
where at least one arm is a function parameter, and whose result is then used:
called, `let`-bound, passed on, or stored in a struct. Which of the three
outcomes you get depends on what the arms are.

Found 2026-11-28 while measuring what
[carrier-fn-param-refused-by-constructor-fn-field](../archive/carrier-fn-param-refused-by-constructor-fn-field.md)
left uncovered: `(H 1 (if c f g))`. That turned out to be one row of the table
below. Every row reproduces on `main` at `318aaaf2`, built separately, and on
`claude/outstanding-reports-priority-x7krlp`. The rows give the same outcome on
both, except that the TUR-E0001 message on `main` reads
`expected (fn [] : ?), got ptr<void>`.

## Repro

```turmeric
(defn a1 [x : int] : int (+ x 1))
(defn a2 [x : int] : int (* x 2))
(defn pick [c : bool f : (fn [int] int)] : int
  ((if c f a2) 10))
(defn main [] : int
  (println (pick false a1))
  0)
```

```
Segmentation fault (exit 139)
```

`(pick true a1)` prints `11`.

## What each shape does

In the table:

- `f` and `g` are parameters typed `(fn [int] int)`.
- A **plain** parameter, with nothing marked on it, rides the poly carrier.
- `a1` and `a2` are named defns.
- `app` takes `(fn [int] int)`, and `H` has a `(fn [int] int)` field.

| Arms | Used as | Result |
| --- | --- | --- |
| plain `f`, `a2` | `((if c f a2) 10)`, `(let [h ...] (h 10))`, `(app (if c f a2))`, `(H 1 (if c f a2))` then called | **SIGSEGV** on the `a2` arm |
| plain `f`, a lambda with no captures | called | **SIGSEGV** on the lambda arm |
| `^fat f`, `a2` | called | **SIGSEGV** |
| `^fat f`, `^fat g` | called, `let`-bound and called, passed to a `^fat` parameter | **SIGSEGV** |
| plain `f`, plain `g` | called, `let`-bound and called | refused: `has type :ptr<void> (a raw pointer), which is not directly callable` |
| plain `f`, plain `g` or `a2` (via `match`, which has no cell-2 branch) | called | refused, same message |
| plain `f`, a capturing lambda | called | refused, same message |
| plain `f`, plain `g`; or a `do` / `let` yielding plain `f` | as a constructor argument: `(H 1 (if c f g))`, `(H 1 (match n 0 f _ g))`, `(H 1 (do ... f))`, `(H 1 (let [g f] g))` | TUR-E0001 `expected (fn [int] : int), got ptr<void>` |
| plain `f`, plain `g` | `(make-struct H 1 (if c f g))`, `(app (if c f g))`, passed to a `^fat` parameter | cc: `incompatible types when assigning to type 'void *' from type 'tur_poly_fn_t'` |
| plain `f`, plain `g` or `a2` | the `if` is a function's result, declared `(fn [int] int)` | correct |
| `a1`, `a2` | called | correct |

`tests/run.sh` sees the segfaults with no sanitizer: the program dies.

## Root cause

A function parameter takes one of three representations, and the choice is
made per BINDING (`repr_of_fn_param`, `src/compiler/elab_fns.c:11397`):

- **Poly carrier.** A plain parameter with a concrete, scalar, effect-free
  signature is a `tur_poly_fn_t {env, fn}`. Its binding is typed
  `TYPE_PTR_VOID` with `is_poly_fn` set.
- **Fat box.** A `^fat` parameter is a pointer to `{shim, fn, ...}`. That is
  the binding's `is_fat`; the TY_FN itself is not marked `boxed`. A
  non-scalar signature is fat as well, via `fn_param_type_is_fat_normalized`.
- **Thin C function pointer.** A named defn, or a lambda with no captures.

Every consumer that bridges these representations asks the binding:
`expr_is_poly_carrier_fn_var`, the type-variable slot box
(`elab_call.c:9707`), the constructor-field box (`elab_call.c:9722`), and the
fat-dispatch call path. An `if` or a `match` is not a binding. All a consumer
can see of it is its type, and the type does not say which representation the
arms are in. Three seams follow from that.

### 1. Plain parameter and a bare function: SIGSEGV

`src/compiler/elab_forms.c:5203`, the "fn-value-carrier-fat-seam-residuals
(cell 2)" branch of the `if` join, matches a carrier arm against an arm whose
TY_FN is `boxed` or passes `fn_param_type_is_fat_normalized`. It boxes the
carrier arm (EX_POLY_TO_FAT) and types the `if` with `boxed = true`.

The branch was written for an arm that is "a fat-normalized fn result", meaning
a call that returns a boxed fn. But `fn_param_type_is_fat_normalized` asks
about the type, and any concrete, effect-free signature passes it. A bare `a2`
passes it, and its value is a thin pointer. So one temp holds a fat box on one
path and a bare code pointer on the other, and the caller fat-dispatches it:

```c
if (c) { ... __t205[0] = (int64_t)(intptr_t)__tur_poly_to_fat1; ... __t203 = __t207; }
else   { __t203 = a2; }
... ((*( tur_thunk_int64_t_int64_t_t *)((void *)(intptr_t)(__t203)))(...)
```

### 2. Two `^fat` parameters: SIGSEGV

The arms' type is the declared TY_FN, which is not `boxed`, because `^fat` is
the binding's `is_fat` and not part of the type. So the joined value reads as
a thin function.

As a call head, it is cast straight to a code pointer:

```c
int64_t (*__call_head_11)(int64_t) = (int64_t (*)(int64_t))(intptr_t)(__t203);
```

Passed to a `^fat` parameter, it is shimmed a second time (EX_FN_TO_FAT), so
the fat box lands where the shim expects code:

```c
__t204[0] = (int64_t)(intptr_t)__tur_fatshim1;
__t204[1] = (int64_t)(intptr_t)__t203;   /* __t203 is already a fat box */
```

The `^fat f` with `a2` row is the same seam: one arm is fat and the other is
thin, and they share one dispatch.

### 3. Two plain parameters: refused, or a cc error

The arms are both `TYPE_PTR_VOID`, so `type_eq` holds and nothing reconciles
them. The `if` comes out as `ptr<void>`, which is not callable. A consumer
that would box a carrier checks for an EX_VAR, so it declines, and the
constructor check reports TUR-E0001. Where no check refuses it (make-struct,
or a fn-typed argument), the emitter declares the join temp from the type, as
`void *`, and assigns a `tur_poly_fn_t` struct to it: a cc error.

The function-result position is correct in every row because
`elab_normalize_fn_tail_leaves` (`src/compiler/elab_fns.c:9454`) walks the
`if`/`match`/`do`/`let` tail leaves and boxes each one by what the leaf
actually is, not by its type.

## Fix directions

1. Make the join choose one representation and convert every arm to it. The
   fat box is the one every consumer accepts:
   - a carrier arm gets EX_POLY_TO_FAT;
   - a thin arm (a bare defn, an EX_FN, a lambda with no captures) gets
     EX_FN_TO_FAT;
   - a fat arm is left as it is: an `is_fat` binding, a fat-normalized
     parameter, a capturing closure, or a call whose result is boxed;
   - the join's TY_FN is then typed `boxed = true`.

   That is the classification `elab_normalize_fn_tail_leaves` already makes.
   Factor it out, so the `if` and `match` elaboration can apply it whenever
   their arms are fn values. That also replaces the cell-2 branch.
2. Ownership: every box made at a join is fresh. As a call head it is dead
   after the call, so give it the stack, as poly-to-fat-box-leaks-per-call's
   `stack_ok` does. Where it is stored, it is owned:
   `fnfield_closure_owned` (`src/compiler/elab_forms.c`) already counts an
   EX_POLY_TO_FAT as owned. Leak-check the result.
3. The alternative is to carry the representation on the type: a `boxed` that
   tells the truth for a `^fat` parameter, plus a carrier mark, so that a
   join's type is enough to dispatch it. That is a larger change, and it
   touches the sites that ask the binding today.
4. Pin every row of the table in fixtures.

## Workaround

- Call each arm in its own branch: `(if c (f 10) (a2 10))`.
- Or return the join from a helper whose result is declared, then call that:
  `(defn sel [c : bool f : (fn [int] int)] : (fn [int] int) (if c f a2))`,
  then `((sel c f) 10)`. This prints `20` and `11` for plain parameters. I
  have not checked it for `^fat` parameters.

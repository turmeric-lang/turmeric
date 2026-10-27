# Positional argument checking ignores nominal type identity

**One-line summary:** A user-defined function's positional parameters are
type-checked by coarse `TypeKind` only, so any struct, opaque (`defopaque`),
or ADT value is silently accepted where a *different* struct/opaque/ADT of
the same kind is expected. The nominal identity of the type is erased at the
call site.

**Severity:** High for type-safety / expressiveness. This is a silent
"accepts ill-typed program" hole: a class of handle-confusion and
wrong-struct bugs that the type system *appears* to prevent are not caught at
compile time and surface only at runtime (SEGV / deadlock / silent
misbehavior). It directly blocks
[`docs/upcoming/stdlib-opaque-handle-types-plan.md`](../upcoming/stdlib-opaque-handle-types-plan.md),
whose entire premise is that wrapping resource handles in `defopaque`
newtypes converts handle-confusion runtime bugs into compile errors.

---

## Minimal repro

### A. Two distinct opaques are interchangeable positionally (wrong)

```turmeric
(defopaque A :int)
(defopaque B :ptr<void>)
(defn take-a [x : A] : int ```c return (int64_t)x; ```)
(defn mk-b   []     : B   ```c return 0; ```)
(defn main   []     : int (take-a (mk-b)))   ; passes :B where :A expected
```

```sh
$ tur check repro.tur ; echo $?
0          # accepted -- expected a TUR-E0001 type mismatch
```

Note the two opaques don't even share a base type (`:int` vs `:ptr<void>`)
and are still interchangeable.

### B. Two distinct structs are interchangeable positionally (wrong)

```turmeric
(defstruct P [x : int])
(defstruct Q [y : int])
(defn take-p [p : P] : int 0)
(defn main   []     : int (take-p (make-struct Q 5)))   ; :Q where :P expected
```

```sh
$ tur check repro.tur ; echo $?
0          # accepted -- expected a type mismatch
```

### Controls that behave correctly

- **Plain primitive where opaque expected -> correctly rejected:**

  ```turmeric
  (defopaque A :int)
  (defn take-a [x : A] : int ```c return (int64_t)x; ```)
  (defn main [] : int (take-a 5))     ; => TUR-E0001 "expected <struct>, got int"
  ```

- **Variadic `& rest :T` DOES enforce nominal identity** (see
  `tests/fixtures/errors/variadic-rest-opaque-mismatch`): passing a
  `Middleware` where a `Route` rest is declared is correctly rejected.

- **Builtin specs** check positional args with full `type_eq`
  (`elab_call.c:1374`), so they are not affected.

So the hole is specifically: **positional arguments to user-defined (`defn`)
functions**.

## Observed vs. expected

| Case | Observed | Expected |
|------|----------|----------|
| `:B` arg at `:A` param | accepted | `TUR-E0001` type mismatch |
| `:Q` arg at `:P` param | accepted | `TUR-E0001` type mismatch |
| plain `int` at `:A` param | rejected | rejected (correct) |
| `& rest :Route` given `Middleware` | rejected | rejected (correct) |

---

## Root-cause analysis

`Type` for a `TY_FN` stores its positional parameters as an array of
`TypeKind` (`arg_kinds[]`), not full `Type`s. The saturated-call checker for
user functions compares only that coarse kind:

- `src/compiler/elab_call.c:2014` -- `expected_arg_kind = fn_type.as.fn.arg_kinds[fn_arg_idx];`
- `src/compiler/elab_call.c:2035` -- `bool arg_ok = (args[i]->type.kind == expected_arg_kind);`

Because `defopaque` lowers to a fieldless `TY_STRUCT` and all structs/ADTs
share their respective kinds, every `A`/`B`/`P`/`Q` compares equal here.

The full per-parameter types are in fact already available on the same
struct: `fn_type.as.fn.arg_full_types[]` is populated and consulted a few
lines away for rank-2 / `TY_TYVAR` detection
(`elab_call.c:2020`, `elab_call.c:2076`). The positional identity check just
never looks at it.

Contrast:

- Builtin path uses full `type_eq` -- `elab_call.c:1374`.
- Variadic rest path keeps `rest_full_type` and compares by resolved type /
  name -- `elab_call.c:~1850-1896`.
- `type_eq` itself already distinguishes structs by `def` pointer
  (`src/compiler/types.c:104`) and ADTs by `def` pointer
  (`types.c:108`), so the machinery to tell `P` from `Q` exists; it is simply
  not invoked for positional `defn` args.

**Availability gap (key subtlety).** The full per-parameter types are *not*
actually present for the nominal case: `fn_type.as.fn.arg_full_types` is
allocated only `if (any_poly)` (`src/compiler/elab_fns.c:2125-2129`), so for
an ordinary `:Chan`/`:P` parameter the array is NULL. Fix direction #1 below
therefore has a prerequisite -- populate `arg_full_types` for all params --
which the implementation plan handles as its Phase 1.

---

## Proposed fix directions

> A fully elaborated, spike-validated implementation plan for these directions
> lives at
> [docs/upcoming/positional-nominal-type-identity-fix-plan.md](../upcoming/positional-nominal-type-identity-fix-plan.md).
> A throwaway spike (always-populate `arg_full_types` + nominal demotion at
> `elab_call.c:2035`) was measured to **pass the entire suite unchanged --
> 1322 passed, 0 failed** -- while correctly rejecting the `B`-at-`A` and
> `Q`-at-`P` cases. In-repo blast radius is zero.

1. **Consult `arg_full_types` in the positional check.** When
   `expected_arg_kind` is a nominal kind (`TY_STRUCT`, `TY_ADT`, and the
   opaque encoding) and `fn_type.as.fn.arg_full_types[fn_arg_idx]` is
   non-NULL, replace the `kind ==` test at `elab_call.c:2035` with a
   `type_eq(args[i]->type, *arg_full_types[fn_arg_idx])` test. Keep the
   existing `TY_TYVAR` / `TY_PTR_VOID` / rank-2 escape hatches that follow,
   so polymorphic and closure-coercion cases are unaffected.

2. **Scope carefully to avoid regressions.** A number of existing patterns
   intentionally pass a `TY_INT`-kinded value at a `TY_TYVAR`/polymorphic
   field (e.g. `(Just 1.5)`, GADT match arms). The fix must only tighten the
   *nominal* cases (struct/opaque/ADT identity) and must not disturb the
   tyvar/`ptr<void>`/fat-closure coercions handled at
   `elab_call.c:2038-2079`.

## How to validate a fix

- Add error fixtures mirroring the existing variadic ones, but positional:
  `tests/fixtures/errors/positional-opaque-mismatch`,
  `.../positional-struct-mismatch` (expect `TUR-E0001`).
- Add a positive fixture confirming a correct handle still type-checks and
  that `TY_TYVAR`/ADT-polymorphic field passing (`(Just 1.5)`) still works.
- Run the full suite: `bash tests/run.sh` must stay at zero `FAIL`. Expect
  some existing programs that *relied* on the loose behavior to surface --
  each is a latent bug to fix, not a reason to revert.

---

## Impact on the opaque-handle-types plan

Until this is fixed, `defopaque`-wrapping stdlib handles delivers only:

- int-vs-handle confusion rejection (already worked), and
- self-documenting signatures,

but **not** the headline guarantee (`(async-chan-send sync-chan ...)`,
`(future-get pool)`, `(condvar-wait mutex condvar)` rejected at compile
time), because those are all positional calls. The plan's acceptance
criterion -- "a representative wrong-handle test fixture demonstrates the
error now manifests at compile time" -- cannot be met for positional APIs
until the checker consults nominal identity. Recommend landing the
type-checker fix first, then proceeding with the stdlib sweep.

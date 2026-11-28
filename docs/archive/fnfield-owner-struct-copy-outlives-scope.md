# An owner struct frees its closure while a copy of it is still in use

**Status: RESOLVED 2026-11-28**, found and fixed the same day -- see
[Resolution](#resolution).

**Severity: high (use-after-free on the default build).** A `let` local of a
by-value struct that minted its closure (`(C (fn ...) 3)`, so the local is the
owner: `Binding.fnfield_owner`) frees that closure when the `let` ends
(`drops_fn_fields`). The free asked only whether the struct was moved,
consumed (`drop!`), or returned (`fnfield_local_leaves_scope`). A copy of the
struct that outlived the `let` any other way kept calling a freed closure.

Found 2026-11-28 investigating
[closure-stored-beyond-its-let-has-no-owner](../reported/closure-stored-beyond-its-let-has-no-owner.md),
whose fix direction ("make the struct the owner") rests on owner structs
being sound. All four repros below are heap-use-after-free on `main` at
`c878a702`, before any of this session's changes.

## Repros

```turmeric
(defstruct ^copy C [run : (fn [int] int)  tag : int])
(defstruct M [run : (fn [int] int)  tag : int])
(defn zero [x : int] : int 0)
(def ^mut kept : C (C zero 0))

;; 1. captured by a lambda that is returned -- ^copy or move-only alike
(defn cap-copy [k : int] : (fn [int] int)
  (let [h (C (fn [x : int] : int (- x k)) 3)]
    (fn [x : int] : int ((.run h) x))))

;; 2. a ^copy copy kept in a global
(defn stash [k : int] : int
  (let [h (C (fn [x : int] : int (* x k)) 4)]
    (set! kept h)
    (.tag h)))

;; 3. a ^copy copy handed to a function that keeps it: (keep! h), with
;;    (defn keep! [c : C] : nil (set! kept c))
```

Calling `((cap-copy 5) 10)`, or `((.run kept) 10)` after `stash`, runs the
freed closure. `tests/fixtures/fnfield-owner-struct-copy-kept` carries all
four.

## Root cause

The scope-end free decides from the struct's `let` alone. A capture by a
lambda is not a move: the lambda copies the struct into its env, and
`is_moved` stays false even for a move-only struct. A `^copy` struct is
copied by `set!` and by every call. `fnfield_local_leaves_scope` follows only
values in tail position and `return`. The closure's own `let` already had the
right question -- `closure_binding_escapes` -- and the struct's did not ask it.

## Resolution

The owner struct's free now asks the closure escape walk too, with the struct
as its root (`fnfield_struct_escapes`, `src/compiler/emit_core.c`).

- **Kept in:** calling through its closure field, reading its other fields,
  and handing it to a parameter the callee only reads.
- **Let out:** returning it, storing it, capturing it, matching on it, or
  reading the closure out anywhere else.

It runs over the whole `let` at elaboration, and again at the end of the
program once every function's masks are in (`elab_fnfield_resolve_pending`).

Three things keep that from turning correct frees into leaks.

**Parameters that only read the struct.** `Binding.nonretain_fnfield_param_mask`
holds the closure-struct parameters a body only reads, so `(push p 1)` on a
`^copy` parser stays a read. `resalias_fnfield_param_mask` holds the ones it
keeps nowhere but its result: `(pick h g n)`, a self-recursive `spin`,
`(ident h)`. A type-variable parameter is a candidate too. The caller's walk
treats such a call's result as another name for the struct. Both masks are
inferred as a greatest fixed point and decided again for the whole program
at the end.

The inference skips a body with an inline-C block anywhere in it, nested
lambdas included (`expr_contains_inline_c_block`). It does not use
`expr_subtree_has_inline_c`, which answers "may hide C" for every node kind it
does not list, a `handle` among them.

**Lambdas that only call it.** A `let`-bound lambda that captures the struct,
or the closure in the closure walk, is another name for it when it is only
called. Its body is walked under the same rules, so `(fn [] b)` handing the
capture back, or `(async b)` inside it, still escapes. That is sound because
a lambda's env glue frees only the env box, never what it captured; the
emitted glue was checked.

**Copies.** `(let [g h] ...)` is another name for `h`.

**Measured.** I emitted every fixture and compared closure and closure-field
frees against the previous commit. Six fixtures gained frees: the closure
walk now sees a closure captured by a sibling lambda that only calls it. Each
was checked under ASan against `main`:

- `closure-calls-let-bound-lambda`, `closure-captures-closure-binding` and
  `letrec-sibling-closure-capture` went from 24 / 24 / 64 B leaked to clean.
  They carry `requires.leak-check` now.
- `letrec-mutual-capturing-closures` went from 128 to 64 B.

No fixture lost a free.

**Regression checks.** `tests/run.sh`: 4366 passed, 0 failed.

Pinned by `tests/fixtures/fnfield-owner-struct-copy-kept` (known-leak: each
copy is used after its `let` ended).

## Still open

The four shapes leak now rather than free early: the owner struct's copy
holds the closure and nothing frees it. That is
[closure-stored-beyond-its-let-has-no-owner](../reported/closure-stored-beyond-its-let-has-no-owner.md).

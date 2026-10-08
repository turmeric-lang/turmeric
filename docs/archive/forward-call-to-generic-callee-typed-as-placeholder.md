# A forward call to a generic callee is typed as the carrier placeholder

**Severity: medium.** An expressiveness gap: a correct program is refused at
`tur check`.  Until 2026-10-02 the top-level half of it was a silent wrong
answer instead; that half is now this refusal.  Filed 2026-10-02 while fixing
[forward-call-to-aggregate-result-types-as-carrier](forward-call-to-aggregate-result-types-as-carrier.md).

**Status: RESOLVED 2026-10-02** by fix direction 2 (defer), which also covered
two shapes the filing did not have: a lambda handed to a later generic HOF
compiled and took **SIGSEGV** at run time, and a lambda handed to a later
NON-generic defn with a function-typed parameter was invalid C.  See
[Resolution](#resolution).

## Repro

```turmeric
(defn wrap-once [x : int] : (Option int)
  (wrap x))
(defn wrap [A] [x : A] : (Option A) (some x))
```

```
error [TUR-E0709]: function 'wrap-once' declares return type '(Option int)'
but its body returns int
```

Defining `wrap` first compiles.  Inside a `defmodule` it is the same.

## Mechanism

A caller elaborated before its callee sees the pass-1 forward declaration
(`elab_pre_declare_toplevel_defn`, `elab_forward_declare_defns`).  For a
GENERIC callee that declaration has the right arity but no parameter types --
`fwd_decl_scan_params` records only scalar kinds -- and no type parameters,
so nothing at the call can instantiate `A`.

Both pre-passes therefore keep the `TY_INT` placeholder for a result that
mentions the defn's own type parameters (`fwd_type_mentions_tp`).  Before
2026-10-02 the top-level pre-pass committed `(Option A)` anyway, and that was
worse than the refusal: with `(some (wrap x))` above `wrap` and a declared
`(Option (Option int))` result, `A` stayed unbound, `wrap` was emitted only as
its carrier base, and the caller read the `(Option A)` carrier box (16 bytes)
as a by-value `(Option (Option int))` -- an out-of-bounds read; the program
printed nothing at all for the `match` on it (gcc: `-Warray-bounds`).  A dynamic
file keeps committing such results: its forward decl carries closed parameter
types (`elab_fwd_param_full_types`).

## Fix directions

1. Forward-declare a generic callee in full: its type parameters (and kinds),
   its parameter types, its constraints.  That is most of `elab_defn`'s
   signature half, which would have to be separable from the body half.
2. Defer: elaborate a defn whose body calls a not-yet-elaborated generic
   binding after the callee, the way the top-level driver already defers a
   defn that needs a later `definstance` (`tl_deferred`, with the capture
   frame and `n_file_scope_defs` rollback).
3. Elaborate the callee on demand at the first forward call.

Whichever lands, `tests/fixtures/forward-call-aggregate-result-in-module`
has the non-generic shapes; the generic one needs a positive fixture of its
own, with a by-value result so a wrong representation cannot pass silently.

## Resolution

Fix direction 2.  Pass 2 (both the top-level driver and the `defmodule` body
loop) now elaborates a defn that calls a not-yet-elaborated "lossy" defn of
the same statement list after that callee.  The code is `FwdGenOrder` in
`src/compiler/elab_toplevel.c`, shared by `elab_module.c`.

**Measured before the fix**, each with the callee below the caller and each
fine with the callee moved up:

| Shape | Before |
| --- | --- |
| `(wrap x)` at `(Option int)` / `(Option float)` | TUR-E0709 / "expected int, got float" |
| `(some (wrap x))` at `(Option (Option int))` | TUR-E0709 |
| `(ident p)` at a struct | TUR-E0709 |
| a float argument to any generic (`both-eq?`, `get-or`, `ping`) | "expected int, got float" |
| `(twice (fn [y : int] : int (+ y 1)) x)`, `twice` generic | compiles, **SIGSEGV** |
| `(apply-k (fn ...) 4)`, `apply-k [k : (fn [int] int) ...]`, not generic | cc: `incompatible type for argument 1` (`void *` for `tur_poly_fn_t`) -- also a named defn or a capturing closure as the argument |

The second family is the same hole from the other side: the pass-1 forward
decl does not mark a function-typed parameter as the `:fn` carrier
(`FA_POLY_FN`), so the call never wraps its argument (`EX_POLY_WRAP`) and the
emitter casts it to `void *`.  [fn-typed-param-forwarded-to-a-later-defn-miscasts](fn-typed-param-forwarded-to-a-later-defn-miscasts.md)
fixed the case where the argument is itself a `:fn` parameter; a lambda,
named defn or closure literal still went in bare.

**The rule.**  A defn is *lossy* when it declares type parameters or a
function-typed parameter.  A defn waits when it calls -- `(name ...)` -- a
lossy defn that has not been reached yet, or names (anywhere) a defn that is
itself waiting.  A name the form binds in a vector (a parameter, a let) is a
local, not a call.  Once the main sweep is done, waiting defns are retried in
source order whenever nothing they name is still waiting; a cycle is broken at
its first lossy member (so `count-up` calling into the generic `ping`/`pong`
pair still comes after both).  The instance deferral (symptom A) shares the
same second chance.

**What keeps the blast radius small:**

- A form that is not deferred -- a `def`, a `definstance`, a defn with no such
  call -- first elaborates any waiting defn it names, so it sees the
  definition, as it did when that defn kept its place.  Without this a
  `(def bumped (bump-opt (fn ...) 41))` saw `bump-opt`'s forward decl and was
  invalid C.
- Stdlib forms neither wait nor hold anything back, and only the first defn of
  a name is tracked.  The first version scanned names, not calls, and counted
  stdlib forms: a user generic named `k` deferred `stdlib/map.tur` defns with a
  LOCAL named `k`, and the `Eq [Map]` instance then saw their forward decls
  (`cps-typed-pointer-into-carrier-slot`,
  `typeclass-default-method-class-var-result` failed).
- A dynamic-file defn is not tracked: its forward decl carries closed
  parameter types (`elab_fwd_param_full_types`).

Across the corpus 7 existing fixtures change elaboration order (none carries a
codegen snapshot; all pass): `defmodule-pap-forward-ref-fat-fn`,
`fn-param-forwarded-to-later-defn`, `generic-defn-forward-reference`,
`generic-defn-forward-reference-defmodule`,
`typeclass-instance-after-use-in-defmodule`,
`typeclass-instance-declared-after-use`, `errors/tailcall-mutual-not-fusable`.

**Pinned by** `tests/fixtures/forward-call-generic-callee` (every shape in the
table, a `def` that flushes a waiting defn, a parameter named like a later
generic, a generic cycle) and `forward-call-generic-callee-in-module`.  Each
prints what the same program prints with every callee moved above its caller,
checked against the compiler before the fix.

**Not covered** (each is what every defn got before):

- ~~A cycle member sees its partners' forward decls.~~  Filed as, and since
  resolved by,
  [mutually-recursive-generics-see-placeholder-result](mutually-recursive-generics-see-placeholder-result.md):
  a stuck cycle's lossy members are primed speculatively first.
- A `def` or `definstance` ABOVE a lossy defn that a defn it names calls: the
  flushed defn is elaborated before the callee is reached.
- A call a macro introduces: the scan is over the unexpanded form.

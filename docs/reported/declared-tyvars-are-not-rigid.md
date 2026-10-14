# A generic's declared type variables are not rigid: `(defn bad [A B] [a : A] : B a)` type-checks

**Severity: high (unsound) -- the variable-vs-variable half is fixed at the
return and `let` positions 2026-10-14; the variable-vs-ground half, and three
more positions, stay open, see "What is fixed" and "Still open" below.** `tur
check` accepted a generic whose body does not have its declared result type,
as long as both sides ride the int64 carrier. A caller then trusts the
signature, and the program segfaults, on both back ends.

**Discovered:** 2026-10-14, writing the fixture for
[heap-sum-generic-never-specialized](../archive/heap-sum-generic-never-specialized.md),
on `main` at v0.64.5 (`819d18bb`). A swapped `(Both b a)` in a
`(defn flip [A B] [t : (Two A B)] : (Two A B) ...)` body was accepted, and so
was everything below.

## Repro

```turmeric
(defn bad [A B] [a : A] : B
  a)
(defn main [] : int
  (let [s : cstr (bad 7)] (println s))
  0)
```

- `tur check`: no diagnostic.
- `tur run`: gcc warns `returning 'int64_t' from a function with return type
  'const char *'` inside `bad__spec__const_char___int64_t`, then the program
  segfaults in `puts`.
- `tur interpret`: SEGV at address `0x7` in the `println` builtin
  (`src/turi/eval.c:5097`), which reads the `7` as a string pointer.

Every one of these also passes `tur check` with no diagnostic:

| Body | Declared |
| --- | --- |
| `(defn bad [A B] [a : A] : B a)` | an `A` returned as `B` |
| `(defn bad [A] [a : A] : int a)` | an `A` returned as `int` |
| `(defn bad [A] [a : int] : A a)` | an `int` returned as `A` |
| `(defn bad [A] [a : cstr] : A a)` | a `cstr` returned as `A` |
| `(defn bad [A B] [xs : (Option A)] : (Option B) xs)` | `(Option A)` as `(Option B)` |
| `(defn bad [A B] [a : A] : B (let [b : B a] b))` | an ascription to `B` |
| `(defn sw [A B] [a : A b : B] : (Pair A B) (pair b a))` | a swapped pair |

For comparison, the monomorphic `(defn bad [] : int "s")` is rejected with
TUR-E0709.

## Root cause

There is no unifier to be lenient: each position has its own ad-hoc
comparison, and every one of them treats a type variable as "unpinned", a
wildcard that cannot conflict with anything. The body-versus-declared-return
check classifies the function and then asks **representation** questions
only (`src/compiler/elab_core.c`, the `RET_CONFLICT_*` predicates): a
committed monomorphic `defn` gets the strict gates, a generic `defn` is a
carrier class where anything sharing the int64 carrier is tolerated, and
even the register-class check that is "symmetric for every class" stands
down when either side is a tyvar (`rc_is_unpinned_kind`), which is why
`[a : float] : A` passed too. The same wildcard treatment is in the `let`
annotation check (`elab_forms.c`, only primitive annotations were compared,
and `applied_type_conflict` returns false on a tyvar), in the constructor
argument check, in the argument check for a monomorphic callee, and in the
`if` join.

What makes a structural check feasible: at the return site the types are
exact. Instrumenting it showed `declared=B body=A` for the repro and
`(Pair A B)` vs `(Pair B A)` for the swap, and a generic relaying through
another generic gets the callee's letters substituted when an argument binds
them (`(defn relay2 [X Y] [x : X y : Y] : Y (id y))` sees `body=Y`).

Two things make "named" the wrong test for "rigid", both found by the suite:

- A callee's result variable that **no argument binds** survives under the
  callee's own letter: `(vec-new)` is `(Vec A)` inside
  `(defn mk [T] [] : (Vec T) ...)`, and that `A` is nobody's declared variable
  there (`assoc-projection-expected-binds-callee-result`). Rigid therefore
  means "named in the enclosing signature": the defn's own type parameters at
  its return check, `e->sig_tyvars` (the set `ng_tyvar_in_sig` consults)
  inside the body.
- When that leftover letter **collides** with a signature variable, the
  existing `generic-call-result-leaks-callee-tyvar-names` rename-apart
  (`elab_call.c`, `apart[]`, marks it `open_slot`) only ran when some argument
  had bound something. `(list-nil)` inside `list-map [A B]` was `(List A)`
  under `list-nil`'s `A`, passed to `acc : (List B)` it bound `B := A`, and
  `list-map`'s own body typed `(List A)` -- wrong before this report too, and
  invisible while everything rode the carrier (15 parsec fixtures). At zero
  bindings the colliding letter is now marked `open_slot` but KEEPS its name:
  `(vec-new)` reading as the body's `A` is load-bearing, since a `vec-get` on
  it stays the abstract `A` only because the name is the signature's
  (`ng_tyvar_in_sig`), and renaming it `vec-new.A` collapsed that read to the
  int carrier (three generic-spec-carrier-crossings fixtures, one of them the
  SIGSEGV its own header describes). With the flag, a binding the letter
  makes is provisional, and `f`'s `B` wins over `acc`'s. A provisional binding
  also yields to the enclosing signature's own variable just as it yields to
  a concrete type.

The check is flow-insensitive: `(let [v (vec-new)] (vec-push! v a) v)` under
a declared `(Vec B)` is still accepted, because `v`'s element is learned only
through the push and `v` keeps its open `(Vec vec-new.A)`. That was accepted
before as well.

## What is fixed (2026-10-14)

`rigid_tyvar_conflict` (`src/compiler/elab_core.c`, declared in
`elab_internal.h`) asks the type question the representation predicates
leave out: a rigid variable (named, not an open constructor slot, not an
associated-type projection) conflicts with a differently named rigid
variable, and -- unless the caller allows the carrier bridge -- with a ground
pinned type (scalar, cstr, float, a bare ADT, a ground application with a
real head). Two applications of one ADT head are compared argument by
argument; anything else (unnamed or open variables, `!`, `any`, unknown,
tyvar-headed `(m b)`, two different heads) is left to the checks that own it.

Only the **variable-vs-variable** half is asked today (every caller passes
`allow_ground`); the ground half is in the predicate and waits on the two
blockers under "Still open".

- **Return position** (`elab_fns.c`, next to the committed-applied check):
  `[a : A] : B a`, `(Option A)` under `(Option B)`, the swapped pair, and
  `(Vec A)` under `(Vec B)` are TUR-E0709 with a message naming both types.
  `tur explain TUR-E0709` has it as case 4.
- **`let` annotation** (`elab_forms.c`): `[b : B a]` with `a : A` is rejected
  with the existing annotation-mismatch message.

Pinned by `tests/fixtures/errors/declared-tyvar-{returned-as-another,
swapped-app-args,let-annotation}` and, for what must keep passing,
`tests/fixtures/declared-tyvar-rigid-accepts` (relay through a generic,
two-variable relay, a diverging body, `(none)` and `(some a)` bodies, a
swapped pair put back in order, a leftover callee letter with and without a
collision, the `#{Unsafe}` bridge).

## Still open

**The variable-vs-ground half**, both directions, at the return position.
`[a : A] : int`, `[a : cstr] : A`, `[a : float] : A`, `[a : A] : P` all still
pass, and `[a : int] : A` is the direction the repro segfaults in. The
predicate already asks it (`allow_ground = false`); turning it on met two
things in the suite:

- `tests/fixtures/return-type-carrier-bridges-still-accepted` pins
  `(defn generic-carrier [A] [x : A] : int x)` as an accepted carrier bridge
  ("a generic defn may legitimately declare the carrier spelling"). That is
  a policy decision, filed with the aggregate-vs-scalar check; a caller that
  picks `float` reads its bits as an integer. Whether a generic keeps that
  bridge is the question to settle before this half can land, and it only
  needs settling for the body-is-variable direction.
- `typeclass-bounded-wrapper-heterogeneous-dispatch`: inside
  `(defn any-get [S E] [(StorageOps S E)] [... ] : E ...)` the method call's
  result is typed `Vel`, one instance's `E`, not the class variable. A
  multi-parameter class's result is resolved to a concrete instance at check
  time, so the ground check sees a real-looking `Vel` under a declared `E`.
  That typing is the thing to fix (the body IS an `E`); until then the
  body-is-ground direction, the dangerous one, cannot be told from it.

The same wildcard treatment at three more positions. Each passes `tur check`
today:

| Body | Where |
| --- | --- |
| `(defn f [x : int] : int x)` `(defn bad [A] [a : A] : int (f a))` | argument to a MONOMORPHIC callee. A generic callee's bound parameter is checked (`vec-push!` on a `(Vec int)` rejects `A` with "expected int, got A"); a monomorphic one is not. |
| `(defstruct P [x : int])` `(defn bad [A] [a : A] : P (P a))` | constructor argument against an `int` field. |
| `(defn bad [A B] [a : A b : B c : bool] : A (if c a b))` | `if` join: the result is typed `A` and the `B` arm is accepted. |

Fix direction: call `rigid_tyvar_conflict` from each site's existing
mismatch check, as the `let` site does. The monomorphic-callee argument check
is the one most likely to meet stdlib code that passes an `A` into an `:int`
carrier parameter on purpose; measure with the suite before widening.

## Fix directions (as filed; direction 2 and 3 are done for two positions)

1. **Skolemize declared type variables while checking the body.** Each `[A
   B]` becomes a rigid constant that unifies only with itself. There is no
   single unifier to do that in, so in practice this means the remaining
   sites above, one by one.
2. **Narrower first step:** keep the representation gates, but add one type
   check for the case where the declared result is a bare declared variable
   (or a type application of one) and the body's type is a different declared
   variable or a ground type. Done as `rigid_tyvar_conflict`; the ground
   half is written but switched off, see "Still open".
3. **Pin it** with an `errors/` fixture per table row. Done for the rows the
   variable-vs-variable half covers.

## Related

- `docs/archive/concrete-result-fn-passed-where-an-any-result-fn-is-expected.md`
  ("keep a rigid type variable rigid") fixed one place where an enclosing
  definition's variable was bound from outside. This is the general case.

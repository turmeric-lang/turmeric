# A generic's declared type variables are not rigid: `(defn bad [A B] [a : A] : B a)` type-checks

**RESOLVED 2026-10-14**, at every position the report names: the defn
return, a `let` annotation, a monomorphic callee's argument, a constructor
argument, and an `if` join. One predicate, `rigid_tyvar_conflict`
(`src/compiler/elab_core.c`), asked at each site; `#{Unsafe}` keeps the
carrier bridge. Pinned by eight `errors/` fixtures,
`tests/fixtures/errors/declared-tyvar-*`, and by
`tests/fixtures/declared-tyvar-rigid-accepts` for what must keep passing.
Two pre-existing typing defects the check exposed are fixed with it (a
multi-parameter class's method result, and a generic's self-call; both
below). The check is flow-insensitive; see the note under "Root cause".

**Severity: high (unsound).** `tur check` accepted a generic whose body does
not have its declared result type, as long as both sides ride the int64
carrier. A caller then trusts the signature, and the program segfaults, on
both back ends.

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

- **Return position** (`elab_fns.c`, next to the committed-applied check):
  every row of the table above is TUR-E0709 with a message naming both
  types, the bare-ADT return `[a : A] : P` included. An `#{Unsafe}` generic
  keeps the ground-handle-as-`A` carrier bridge and is held only to the
  variable-vs-variable half. `tur explain TUR-E0709` has it as case 4.
- **`let` annotation** (`elab_forms.c`): `[b : B a]` with `a : A` is rejected
  with the existing annotation-mismatch message. Only the variable half
  there; `[b : int a]` was already rejected by the primitive compare.

Two things stood in the way of the ground half, both settled:

- `tests/fixtures/return-type-carrier-bridges-still-accepted` pinned
  `(defn generic-carrier [A] [x : A] : int x)` as an accepted bridge ("a
  generic defn may legitimately declare the carrier spelling", asserted in
  the archived struct-return-type-mismatch-unchecked-until-cc report, never
  argued). Measured: compiled, `(generic-carrier 7.1)` printed
  `4619679907765970534` (the bits of 7.1), `(generic-carrier "hello")` the
  string's address, a struct a heap box address; the interpreter printed
  `7.1` and `hello`. A signature that says `int` and delivers a pointer is
  not a bridge, and nothing in the stdlib used the shape (with the half on,
  only this fixture and `any-get` below failed). The fixture now records the
  decision and points at the errors fixtures; `#{Unsafe}` is the one escape
  hatch, and the TUR-E0709 explain text no longer offers "give it a type
  parameter" as another.
- `typeclass-bounded-wrapper-heterogeneous-dispatch`: inside
  `(defn any-get [S E] [(StorageOps S E)] ... : E (sop-get s idx))` the
  method call was typed `Vel`, one instance's `E`. `elab_method_call` takes
  an abstract receiver's result from the representative instance and only
  corrected a result that is the receiver's own class variable
  (`type_params[0]`); `E` is the second parameter. The middle-vector
  constraint `(StorageOps S E)` also recorded only `S` (`TypeConstraint.tyvar`),
  so the call site had nothing to read `E` from. `TypeConstraint` now keeps
  every variable in class-parameter order (`arg_tyvars`), and the call takes
  the k-th from the enclosing generic's constraint on that receiver.

Pinned by `tests/fixtures/errors/declared-tyvar-{returned-as-another,
returned-as-ground,ground-as-tyvar,swapped-app-args,let-annotation}` and,
for what must keep passing, `tests/fixtures/declared-tyvar-rigid-accepts`
(relay through a generic, two-variable relay, a diverging body, `(none)` and
`(some a)` bodies, a swapped pair put back in order, a leftover callee letter
with and without a collision, the `#{Unsafe}` bridge).

## The three remaining positions (fixed 2026-10-14, same day)

The same wildcard treatment at three more positions. Each passed `tur check`:

| Body | Where |
| --- | --- |
| `(defn f [x : int] : int x)` `(defn bad [A] [a : A] : int (f a))` | argument to a MONOMORPHIC callee. A generic callee's bound parameter is checked (`vec-push!` on a `(Vec int)` rejects `A` with "expected int, got A"); a monomorphic one is not. |
| `(defstruct P [x : int])` `(defn bad [A] [a : A] : P (P a))` | constructor argument against an `int` field. |
| `(defn bad [A B] [a : A b : B c : bool] : A (if c a b))` | `if` join: the result is typed `A` and the `B` arm is accepted. |

All three now ask `rigid_tyvar_conflict` with the enclosing signature's
variables (`e->sig_tyvars`), the carrier bridge allowed inside `#{Unsafe}`
(`e->unsafe_depth`):

- **Call argument** (`elab_call.c`, after every argument tolerance, before
  the mismatch diagnostic): a parameter is a commitment when it is a defn's
  typed parameter (not a carrier-default untyped one) or a constructor's
  field; a parameter that mentions the callee's own variable is an
  instantiation and is left to the binding machinery. A constructor call
  takes this path, so the constructor-argument position came with it. The
  stdlib passed no `A` into an `:int` carrier parameter anywhere.
- **`if` join** (`elab_forms.c`): the existing branch-mismatch diagnostic,
  which already printed both types.

What the `if` check exposed, and what is fixed with it: a generic's
**self-call** was typed `int`. `elab_defn` forwards the declared return to
the function's own binding before elaborating the body (RR1), but excluded
`return_kind == TY_TYVAR` as "inferred", which also excluded a written
`: B`; so `list-foldl`'s `(if (list-empty? xs) init (list-foldl ...))` was
`then=B else=int` (16 fixtures), and `(defn go [A] ... : A (go ...))` returned
the carrier under `A`. A written type variable is forwarded now; the
self-call types as `A`, and a recursive generic runs correctly at float and
string (`go5` in the accepts fixture).

## Fix directions (as filed; direction 2 and 3 are done for two positions)

1. **Skolemize declared type variables while checking the body.** Each `[A
   B]` becomes a rigid constant that unifies only with itself. There is no
   single unifier to do that in, so in practice this means the remaining
   sites above, one by one.
2. **Narrower first step:** keep the representation gates, but add one type
   check for the case where the declared result is a bare declared variable
   (or a type application of one) and the body's type is a different declared
   variable or a ground type. Done as `rigid_tyvar_conflict`.
3. **Pin it** with an `errors/` fixture per table row. Done for the return
   and `let` rows.

## Related

- `docs/archive/concrete-result-fn-passed-where-an-any-result-fn-is-expected.md`
  ("keep a rigid type variable rigid") fixed one place where an enclosing
  definition's variable was bound from outside. This is the general case.

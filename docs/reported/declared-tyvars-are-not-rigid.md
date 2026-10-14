# A generic's declared type variables are not rigid: `(defn bad [A B] [a : A] : B a)` type-checks

**Severity: high (unsound).** `tur check` accepts a generic whose body does
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

## Root cause (partly traced)

The body-versus-declared-return check classifies the function and then asks
**representation** questions, not type questions
(`src/compiler/elab_core.c:3195-3240`, the `RET_CONFLICT_*` gates). A
committed monomorphic `defn` gets the strict gates (TUR-E0709). A generic
`defn` is a carrier class: any body whose C representation shares the int64
carrier with the declared result is tolerated, on the grounds that "the
carrier bridges it". That is true of the bits but not of the types. Nothing
treats a declared type variable as rigid, so `A` unifies with `B`, `int` or
`cstr`.

Not yet checked: whether the unifier itself lets two declared variables bind
to each other (the ascription row suggests so), or whether only the final
result check is lenient.

## Fix directions

1. **Skolemize declared type variables while checking the body.** Each `[A
   B]` becomes a rigid constant that unifies only with itself. A mismatch is
   then an ordinary type error, before any representation question is asked.
   Expect stdlib and fixture fallout: anything that leans on the carrier
   tolerance in a generic body will start failing, and each of those needs a
   real signature (as `Clone` got `[x : a] : a`).
2. **Narrower first step:** keep the representation gates, but add one type
   check for the case where the declared result is a bare declared variable
   (or a type application of one) and the body's type is a different declared
   variable or a ground type. This catches the rows above without touching
   the carrier tolerance between `int`-family scalars.
3. **Pin it** with an `errors/` fixture per table row.

## Related

- `docs/archive/concrete-result-fn-passed-where-an-any-result-fn-is-expected.md`
  ("keep a rigid type variable rigid") fixed one place where an enclosing
  definition's variable was bound from outside. This is the general case.

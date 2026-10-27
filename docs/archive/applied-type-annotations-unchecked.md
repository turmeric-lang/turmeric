# Applied types were never compared against what they annotated

**Severity: high.** A program states one type and gets another, silently.
Where the value reached a scalar slot it was a box's address:

```turmeric
(defn f [o : int] : int o)
(f (some 7.1))                              ; printed 140721223092080, both engines

(defn h [x : float] : cstr (some x))        ; type-checked; cc then refused it
(defn opt-carrier [o : (Option int)] : int o)   ; "accepted" -- until called
(let [o : (Option int) (some 7.1)] ...)     ; `o` silently an (Option float)
(let [o : (Vec int) (some 1)] ...)          ; accepted
(defn k [] : (Option int) 7)                ; accepted
```

Found 2026-10-01 while widening the type fuzzer's rank-2 class crossing to a
method with an `(Option a)` result: `tur check` accepted a `: cstr` defn whose
body was that Option, which led to the general probe below.
**RESOLVED 2026-10-01.**

## Mechanism

Three positions state a type, and each compared only part of it.

- **defn return.** `return_position_conflict` takes the declared return as a
  `TypeKind` (plus an ADT def for a bare ADT). Every predicate it runs is a
  pair of scalar kinds, and `return_type_carrier_aggregate_conflict` left
  `TY_APP` out on purpose ("a parametric return position has a crossing that
  grounds it"). That holds for a generic, an `#{Unsafe}` defn or an instance
  method. A monomorphic defn has no crossing. A probe of 5 bodies × 6 declared
  returns found 13 wrong programs accepted.
- **call argument.** The arg check accepts any `TY_APP` where `int` is
  expected ("Phase HKT §3: partial type application values are opaque int64_t
  at runtime"). A by-value `(Option float)` is a 16-byte aggregate, not a
  word, so the call boxed it and the callee read the pointer.
- **let annotation.** It is compared only when the annotation is a primitive
  kind ("Complex types ... parse but skip the equality check").

`return-type-carrier-bridges-still-accepted` pinned
`[o : (Option int)] : int o` as a tolerated bridge. It compiled only because
the fixture never called it. A call is `aggregate value used where an integer
was expected`.

## Fix

`applied_type_conflict(want, got)` (`elab_core.c`) answers the question once:

- `want` an applied type with a real ADT head, `got` one with a different head;
- the same head, both ground, different arguments (`(Option int)` vs
  `(Option float)`, `(Vec int)` vs `(Vec float)`);
- `want` a by-value applied type, `got` an int-family / bool / cstr / float
  scalar, or the reverse;
- `want` a `:heap` app, `got` a bool / cstr / float (an integer stays accepted:
  `0` is the cons-list family's nil).

An opaque-over-`:int` head and a transparent int newtype are the carrier, and
are never in conflict. It is called from:

- `elab_defn`, after the kind dispatcher, for a committed (monomorphic,
  non-`#{Unsafe}`) defn only;
- the arg check, for a Turmeric-bodied callee whose elaborated parameter is
  a declared `int`. An inline-C callee's `:int` is a deliberate erasure. A
  forward-declared callee's `int` can be a placeholder, so it isn't elaborated
  yet and is left alone;
- the let annotation check, for a non-primitive annotation.

Unlike `rv_is_noncarrier_aggregate`, nothing here is exempt under the
interpreter. The two engines now reject the same programs.

## Verified

- `tests/fixtures/errors/applied-return-into-scalar-rejected`,
  `applied-return-wrong-app-rejected`, `scalar-return-into-applied-rejected`,
  `applied-arg-into-int-param-rejected`,
  `applied-let-annotation-mismatch-rejected` and
  `applied-param-returned-as-int-rejected`, compiled and `--interpret`.
- The full suite, turi, `tur_examples_check` and `tur_stdlib_checks` pass.
  The only corpus program affected was the pinned `opt-carrier`, which has
  moved to the error side.

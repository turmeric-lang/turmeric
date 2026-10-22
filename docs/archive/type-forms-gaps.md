# The type forms: a `forall` argument is trusted, `&mut` ignores `^mut`, and `type-of` names no sized kind

**ARCHIVED 2026-10-21** -- split into reports and plans; see the disposition below.

## Disposition (2026-10-21) -- archived; every open item has a home

- Item 1 (`forall` argument trusted): decision -- enforce it. Plan:
  [rank2-argument-genericity-plan](../upcoming/rank2-argument-genericity-plan.md).
- Item 2 (borrows): reports
  [borrow-checks-gaps](borrow-checks-gaps.md) (the checker) and
  [turi-borrow-is-a-copy](turi-borrow-is-a-copy.md) (the interpreter; resolved
  2026-10-22).
- Item 3 (sized numbers in `any`): decision -- an interpreter limit, documented
  in eval-api ("Where the interpreter's answer differs by design") and the
  `type-of` forms entry. Compiled `type-of` names them since 2026-10-21.
- Item 4 (`coerce` keeps `x`'s type) and item 5 (`type-app`): decisions -- the
  proof's `b` when it is a type variable; retire `type-app`. Plan:
  [coerce-result-and-type-app-retirement-plan](../upcoming/coerce-result-and-type-app-retirement-plan.md).
- Item 6 (interpreter picks the wrong instance for packed `defopaque`): report
  [turi-packed-defopaque-wrong-instance](../reported/turi-packed-defopaque-wrong-instance.md).
- Item 7, float casts: decision -- saturate on both; report
  [float-to-int-cast-out-of-range-differs](float-to-int-cast-out-of-range-differs.md)
  (which found the compiled side is a bare C cast too; resolved 2026-10-22,
  both engines saturate). `(:: "hi" :int)`: decision
  -- store literals once per unit; [string-literal-interning-plan](../upcoming/string-literal-interning-plan.md).

**Severity: medium.** Found 2026-10-19 checking the examples for the type, cast,
existential and borrow forms' API reference (`stdlib/forms/types.tur`). Each repro
below was run on `tur run` and `tur --interpret`.

**Narrowed 2026-10-21.** `?` on a typed `Result` / `Option` and `coerce` with an
applied `(Equal a b)` proof annotation are resolved: see
[question-op-typed-sums-and-coerce-proof](question-op-typed-sums-and-coerce-proof.md).
Two checks were tried and reverted because the corpus relies on the permissive
behavior (below).

## 1. `rank-2 forall` does not check that the argument is generic (trusted)

```turmeric
(defn both [f (forall [a] (-> a a)) x : int s : cstr] : int
  (do (println (f s)) (f x)))
(defn inc [x : int] : int (+ x 1))
(defn main [] : int (println (both inc 5 "hi")) 0)
```

Accepted. `tur run` prints `i` (the string's address plus one, read as text) and
6; `tur --interpret` prints a large integer and 6. Passing a function that is
generic (`(defn id [x] x)`) prints `hi` and 5. Only a non-function (TUR-E0001) and
a capturing closure ("rank-2 argument must be a named function") are rejected.
hrt-guide says the callee instantiates `a` for each use; the caller's obligation to
pass something generic is not checked. **Tried 2026-10-21 and reverted:** the check
(each parameter the forall types by a bound variable must be a type variable, `any`
or un-annotated in the named function passed) rejects ten-odd existing fixtures
(`hrt-rank2-annotation`, `hrt-forall-constraint-show`, `currying-rank2-partial`,
...) that pass an `int`-only function and apply it at `int`. Decision: keep it
trusted (documented on the `forall` page) or migrate those fixtures to generic
functions and enforce it.

## 2. `&mut` and `&` do not enforce what the guide says

```turmeric
(defn main [] : int
  (let [x 1]                    ; not ^mut
    (let [r &mut x]
      (set! @r 5))
    (println x))                ; 5, both engines
  0)
```

- A mutable borrow of a variable that is not `^mut` is accepted and mutates it.
- `(let [^mut x 1 r &x] (set! x 2) (println @r))` is accepted: compiled prints 2
  (the borrow sees the write), interpreted 1 (it holds a copy). The checker's
  "N readers XOR 1 writer" does not cover an assignment to the variable itself.
- `(let [^mut p (make-struct P 3 4)] (let [m &mut (.x p)] (set! @m 10)) (println
  (.x p)))` prints 10 compiled and 3 interpreted: the interpreter's field borrow
  is a copy.
- `(let [r &7.25] (println @r))`: C compiler error `lvalue required as unary '&'
  operand`; interpreted prints 7.25. A borrow of a literal should be an error or
  borrow a temporary.
- **Tried 2026-10-21 and reverted:** rejecting `&mut` of a non-`^mut` local and `&`
  of a literal broke `borrow-basic`, `errors/borrow-conflict` and
  `saffron-borrowed-struct-key-widened` (a literal borrowed as a call argument,
  `(peek-key m (& 4.25))`, is deliberate there); only `let r &7.25` fails, in the
  C compiler. A narrower rule (a `set!` through the borrow) is the way in.
- (The guide's `(@ r)` spelling is fixed: it is `@r` and `(set! @r v)`.)

## 3. `any` and sized numbers: the engines disagree

```turmeric
(defn main [] : int
  (println (type-of (:: 3i32 any)))
  (println (is? (:: 3i32 any) int32))
  (println (is? (:: 3i32 any) int))
  (println (is? (:: 2.5f32 any) float32))
  0)
```

Compiled: `int32`, `true`, `false`, `true`. Interpreted: `int`, `false`, `true`,
`false`. The interpreter has one `int` and one `float`. (Compiled `type-of` said
`unknown` for a sized kind until 2026-10-21: the name table had no rows for them;
pinned by `type-of-sized-kinds`. A `float32` is widened as a float, so it reads
`float` on both.) And
`(is? (:: (Some 3) any) (Option float))` is `false` compiled and `true` interpreted
(archived `any-narrowing-broken-for-parametric-receivers` records the
interpreter's side as a deliberate limit).

## 4. `coerce` cannot change a type

`(defn conv [eq : Equal x : int] : float (coerce eq x))` is TUR-E0707: `elab_coerce`
(`src/compiler/elab_structs.c`) returns `x` at its own type, so `(Equal a b)`'s `b` is
never the result type, despite its comment ("x : a -> x : b") and gadts-cookbook.
The applied annotation `(Equal int int)` is accepted now (and checked against `x`'s
type). Taking `b` as the result type needs a retyping node; the proof cannot exist
when `a` and `b` differ, so such a branch is vacuous. Decide: implement the
retype, or document `coerce` as an assertion that returns its argument.

## 5. `type-app` is unusable in a type position

`(defn f [x : (type-app Option int)] ...)` is `TUR-E0012: cannot apply a type of
kind '*' as a type constructor`, for `Option`, `Result` and a user `defdata` alike;
`(Option int)` works. As a statement it checks and does nothing. No stdlib file or
fixture uses it in a program (`tests/fixtures/type-app` is a bare top-level
statement). Either make the annotation form work or retire the form.

## 6. The interpreter picks the wrong instance for packed `defopaque` values

```turmeric
(defclass Shape [a] (area [x : a] : int))
(defopaque Sq :int)
(defopaque Rect :int)
(definstance Shape [Sq]   (area [x : Sq]   (* (:: x :int) (:: x :int))))
(definstance Shape [Rect] (area [x : Rect] (+ 1000 (:: x :int))))
(defn area-of [e : (exists [a] [(Shape a)] a)] : int (open e [a v] (area v)))
(defn main [] : int
  (println (area-of (pack (:: 5 :Sq) (exists [a] [(Shape a)] a))))   ; 25 compiled, 1005 interpreted
  0)
```

The interpreter erases the existential and dispatches on the runtime value, which
cannot tell two opaque `:int` types apart.
`tests/fixtures/exists-open-witness-dispatch` carries `requires.compiled` for this.
Two packed values of different machine types (`int`, `cstr`) work on both.

## 7. Smaller things

- `(as uint8 300.7)`, `(as int8 -129.5)`, `(as int 1e30)`: compiled saturates (255,
  -128, `INT64_MAX`), interpreted wraps or takes the C result (44, 127,
  `INT64_MIN`). A float outside the target's range has no defined result in C; pick
  one behaviour for both (sized-primitives-guide documents only the integer
  wrap).
- `(:: "hi" :int)` is the string's address compiled and the string interpreted;
  and since the address is exposed, two identical literals compare equal under
  `tur run` and unequal under `tur jit`
  ([cstr-literal-address-differs-by-engine](../reported/cstr-literal-address-differs-by-engine.md)).
- (hrt-guide's `pack` / `open` spellings are fixed.)

## Fix directions

1. Decide whether a `forall` argument is trusted or checked (migrating the
   fixtures), and whether `&mut` of a non-`^mut` local is an error (a `set!` through
   the borrow is the narrow form).
2. Make `is?`/`cast` on sized kinds agree between engines, or document the
   interpreter's single `int` / `float` as the limit.
3. Give `coerce` the result type of the proof, or document it as an assertion.
4. Make `type-app` work in a type position or retire it.

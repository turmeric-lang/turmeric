# The type forms: `?` does not work on `Result`, a `forall` argument is not checked, and `&mut` ignores `^mut`

**Severity: medium.** Found 2026-10-19 checking the examples for the type, cast,
existential and borrow forms' API reference (`stdlib/forms/types.tur`). Each repro
below was run on `tur run` and `tur --interpret`.

## 1. `?` cannot be used on the language's `Result`

```turmeric
(defn halve [n : int] : (Result int cstr)
  (if (= (mod n 2) 0) (ok (/ n 2)) (err "odd")))
(defn quarter [n : int] : (Result int cstr)
  (let [h (? (halve n))]
    (halve h)))
```

`tur check`: `TUR-E0001: function '__tur-q-is-err?' arg 1: expected ptr<void>, got
(Result int cstr)`. `(let [x (? (ok 1))] (ok x))` is the same. The helpers
`__tur-q-is-err?` / `__tur-q-ok-val` (`stdlib/result.tur:198`, `:207`) take a
`ptr<void>` and read a `struct { int tag; int64_t payload; }`, which is the layout
of the old pointer-carried Result. `(Result T E)` is now a by-value ADT. The only
program that exercises `?` is `tests/fixtures/result-question-op`, which builds its
own carriers in inline C. Erasing a real Result with `(:: (ok 21) ptr<void>)` gets
past the check, but nothing can read it back (`(:: r (Result int int))` is a C type
error). error-handling-guide documents `?` as the early-return operator for
`Result`.

Interpreted, `?` cannot run at all: the fixture above under `tur --interpret`
stops with `defn: '__tur-q-is-err?' is already defined by an auto-loaded stdlib
module` (the REPL JIT recompiles the inline-C helper). Fix: lower `?` to a `match`
on the typed Result (and an `Option` twin), keep the carrier path only if the
fixture needs it.

## 2. `rank-2 forall` does not check that the argument is generic

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
pass something generic is not checked.

## 3. `&mut` and `&` do not enforce what the guide says

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
- borrow-checking-and-lifetimes-guide writes dereference as `(@ r)` and `(set! (@ r)
  v)`; that spelling is `expression in call head has type 'int', which is not
  callable`. The working spelling, in every fixture, is `@r` and `(set! @r v)`.

## 4. `any` and sized numbers: `type-of` says `unknown`, and the engines disagree

```turmeric
(defn main [] : int
  (println (type-of (:: 3i32 any)))
  (println (is? (:: 3i32 any) int32))
  (println (is? (:: 3i32 any) int))
  (println (is? (:: 2.5f32 any) float32))
  0)
```

Compiled: `unknown`, `true`, `false`, `true`. Interpreted: `int`, `false`, `true`,
`false`. The interpreter has one `int` and one `float`. Compiled, `type-of` has no
name for a sized kind (`unknown`), though `is?` and `cast` do distinguish them. And
`(is? (:: (Some 3) any) (Option float))` is `false` compiled and `true` interpreted
(archived `any-narrowing-broken-for-parametric-receivers` records the
interpreter's side as a deliberate limit).

## 5. `coerce` cannot change a type, and a parametrised proof type is rejected

```turmeric
(load "stdlib/equal.tur")
(defn conv [eq : Equal x : int] : float (coerce eq x))          ; TUR-E0707
(defn same [eq : (Equal int int) x : int] : int (coerce eq x))  ; coerce requires an (Equal a b) proof
```

`elab_coerce` (`src/compiler/elab_structs.c:2908`) checks the first argument is an
ADT named `Equal` and returns `x` as it was, so `(Equal a b)`'s `b` is never the
result type, despite its comment ("x : a -> x : b") and gadts-cookbook. The second
line fails because only the bare `Equal` annotation produces the ADT type the check
looks for; `(Equal int int)` is a type application. The only working shape is
`(coerce (Refl) x)` with the result at `x`'s own type.

## 6. `type-app` is unusable in a type position

`(defn f [x : (type-app Option int)] ...)` is `TUR-E0012: cannot apply a type of
kind '*' as a type constructor`, for `Option`, `Result` and a user `defdata` alike;
`(Option int)` works. As a statement it checks and does nothing. No stdlib file or
fixture uses it in a program (`tests/fixtures/type-app` is a bare top-level
statement). Either make the annotation form work or retire the form.

## 7. The interpreter picks the wrong instance for packed `defopaque` values

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

## 8. Smaller things

- `(as uint8 300.7)`, `(as int8 -129.5)`, `(as int 1e30)`: compiled saturates (255,
  -128, `INT64_MAX`), interpreted wraps or takes the C result (44, 127,
  `INT64_MIN`). A float outside the target's range has no defined result in C; pick
  one behaviour for both (sized-primitives-guide documents only the integer
  wrap).
- `(:: "hi" :int)` is the string's address compiled and the string interpreted.
- hrt-guide spells `pack` and `open` as `(pack int 0)` and `(open c as [s v] v)`;
  neither is accepted (`(pack value (exists ...))` and `(open e [a v] body)` are,
  as existential-types-guide says).

## Fix directions

1. Re-base `?` on the typed `Result` / `Option` and give it an interpreter path
   and a fixture on both.
2. Require the argument of a `forall` parameter to be generic (instantiate it at
   a fresh type and check), or document that it is trusted.
3. Reject `&mut` of a non-`^mut` binding, assignment to a borrowed variable and
   `&` of a literal; make the interpreter's field borrow write through.
4. Name sized kinds in `type-of`; make `is?`/`cast` agree between engines or
   document the interpreter limit.
5. Give `coerce` the result type of the proof, or document it as an assertion.

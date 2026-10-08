---
title: Refinement Types
category: Type Safety
description: `#refine{ x : T | p }` predicates the compiler tries to prove statically, emitting a runtime check only where the proof fails -- the predicate language, named refinements, diagnostics, and the documented limits
---

# Refinement Types

> **Status:** shipping, always on (graduated in v0.33.0; the `refined`
> experiment gate is gone). No flag is needed.
> See [contract-types-guide.md](contract-types-guide.md) for the runtime half,
> and
> [../archive/refinement-types-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/refinement-types-plan.md)
> for the design.

Contract types (`#refine{ x : T | p }`) check their predicate at **runtime**.
Refinement types go one step further: the compiler tries to **prove** the
predicate at compile time, and emits a runtime check only where the proof
fails.

The single fact that shapes the whole feature: **every refinement already has a
runtime meaning.** So the static discharger is allowed to give up on any
obligation and stay sound -- the obligation just falls back to the check it
would have had anyway. That is why a partial, hand-rolled solver is a real
feature rather than a broken one, and why static discharge can never make a
correct program wrong.

```turmeric
;; Proved: x > 0 entails 2x > 0, so no runtime check is emitted for the return.
(defn double-pos [x : #refine{ v : int | (> v 0) }] : #refine{ r : int | (> r 0) }
  (* x 2))
```

---

## Turning it on

Nothing to turn on -- static discharge runs on every compile. Write a
`#refine{...}` and the compiler tries to prove it.

It graduated from the `refined` experiment in v0.33.0. If you opted in
earlier, the old spellings must now be **deleted** -- their compatibility shims
were retired in 0.38.0, one minor line after graduation, and each is a hard
error again:

| Old spelling | Through 0.37.0 | From 0.38.0 |
|---|---|---|
| `--enable=refined` | accepted, `TUR-W0063`, no effect | `TUR-E0310` |
| `:experiments [:refined]` in `build.tur` | accepted, `TUR-W0063`, no effect | `TUR-E0310` |
| `~/.config/turmeric/experiments.tur` `:enable [:refined]` | accepted, `TUR-W0063`, no effect | `TUR-E0310` |
| `#lang turmeric refined` (first line of a file) | accepted, `TUR-W0064`, no effect | `TUR-E0330` |

Refinement checking itself is unaffected: `#refine{...}` has been unconditional
since 0.33.0 and needs no flag or layer.

These compatibility shims age out one minor line after graduation, so drop the
flag when convenient rather than relying on it.

The one **user-visible consequence** of graduation: a refinement that is
violated on every execution reaching it is now a compile error (`TUR-E0371`)
rather than a runtime contract failure. Everything the solver cannot decide is
unchanged -- it still falls back to the runtime check.

### `--strict-refine`

`--strict-refine` is a diagnostic-strictness knob, not an experiment. It turns
every obligation the solver could not prove into a hard compile error instead of
a warning plus a runtime check. Use it when you want a build in which *every*
refinement is discharged statically:

```sh
tur build --strict-refine src/main.tur
```

---

## What gets checked

Two things become **hypotheses** -- facts the prover may assume:

- each parameter declared with a contract type (or a named refinement alias);
- the function's `:pre` predicate.

Three things become **goals** -- things it must prove:

- a contract **return type**, `: #refine{ r : T | p }`;
- the function's `:post` predicate;
- every **argument** passed where the parameter declares a refinement.

```turmeric
(defn index-ok [v : int, n : int, i : int] : #refine{ r : int | (< r (size-of v)) }
  :pre (and (= (size-of v) n) (>= i 0) (< i n))
  i)
```

Here the `:pre` clause supplies three hypotheses, and the return refinement is
the goal. It is discharged statically, so the return check disappears; the
`:pre` clause itself is still checked at runtime, since it constrains the
caller rather than the body.

When a return or postcondition goal is proved, **no runtime check is emitted
for it**. When it is not, the check stays exactly where it was.

### Call sites

Passing a value where the parameter declares a refinement is a crossing into
it, so the *caller* owes the proof:

```turmeric
(load "stdlib/refine.tur")

(defn safe-div [n : int, d : NonZero] : int
  (/ n d))

(safe-div 10 2)   ; proved: 2 != 0
(safe-div 10 0)   ; error[TUR-E0371] -- a compile failure, not a runtime panic
```

Definition order does not matter. Crossings are resolved after the whole
compilation unit is elaborated, so a call to a function defined later in the
file is checked exactly like a call to one defined earlier.

A predicate may mention a **sibling parameter**, and the call site substitutes
the argument in that slot rather than leaving a free variable:

```turmeric
(defn at [n : int, i : #refine{ j : int | (and (>= j 0) (< j n)) }] : int ...)

(at 10 3)   ; proved: 0 <= 3 < 10
(at 3 10)   ; error[TUR-E0371]
```

**An argument the solver cannot prove is not an error by default.** Only an
argument that is *definitely* wrong -- one whose goal is closed, so it
evaluates to false on the values written at the site -- fails the build. That
distinction is deliberate: the callee still checks its parameters on entry, and
erroring on every argument whose value is not statically known would make the
experiment impossible to adopt incrementally. `--strict-refine` opts into the
stricter reading, where any undischarged argument is a hard error.

The callee's **entry check is always emitted**, even when every visible call
site is proved. Eliding it would need whole-program knowledge of the call graph
(including exported and indirect callers), and getting that wrong drops a check
that was protecting something. The call-site layer is a diagnostic on top of
that guard, not a licence to remove it.

### Indirect callees

A call does not have to name a global function to be checked. Two shapes carry
their parameter refinements to the call site:

```turmeric
;; a lambda with contract parameters
(let [f (fn [x : #refine{ v : int | (> v 0) }] : int (* x 2))]
  (f 21))     ; proved
  ;; (f 0)   -- error[TUR-E0371]

;; an alias of a global function
(let [g safe-div]
  (g 10 0))   ; error[TUR-E0371] -- the alias resolves to safe-div
```

A lambda's contract parameter gets an entry check exactly like a `defn`'s, and
a captured value's own refinement is in scope as a hypothesis inside the
lambda body.

A **typeclass method** is checked too, when the dispatch resolves to a known
instance -- and what is checked is **that instance's** parameter predicate, not
the class's:

```turmeric
(defclass Scaler [a]
  (scale-by [self : a, k : #refine{ v : int | (> v 0) }] : int))

;; an instance that RESTATES the class demand
(definstance Scaler [int]
  (scale-by [self : int, k : #refine{ v : int | (> v 0) }] : int (* self k)))

(.scale-by 3 4)   ; proved
(.scale-by 3 0)   ; error[TUR-E0371] on argument 2 of 'scale-by'
```

Both the dotted `(.m x ...)` and bare `(m x ...)` forms go through the same
resolution and are checked identically.

An instance parameter with **no annotation inherits** the class's refinement,
the same way an unannotated result inherits the class's promise:

```turmeric
(definstance Scaler [int]
  (scale-by [self k] (* self k)))   ; k inherits (> v 0)

(.scale-by 3 0)   ; error[TUR-E0371] -- and the entry check enforces it too
```

Writing an explicit annotation is how an instance **demands less**, which is
legal (`TUR-E0374` only forbids demanding *more*). A bare `: int` carries no
predicate, so it opts the parameter out entirely:

```turmeric
(definstance Scaler [float]
  (scale-by [self : float, k : int] : int k))   ; demands nothing of k

(.scale-by 1.5 0)   ; allowed -- correct for the instance that runs
                    ; warning[TUR-W0377]: relies on instance leniency
```

That call is allowed because a statically-resolved dispatch knows which
implementation runs, and is checked against that implementation's contract --
the more precise of the two. It is *linted* because the leniency is not part of
the interface: the same argument fails the moment dispatch goes dynamic, or a
stricter instance appears. Only a **definite** violation warns -- an argument
the class predicate rejects outright, not one it merely cannot prove -- so
`(.scale-by 1.5 n)` for an unconstrained `n` is silent.

A **dynamic** dispatch -- an abstract receiver, so no instance is selected --
is checked against the **class** signature instead:

```turmeric
(defn use-dynamic [^Scaler a x : a] : int
  (.scale-by x 5))    ; proved from the CLASS predicate alone
```

That is sound by the same variance argument that licenses result propagation,
run in the other direction. `TUR-E0374` rejects an instance that demands more
than its class, so every instance's parameter predicate is implied by the
class's -- which makes the class predicate the strongest demand true of *every*
instance, and an argument satisfying it acceptable to whichever instance runs.

It is also the only honest choice available there. With no resolved instance
the dispatch falls back to an arbitrary carrier-compatible one, which is not
necessarily the instance that will run, and whose predicate is weaker than the
class's -- checking against it would demand less than the contract while
appearing to check the contract.

An instance may accept **more** than its class signature promises, but not
less. The class signature is the contract callers program against, so an
instance that demanded more would reject an argument a generic caller was
entitled to pass:

```turmeric
(defclass Scaler [a]
  (scale-by [self : a, k : #refine{ v : int | (>= v 0) }] : int))

;; error[TUR-E0374]: rejects 0, which the class admits
(definstance Scaler [int]
  (scale-by [self : int, k : #refine{ v : int | (> v 0) }] : int ...))

;; fine: accepts everything the class promises, and more
(definstance Scaler [int]
  (scale-by [self : int, k : int] : int ...))
```

A class parameter with no refinement promises nothing, so *any* instance
refinement on it is a strengthening. The check is `class_pred |- instance_pred`
through the same solver seam, and it reports only on a refutation -- an
undecidable pair keeps the runtime check.

A method's **result** refinement varies the other way. A parameter refinement
is something the method *demands*, so an instance may demand less. A result
refinement is something it *delivers*, so an instance must deliver at least as
much -- a caller programming against the class signature is relying on it:

```turmeric
(defclass Boxed [a]
  (unbox [self : a, k : int] : #refine{ r : int | (>= r 0) }))

;; fine: no annotation, so the class's promise is INHERITED and checked
(definstance Boxed [int]
  (unbox [self : int, k : int] : int (* self k)))

;; fine: delivers more than the class promises
(definstance Boxed [int]
  (unbox [self : int, k : int] : #refine{ r : int | (> r 0) } ...))

;; error[TUR-E0374]: promises less about its result than the class does
(definstance Boxed [int]
  (unbox [self : int, k : int] : #refine{ r : int | (>= r -5) } ...))
```

| position | obligation | an instance may... |
|---|---|---|
| parameter | `class_pred(p) \|- instance_pred(p)` | demand LESS (accept more) |
| result | `instance_pred(r) \|- class_pred(r)` | deliver MORE (promise more) |

Because of that rule, **the class's result refinement propagates to callers**
even though which instance runs is unknown at the site -- it is the one promise
true of every instance:

```turmeric
(defn use-it [] : #refine{ r : int | (>= r 0) }
  (.unbox 3 4))          ; proved from the CLASS promise alone
```

An instance that restates a predicate the solver cannot prove implies the
class's gets the class predicate checked alongside its own ("Class result
contract violated"). That is what keeps the propagation honest: the variance
check reports only on a refutation, so an *undecidable* pair emits no error,
and without the extra check a caller would be relying on a promise nothing
enforced. Under `--no-contracts` nothing enforces it either, so nothing is
propagated.

What is *not* checked is a genuinely higher-order callee -- see
[Limits](#limits).

### Results carry their refinements

A call is an opaque term to the solver -- but when the callee has a return
refinement, that predicate is a *fact about the value this call produced*, so
it is asserted. Refined code composes instead of going opaque at every call:

```turmeric
(defn double-pos [x : Pos] : #refine{ r : int | (> r 0) }
  (* x 2))

(defn twice [y : Pos] : int (* y 2))

(defn use [p : Pos] : int
  (twice (double-pos p)))    ; proved -- double-pos's result is known positive
```

Assuming a declared return refinement is sound because something enforces it:
either it was proved statically, or the runtime check that guarantees it would
have panicked first. When neither is true -- contracts stripped by
`--no-contracts`, or a release build without `--keep-contracts` -- the
refinement is **not** published, and call sites go back to treating the result
as opaque.

### Inferred result refinements

A function with no declared return refinement gets one tried for it. RT4 runs a
small fixed vocabulary of shapes (`> 0`, `< 0`, `>= 0`, `<= 0`, `!= 0`) against
the body and keeps the first a backend **proves**:

```turmeric
(defn inc-pos [x : Pos] : int
  (+ x 1))          ; (> result 0) is inferred and published to call sites

(defn use [p : Pos] : int
  (twice (inc-pos p)))    ; proved, with no annotation on inc-pos at all
```

This is not general refinement inference. There is no search over a recursive
constraint system -- the layer that makes a LiquidHaskell-style backend hard is
exactly the one Turmeric deletes by making refinements *written*. These are a
handful of guesses, each individually checked, so a wrong guess costs a
discarded proof attempt and never a wrong answer. A function whose result
genuinely satisfies none of them simply gets nothing:

```turmeric
(defn lower [x : Pos] : int (- x 5))   ; can be negative -- nothing inferred
(defn use [p : Pos] : int (twice (lower p)))   ; correctly stays undischarged
```

Scope, deliberately narrow: single-expression bodies, a numeric result, at most
four refined parameters. Branching bodies need a path-sensitive join at the
merge point and are deferred. `TUR_REFINE_STATS=1` reports how many
refinements were inferred and how many probes it took.

---

## The predicate language

The supported fragment is quantifier-free linear integer/real arithmetic with
equality and uninterpreted functions.

```ascii
pred ::= (= e e) | (not= e e) | (< e e) | (<= e e) | (> e e) | (>= e e)
       | (and pred ...) | (or pred ...) | (not pred) | (=> pred pred)
       | (measure e ...)          ; a bool-returning measure IS a proposition
       | true | false

expr ::= <int literal> | <float literal>
       | <the refinement's bound variable>
       | <any parameter in scope>
       | (+ e e) | (- e e) | (* e <literal>) | (/ e <literal>) | (mod e <literal>)
       | (measure e ...)          ; an int/float-returning measure is a value
```

`(/ e <literal>)` and `(mod e <literal>)` on integers mean what the runtime
means: C's truncating `/` and `%` (the remainder takes the dividend's sign, so
`(mod -7 2)` is `-1`). The solver knows this -- it asserts `e = k*q + r` and
the sign clause for the two terms -- so `(= (mod n 2) 0)` on a parameter proves
`(= (mod (+ n 2) 2) 0)` on the result, and `(>= n 0)` proves
`(<= 0 (/ n 2) n)`. Division or remainder by a *variable* is nonlinear and
falls under the rule below.

Two rules keep this on the cheap side of the solver cliff. They are rules, not
accidents:

**Named measures are uninterpreted functions.** Any call the encoder does not
recognise -- `len`, `size-of`, your own helper -- becomes an opaque symbol that
congruence closure reasons about but never unfolds. This is what makes the
equality theory tractable, and it is why `(= (size-of v) n)` above is usable as
a hypothesis without the solver knowing anything about `size-of`'s body.

**A measure is sorted by its return type.** `:bool` denotes a *proposition*,
`:float` a real, everything else an integer -- so the domain predicate you
actually want to write works as written:

```turmeric
(defn alive? [w : int e : int] #fx{} : bool (= w e))

(defn use-it [w : int e : #refine{ x : int | (alive? w x) }] : int e)

(defn guarded [w : int e : int] : int
  (if (alive? w e) (use-it w e) 0))      ; the guard IS the proof
```

The sorting matters for soundness as well as expressiveness: a `bool` measure
mis-sorted as an integer would push people to the `(= (alive-i w x) 1)`
spelling -- the exact `:int` stand-in `CLAUDE.md` forbids -- and a `float`
measure mis-sorted as an integer would let integer tightening (`e < 4` implies
`e <= 3`, valid only over the integers) prove an unprovable goal and elide a
check that should fire. `refine-bool-measure`, `refine-float-measure`, and
`errors/refine-float-measure-not-tightened` pin all three behaviors.

An **abstract** measure -- a name that resolves to no function at all -- has no
return type to read, so its *position* decides: a proposition where the grammar
requires one (the goal itself, or an operand of `and`/`or`/`not`/`=>`), a value
everywhere else. Equality is neutral, since `(= (alive? w x) (alive? w y))` is
as legitimate as `(= (len v) n)`. A name genuinely used at *both* sorts in one
verification condition is rejected outright rather than resolved by guessing:
one symbol meaning two things across a hypothesis and a goal is how a
congruence bug is written. The obligation falls to unknown with a stated
reason, and the runtime check is kept -- see
`errors/refine-measure-sort-conflict`.

**A measure must be provably pure.** Congruence -- treating two occurrences of
`(size-of v)` as the same value -- is only valid for a pure function, and the
compiler decides that for itself by walking the callee's body:

```turmeric
(defn size-of [v : int] #fx{} : int
  (* v 2))
```

The walk is **default-deny**. It admits literals, reads of immutable bindings,
`if` / `let` / `do` / `return` / `match`, the arithmetic, comparison, and
logical builtins, and direct calls to functions that are themselves pure --
recursion included, so a recursive measure still gets congruence. Everything
else is impure: inline C, `set!`, `perform`, dereferences, field reads,
closures, indirect and rank-2-polymorphic calls, the `println` family, raw
memory, and any callee whose body is not available (an `extern`, or a forward
reference not yet elaborated). Without a purity proof each occurrence is a
*distinct* opaque value and the proof does not go through.

`match` is classified from its scrutinee and *every* arm, joined -- nothing
about dispatching on a constructor is observable, but one impure arm makes the
whole form impure. Pattern binders are not walked: they are introduced by the
pattern rather than evaluated, so an arm that merely reads one stays pure. Both
halves are pinned -- `refine-match-pure-congruent` (a `match`-bodied measure is
congruent) and `refine-match-impure-arm` (one arm calling a counter is not, and
the surviving check fires).

A declared effect row is a **veto, not evidence**. `#fx{Log}` rules purity out;
`#fx{}` on its own proves nothing, because the effect system tracks *algebraic*
effects and infers nothing from `set!`, from a mutable global, or from inline C.
A function can declare an empty row, keep a `static` counter in a C block, and
return a different value on every call.

The veto is applied where a function's verdict is memoized, so it holds on
every later lookup and it is **transitive**: a function with no row of its own
whose body calls a `#fx{Bt}` (or `#fx{Log}`, or `#fx{e}`) function is not
proven pure either. It used to be applied once at the top of the walk and
skipped on a memo hit, and the memo was usually written first by another
walk -- so in practice it never fired. `sx1-bt-row-checked` pins both the
direct and the transitive case, with an unannotated control that still proves.

The rule has teeth. Given a `tick` that counts up, `(- (tick) (tick))` is `-1`,
never `0` -- but encoded congruently it becomes `t - t`, and a refinement of
`(>= r 0)` on it would be "proved" and its check elided. Three separate routes
into that hole are pinned by regression fixtures:
`errors/refine-impure-not-congruent` (an unannotated callee),
`errors/refine-impure-fx-empty` (a callee declaring `#fx{}` and lying), and
`refine-typeclass-not-congruent` (a typeclass method, which has no global
binding under its bare name and so used to be mistaken for an abstract
measure).

**A typeclass method is never congruent.** Which instance runs is not known at
the encoder, and an instance body can do anything, so both `(m x)` and `(.m x)`
get a distinct symbol per occurrence.

**A field read is as pure as its receiver**, so an ordinary getter is
congruent:

```turmeric
(defn width-of [b : Box] : int (.width b))
```

There is no per-field `mut` marker in the language and none is needed: `set!`
on `(.f s)` requires `s` to be bound `^mut`, which is the same
declaration-level guarantee that already makes a non-mut variable read
congruent. Computing the receiver still counts -- `(.w (next-box))` is as
impure as `next-box`.

**Behind a reference it is declined.** With `rc<T>`, `ref<T>`, or a borrow, a
caller can hold a `^mut` handle to the very object the callee reads through a
non-mut one and mutate it between two calls -- exactly the aliasing congruence
assumes away. A by-value receiver has no second handle to mutate through
(`:copy` copies; a moved value leaves the caller nothing), so the vector closes
by construction. `rc` getters lose precision and nothing else.

Note this does not make `width-of` and `.width` interchangeable: they are two
different uninterpreted symbols and nothing unfolds one into the other.
Congruence is about repeated occurrences of the *same* call.

**A measure about mutable state can be made congruent *within a scope where the
state cannot change*** -- a predicate like `alive?` or `open?` that reads a
generation counter or a socket flag is impure and gets a fresh symbol per
occurrence by the rule above, but a `frozen` region plus a `#reads` annotation
recovers congruence for it, soundly, without eliding the callee's kept entry
check. That is a separate feature (implemented, experimental); see the
[Stateful Refinements guide](stateful-refinements-guide.md).

### What a `match` arm knows

A body that is a `match` is proved one arm at a time, and each arm is proved
under everything its own selection implies. There are four sources:

| Pattern | Hypothesis |
|---|---|
| `(Circle r)` | `(= (#dt/tag s) 2)` -- the constructor's discriminant |
| `(Circle r)`, record field `radius` | `(= r (.radius s))` |
| `0` | `(= s 0)` |
| `(Pos n) when (> n 100)` | `(> n 100)` |

That is enough for a getter to state a postcondition about the field it
returns:

```turmeric
(defdata Box (Box [width : int height : int]))

(defn get-width [b : Box] : #refine{ v : int | (= v (.width b)) }
  (match b (Box w h) w))
```

`w` and `(.width b)` are the same value, and saying so is what discharges the
obligation. Without the field hypothesis `w` is an unconstrained name and
`(.width b)` an unrelated opaque term.

**There is still no datatype sort in the VC.** These are ordinary equations
over uninterpreted functions, which congruence closure already decides -- the
theory is synthesized before encoding rather than built into the solver.
`#dt/tag` is a total function from the datatype to its discriminant; a field
selector is total too, taking some unspecified value off its own constructor,
which is exactly what an uninterpreted symbol does. Both facts are true on the
path being proved.

The one thing the tag buys on its own is **dead arms**. Two matches on the same
scrutinee pin its tag to two different constructors, and the inner arm's
hypotheses become contradictory -- so its obligation holds vacuously, which is
correct, because that arm cannot run:

```turmeric
(match s
  (Pos n) (match s
            (Pos m) 0
            (Neg m) -1)   ; unreachable, and proved so
  (Neg n) 0)
```

A guard is a **necessary** condition for its arm, never a sufficient one -- an
arm also requires every earlier arm to have failed, which is not asserted, so a
guarded arm knows its guard and nothing about the arms above it.

### Constructor axioms

The arm hypotheses run one way: a binder is the field it destructures. The
defining equation of the constructor runs the other way, and is asserted once
per constructor application in the obligation:

```
(= (.width (Box p 3)) p)
```

It is universally true, not path-dependent. Together with the arm's field
hypothesis it closes the build-then-destructure round trip:

```turmeric
(defn roundtrip [p : int] : #refine{ r : int | (= r p) }
  (let [b (Box p 3)]
    (match b (Box w h) w)))
```

`b = Box(p,3)` from the `let` split, `w = (.width b)` from the arm, and
`(.width (Box p 3)) = p` from the axiom -- congruence closure does the rest.

Only **record** constructors take part: a positional variant (`(Just :int)`)
has no field name, so there is no accessor to write the equation about.

A data constructor is treated as **pure**. It stores its arguments and runs no
user code, so two applications to equal arguments hold equal fields, which is
the only thing the VC ever asks -- a constructor term is reached only through a
selector. Object identity does differ between two applications, but nothing in
the predicate language can observe it. This says nothing about the constructor's
*arguments*: `(Box (tick) 3)` twice is two different terms, exactly as `(tick)`
twice is.

Arm hypotheses are scoped to their own arm. Sibling arms assert different tags
for the same scrutinee, so letting them accumulate would be a contradiction that
proves everything -- `refine-match-arm-hyps-not-shared` pins that.

The asymmetry is the point: a case the walk has not learned costs one runtime
check, while a wrong purity guess elides a check that was protecting something.
A name that resolves to no function at all is still treated as congruent: that
is an abstract measure, a mathematical function by definition rather than code
that runs.

**Variable * variable is uninterpreted.** `(* x 2)` is linear and fully decided;
`(* x y)` with both sides variable is abstracted to an opaque term and reported
with `TUR-W0373`. Congruence closure still relates two occurrences of the same
product, so nothing becomes unsound -- the arithmetic facts are simply gone, and
the obligation falls back to its runtime check.

```turmeric
;; TUR-W0373 + TUR-W0372: true, but not provable in the linear fragment.
(defn mul-pos [x : #refine{ v : int | (> v 0) }, y : #refine{ w : int | (> w 0) }]
             : #refine{ r : int | (> r 0) }
  (* x y))
```

We do not climb the nonlinear wall. A genuinely nonlinear obligation gets a
runtime check, and that is the intended outcome.

### Reflected measures (`^reflect`, experimental)

Everything above treats a named measure as an **uninterpreted function**:
`len` is a symbol congruence closure can compare to itself and nothing more.
`(= (len xs) 3)` as a hypothesis does discharge `(> (len xs) 0)`, but
`(> (len (Cons 1 (Nil))) 0)` does not prove -- nothing relates `len` to the
*structure* of its argument, so it falls to `TUR-W0372` and keeps its check.

Behind `--enable=reflected-measures`, `^reflect` on the `defn` lets the
solver use the definition:

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))

(defn ^reflect len [xs : Lst] : int
  (match xs
    (Nil) 0
    (Cons _ t) (+ 1 (len t))))

(defn head-of [xs : #refine{ v : Lst | (> (len v) 0) }] : int ...)

(head-of (Cons 1 (Nil)))     ; proves: len(Cons(1,Nil)) = 1 + len(Nil), len(Nil) = 0
```

**How it is admitted.** The fragment is quantifier-free, so the equation is
never asserted as `forall x. len(x) = ...`. Instead, at each application whose
argument is a constructor term or a literal, the encoder *reduces* the body --
substitutes the argument, selects the `match` arm syntactically, encodes what
survives -- and asserts that one ground equation. New applications the
reduction introduces (`len(Nil)`) unfold in turn, up to a **fuel** of 8 per
obligation (`TUR_REFLECT_FUEL=<n>` overrides it). Running out costs
completeness only: the obligation stays Unknown, reports `TUR-W0385` beside
the usual `TUR-W0372`, and keeps its runtime check. A `match` on a
*variable* unfolds only where the hypotheses already pin the variable's
constructor -- inside a `match` arm of the function being proved, whose tag
fact `(= (#dt/tag xs) k)` selects the arm and whose binders are the
`.field` selectors the arm's variables are bound to. Anywhere else,
`(len xs)` for a parameter `xs` is exactly as opaque as before. Call-site
crossings do not receive an arm's tag facts today (see the plan), so this
mostly serves return refinements.

`if`, `let`, `do` and guarded arms are supported; an `if` has no term in the
logic, so a body under one is admitted as a proposition,
`(c => f(t) = a) and (not c => f(t) = b)`. A `bool`-returning reflected
measure is a predicate atom whose equation is an `iff`.

**The totality gate.** An unfolded equation is only a fact when the function
is total: `f(x) = 1 + f(x)` asserts `0 = 1`, and one inconsistent hypothesis
silently discharges *every* obligation in the unit. So `^reflect` is admitted
only after the compiler shows the function

- **pure** -- the same default-deny walk that grants congruence (an inline-C
  body, or a call to anything not known pure, rejects);
- **terminating** -- every self-call passes, in **one fixed argument
  position**, a strict structural subterm of that parameter: a variable
  bound by a constructor pattern in a `match` on it (the `t` above), or a
  subterm of such. `(f (- n 1))` rejects by design, as does mutual recursion
  and a call through a variable;
- **covered** -- every `match` is proven exhaustive. ADT and union
  scrutinees already are (compiling implies it); a `^non-exhaustive`
  opt-out or a literal-scrutinee `match` with no `_`/variable arm rejects,
  and so does any form the walk does not positively recognise (macros,
  lambdas, `panic`, loops, `set!`).

A `^reflect` that fails is a hard **`TUR-E0384`** on the definition naming
the gate, never a silent downgrade. This is *not* a termination checker for
programs: a function that is never `^reflect`ed is untouched, and the
language's non-goal on total correctness stands.

A false obligation over reflected measures is **refuted**, not left unknown:
the bounded model search, which otherwise declines any VC that mentions an
uninterpreted function, runs when every such function is a constructor or a
reflected measure the encoder unfolded, and evaluates each application by
its own equation. `(> (len (Cons 1 (Cons 2 (Cons 3 (Nil))))) 3)` is
`TUR-E0371` "false for the value given here".

`--dump-reflect` prints one line per site (`reflect len: TOTAL dec=0`, or
`REJECTED gate=... reason=...`). Without the flag, `^reflect` warns that it
is ignored and the measure stays opaque. The plan, with what the first cut
settled and what is still open (non-ground unfolding, counterexamples
through a reflected measure), is
[reflected-measures-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/reflected-measures-plan.md)
(the docs pack carries guides, not plans, so the link is by URL).

---

## Named refinements: `stdlib/refine.tur`

A `deftype` bound to a contract type is a **refinement alias**:

```turmeric
(deftype Pos #refine{ x : int | (> x 0) })
```

A parameter declared `[n : Pos]` takes the base `int` at the C level, gets
`(> n 0)` as its entry check, and contributes that predicate as a hypothesis.

`stdlib/refine.tur` ships the common ones. It is **load-on-demand**, not
auto-loaded -- names like `Byte` and `Percent` do not belong in every program's
type namespace by default:

```turmeric
(load "stdlib/refine.tur")

(defn safe-div [n : int, d : NonZero] : int
  (/ n d))
```

| Alias | Predicate |
|---|---|
| `Nat` | `(>= x 0)` |
| `Pos` | `(> x 0)` |
| `Neg` | `(< x 0)` |
| `NonZero` | `(not= x 0)` |
| `Byte` | `0 <= x <= 255` |
| `Percent` | `0 <= x <= 100` |
| `NonNegFloat` | `(>= x 0.0)` |
| `PosFloat` | `(> x 0.0)` |
| `UnitFloat` | `0.0 <= x <= 1.0` |

A refinement alias takes no type parameters in this prototype.

---

## Loop invariants: `(while c :invariant p ...)`

A value a `while` loop builds cannot satisfy a refinement on its own: the
solver does not infer what a loop does, and never will (the same *checking,
not inference* rule as everywhere else here). You can **write** what the loop
maintains:

```turmeric
(defn count-up [n : int] : #refine{ r : int | (>= r 0) }
  (let [^mut acc 0
        ^mut i   0]
    (while (< i n) :invariant (and (>= acc 0) (>= i 0))
      (set! acc (+ acc 1))
      (set! i   (+ i 1)))
    acc))
```

`:invariant` goes directly after the condition, at most once (combine
predicates with `and`). It must be a pure `bool` -- the same purity gate as any
contract predicate (`TUR-E0375`). A `#reads` measure passes that gate, so an
invariant can mention a container's length, as `(<= i (vlen v))`. It is
runtime-checked: the analysis does not prove one yet.

**It is a contract first.** The invariant is checked on entry and again as the
last statement of every iteration; a failure panics with
`Loop invariant failed on entry to the while loop at file.tur:L:C` or
`Loop invariant not re-established by the body of the while loop at ...`. That
holds in compiled and interpreted code alike, whether or not anything is
proved.

**Then it is proved, which removes the checks.** The loop owes the two
Floyd-Hoare obligations, and each one proved elides its own check:

| obligation | hypotheses | goal |
|---|---|---|
| initiation | what reaches the loop: branch conditions, `let` bindings, parameter refinements and `:pre`, an earlier proved loop's post-fact | `p` |
| preservation | `p`, `c`, and every entry fact about a name the loop never assigns | `p` after one pass of the body |

**And once both are proved, code after the loop can use it**: `p AND (not c)`
holds on exit, which is what lets `count-up`'s return refinement discharge --
`acc >= 0` survives the loop. The same fact reaches a call after the loop, and
inside the body a call sees `p` and `c` (minus whatever the statements before it
assign), so a bounds-checked access in the body discharges both bounds:

```turmeric
(defn slot-get [n : int x : #refine{ j : int | (and (>= j 0) (< j n)) }] : int ...)

(while (< i n) :invariant (>= i 0)       ; upper bound: c; lower bound: p
  (set! acc (+ acc (slot-get n i)))
  (set! i (+ i 1)))
```

A body that assigns under `if` / `when` is checked path by path. A macro that
expands to an annotated `while` (a `for-each` lowering) gets all of this with
no extra work.

**What a failure tells you.** An invariant false on entry is `TUR-E0371`
("does not hold on entry"). One the body can break is `TUR-E0371` naming the
**conjunct** that broke, with the invariant as it reads after the body and,
for a branching body, the path that breaks it:

```text
error [TUR-E0371]: the body of the while loop in 'shrink' does not preserve `(>= acc 0)`
note: the predicate (>= (- acc 1) 0) does not hold for every input here
note: counterexample: i = -2, acc = 0, n = 0
note: checked assuming the invariant and the loop condition (< i n) hold at the top of the body
```

The usual fix is a *stronger* invariant -- the counterexample names the variable
it says too little about. An obligation the solver cannot decide is
`TUR-W0372` and keeps its check; `--strict-refine` makes that an error, and a
loop with **no** `:invariant` generates no obligation at all, so it stays
silent under strict mode. `TUR_REFINE_STATS=1` prints a line of its own:
`refine: invariant: 4 proven, 0 unproven, 0 loop(s) declined`.

**What is declined, not approximated.** A loop whose effect on its own
variables the analysis cannot see is reported (`TUR-W0372`, `is not analysed statically: <reason>`) and keeps
both checks:

- an assignment through a place (`(set! (.f s) v)`) or to an atom;
- an early exit other than `return` (`?`, a captured continuation), a
  `return` in a nested loop, or one the body composer cannot place (inside a
  call's argument, a `match` arm);
- a body `let` that rebinds a name in scope, or a nested loop whose assigned
  name is read after it (its value there is unknown).

Two shapes look like they should decline and do not:

- **A nested loop is havocked, not declined.** Nothing it assigns has a
  known value afterwards, but that only matters to code that reads one of
  those names again. An inner counter (`(let [^mut j 0] (while ...))`) costs
  nothing. An outer name it assigns leaves only the conjuncts that mention
  that name unproved.
- **A `return` in the body is pruned.** The paths through it leave the
  function, so they owe no re-establishment, and initiation and preservation
  are still proved. Only the post-loop fact `p AND (not c)` is withheld from
  the code after the loop, because the condition may still hold where the
  body returned.
- a variable the loop depends on that is **borrowed** anywhere in the function
  (`(& x)`, `&mut x` -- a callee can write through it), or **assigned inside a
  lambda or an effect-handler clause** (a call can then change it with no
  `set!` in the loop);
- a field read, a deref, or a mutable global in the invariant or the condition.

Termination is not part of any of this: a loop that never ends with a true
invariant is perfectly well-typed. See
[docs/archive/loop-invariants-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/loop-invariants-plan.md).

## The solver

There is **no heavyweight solver dependency**. The shipped compiler carries a
small, staged, in-house decision procedure, which is also why the WASM
playground gets static checking at zero download cost. Obligations run through
the stages in ascending cost and stop at the first one that decides:

| Stage | What it decides |
|---|---|
| **S0** trivial | constant goals, a goal that is syntactically a hypothesis, contradictory hypotheses |
| **S1** congruence closure (EUF) | equality and uninterpreted functions -- measures, abstracted products |
| **S2** linear arithmetic | conjunctions of linear constraints, by Fourier-Motzkin elimination over exact rationals (a superset of difference logic) |
| **S3** Nelson-Oppen | mixed goals, by exchanging entailed equalities between S1 and S2 |

Anything none of them decides is answered *unknown* and keeps its runtime check.
Every internal cap -- cube count, variable count, constraint growth -- degrades
the same way. The invariant the whole design rests on is one-directional: a
stage may never say "valid" for something that is not, but saying "unknown" is
always allowed.

Integers get one extra step: a strict constraint over integral data is tightened
(`e < 0` becomes `e <= -1`), which is what lets `x > 0` entail `2x > 0`. Full
integer completeness (branch-and-bound, Omega) is deliberately not attempted.

### Counterexamples

Failing to *prove* something proves nothing, so a separate bounded search tries
to **refute** the obligation: it enumerates a small candidate assignment space
and evaluates the formula exactly. A satisfying assignment is a real
counterexample, so it is reported with a model rather than a shrug.

The search declines VCs containing uninterpreted symbols -- a measure has no
fixed interpretation to evaluate, so guessing one would be dishonest.

### What a failure tells you

A failing obligation reports in three parts: the claim, a witness against it,
and a remedy.

```
error[TUR-E0371]: refinement on the return value of 'wrong' cannot be proved statically
note: the predicate (> r 0) does not hold for every input here
note: counterexample: x = -2
help: (> x 0) would discharge it -- e.g. declare x : #refine{ v : int | (> v 0) }
```

That predicate note is a claim against your code, so it appears only on a
refutation, where a counterexample exists. An unknown (`TUR-W0372`) gets a
different note instead: `the predicate ... could not be proved here, which is
not evidence that it fails`. The solver declining to decide says nothing about
whether the code is right.

The `help:` line is **not a heuristic**. It is a second query through the same
solver seam: a candidate fact is asserted as a hypothesis and the chain is
asked again, so only a candidate that genuinely discharges the goal is ever
offered. Two families are tried -- comparisons against the literals the code
already mentions, and relations between two variables, which is what produces
an index bound:

```
error[TUR-E0371]: refinement on the return value of 'at' cannot be proved statically
note: the predicate (< r n) does not hold for every input here
help: (< i n) would discharge it -- e.g. declare i : #refine{ v : int | (< v n) }
```

A candidate that *contradicts* what is already known is rejected explicitly.
Without that check a contradictory hypothesis would discharge the goal by ex
falso, and the compiler would cheerfully suggest constraining a variable to be
both negative and positive. When nothing consistent helps, no `help:` line is
printed -- silence beats a nonsense suggestion:

```turmeric
(defn impossible [x : #refine{ v : int | (< v 0) }] : #refine{ r : int | (> r 0) }
  x)      ; reported, with a counterexample, and NO hint -- none exists
```

When the values are written right at the site, the wording sharpens
accordingly -- this is not "not for every input", it is "not for this one":

```
error[TUR-E0371]: refinement on argument 2 of 'safe-div' in 'main' cannot be proved statically
note: the predicate (not= x 0) is false for the value given here
```

---

## Diagnostics

| Code | Meaning |
|---|---|
| `TUR-E0371` | the predicate genuinely does not hold: a function's own claim is falsifiable, or an argument is definitely wrong at its call site |
| `TUR-W0372` | nothing decided it; the runtime check is kept |
| `TUR-W0373` | a nonlinear subterm was abstracted; arithmetic reasoning is incomplete for it |
| `TUR-E0310` / `TUR-E0330` | a lingering `--enable=refined` / `#lang turmeric refined`; both shims retired in 0.38.0, so both are errors -- delete them |

`TUR-E0371` and `TUR-W0372` both leave the program safe -- the runtime check
survives in each case. Under `--strict-refine` both become hard errors.

Run `tur explain TUR-W0372` (or any of the codes above) for the long form.

---

## Debugging an obligation

Two environment variables:

```sh
TUR_REFINE_STATS=1 tur build main.tur
# refine: 3 obligation(s): 2 proven, 0 refuted, 1 unknown (7 backend call(s))

TUR_REFINE_DUMP=1 tur emit-c main.tur
# --- refinement VC (return value of double-pos) ---
# (set-logic QF_UFLIA)
# (declare-const x Int)
# (assert (< 0 x))
# (assert (not (< 0 (* x 2))))
# (check-sat)
```

The dump is SMT-LIB2 with the goal **negated** -- `unsat` means valid. It is
there to read, and to paste into an external solver when you want a second
opinion; the in-house stages consume the internal representation directly and
never go through this text.

An obligation reported `unknown` may never have reached a backend at all: if it
escapes the supported fragment there is no VC to dump, and `TUR_REFINE_DUMP=1`
prints nothing for it. Stats names those separately, with the reason:

```sh
TUR_REFINE_STATS=1 tur check main.tur
# refine: not encoded (measure 'ready' is used both as a proposition and as a
#         value): the return value of 'f'
```

This is a note about the *fragment*, not a defect in your program -- which is
why it rides on the stats switch rather than being a diagnostic. A call-site
crossing is `runtime_guarded`, so it does not warn by default at all; without
this line an obligation the encoder dropped and one the solver could not decide
were indistinguishable.

### Cross-checking against Z3 -- removed in 0.32.5

A development build used to be able to link a system Z3 as a correctness
oracle, decide every obligation twice, and report disagreements as
`TUR-I0379`. That scaffold has been **retired**: there is no
`TUR_REFINE_Z3_ORACLE` option, no `tur_refine_fuzz` binary, and no way to link
a solver into `tur`. The compiler ships no solver dependency and now has no
build mode that adds one.

The standing replacement is `tests/corpus/smtlib/`, replayed by the
`tur_refine_corpus` ctest target: 125 benchmarks whose `sat`/`unsat` labels
live in the repo as data, checked against the in-house chain in every build
rather than only on a machine with Z3 installed.

One caveat the oracle era taught, still worth knowing when you check a VC
against an external solver by hand (dump it with `TUR_REFINE_DUMP=1`): `sat` is
only a counterexample when the VC contains no uninterpreted symbols.
Abstraction is sound in one direction only -- `unsat` of an abstracted VC
implies the concrete obligation holds, but a model that assigns an opaque
symbol a convenient value proves nothing about the function it stands for.

### Source-level differential fuzzing

The deleted `tur_refine_fuzz` started at the VC, *below* the encoder, so it was
blind to every bug in the translation from Turmeric source into a VC. Both
soundness bugs found in this work lived exactly there, and neither was visible
to it -- which is why its deletion costs less coverage than it appears to.

`tests/refine-fuzz-src.py` starts at the top instead. It generates whole
programs, compiles and runs each one with the gate off and on, and compares
what actually happened. It never needed Z3 -- the oracle is the gate-off build
-- so it is unaffected by the retirement and is now the primary fuzzer.

```sh
python3 tests/refine-fuzz-src.py --n 500 --seed 3
bash tests/run-refine-fuzz-src.sh          # smoke size, also ctest
```

The property it enforces is the one the design turns on:

> gate-off aborted on a contract  =>  gate-on must not run to completion

plus identical stdout when both run clean, and no gate-on abort on a program
that ran clean without it. A fourth bucket, `SUSPICIOUS_over_refute`, collects
programs that run clean with the gate off and are rejected with it on. Those
are usually *correct*: `TUR-E0371` is universally quantified over a function's
inputs while the program only exercises the arguments it happens to pass, so a
sound refutation and a clean run coexist routinely. They are counted and saved
for triage, never failed on.

### wasm32

The solver ships in the WebAssembly build, so it is checked there too --
`bash tests/run-refine-wasm.sh` (ctest `tur_refine_wasm`) compiles every
refine source under Emscripten with the real `tur_wasm` flags AND runs the
solver's unit checks at 32-bit pointers under node. Compiling is the easy
half; agreeing is the point, since S2 is Fourier-Motzkin over exact rationals
with overflow guards and the hash-cons table keys off integer widths. The
script skips cleanly when emcc is not installed.

A fuzzer nobody has watched fail is a fuzzer that reports zero because it is
broken. Two one-line sabotages are documented in the script header, both
verified to be caught where the shipped build reports zero: breaking purity
(`rt_binding_is_pure` returns `true`), and eliding a callee's entry checks
under the gate (`rt_inject_param_checks` returns early). The second simulates
the whole-program entry-check elision feature built unsoundly, so rerunning it
is the acceptance test for that work when it lands.

---

## Limits

Known and deliberate, in rough order of how likely you are to hit them.

Each carries a tag, because "not checked" covers two very different promises
and the difference matters if you are deciding whether to depend on this:

| tag | meaning |
|---|---|
| **[by design]** | A deliberate decision, usually forced by soundness or by the adoption philosophy. It will not change. |
| **[prototype]** | Needs a design change this prototype excludes -- refinements in function types, refinements on type parameters. Not planned. |
| **[incomplete]** | Could be improved, nobody is working on it. Safe: the effect is always an obligation that falls back to its runtime check, never a wrong answer. |
| **[deferred]** | Has a written plan and a trigger condition; waiting on demand rather than on effort. |

Nothing here is a bug being worked around. Every one of them lands on the safe
side of the one-directional invariant: the worst outcome is an obligation the
solver declines to prove, which keeps the runtime check it would have had
anyway.

- **[by design] A `float` refinement is proved only where `double` cannot
  disagree with the reals.** A `float` is a C double, and the solver decides
  over exact rationals, so it uses only what the two agree on:
  - Comparisons, bounds and transitivity between float terms and literals:
    `0.25 <= x < 2.5` proves `x < 3.75`, and `norm(v) < 3.25` proves
    `norm(v) < 3.75`.
  - Not `+ - * /` or negation on a float: each rounds, so the solver treats
    `(* x 0.25)` as an opaque term. `(- (+ x 0.1) 0.1)` is not proved equal to
    `x` (it is not, for `x = 0.3`), and neither is the true but rounding-
    sensitive `x >= 0.0 |- (* x 0.25) >= 0.0`. Both keep their runtime check.
  - A negated comparison only for a term the same path rules a NaN out for.
    A NaN is on neither side of `<`, so `(not (< x 0.0))` says nothing about
    `x` unless a positive comparison on that path mentions it. The two-`if`
    `clamp` is not proved in range, because for a NaN argument it returns the
    NaN. Likewise `(= x x)` and `(<= x x)` are not proved for a float `x`.

  The counterexample search evaluates in `double`, so it still refutes
  concrete calls (`errors/reflect-float-not-tightened`). Fixtures:
  `refine-float-lra`, `refine-float-measure` (proved),
  `refine-float-rounding-keeps-check` and `refine-float-nan-keeps-check`
  (kept and firing). One gap remains: congruence through a float equality
  (`a = b |- f(a) = f(b)`), which `double` breaks only at signed zero
  (`0.0 = -0.0`, but `1/0.0` is not `1/-0.0`). See
  [`docs/archive/float-proofs-assume-exact-reals.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/float-proofs-assume-exact-reals.md).
- **[by design] A callee's entry check is never elided.** See above -- the call-site layer
  reports, it does not remove the callee's guard. Whole-program elision is a
  separate piece of work with real soundness preconditions.
- **[prototype] Higher-order callees are not checked.** A function-typed parameter carries no refinements in its type, so
  `(defn apply2 [f : (fn [int int] int) ...])` cannot know what `f`'s arguments
  must satisfy, and neither can a call to `apply2`. Passing a refined function
  as a value is legal and the callee's own entry checks still run, so nothing
  unsound follows -- only the static crossing is lost.

  What *does* work, and is worth knowing before assuming the limit is wider
  than it is: a direct call, a call through an alias of a global
  (`(let [g safe-div] (g 10 0))`), and a call to a lambda with refined
  parameters are all checked. The gap is specifically a value reached through a
  function-typed parameter.

  Writing the refinement into the function type is **rejected** with
  `TUR-E0378`, not ignored:

  ```turmeric
  (defn apply2 [f : (fn [int #refine{ v : int | (not= v 0) }] int)] : int ...)
  ;; error[TUR-E0378]: a refinement cannot be written on the parameter of a
  ;; (fn ...) type; function types do not carry refinements
  ```

  Closing the gap needs refinements to be part of function types, with the
  contravariant subtyping check that implies -- a type-system change the
  prototype excludes.
- **[by design] An instance that explicitly demands less is checked against its own,
  weaker, predicate at a statically-resolved site** -- deliberately, since it
  is the implementation that will actually run. `TUR-W0377` marks a call that
  depends on that leniency; it is a warning rather than an error because the
  call is correct for the instance it resolved to. See
  [docs/archive/class-param-refinement-not-demanded-of-callers.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/class-param-refinement-not-demanded-of-callers.md)
  for why that reading was chosen over making the class signature binding on
  callers.
- **[by design] An argument that cannot be PROVED is not an error; one that is DEFINITELY
  wrong is.** A crossing reports `TUR-E0371` when the goal mentions no variable
  and evaluates false -- `(safe-div 10 0)` -- because every execution reaching
  that call violates it. When the goal depends on a variable, the
  counterexample only says "not for every input", the runtime check is kept,
  and nothing is reported unless `--strict-refine` is on. Note that closedness
  is a property of the goal, not of what is in scope: an unrelated parameter on
  the caller does not make a literal violation unreportable.

- **[incomplete] Result-refinement propagation is order-dependent for a function's OWN
  return obligation.** Call-site crossings are resolved after the whole unit,
  so they always see every callee's refinement. A function's own return
  obligation is decided inline (that is what lets its check be elided), so it
  only sees refinements of functions already elaborated. Under mutual
  recursion, one direction may miss one. This can only lose a hypothesis, never
  add a false one.
- **[incomplete] Path splitting covers `if`, `let`, `match`, and `do`.** A branching body is
  discharged per path -- `c |- pred[then/r]` and `(not c) |- pred[else/r]` --
  and a `let` contributes `x = v`. A `let` whose binding shadows a name in
  scope is alpha-renamed first, so the hypothesis relates a fresh name rather
  than asserting the contradiction `x = x - 1`. A body that rebinds the same
  name a second time declines to split. A branching `let` VALUE splits too, so
  `(let [m (if c a b)] ...)` gives the body `m = a` on one path and `m = b` on
  the other rather than an unconstrained `m`. A `match` arm contributes
  everything its selection implies (see below); a pattern binder that shadows a
  name in scope declines the split. A `do` block is proved through its last
  form, but only when the preceding statements contain no assignment -- an
  assignment can stale a hypothesis about a parameter, and carrying that
  hypothesis across it would prove a function that violates its own refinement.
  The one exception is a `while` whose `:invariant` is proved (experimental,
  above): it is stepped over by renaming every earlier fact about a name it
  assigns to a fresh name, then assuming `p AND (not c)` of the current one.
- **[incomplete] A call-site crossing sees path conditions from `if`, `let`, and `match`.**
  A crossing is resolved after the whole unit, which is what lets it see every
  callee's refinement; the branches that had to be taken to reach it are
  recovered from the caller's body, so `(if (= n 0) 0 (+ 1 (f (- n 1))))`
  discharges its recursive crossing from `n >= 0` and `n != 0` together. A
  `let` contributes `x = v`, a `match` arm contributes a literal pattern's
  equation and its guard. The caller's **whole** body is searched, so a call in
  any body form keeps its guards -- not only one in the form the function
  returns.

  Four things are deliberately left out, and all four cost a diagnostic rather
  than soundness -- the callee's own entry check always remains:
  a caller whose body **assigns** anywhere (a condition naming a reassigned
  variable may no longer hold at the call); a **constructor tag or field
  selector**, except where a reflected measure needs one (below); a `let`
  that binds a **function**, which is not an arithmetic fact; and a call
  reachable by more than one route, which a macro sharing a node can produce.

  The constructor exception is for `^reflect` measures. When the callee's
  predicate mentions a reflected measure, an arm that connects to a variable
  the argument mentions contributes `(= (#dt/tag s) k)` and its Int-sorted
  record selectors, so RF4 can select the arm, as it does for a return
  obligation. It is no wider than that, because those symbols switch off the
  model search that produces a counterexample. A fact that names a variable
  rebound further down the path is dropped: at the call, the name means the
  inner binding.
- **[by design] A crossing under a shadowing binder is abandoned, not answered.** The
  encoder has one flat namespace, so an argument naming a shadowed variable
  would inherit the outer one's hypotheses -- `(let [x (- x x)] (sdiv 10 x))`
  under `x > 0` once "proved" `x != 0` of a value that is zero. Dropping the
  binding's equation is not enough, because the collision is in the name rather
  than the fact, so the whole crossing is skipped.
- **A `while` loop needs a written `:invariant`** -- see *Loop invariants*
  above.
  An unannotated loop is still not analysed: an accumulator it builds is
  Unknown, and there is no invariant *inference* -- inferring facts is the
  thing this design deliberately does not do. An annotated loop whose body
  the analysis cannot model is declined (both runtime checks kept), never
  approximated.
- **[prototype] No refinements on type parameters or higher-order predicates.** These are
  rejected or fall through to runtime. (Typeclass method signatures *are*
  supported now, on parameters and results alike -- see above.)
- **[by design] Nonlinear arithmetic** is uninterpreted, as described above.
  One sign fact survives the abstraction: a square `(* x x)` is known to be
  non-negative, so `(>= (* x x) 0)` and `(> (+ (* x x) 1) 0)` prove while
  `(* x y)` with distinct operands stays Unknown (`TUR-W0373` is emitted
  either way).
- **[incomplete] Integer reasoning is exact for equations, not yet for every
  inequality system.** `2v = 2x + 1` is recognised as having no integer
  solution (parity), an equation is substituted away through any variable
  with coefficient +/-1, and each inequality is rounded to its integer hull
  (`2q >= -1` gives `q >= 0`). What is left is the long tail: an all-integer
  equation with no unit coefficient (`2x + 3y = 1`) is read as two
  inequalities, and a system that is only infeasible over the integers
  *without* any equation in it is not decided. Both fall to Unknown and keep
  the runtime check; see `docs/archive/solver-integer-tail-plan.md`.
- **[incomplete] A counterexample needs the search to fit its budget.** The
  bounded search that produces `TUR-E0371`'s witness enumerates at most
  131072 assignments over at most 8 integer variables; an obligation wider
  than that is reported Unknown even when it is plainly false. (The limit was
  three variables until 2026-09-05, which made every four-parameter function
  with a refined return unrefutable.) `TUR_REFINE_STATS=1` reports both
  declines.
- **[by design] A predicate that calls an effectful function is rejected** (`TUR-E0375`),
  in all four positions: a refined parameter, `:pre`, `:post`, and a refined
  return. Whether a check runs depends on the build -- `--no-contracts` strips
  them, a release build drops them, and this feature elides the ones it can
  prove -- so an effectful predicate makes behaviour depend on whether its own
  contracts were compiled in. Reported only on PROVEN impurity: a predicate
  calling a function whose body the purity walk does not model (a field read, a
  loop) is left alone, since a wrong "impure" would reject working code. A
  direct call to a `#reads` measure is not counted either: it only reads, so
  running it is not observable (see
  [stateful-refinements-guide](stateful-refinements-guide.md#codegen-and-enforcement)).
- **[by design] A function that can `return` early is not proved.** Its
  refined return and `:post` are reported unknown (`TUR-W0372`, "the body can
  leave early through `return`") and checked at runtime on every exit, the
  early `return`s included. Proving only the last body form would cover the
  fall-through path and nothing else. Until 2026-10-03 that is exactly what
  happened, and the early value went unchecked
  ([report](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/early-return-bypasses-return-refinement.md)).
- **[by design] Decisions are memoized within a compilation unit**, keyed by a fingerprint
  of the normalized VC under alpha-renaming, and every hit is confirmed by
  structural comparison before its verdict is reused. Repeating the same
  refinement across many functions therefore costs one decision, not many.
  There is no cross-build cache.
- **[by design] Purity is a syntactic whitelist, not an analysis.** A function whose body
  steps outside the admitted forms is impure even when it is in fact pure --
  a struct field read through a computed receiver or a loop is enough.
  Its calls are then not congruent and measure-style reasoning over it
  does not go through. This costs completeness, never soundness. Widening the
  whitelist further is the natural next increment; the effect row cannot
  substitute for it, because an empty row is not a purity claim.
- **[by design] The `#reads` stateful slice is TRUSTED, not checked -- and its region
  guarantee is against ordinary code, not adversarial code.** `#reads w` is a
  declared promise the purity walk cannot verify for an inline-C body, and a
  `frozen` region's mutator lockout can be stepped around by a deliberate
  `::`-cast or inline C (an opaque carrier handle is reconstructable). The
  worst adversarial outcome is the forgiving default semantics -- a stale read
  of in-bounds memory -- wearing a proven badge; never an elided check (an
  impure measure's entry contract is unemittable, so compile-time rejection
  was always the enforcement, and that remains intact for ordinary code). A
  hard guarantee needs module-private construction / a `::`-sealed newtype --
  an independent language feature, tracked in
  `docs/archive/frozen-region-aliasing-via-coercing-cast.md`. Trust is no
  longer the whole story, though: a demonstrably broken frame (a direct read
  of a mutable global, or of state rooted in a parameter the frame omits)
  draws `TUR-W0383` and is refused the congruence grant, and
  `--dump-read-frames` shows the verification walk's verdict, which can stamp
  a walkable body's frame VERIFIED. An inline-C body stays exactly as trusted
  as this entry describes. See the
  [Stateful Refinements guide](stateful-refinements-guide.md).

Every one of these fails toward a runtime check, never toward a wrong answer
-- except the trusted `#reads` slice, which fails toward the forgiving
default semantics under a deliberately false declaration, as its entry above
states.

---

## See also

- [contract-types-guide.md](contract-types-guide.md) -- the always-on runtime half
- [experimental-flags-guide.md](experimental-flags-guide.md) -- the `--enable=` mechanism
- [syntax-guide.md](syntax-guide.md) -- `#lang` layers
- [../archive/refinement-types-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/refinement-types-plan.md) -- design, staging, and what is left

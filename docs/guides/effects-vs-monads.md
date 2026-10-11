---
title: Effects vs. Monads
category: Functional Patterns
description: When to reach for an effect handler and when to reach for a monad value
---

# Effects vs. Monads in Turmeric

Turmeric has both algebraic effects and monad typeclasses. They solve
overlapping problems, and the guide's job is to tell you which one to reach for.

The short version:

- **Effects are the default.** Sequencing, short-circuiting, state, errors,
  async, and nondeterminism all have direct-style effect formulations. Code
  written this way does not look monadic at all -- no `>>=`, no nesting, no
  transformer stack.
- **Monad values are for when the computation itself is the thing you want to
  hold.** `Functor` / `Applicative` / `Monad` / `Alternative` / `MonadError` are
  real higher-kinded typeclasses with instances for `Option`, `Result`,
  `Parser`, `Backtrack`, and `Goal`, and `do-m` gives you do-notation over any
  of them.

**Polymorphism over the monad works.** A constrained kind-polymorphic function
-- Turmeric's spelling of `Monad m => m a -> ...` -- compiles once and
dispatches through a dictionary the caller resolves, so the same body runs at
whichever instance the caller picks: `Option`, `Result`, `Either`, or your own
constructor. [Polymorphism over the monad](#polymorphism-over-the-monad) has
the details.

## Picking a tool

| You want | Reach for |
|---|---|
| I/O, async | Effects (`IO`, `Async`, `Await`) |
| Mutable state threaded through a call tree | Effects (`Get` / `Put`) |
| Errors with rich payloads | Effects (a `Throw`-shaped effect) |
| Short-circuit on a missing value | Effects (a `Fail`-shaped effect) |
| Logging / `Writer` | Effects (`Log`) |
| Nondeterminism, search | Multi-shot effect handlers (`^multishot`) |
| Parsing | Either -- effects for direct style, `Parser` + `do-m` for combinators |
| A computation you store, pass, or interpret later | Monad value + `do-m` |
| Backtracking search you want as a *value* | `Backtrack` / `Goal` + `do-m` |

The dividing line is whether the computation needs to be **reified**. An effect
is performed where it occurs and interpreted by whatever handler is installed;
there is no value sitting around representing "the rest of the work". When you
need that value -- to send it across a thread, to store it in an AST, to
interpret it twice -- you want a monad.

## The effect formulations

This is what most "monad chaining" turns into.

### Short-circuit on a missing value

```turmeric
(defeffect Fail-Lookup [] : int)
(defeffect Read-Config [key : cstr] : int)

(defn lookup-port [key : cstr] #fx{Fail-Lookup Read-Config} : int
  (let [v (perform (Read-Config key))]
    (if (< v 0)
      (perform (Fail-Lookup))
      v)))

(defn main [] : int
  (println (handle (lookup-port "http.port")
             (Fail-Lookup [] _)  8080
             (Read-Config [k] c) (resume c -1)))
  0)
```

```sweet-exp
defeffect Fail-Lookup [] : int
defeffect Read-Config [key : cstr] : int

defn lookup-port [key : cstr] #fx{Fail-Lookup Read-Config} : int
  let [v perform(Read-Config(key))]
    if {v < 0}
      perform(Fail-Lookup())
      v

defn main [] : int
  println
    (handle (lookup-port "http.port")
      (Fail-Lookup [] _)  8080
      (Read-Config [k] c) (resume c -1))
  0
```

Prints `8080`. No `>>=`, no nested `Just`, no chain of `match`. The failure path
is a single `perform`; the default lives in the handler.

Note the `#fx{...}` effect row: it declares which effects the function may
perform, and the compiler checks it (`TUR-E0009`). Rows are opt-in -- an
unannotated function is not checked at all.

### Errors with rich payloads

```turmeric
(defstruct Cfg-Error [what : cstr where : cstr])
(defeffect Throw-Cfg [e : Cfg-Error] : int)

(defn parse-port [raw : int] #fx{Throw-Cfg} : int
  (if (< raw 0)
    (perform (Throw-Cfg (Cfg-Error "not a number" "http.port")))
    raw))

(defn read-config [raw : int] #fx{Throw-Cfg} : int
  (parse-port raw))

(defn main [] : int
  (println (handle (read-config -1)
             (Throw-Cfg [e] _)
             (do (println (.what e))
                 (println (.where e))
                 8080)))
  0)
```

```sweet-exp
defstruct Cfg-Error [what : cstr where : cstr]
defeffect Throw-Cfg [e : Cfg-Error] : int

defn parse-port [raw : int] #fx{Throw-Cfg} : int
  if {raw < 0}
    perform $ Throw-Cfg $ Cfg-Error "not a number" "http.port"
    raw

defn read-config [raw : int] #fx{Throw-Cfg} : int
  parse-port(raw)

defn main [] : int
  println
    (handle (read-config -1)
      ;; Report what failed and where, then fall back to the default port.
      (Throw-Cfg [e] _)
        (do println(.what(e))
            println(.where(e))
            8080))
  0
```

A chain of `bind`s threading `Result<T, Error>` collapses into linear code.
`read-config` does not mention the error at all; it just calls `parse-port`. The
handler is the only place errors are visible.

**Payload lifetime.** The error travels up to a handler that runs *after*
`parse-port` has returned, so anything the payload carries outlives the frame
that produced it. `cstr` fields are safe only while they hold static literals.
The moment a message is *computed* (`(str-concat "bad key: " key)`), a `cstr`
field dangles -- use owned `String` fields for computed text. See
[strings-guide.md](strings-guide.md).

### State threading

```turmeric
(defeffect Get []        : int)
(defeffect Put [v : int] : nil)

(defn counter-step [] #fx{Get Put} : nil
  (perform (Put (+ (perform (Get)) 1))))

(defn main [] : int
  (let [^mut s 0]
    (handle (do (counter-step) (counter-step) (counter-step))
      (Get []  k) (resume k s)
      (Put [v] k) (do (set! s v) (resume k nil)))
    (println s))
  0)
```

```sweet-exp
defeffect Get []        : int
defeffect Put [v : int] : nil

defn counter-step [] #fx{Get Put} : nil
  perform $ Put {perform(Get()) + 1}

defn main [] : int
  let [^mut s 0]
    (handle (do (counter-step) (counter-step) (counter-step))
      (Get []  k) (resume k s)
      (Put [v] k) (do (set! s v) (resume k nil)))
    println(s)
  0
```

Prints `3`. This is the `State` monad as a handler: the handler *is* the
interpretation, and callers of `counter-step` never see the threading.

The state is an ordinary `let`-bound `^mut`. A clause is emitted as its own C
frame, so the compiler promotes a mutable that a clause touches to a shared
cell behind the scenes -- every view of it (the enclosing frame, each clause,
the code after the `handle`) reads and writes the same storage, which is what
the source says.

### Nondeterminism

A `^multishot` handler resumes the same continuation more than once, which is
exactly what the list monad does. One `perform` explores every branch:

```turmeric
(defeffect Choose [lo : int hi : int] : int)

(defn pair-sum [] #fx{Choose} : int
  (+ (perform (Choose 1 3)) (perform (Choose 10 20))))

(defn main [] : int
  (println (handle (pair-sum)
             (Choose [lo hi] ^multishot k) (+ (resume k lo) (resume k hi))))
  0)
```

```sweet-exp
defeffect Choose [lo : int hi : int] : int

defn pair-sum [] #fx{Choose} : int
  {perform(Choose(1 3)) + perform(Choose(10 20))}

defn main [] : int
  println
    (handle (pair-sum)
      (Choose [lo hi] ^multishot k) {(resume k lo) + (resume k hi)})
  0
```

Prints `68` -- every combination of `{1, 3}` with `{10, 20}`, summed:
`11 + 21 + 13 + 23`. Each `resume` runs an independent copy of the captured
continuation, and the two `perform`s compose, so the second `Choose` re-explores
under each branch of the first.

To resume a *variable* number of times, fold the continuation over the range
with a loop in the clause -- the direct expression of bounded nondeterminism:

```turmeric
(defn main [] : int
  (println (handle (+ 10 (perform (Choose 1 3)))
             (Choose [lo hi] ^multishot k)
             (let [^mut a 0 ^mut i lo]
               (while (<= i hi) (set! a (+ a (resume k i))) (set! i (+ i 1)))
               a)))
  0)
```
```sweet-exp
defn main [] : int
  println
    (handle {10 + perform(Choose(1 3))}
      ;; Fold the continuation over [lo, hi] -- one full run per iteration.
      (Choose [lo hi] ^multishot k)
        (let [^mut a 0 ^mut i lo]
          (while {i <= hi} set!(a {a + resume(k i)}) set!(i {i + 1}))
          a))
  0
```

Prints `36` -- `(10+1) + (10+2) + (10+3)`, one full run of the continuation per
iteration.

When the resumption strategy is itself recursive -- or you want it in a named
helper -- pass it through the effect payload instead. `k` is type-erased inside
a handler clause and cannot be handed to a helper directly, but a receiver
carried in the payload gets it as a properly typed continuation and may call it
as often as it likes:

```turmeric
(defeffect ChooseE [f : (fn [multishot-effect-cont] int)] : int)

(defn each [k : multishot-effect-cont lo : int hi : int] : int
  (if (> lo hi)
    0
    (+ (k lo) (each k (+ lo 1) hi))))

(defn choose [lo : int hi : int] : int
  (perform (ChooseE (fn [k : multishot-effect-cont] : int (each k lo hi)))))

(defn pair-sum [] : int
  (+ (choose 1 3) (choose 10 20)))

(defn main [] : int
  (println (handle (pair-sum)
             (ChooseE [f] ^multishot k) (f k)))
  0)
```

```sweet-exp
defeffect ChooseE [f : (fn [multishot-effect-cont] int)] : int

defn each [k : multishot-effect-cont lo : int hi : int] : int
  if {lo > hi}
    0
    {k(lo) + each(k {lo + 1} hi)}

defn choose [lo : int hi : int] : int
  perform $ ChooseE (fn [k : multishot-effect-cont] : int each(k lo hi))

defn pair-sum [] : int
  {choose(1 3) + choose(10 20)}

defn main [] : int
  println
    (handle (pair-sum)
      (ChooseE [f] ^multishot k) (f k))
  0
```

Prints `561` -- the sum over all 3 x 11 combinations.

### Parsing

Classical combinator code reads `digit >>= \d -> ...`. The effect version reads
like consuming a stream and failing through an effect:

```turmeric
(defeffect Peek []       : int)
(defeffect Advance []    : int)
(defeffect Parse-Fail [] : int)

(defn digit [] #fx{Peek Advance Parse-Fail} : int
  (let [c (perform (Peek))]
    (if (and (>= c 48) (<= c 57))
      (do (perform (Advance)) (- c 48))
      (perform (Parse-Fail)))))

(defn main [] : int
  (println (handle (digit)
             (Peek []          k) (resume k 55)
             (Advance []       k) (resume k 0)
             (Parse-Fail []    _) -1))
  0)
```

```sweet-exp
defeffect Peek []       : int
defeffect Advance []    : int
defeffect Parse-Fail [] : int

defn digit [] #fx{Peek Advance Parse-Fail} : int
  let [c perform(Peek())]
    if {{c >= 48} and {c <= 57}}
      do
        perform(Advance())
        {c - 48}
      perform(Parse-Fail())

defn main [] : int
  println
    (handle (digit)
      (Peek []       k) (resume k 55)
      (Advance []    k) (resume k 0)
      (Parse-Fail [] _) -1)
  0
```

Prints `7`. The handler decides what the input is and what failure means.

For parsers you want to *build up and reuse* -- combinators like `many`,
`sep-by`, `between` -- use the `Parser` monad instead; see below.

## When you want a monad value

Reach for a monad value when the computation must be reified:

- Building an AST or query plan that another mechanism executes later.
- Cross-thread message passing, where the sender constructs a chain and the
  receiver runs it.
- Combinator libraries, where the combinators take and return computations.
- Search you want to hold, restart, or interleave.

### The typeclasses

`Functor`, `Applicative`, `Monad`, `Alternative`, `MonadError`, `Bifunctor`,
`Foldable`, and `Traversable` are higher-kinded typeclasses (`defclass Monad
[^m]`). The `^m` marks a parameter of kind `* -> *`. They form Haskell's
hierarchy: `Monad` is declared over `Applicative`, which is over `Functor`;
`Alternative` over `Applicative`; `MonadError` over `Monad`; `Traversable` over
`Functor` and `Foldable`. So a function constrained by `^Monad M` may also call
`pure` and `fmap` (see
[Superclasses](typeclass-guide.md#superclasses)). Instances ship for:

| Type | Instances |
|---|---|
| `Option` | `Functor`, `Applicative`, `Monad`, `Alternative` |
| `(Result _ B)` | `Functor`, `Applicative`, `Monad`, `MonadError`, `Bifunctor` |
| `Parser` | `Functor`, `Applicative`, `Monad`, `Alternative` |
| `Backtrack` | `Functor`, `Applicative`, `Monad` |
| `Goal` | `Functor`, `Applicative`, `Monad` |
| `(Either E)` | `Functor` |

Instance heads may be partially applied, so a binary constructor can implement a
unary class by fixing a parameter: `(definstance Monad [(Result _ B)] ...)`
fixes the error type and leaves the ok type free (the right-biased convention).

### `do-m` notation

`do-m` is do-notation over any `Monad` instance. It desugars to nested `.bind`:

```turmeric
(defn half [x : int] : (Option int)
  (if (= x (* 2 (/ x 2)))
    (some (/ x 2))
    (none)))

(defn quarter-sum [x : int] : (Option int)
  (do-m a (half x)
        b (half a)
        (some (+ a b))))

(defn main [] : int
  (println (unwrap-or (quarter-sum 20) -1))
  (println (unwrap-or (quarter-sum 21) -1))
  0)
```

```sweet-exp
defn half [x : int] : (Option int)
  if {x = {2 * {x / 2}}}
    some({x / 2})
    none()

defn quarter-sum [x : int] : (Option int)
  do-m a half(x) b half(a) some({a + b})

defn main [] : int
  println(unwrap-or(quarter-sum(20) -1))
  println(unwrap-or(quarter-sum(21) -1))
  0
```

Prints `15` then `-1`: `half 20` gives `10`, `half 10` gives `5`, and `10 + 5 =
15`; `half 21` is `none`, which short-circuits the whole chain.

The same chain over `(Result A B)` is the identical shape -- only the failure
case carries a payload:

```turmeric
(defn half-r [x : int] : (Result int int)
  (if (= x (* 2 (/ x 2)))
    (ok (/ x 2))
    (err 1)))

(defn quarter-sum-r [x : int] : (Result int int)
  (do-m a (half-r x)
        b (half-r a)
        (ok (+ a b))))

(defn main [] : int
  (let [r (quarter-sum-r 20)] (println (if (ok? r) (ok-val r) -1)))
  (let [r (quarter-sum-r 21)] (println (if (ok? r) (ok-val r) -1)))
  0)
```

```sweet-exp
defn half-r [x : int] : (Result int int)
  if {x = {2 * {x / 2}}}
    ok({x / 2})
    err(1)

defn quarter-sum-r [x : int] : (Result int int)
  do-m a half-r(x) b half-r(a) ok({a + b})

defn main [] : int
  let [r quarter-sum-r(20)]
    println $ if ok?(r) ok-val(r) -1
  let [r quarter-sum-r(21)]
    println $ if ok?(r) ok-val(r) -1
  0
```

Prints `15` then `-1` as well; the second call's `err 1` short-circuits both
binds, and `err-val` would recover the `1`.

There is no separate `do-option` / `do-result` macro and no `bind`/`pure` name
to thread through -- `do-m` dispatches on the receiver's type through the
`Monad` typeclass.

### `bind`, `pure`, `alt-or`

The methods are callable directly when a macro would obscure things:

```turmeric
(defn lookup [k : int] : (Option int)
  (if (= k 1) (some 10) (none)))

(defn doubled [k : int] : (Option int)
  (bind (lookup k) (fn [x] (some (* x 2)))))

(defn either-key [] : (Option int)
  (alt-or (lookup 2) (lookup 1)))

(defn main [] : int
  (println (unwrap-or (doubled 1) -1))
  (println (unwrap-or (either-key) -1))
  0)
```

```sweet-exp
defn lookup [k : int] : (Option int)
  if {k = 1} some(10) none()

defn doubled [k : int] : (Option int)
  bind(lookup(k) (fn [x] some({x * 2})))

defn either-key [] : (Option int)
  alt-or(lookup(2) lookup(1))

defn main [] : int
  println(unwrap-or(doubled(1) -1))
  println(unwrap-or(either-key() -1))
  0
```

Prints `20` then `10`.

`bind` and `alt-or` dispatch on their receiver argument. `pure` and `empty` have
no receiver -- they dispatch on the *expected* type, so they need a return-type
annotation or an ascription (`(:: (pure 42) (Option int))`) to resolve.

### Parser combinators as values

`stdlib/parsec.tur` exposes `Parser` as a kind-`(* -> *)` type constructor with
the full instance set, so combinator code is ordinary `do-m` and `alt-or` over
the `Parser` monad, with `alt-or` giving full backtracking choice. See the
[parsec tutorial](parsec-tutorial.md) and [reference guide](parsec-guide.md),
or [parser-combinators-tutorial.md](parser-combinators-tutorial.md) to build
one from scratch.

### Polymorphism over the monad

This is Turmeric's `Monad m => ...`, and it is worth a section because the
spelling is unfamiliar even though the capability is ordinary.

A constrained kind-polymorphic function -- `^m` for the type constructor,
`^Monad m` for the constraint -- is compiled **once** and dispatches its method
calls through a dictionary the caller resolves. It can be called directly at a
concrete type, or passed as a rank-2 `forall` argument and instantiated at
several instances:

```turmeric
(defopaque Id [a] :int)
(defn mk-id [A] [x : A] : (Id A) ```c return (int64_t)x; ```)
(defn un-id [A] [b : (Id A)] : A ```c return (int64_t)b; ```)
(definstance Monad [Id] (bind [ma k] (k (un-id ma))))

(defopaque Halt [a] :int)
(defn mk-halt [A] [x : A] : (Halt A) ```c return (int64_t)x; ```)
(defn un-halt [A] [h : (Halt A)] : A ```c return (int64_t)h; ```)
(definstance Monad [Halt] (bind [ma k] ma))

(defn double-it [^m] [^Monad m x : (m int)] : (m int)
  (bind x (fn [v] (mk-id (* v 2)))))

(defn at-id   [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
  (un-id   (g (mk-id 5))))
(defn at-halt [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
  (un-halt (g (mk-halt 5))))

(defn main [] : int
  (println (at-id   double-it))
  (println (at-halt double-it))
  0)
```

```sweet-exp
defopaque Id [a] :int
defn mk-id [A] [x : A] : (Id A) ```c return (int64_t)x; ```
defn un-id [A] [b : (Id A)] : A ```c return (int64_t)b; ```
definstance Monad [Id] (bind [ma k] (k (un-id ma)))

defopaque Halt [a] :int
defn mk-halt [A] [x : A] : (Halt A) ```c return (int64_t)x; ```
defn un-halt [A] [h : (Halt A)] : A ```c return (int64_t)h; ```
definstance Monad [Halt] (bind [ma k] ma)

defn double-it [^m] [^Monad m x : (m int)] : (m int)
  bind x (fn [v] mk-id({v * 2}))

defn at-id   [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
  un-id   $ g mk-id(5)
defn at-halt [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
  un-halt $ g mk-halt(5)

defn main [] : int
  println $ at-id   double-it
  println $ at-halt double-it
  0
```

Prints `10` then `5` -- one body, two instances, chosen by the caller. This is
the same dictionary-passing machinery the van Laarhoven optics need.

`pure` works in these bodies too, dispatching to whichever instance the caller
picked, so a full `bind`-then-`pure` combinator is expressible:

```turmeric no-check
(defn bind-then-pure [^m] [^Monad m ^Applicative m x : (m int)] : (m int)
  (bind x (fn [v] (pure (* v 2)))))
```
```sweet-exp
defn bind-then-pure [^m] [^Monad m ^Applicative m x : (m int)] : (m int)
  bind(x (fn [v] pure({v * 2})))
```

Several constraints on one type constructor are fine, as above.

Both constraint spellings work -- the in-parameter `^Monad m` form and the
middle vector `[(Monad m) (Applicative m)]` -- and both by-value containers
(the stdlib `Option`) and int-carrier `defopaque`s can fill `m`.

Constructors of any arity fill `m`, and the abstracted parameter need not be
the last: `Either` abstracts through the curried head `(Either E)`, and
`Result` -- whose instances are ok-biased, so the mapped slot is *first* --
through the hole-headed `(Result _ cstr)`.

**In practice** you will still often write concrete monads or effect-style code
-- handler choice *is* the polymorphism there, and the code that wants
`Monad m =>` in Haskell is largely the code that becomes an effect here. Reach
for this when you genuinely want one body to serve every instance.

## Sharp edges

### Return-position dispatch needs an expected type

`pure` and `empty` dispatch on the expected type, not on an argument. With more
than one `Applicative` instance in scope they are ambiguous unless the context
pins the type:

```turmeric no-check
(pure 42)                        ;; ambiguous
(:: (pure 42) (Option int))      ;; OK -- ascribed
(defn mk [] : (Option int) (pure 42))   ;; OK -- return type pins it
```
```sweet-exp
pure(42)                        ;; ambiguous
(:: pure(42) (Option int))      ;; OK -- ascribed
defn mk [] : (Option int) pure(42)   ;; OK -- return type pins it
```

The `for` comprehension macro is not caught by this. It desugars the body to
`.pure`, and the dot-dispatch path consults the expected type (`bind`'s
signature pins the lambda's result), so `for` resolves against the auto-loaded
instances:

```turmeric no-check
(defn sums [] : (Option int)
  (for [x (half 20) y (half x)] (+ x y)))   ;; => 15
```
```sweet-exp
defn sums [] : (Option int)
  for [x half(20) y half(x)] {x + y}   ;; => 15
```

`do-m` remains the more explicit spelling and is what most of this guide uses;
it desugars only to `.bind`, which is receiver-directed.

### Type erasure at instance boundaries

Typeclass method parameters and results arrive type-erased (`int64_t`). Instance
bodies for handle-carrying types therefore ascribe their arguments back
(`(:: f :int)`), and code that needs a typed handle out of a method result
ascribes at the boundary (`(:: (fmap p f) (Parser int))`). For `Option` the
round trip is transparent; for other carriers expect ascriptions.

Where two instances of the same class are in scope and the receiver is erased,
the compiler raises `TUR_E0020_AMBIGUOUS_DISPATCH`; a `@TypeName` witness
(`(.fmap @option opt f)`) picks the instance at zero runtime cost. The witness
selects the *instance*, not the element type, so a result still needs an
ascription if you want a typed handle.

### Handler-clause restrictions

A handler clause is emitted as its own frame, which constrains what can go in
one:

- Reading and `set!`-ing a `^mut` binding from the enclosing function works --
  the compiler promotes such a mutable to a shared cell (the state example
  above). The one shape still unsupported is writing a mutable that a `while`
  loop *carries* while the loop also spans the `handle`; that evicts with a
  located error rather than miscompiling.
- `k` is type-erased inside the clause and cannot be passed to a helper
  expecting a `cont<...>`. Route the resumption strategy through the effect
  payload instead (as the nondeterminism example above does).
- Loops and conditionals in a clause are supported -- including a `while` that
  `resume`s per iteration (the multi-shot fold above). A `perform` from inside
  a loop, in a clause or anywhere else, is supported too, including a loop that
  assigns its carried `^mut` conditionally or more than once per iteration (see
  "Performs inside loops and conditionals" in the effects system guide, and
  `TUR_TRACE_EVICT=1` for the exact form when something still evicts).

The second is a compiler limitation; the first and third are designed
evictions with their own diagnostics.

## Compared to Haskell

| Property | Turmeric | Haskell |
|---|---|---|
| `Monad m =>` polymorphism | Dictionary-passed, one compiled body per fn | Dictionary-passed / specialized |
| `do`-notation | `do-m`, dispatched on the receiver's type | `do`, polymorphic over `Monad` |
| Most "monad" use cases | Effect handlers, direct style | Monad transformers / `mtl` |
| Async / IO | Effects | `IO` |
| State | Effects | `State` |
| Errors | Effects, or `Result` + `MonadError` | `Either` / `ExceptT` |
| Nondeterminism | `^multishot` handlers, or `Backtrack` | `[]` |
| Parsers | Effects, or the `Parser` monad | Parser combinator monad |
| Stacking several of the above | Nest handlers -- no lifting | Transformer stack + `lift` |

**The deal Turmeric makes:** direct-style code, and composition without lifting.
Nesting two handlers is the whole of what a two-layer transformer stack does,
with no `lift` and no `MonadTrans` instance to write. Writing a combinator once
and instantiating it at every monad works too -- it is just rarely the thing you
reach for, because the programs that need it in Haskell are mostly the programs
that become effect handlers here.

## See also

- [effects-system-guide.md](effects-system-guide.md) -- the algebraic effects API
- [custom-effects-tutorial.md](custom-effects-tutorial.md) -- writing custom effects
- [hkt-guide.md](hkt-guide.md) -- higher-kinded types, `defclass [^f]`, dispatch model
- [typeclass-guide.md](typeclass-guide.md) -- constraints, partially applied instance heads
- [error-handling-guide.md](error-handling-guide.md) -- error handling patterns
- [logic-programming-guide.md](logic-programming-guide.md) -- cloneable continuations and search
- [parser-combinators-tutorial.md](parser-combinators-tutorial.md) -- combinators on the backtracking monad

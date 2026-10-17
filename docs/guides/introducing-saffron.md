---
title: "Introducing Saffron"
category: Getting Started
description: "A first tour of Saffron, the dynamically typed dialect of Turmeric: Try Turmeric in the browser, printing, flow control, functions, ADTs, typeclasses, and algebraic effects -- every example in both s-expression and sweet-expression syntax."
---

# Introducing Saffron

Saffron is Turmeric with the type annotations made optional. One line at the
top of a file switches it on, and an unannotated parameter or return becomes
`any` instead of `int`:

```turmeric
#lang saffron

(defn double [x] (* x 2))

(defn main []
  (println (double 21))     ; 42
  (println (double 3.55))   ; 7.1
  0)
```
```sweet-exp
#lang saffron/sweet

defn double [x]
  {x * 2}

defn main []
  println(double(21))       ; 42
  println(double(3.55))     ; 7.1
  0
```

One `double`, two argument types, no annotations. It is the same compiler, the
same runtime, and the same object files as typed Turmeric -- a Saffron module
and a Turmeric module link together in one program.

This guide is the **tour**: what writing Saffron feels like, start to finish,
for someone new to the Turmeric ecosystem. It assumes nothing but a browser.
For the reference -- exactly what `any` supports, how the boundary with typed
code is checked, and the one feature Saffron gives up -- read
[saffron-guide.md](saffron-guide.md) afterwards.

**Every example below appears twice**: once in ordinary s-expressions
(`#lang saffron`), once in sweet-expression syntax (`#lang saffron/sweet`).
The two are the same program -- pick whichever you prefer and stay with it.
The `#lang` line is shown only on complete programs; the fragments in between
assume it is at the top of the file.

---

## 1. Try Turmeric

You do not have to install anything to follow along.

Open **[Try Turmeric](https://turmeric-lang.com/try)** in a browser. It runs
the real compiler, built to WebAssembly, in the page -- nothing is sent to a
server. Open the **Language** menu in the toolbar and pick a dialect:

- **`saffron`** -- s-expressions, the left-hand column below.
- **`saffron/sweet`** -- sweet-expressions, the right-hand column.

Then paste a program and run it. The **Docs** panel has every guide and the
whole API reference, precached, so it works offline too.

Prefer a terminal? Install the compiler with the version manager:

```sh
sh tvm/install.sh
tvm install 0.47.0 && tvm use 0.47.0
```

Homebrew, prebuilt binaries, Docker and build-from-source are all covered in
the [releases and installation guide](releases-and-installation-guide.md).

Then write your first file and run it:

```sh
cat > hello.tur <<'EOF'
#lang saffron

(defn main []
  (println "Hello from Saffron!")
  0)
EOF
tur run hello.tur
```

The sweet-expression version is the same program with the outer parentheses
replaced by indentation. Save it as `hello.tur.sweet` (or keep the `.tur`
extension -- the `#lang` line is what selects the dialect):

```sh
cat > hello.tur.sweet <<'EOF'
#lang saffron/sweet

defn main []
  println("Hello from Saffron!")
  0
EOF
tur run hello.tur.sweet
```

Both print `Hello from Saffron!`. `main` returns `0` -- the process exit code.

`tur run` compiles and executes in one step. `tur build hello.tur` leaves an
executable behind instead, and `tur repl` gives you an interactive prompt.

---

## 2. Printing

`println` prints a value and a newline. In Saffron it works on a value whose
type is only known at run time, which is what lets a function print whatever
it is handed:

```turmeric
(defn show [x]
  (println x))

(show 42)      ; 42
(show 7.1)     ; 7.1
(show "hi")    ; hi
(show true)    ; true
```
```sweet-exp
defn show [x]
  println(x)

show(42)       ; 42
show(7.1)      ; 7.1
show("hi")     ; hi
show(true)     ; true
```

`x` here is an `any` -- a two-word box carrying a type tag and a payload --
and `println` reads the tag to decide how to print. Ints, floats, booleans and
strings all print. A payload with no printer, a keyword for instance, panics
naming the type (`println: no operator for a Sym argument`) rather than
printing something wrong.

Note that the default applies to **parameters and returns**, not to
expressions. The literal `7.1` written directly in a Saffron file is still a
`float`; it becomes an `any` on the way into `show`.

---

## 3. Flow control

### `if` is an expression

Both branches are required, and the form produces a value:

```turmeric
(defn abs [n]
  (if (< n 0) (- 0 n) n))

(abs -5)     ; => 5
(abs -7.1)   ; => 7.1
```
```sweet-exp
defn abs [n]
  if {n < 0}
    {0 - n}
    n

abs(-5)      ; => 5
abs(-7.1)    ; => 7.1
```

### Truthiness: only `nil` and `false` are falsy

This is the Lisp and Clojure convention, not C's. `0`, `""` and an empty
container are all **truthy**:

```turmeric
(defn describe [x]
  (if x "truthy" "falsy"))

(describe 0)       ; => "truthy"
(describe "")      ; => "truthy"
(describe false)   ; => "falsy"
(describe nil)     ; => "falsy"
```
```sweet-exp
defn describe [x]
  if x
    "truthy"
    "falsy"

describe(0)        ; => "truthy"
describe("")       ; => "truthy"
describe(false)    ; => "falsy"
describe(nil)      ; => "falsy"
```

Deciding truthiness needs the value's runtime tag, which is why this is a
Saffron rule: in typed Turmeric an `if` condition must already be a `bool`.

### Many branches: `cond`

`cond` takes test/result pairs in order. `else` is the fallback:

```turmeric
(defn classify [n]
  (cond (> n 0) "positive"
        (< n 0) "negative"
        else    "zero"))

(classify 7.1)   ; => "positive"
(classify -3)    ; => "negative"
(classify 0)     ; => "zero"
```
```sweet-exp
defn classify [n]
  cond
    {n > 0}
    "positive"
    {n < 0}
    "negative"
    else
    "zero"

classify(7.1)    ; => "positive"
classify(-3)     ; => "negative"
classify(0)      ; => "zero"
```

### One-armed: `when` and `unless`

```turmeric
(when   (> n 0) (println "positive"))
(unless (= n 0) (println "non-zero"))
```
```sweet-exp
when {n > 0}
  println("positive")
unless {n = 0}
  println("non-zero")
```

Each takes exactly **one** body form. For several statements, wrap them in a
`do`:

```turmeric
(when (> n 0)
  (do (println "positive")
      (println n)))
```
```sweet-exp
when {n > 0}
  do
    println("positive")
    println(n)
```

### Looping

There is no counted `for`. A loop is either recursion:

```turmeric
(defn countdown [n]
  (when (> n 0)
    (do (println n)
        (countdown (- n 1)))))

(countdown 3)   ; prints 3, 2, 1
```
```sweet-exp
defn countdown [n]
  when {n > 0}
    do
      println(n)
      countdown({n - 1})

countdown(3)    ; prints 3, 2, 1
```

or `while` with a `^mut` binding you advance by hand:

```turmeric
(let [^mut i 0]
  (while (< i 3)
    (println i)
    (set! i (+ i 1))))
; prints 0, 1, 2
```
```sweet-exp
let [^mut i 0]
  while {i < 3}
    println(i)
    set!(i {i + 1})
; prints 0, 1, 2
```

`^mut` is what makes a binding writable; without it `set!` is rejected, so the
one mutable thing in the loop is visibly marked.

---

## 4. Functions

`defn` names a function. In Saffron the parameter list is just names:

```turmeric
(defn double [x] (* x 2))
(defn add    [a b] (+ a b))

(double 21)     ; => 42
(add 1 2)       ; => 3
(add 1.5 2.25)  ; => 3.75
```
```sweet-exp
defn double [x]
  {x * 2}
defn add [a b]
  {a + b}

double(21)      ; => 42
add(1 2)        ; => 3
add(1.5 2.25)   ; => 3.75
```

Arithmetic on an `any` is **numeric only**. `(+ "a" "b")` is not string
concatenation -- it panics with `+: no operator for a cstr argument`.

### Anonymous functions and closures

`fn` builds a function value that captures its enclosing scope:

```turmeric
(defn make-adder [n]
  (fn [x] (+ x n)))

(let [add7 (make-adder 7)]
  (add7 35))    ; => 42
```
```sweet-exp
defn make-adder [n]
  (fn [x] {x + n})

let [add7 make-adder(7)]
  add7(35)      ; => 42
```

### Functions as arguments

A function value is just another thing an `any` can hold, so higher-order code
needs no annotations either:

```turmeric
(defn apply-twice [f x]
  (f (f x)))

(apply-twice double 3)             ; => 12
(apply-twice (fn [n] (+ n 1)) 10)  ; => 12
```
```sweet-exp
defn apply-twice [f x]
  f(f(x))

apply-twice(double 3)              ; => 12
apply-twice((fn [n] {n + 1}) 10)   ; => 12
```

### Vectors

A `[...]` literal builds a vector whose elements are `any`, so it can be
heterogeneous. The Saffron prelude ships `vec-map`, `vec-filter` and
`vec-fold` over it, alongside the ordinary `vec-len` and `vec-get`:

```turmeric
(defn sum [v]
  (vec-fold v 0 (fn [acc x] (+ acc x))))

(sum [1 2 3 4])                                    ; => 10
(vec-len [1 "two" 3.5])                            ; => 3
(vec-get (vec-map [1 2 3] double) 2)               ; => 6
(vec-len (vec-filter [1 2 3 4] (fn [x] (> x 2))))  ; => 2
```
```sweet-exp
defn sum [v]
  vec-fold(v 0 (fn [acc x] {acc + x}))

sum([1 2 3 4])                                     ; => 10
vec-len([1 "two" 3.5])                             ; => 3
vec-get(vec-map([1 2 3] double) 2)                 ; => 6
vec-len(vec-filter([1 2 3 4] (fn [x] {x > 2})))    ; => 2
```

`vec-filter` uses the same truthiness rule as `if`: only `nil` and `false`
drop an element.

---

## 5. Algebraic data types

A struct is *all of* several fields at once; an ADT is *one of* several
shapes. `defdata` declares one, `match` takes it apart:

```turmeric
(defdata Shape :copy
  (Circle :float)
  (Rect   :float :float))

(defn area [s]
  (match s
    (Circle r)  (* 3.14159 (* r r))
    (Rect w h)  (* w h)))

(area (Circle 2.5))    ; => 19.6349
(area (Rect 3.5 4.0))  ; => 14
```
```sweet-exp
defdata Shape :copy
  Circle(:float)
  Rect(:float :float)

defn area [s]
  match s
    (Circle r)
    {3.14159 * {r * r}}
    (Rect w h)
    {w * h}

area(Circle(2.5))      ; => 19.6349
area(Rect(3.5 4.0))    ; => 14
```

Note what happened to `s`: it arrives as an `any`, and the arms name `Shape`,
so `match` narrows the box to that type before dispatching on the tag. A
value of some other type panics at the narrow (`cast: any holds float, not
Shape`) rather than taking a wrong arm.

`:copy` after the name is the value discipline. Without it an ADT value is
single-use -- passing it somewhere moves it, and a second use is a compile
error -- which is the right default for a value owning a resource. `Shape` is
immutable and owns nothing, so `:copy` lets it be used as many times as you
like -- which the program at the end of this guide needs, since it asks each
shape for both its name and its area.

Constructors may carry nothing at all, and each arm binds its own payload:

```turmeric
(defdata Json
  (JNull)
  (JBool :bool)
  (JNum  :float)
  (JStr  :cstr))

(defn render [j]
  (match j
    (JNull)   "null"
    (JBool b) (if b "true" "false")
    (JNum n)  "a number"
    (JStr s)  s))

(render (JNull))         ; => "null"
(render (JBool false))   ; => "false"
(render (JStr "hi"))     ; => "hi"
```
```sweet-exp
defdata Json
  JNull()
  JBool(:bool)
  JNum(:float)
  JStr(:cstr)

defn render [j]
  match j
    (JNull)
    "null"
    (JBool b)
    if b
      "true"
      "false"
    (JNum n)
    "a number"
    (JStr s)
    s

render(JNull())          ; => "null"
render(JBool(false))     ; => "false"
render(JStr("hi"))       ; => "hi"
```

The constructors' payload types are annotated, and that is deliberate: an ADT
declaration is the *shape* of your data, which is exactly the edge worth
pinning down even in a dynamic file. The functions over it stay unannotated.

If you want to ask a value what it is rather than match on it, `type-of` names
the type -- it needs an `any`, so route the value through an unannotated
parameter:

```turmeric
(defn kind-of [x] (type-of x))

(kind-of 7.1)          ; => "float"
(kind-of "hi")         ; => "cstr"
(kind-of (JNull))      ; => "Json"
```
```sweet-exp
defn kind-of [x]
  type-of(x)

kind-of(7.1)           ; => "float"
kind-of("hi")          ; => "cstr"
kind-of(JNull())       ; => "Json"
```

`is?` tests a type and `cast` narrows to one with a runtime check. See
[sum-types-guide.md](sum-types-guide.md) for parametric sums, record-style
variants and exhaustiveness, and [saffron-guide.md](saffron-guide.md) for the
type-case idiom that `is?` enables.

---

## 6. Typeclasses

A typeclass is an interface a type can implement. `defclass` declares the
methods; `definstance` supplies them for one type:

```turmeric
(defclass Speaks [a]
  (speak [x] : cstr))

(definstance Speaks [int]   (speak [x] "beep"))
(definstance Speaks [float] (speak [x] "boop"))
(definstance Speaks [cstr]  (speak [x] "echo"))

(defn tell [x]
  (println (.speak x)))

(tell 42)     ; beep
(tell 7.35)   ; boop
(tell "hi")   ; echo
```
```sweet-exp
defclass Speaks [a]
  speak([x] : cstr)

definstance Speaks [int]
  speak([x] "beep")
definstance Speaks [float]
  speak([x] "boop")
definstance Speaks [cstr]
  speak([x] "echo")

defn tell [x]
  println(.speak(x))

tell(42)      ; beep
tell(7.35)    ; boop
tell("hi")    ; echo
```

`tell` takes one `any` and reaches three different instances. The method name
`.speak` resolves at compile time -- that is what fixes the class and the slot
-- and only the *instance* waits for runtime, chosen from the box's own tag.

Your own types join in the same way:

```turmeric
(defdata Animal
  (Dog)
  (Cat))

(definstance Speaks [Animal]
  (speak [x] (match x (Dog) "woof" (Cat) "meow")))

(tell (Dog))   ; woof
(tell (Cat))   ; meow
```
```sweet-exp
defdata Animal
  Dog()
  Cat()

definstance Speaks [Animal]
  speak([x] match(x (Dog) "woof" (Cat) "meow"))

tell(Dog())    ; woof
tell(Cat())    ; meow
```

A type with no instance of the class panics naming both, rather than
returning a wrong answer:

```
panic: no instance of Speaks for bool (dispatching .speak on an any)
```

Runtime dispatch is keyed on the box tag, so it covers methods that take only
the receiver -- which is most of them. A method taking a second argument of
the class's own type variable (`eq [x : a y : a]`), or returning one, needs a
type the tag cannot supply and panics saying so; narrow with `is?` or `cast`
first in those cases. [saffron-guide.md](saffron-guide.md) lists the exact
set, and [typeclass-guide.md](typeclass-guide.md) covers constraints,
superclasses, defaults and the stdlib classes.

`Animal`'s constructors happen to carry nothing, but that is not a
restriction: an ADT whose constructors carry payloads dispatches the same way,
and the program at the end of this guide does exactly that with `Shape`.

---

## 7. Algebraic effects

Effects separate *what* an operation is from *how* it is carried out. A
computation `perform`s an effect; a `handle` installed by the caller decides
what actually happens.

`defeffect` declares one. Its **result** type is required -- an effect is a
protocol between a computation and its handler, and the handler has to know
what to `resume` with. Its parameters take the file's own default like any
other, so leaving them off means `any`; annotate one when you want that edge
pinned down:

```turmeric
(defeffect Log [msg : cstr] : int)
(defeffect Ask [] : int)
```
```sweet-exp
defeffect Log [msg : cstr] : int
defeffect Ask [] : int
```

`perform` raises the effect and suspends. Everything else is ordinary
unannotated Saffron:

```turmeric
(defn work []
  (perform (Log "starting"))
  (let [n (perform (Ask))]
    (perform (Log "done"))
    (+ n 1)))
```
```sweet-exp
defn work []
  perform(Log("starting"))
  let [n perform(Ask())]
    perform(Log("done"))
    {n + 1}
```

`handle` supplies a clause per effect. `k` is the continuation:
`(resume k v)` sends `v` back to the `perform` and carries on:

```turmeric
(handle (work)
  (Log [msg] k) (do (println msg) (resume k 0))
  (Ask [] k)    (resume k 41))
; prints:
; starting
; done
; => 42
```
```sweet-exp
(handle (work)
  (Log [msg] k) (do (println msg) (resume k 0))
  (Ask [] k)    (resume k 41))
; prints:
; starting
; done
; => 42
```

`work` never named a logger or a source of numbers. Swap the handler and the
same function behaves differently, with no edit to `work`:

```turmeric
(handle (work)
  (Log [msg] k) (resume k 0)
  (Ask [] k)    (resume k 1))
; prints nothing
; => 2
```
```sweet-exp
(handle (work)
  (Log [msg] k) (resume k 0)
  (Ask [] k)    (resume k 1))
; prints nothing
; => 2
```

That is dependency injection with no framework: logging, configuration,
mocked I/O in a test, a retry policy. Deep and shallow handlers, effect rows
and one-shot versus cloneable continuations are in
[effects-system-guide.md](effects-system-guide.md).

---

## Putting it together

One complete program using the pieces above:

```turmeric
#lang saffron

(defdata Shape :copy
  (Circle :float)
  (Rect   :float :float))

(defclass Describe [a]
  (label [x] : cstr))

(definstance Describe [Shape]
  (label [x] (match x (Circle r) "circle" (Rect w h) "rect")))

(defeffect Report [line : any] : int)

(defn area [s]
  (match s
    (Circle r)  (* 3.14159 (* r r))
    (Rect w h)  (* w h)))

(defn survey [a b]
  (do (perform (Report (.label a)))
      (perform (Report (.label b)))
      (+ (area a) (area b))))

(defn main []
  (println
    (handle (survey (Circle 2.5) (Rect 3.5 4.0))
      (Report [line] k) (do (println line) (resume k 0))))
  0)
```
```sweet-exp
#lang saffron/sweet

defdata Shape :copy
  Circle(:float)
  Rect(:float :float)

defclass Describe [a]
  label([x] : cstr)

definstance Describe [Shape]
  label([x] match(x (Circle r) "circle" (Rect w h) "rect"))

defeffect Report [line : any] : int

defn area [s]
  match s
    (Circle r)
    {3.14159 * {r * r}}
    (Rect w h)
    {w * h}

defn survey [a b]
  do
    perform(Report(.label(a)))
    perform(Report(.label(b)))
    {area(a) + area(b)}

defn main []
  println
    (handle (survey (Circle 2.5) (Rect 3.5 4.0))
      (Report [line] k) (do (println line) (resume k 0)))
  0
```

Both print:

```
circle
rect
33.6349
```

Only the declarations -- the shape of the data, the class's method signature,
and the effect -- carry annotations. `area`, `survey` and `main` are all plain
names, and `survey` handles a circle and a rectangle without ever naming either
type: `.label` picks the instance from each value's own tag at run time.

`handle` finds the `perform`s in the whole computation it wraps, however deep
-- `survey` performs directly here, but a `perform` several calls down is caught
just the same. That is worth saying because it did not always hold in a Saffron
file, and the fix is recent.

---

## Where to go next

- **[saffron-guide.md](saffron-guide.md)** -- the reference: the eight dynamic
  operations, the checked boundary with typed Turmeric, refinements as runtime
  contracts, and the one feature Saffron gives up (`with-region`).
- **[quickstart.md](quickstart.md)** -- the same tour in *typed* Turmeric,
  with `Option`, `Result` and structs.
- **[syntax-guide.md](syntax-guide.md)** -- `#lang` bases and layers; the full
  sweet-expression rules.
- **[repl.md](repl.md)** -- `tur repl`, and how it picks up a project.
- **[union-intersection-types-guide.md](union-intersection-types-guide.md)** --
  `any`, unions and gradual typing on the typed side.

Examples in this guide are compiled and run by
`tests/fixtures/docs-introducing-saffron-examples`, and every
s-expression/sweet-expression pair is checked to read to the same AST by
`tools/check-guide-pairs.tur`.

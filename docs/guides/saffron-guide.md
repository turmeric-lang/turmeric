---
title: "Saffron -- the dynamically typed dialect"
category: Getting Started
description: "#lang saffron makes `any` the default type, so parameters and returns need no annotations. This guide covers what changes, what stays, the eight dynamic operations, the checked boundary with typed Turmeric, and the one feature Saffron gives up."
---

# Saffron

Saffron is Turmeric with the types made optional. One line at the top of a file
switches it on:

```turmeric
#lang saffron

(defn add [a b] (+ a b))

(defn main []
  (println (add 1 2))        ;; 3
  (println (add 1.5 2.25))   ;; 3.75
  0)
```

No annotations, and the same `add` handles ints and floats. It is the same
compiler, the same runtime, and the same object files: a Saffron module and a
Turmeric module link together in one program.

Saffron is on by default. The `#lang saffron` line is all you need -- no
`--enable=` flag, no manifest entry, and no lifecycle warning on stderr. It is
an ordinary base dialect, on the same footing as `#lang turmeric`, and
`tur dialects` lists every base as `stable` -- the eight Turmeric and Saffron
ones, and the two `r7rs` bases, the Scheme dialect that rides the same dynamic
substrate and graduated alongside it in v0.57.0 (see
[docs/archive/r7rs-lang-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/r7rs-lang-plan.md)).

The dialect is young, though, and parts of the dynamic surface are still being
built out -- design notes, remaining stages and known gaps live in
[docs/archive/saffron-lang-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/saffron-lang-plan.md).

## What actually changes

Exactly one thing: **an unannotated parameter or return defaults to `any`
instead of `int`.** Everything else follows from that.

`any` is Turmeric's existing top type -- a two-word tagged box carrying a type
id and a payload. It is not new, and it is not a Saffron invention; Saffron just
makes it the default.

### Literals keep their own types

This trips people up, so it is worth being precise. The default applies to
**parameters and returns**, not to expressions. A literal `7.1` in a Saffron
file is still a `float`:

```turmeric
(println (type-of 7.1))     ;; ERROR: 'type-of' expects an 'any'-typed argument, got 'float'
```

Route it through an unannotated parameter and it is an `any`:

```turmeric
(defn kind-of [x] (type-of x))

(kind-of 7.1)    ;; "float"
(kind-of "hi")   ;; "cstr"
(kind-of true)   ;; "bool"
```

### Annotations stay legal

Saffron does not take anything away. Annotate whatever you want, wherever you
want, in the ordinary syntax -- and you will need to when a value crosses into
typed code:

```turmeric
(defstruct Point [x : float  y : float])

(defn px [p] (.x p))          ;; p is `any`, the struct's fields are typed
```

Mixed annotation is the point, not a compromise: annotate the edges of a module
and leave its interior dynamic, or the other way round.

## Truthiness

**Only `nil` and `false` are falsy.** `0`, `""` and an empty container are all
truthy -- the Lisp and Clojure convention, not C's:

```turmeric
(defn describe [x] (if x "truthy" "falsy"))

(describe 0)       ;; "truthy"
(describe "")      ;; "truthy"
(describe false)   ;; "falsy"
```

This is a runtime decision on the value's tag, which is why it needs the dynamic
dialect: in typed Turmeric an `if` condition must already be a `bool`.

## The dynamic surface is eight operations

`any` supports exactly these, and the list is deliberately closed:

| | |
|---|---|
| arithmetic | `+ - * / mod` |
| bit operators | `bit-and bit-or bit-xor bit-shl bit-shr` |
| comparison | `= not= < > <= >=` |
| print | `println` |
| call | `(f x ...)` where `f` is `any` |
| truthiness | `if when and or` |
| field access | `(.field x)` |
| type inspection | `type-of` `is?` `cast` |

Arithmetic is **numeric only**. `(+ "a" "b")` is not string concatenation --
it panics with `+: no operator for a cstr argument`.

Higher-order code works without annotations, because a function value is just
another thing an `any` can hold:

```turmeric
(defn apply-twice [f x] (f (f x)))

(apply-twice (fn [n] (+ n 1)) 10)   ;; 12
```

Annotating the parameter with the all-`any` signature it already has --
`(defn apply-twice [f : (fn [any] any) x] ...)` -- is fine too, and an `any`
holding a function reaches such a parameter on both back ends. (That was not
always so: see `all-any-fn-param-is-unusable` in `docs/archive/`.)

## Containers

Container literals hold `any` elements, so they can be heterogeneous, and the
stdlib operations work on them unannotated:

```turmeric
(defn total   [v] (vec-fold v 0 (fn [acc x] (+ acc x))))
(defn doubled [v] (vec-map  v (fn [x] (* x 2))))

(total [1 2 3])                  ;; 6
(vec-get (doubled [1 2 3]) 2)    ;; 6
(vec-len [1 2 3])                ;; 3
```

`#map{...}` and `#set{...}` widen the same way -- a `#map{...}` is a
`(Map any any)`, keys included, and so is `(map-new)`; a `#set{...}` is a
`(Set any)`. Keys of different types can share one map (`#map{"a" 1 :b 2}`),
because `Hash[any]` and `MapKey[any]` hash and compare by the payload: a
keyword held in an `any` finds the entry a bare keyword put there, and a key
whose payload matches no entry is a miss. Like `[1 2 3]` against a `(Vec int)`
parameter, a literal does not satisfy a parameter annotated with a narrower
map type; build a typed map by ascription, `(:: (map-new) (Map Sym int))`. A miss on a map whose values are
`any` is `nil`, which under the truthiness rule below makes
`(if (map-get m k) ...)` a presence test.

## Reading a value back out

`type-of` names the type, `is?` tests it, `cast` narrows it with a runtime
check. A wrong `cast` panics rather than reinterpreting the payload:

```turmeric
(cast x int)      ;; panics `cast: any holds cstr, not int` on a mismatch
```

The idiomatic form is a **type-case**, which is how dynamic languages dispatch
anyway (Clojure's `condp instance?`, Racket's predicate `cond`). The `if` guard
narrows the binding for you, so the branch body sees the real type:

```turmeric
(defn area [shape]
  (if (is? shape (Option float))
    (unwrap-or shape 0.0)      ;; `shape` is an (Option float) here
    -1.0))
```

That works for a bare target (`(is? x Circle)`) and an applied one
(`(is? x (Option float))` above) alike, which means the whole
`Functor`/`Applicative`/`Monad` stack is reachable from an `any`. Note the
consequence: once the guard has narrowed, an explicit `(cast shape (Option
float))` inside the branch is **redundant, and therefore an error** -- the
binding is no longer an `any`.

## The boundary with typed Turmeric

A Saffron caller reaching a typed function is a **checked** crossing. The
compiler inserts a `cast` at each argument whose static type is `any` and whose
parameter type is concrete, so a mismatch panics at the boundary instead of
reinterpreting a payload word:

```turmeric
;; typed module
(defn scale [x : float k : int] : float ...)

;; Saffron caller -- the compiler checks `x` and `k` on the way in
(scale 7.5 2)
```

The cost is one tag compare per argument. There is deliberately **no** flag to
turn it off: an unchecked boundary turns a type error into a memory-safety bug,
which is the whole reason `cast` was built checked.

One consequence for **containers**: a container a Saffron file builds is always
the all-`any` instantiation (a `[1 2 3]` literal is a `(Vec any)`, holding
boxes), so it does not fit a typed `(Vec int)` parameter, whose elements are
raw ints -- the checked crossing panics with `different instantiation of Vec`
rather than reinterpreting the boxes. A typed container parameter can only be
fed a container built in typed code; scalars cross freely.

The reverse direction works too -- a typed module can `import` a Saffron one and
narrow its `any`-typed exports.

## Refinements become runtime contracts

`#refine{...}` needs a static base type and an SMT discharge, and an `any` has
neither. Rather than reject it or silently drop it, Saffron **checks it at run
time**:

```turmeric
(defn pos [n : #refine{v : any | (> v 0)}] n)

(pos 5)    ;; 5
```

A violation the compiler can see is still a static error (`TUR-E0371`, "cannot
be proved statically"). A violation it cannot -- a value arriving at run time --
panics `Contract violated`. What does *not* happen is the predicate being
accepted and ignored, which would leave the file looking like it carries a
guarantee it does not.

## Memory: a compiled Saffron program is collected

An `any` value can alias freely: widening a by-value payload boxes it, the box
is copied into arguments, fields and results, and a dynamic call `(f h)` may
keep any of them -- there is no body to look at. So no static owner exists for
most of those boxes. A compiled single-unit `#lang saffron` program therefore
allocates from the same conservative mark-sweep collector a `#lang r7rs`
program does (`src/runtime/r7gc.c`; `docs/guides/r7rs-guide.md` describes it),
on Linux and macOS:

- A loop that rebuilds a structure runs in bounded memory. Measured: a
  100-cell `any` list rebuilt, mapped through a dynamic call and folded 20,000
  times peaks at 6 MB, against 84 MB (and growing) without the collector,
  and runs no slower.
- The compiler's static drops still run where it can prove an owner; the
  collector reclaims what it cannot.
- `TUR_SAFFRON_GC=0 tur build prog.tur`, or `tur --no-saffron-gc build ...`,
  builds on plain `malloc`/`free` instead, where those boxes are simply never
  freed (`docs/reported/saffron-static-ownership-residue.md` lists the
  shapes). `TUR_GC_STATS=1` on a collected program prints what the collector
  did.
- A `--shared` build (several translation units) keeps plain `malloc`: each
  unit would otherwise get its own heap.

## What Saffron gives up

**One feature: `with-region`.**

```turmeric
(defn mk [n] (with-region (fn [] (+ n 1))))
;; error [TUR-E0312]: `with-region` is not available in a `#lang saffron` file
```

A region reclaims memory only when a static walk over the bracket's **result
type** proves nothing outside it points in. An `any` result clears nothing, so
the generation would be retired rather than reclaimed -- the bracket would cost
a push and a pop and save nothing, with nothing saying so. The error refuses
that silence. Run `tur explain TUR-E0312` for the full reasoning.

The restriction is per **file**. Put the allocation-heavy code in a Turmeric
module, keep the bracket there, and call across the boundary.

### What Saffron does *not* give up

An earlier draft of the design listed GADTs, session types, linearity and
borrows as unavailable too. Measured, all four check **identically** in a
Saffron file, because their proofs read annotations (which stay legal) or walk
uses and scopes -- none of which `any` touches:

| Feature | In Saffron |
|---|---|
| `defgadt` | works -- skolem escape still fires |
| `Session[P]` | works -- `TUR-E0211` still fires, same protocol state |
| `^linear` / `^unique` / `lref<T>` | works -- `TUR-E0101` / `TUR-E0100` still fire |
| `&T` / `&mut T` | works -- the aliasing conflict still fires |

## Typeclasses

A method call on an un-narrowed `any` **dispatches on the box's own tag**. The
method name resolves statically -- that is what fixes the class and the slot --
and only the instance waits for runtime:

```turmeric
#lang saffron
(defclass Named [a]
  (name-of [x] : cstr))
(definstance Named [int]   (name-of [x] "int"))
(definstance Named [float] (name-of [x] "float"))

(defn describe-kind [x] (.name-of x))   ;; x is `any`

(describe-kind 7)      ;; => "int"
(describe-kind 7.35)   ;; => "float"
```

A type with no instance of the class is a panic naming both, not a wrong answer:

```
panic: no instance of Named for bool (dispatching .name-of on an any)
```

**In typed Turmeric the same code is still a diagnostic**, and deliberately so:
deferring a decision to runtime is the wrong default for a language whose types
are static. There you get `cannot dispatch '.name-of' on an 'any' receiver`,
with a help line naming the three static routes -- which also remain available
in Saffron, and are still worth preferring when the type IS known:

1. **Narrow with a type-case** -- `(if (is? x Circle) (area x) ...)`.
2. **Unbox** -- `(area (cast x Circle))`.
3. **Pin the instance** -- `(.hash @int x)`, one token, and a wrong witness
   panics rather than reinterpreting.

Each of these resolves at compile time, so it costs nothing at runtime and
cannot panic for a missing instance.

### What dynamic dispatch does not cover yet

The registry is keyed on the **box tag**, so an instance the tag cannot name is
not reachable through it. The cases that panic, with a message saying which
(a payload-carrying ADT receiver is **not** among them -- it used to emit
uncompilable C, fixed 2026-09-14 and pinned by
`tests/fixtures/saffron-dyn-dispatch-payload-adt`):

- A method taking more than the receiver (`eq [x : a y : a]`) or returning
  the class's own type variable (`clone : a -> a`) on an instance whose
  receiver is a **parametric** or **`:heap`** type. On a primitive or a
  non-parametric ADT receiver such a method dispatches (since 2026-09-19)
  through a per-instance witness that checked-casts each extra argument to
  the impl's parameter type and answers `any`; a mismatched extra panics at
  the cast.
- An instance whose receiver is itself a type variable
  (`definstance Clone [T]`), which has no ground tag at all.
- A higher-kinded instance whose method body is not by-value-expressible --
  one that delegates to a carrier helper, as `Functor [(Either E)]`'s `fmap`
  does through `either-map`.

A higher-kinded instance whose method body constructs its result **is**
reachable, keyed on the one instantiation a Saffron file ever builds -- the
all-`any` one -- whether its head is the bare constructor
(`definstance Functor [Option]`) or a partial application
(`definstance Functor [(Result _ B)]`): `.fmap` and `.bind` on an `any`-held
Option or Result dispatch on both back ends. A Turmeric-built `(Option float)`
handed across still has no row and panics cleanly: its elements are raw
floats, and a Saffron closure expects boxes.

## Try it

```sh
cat > hello.tur <<'EOF'
#lang saffron
(defn greet [name] (println name))
(defn main [] (greet "world") 0)
EOF
tur run hello.tur
```

## See also

- [introducing-saffron.md](introducing-saffron.md) -- the tour this guide is the
  reference for: Try Turmeric, printing, flow control, functions, ADTs,
  typeclasses and effects, every example in both syntaxes
- [docs/archive/saffron-lang-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/saffron-lang-plan.md) --
  the design decisions and their measurements
- [union-intersection-types-guide.md](union-intersection-types-guide.md) --
  `any`, unions, and gradual typing in typed Turmeric
- [syntax-guide.md](syntax-guide.md) -- `#lang` bases and layers
- `tests/fixtures/docs-saffron-guide-examples` -- every example above,
  compiled and run by both suites

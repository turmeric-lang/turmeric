---
title: Lens Guide
category: Functional Patterns
description: First-class functional lenses (view / set / over) via stdlib/lens.tur, which ships the profunctor-by-record encoding. The classic van Laarhoven form is also expressible by hand.
---

# Lens Guide

A **lens** is a first-class getter/setter for a part `A` of a whole `S`. It lets
you read, replace, and transform a nested field without hand-writing the
rebuild-the-whole boilerplate at every use site. Turmeric ships lenses in
[`stdlib/lens.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/lens.tur).

```turmeric
(load "stdlib/lens.tur")

(defstruct Point :copy :heap [x : int y : int])

(defn point-x [] : (Lens Point int)
  (lens
    (fn [p : Point] : int (.x p))
    (fn [nx : int p : Point] : Point (make-struct Point :x nx :y (.y p)))))

(defn main [] : int
  (let [px (point-x)
        p  (make-struct Point :x 3 :y 9)]
    (println (view px p))                                     ; 3
    (println (.x (set px 42 p)))                              ; 42
    (println (.x (over px (fn [v : int] : int (* v 10)) p)))) ; 30
  0)
```
```sweet-exp
load "stdlib/lens.tur"

defstruct Point :copy :heap [x : int y : int]

defn point-x [] : (Lens Point int)
  lens
    (fn [p : Point] : int .x(p))
    (fn [nx : int p : Point] : Point make-struct(Point :x nx :y .y(p)))

defn main [] : int
  let [px point-x()
       p  make-struct(Point :x 3 :y 9)]
    println(view(px p))                                     ; 3
    println(.x(set(px 42 p)))                              ; 42
    println(.x(over(px (fn [v : int] : int {v * 10}) p)))   ; 30
  0
```

## The API

| Combinator | Type | Meaning |
| --- | --- | --- |
| `lens`   | `(fn [S] A) -> (fn [A S] S) -> (Lens S A)` | build a lens from a getter and a setter |
| `view`   | `(Lens S A) -> S -> A`                     | read the focused `A` |
| `set`    | `(Lens S A) -> A -> S -> S`                | replace the focused `A` |
| `over`   | `(Lens S A) -> (fn [A] A) -> S -> S`       | map a function over the focused `A` |

`view l s` is `((.lget l) s)`; `set l a s` is `((.lput l) a s)`; `over l f s`
is `set l (f (view l s)) s`.

## Value semantics

A `Lens` is `:copy`, so a single lens value can be `view`/`set`/`over`'d
repeatedly. The whole `S` a lens focuses is usually a `:copy` struct too, so it
can be reused across calls; if the setter rebuilds the record (the common case),
make it `:copy :heap` so it is pointer-carried and cheap to thread. A move-only
`S` can still be used, but only linearly (each whole consumed once).

## Which encoding does the stdlib use?

**`stdlib/lens.tur` uses the profunctor-by-record encoding.** A `Lens S A` is a
plain record holding two functions -- a getter `(fn [S] A)` and a setter
`(fn [A S] S)`:

```turmeric
(defstruct Lens :copy [S A] (lget (fn [S] A)) (lput (fn [A S] S)))
```
```sweet-exp
defstruct Lens :copy [S A] (lget (fn [S] A)) (lput (fn [A S] S))
```

`view`, `set`, and `over` just project those two fields and call them. There is
no functor, no `forall`, and no runtime dictionary involved. This is the form
you get from `(load "stdlib/lens.tur")` and the form every example above uses.

The stdlib does **not** use the van Laarhoven encoding (below). If you only want
to read and update nested fields, you never need to think about van Laarhoven at
all -- the record form is the whole story.

## The van Laarhoven form (optional, hand-written)

The classic Haskell optic encodes a lens as a single polymorphic function:

```
type Lens s a = forall f. Functor f => (a -> f a) -> (s -> f s)
```

Turmeric can express this form -- the kind, constraint, and higher-rank
machinery it needs, plus the runtime typeclass-dictionary passing it relies on,
are all part of the language. A van Laarhoven lens carries the caller's
`Functor` dictionary through the poly carrier at runtime, so its body dispatches
`fmap` on whichever instance the caller picks (`Const` for `view`, `Identity`
for `set`/`over`).

It is **not** what `stdlib/lens.tur` ships, and you write it by hand when you
want it. The one thing it buys you over the record form is composition (below).

## Composition

The record form gives up composing optics with ordinary function composition. A
generic `lens-compose` would need its setter to read the whole `s` (to view the
intermediate part) *and* write it back -- using `s` twice -- which the linearity
checker rejects for an abstract move-only `S`. Compose by hand at concrete,
copyable whole types:

```turmeric
;; Line -> start:Point -> x:int
(defn line-start-x [] : (Lens Line int)
  (lens
    (fn [l : Line] : int (.x (.start l)))
    (fn [nx : int l : Line] : Line
      (make-struct Line
        :start (make-struct Point :x nx :y (.y (.start l)))
        :end   (.end l)))))
```
```sweet-exp
;; Line -> start:Point -> x:int
defn line-start-x [] : (Lens Line int)
  lens
    (fn [l : Line] : int .x(.start(l)))
    (fn [nx : int l : Line] : Line
       make-struct(Line
         :start make-struct(Point :x nx :y .y(.start(l)))
         :end   .end(l)))
```

where the whole types (`Line`, `Point`) are `:copy` so `l`/`s` can be used more
than once.

The van Laarhoven form composes freely (`l1 . l2` is just function composition);
reach for it when you need optic composition and don't want to hand-write the
combined getter/setter.

## Functor width (van Laarhoven internals)

This section only matters if you write van Laarhoven lenses. The record form
does not thread a functor and can ignore it.

A van Laarhoven lens threads `(f a)` through a one-int64 poly carrier. Two shapes
of functor work through that carrier:

- **Carrier-compatible functors** -- an opaque or `:heap` type whose `(f a)`
  fits in one int64 word (e.g. `(defopaque Const [r a] :int)`,
  `(defopaque Identity [a] :int)`). These ride the carrier directly at no
  runtime cost beyond the dict dispatch.
- **Wide by-value functors** -- a `:copy` struct or flat-product ADT whose
  `(f a)` is wider than one word. Codegen boxes the aggregate into the carrier at
  each lens crossing and unboxes it on the other side. `view`/`set`/`over`,
  generic focus inference, and composition all thread through unchanged; the box
  pays one heap alloc + copy + free per crossing.

Where the compiler can prove a lens call site **statically and uniquely**
resolves to a *simple* lens (one with a single `fmap` dispatch at its body tail),
it redirects the call to a monomorphized by-value body with **no heap box** on
either the `(f S)` or `(f A)` result. A consumer lens param that resolves to
several distinct simple lenses gets one box-free clone per lens. Composed
lenses (a body that tails into *another lens*, e.g.
`line-a-x = line-a . point-x`) also thread `(f a)` by value end to end --
no carrier box at any composition crossing (see
[van-laarhoven-composed-byvalue-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/van-laarhoven-composed-byvalue-plan.md)).
A few shapes still ride the boxed carrier bridge as a correctness backstop:

- **Runtime-selected lenses** -- a lens chosen at run time (not a named-function
  argument) has no static resolution, so there is nothing to redirect. The
  same applies to a runtime-selected *nested* lens inside a composition.
- **Unloweable compositions** -- a composed body whose tail is not a lens
  application or `fmap` dispatch, one missing its adapter, or nesting past
  the lowering depth cap.

## Related

- [`stdlib/lens.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/lens.tur) -- the module (record encoding)
- [constrained-hkt-forall plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/constrained-hkt-forall-plan.md) --
  the van Laarhoven roadmap and the mode-A/mode-B decision
- [constrained-hkt-forall mode-B plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/constrained-hkt-forall-mode-b-plan.md) --
  the dictionary passing + dispatch the van Laarhoven form runs on
- [van-laarhoven-wide-functor-carrier-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/van-laarhoven-wide-functor-carrier-plan.md) --
  the wide-by-value functor carrier bridge
- [van-laarhoven-monomorphization-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/van-laarhoven-monomorphization-plan.md) --
  the zero-overhead by-value monomorphization
- [van-laarhoven-consumer-mono-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/van-laarhoven-consumer-mono-plan.md) --
  consumer monomorphization: box-free clones for a lens param resolving to
  several simple lenses
- [van-laarhoven-composed-byvalue-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/van-laarhoven-composed-byvalue-plan.md) --
  by-value propagation for composed lenses (CB1-CB5)
- [hrt-guide.md](hrt-guide.md) -- the rank-2 `forall` mechanism lenses use

---
title: Ownership Guide
category: Compiler Internals
description: Which ownership strategy to reach for -- persistent-immutable, single-owner mutable, linear/affine handles, rc<T> for genuine sharing, and weak<T> to break the resulting cycles
---

# Ownership Guide

How to decide who owns what. This is design guidance for stdlib and spice
authors, and it is descriptive before it is prescriptive: it writes down the
rule the standard library already follows.

For the machinery underneath -- reference counting, the Bacon-Rajan cycle
collector, arenas -- see [gc-guide.md](gc-guide.md). This guide is about which
of them to pick.

## The rule, in one line

**Persistent-immutable first; linear/affine handles for single-owner resources;
`rc<T>` only for genuine shared ownership; `weak<T>` to break any cycle that
sharing creates.**

Each step down that list buys you something and costs you something. Take the
first one that fits, not the most general one.

## The decision table

| If the value is ... | Reach for | Why |
|---|---|---|
| A collection or tree read from several places, updated by producing a new version | **Persistent-immutable** -- `hamt`, `map`, `set`, `list`, `string` | Sharing is structural and refcounted at the C layer. An immutable DAG has no mutable back-edges, so no cycle can form. |
| A buffer one piece of code mutates and nobody else holds | **By-value / single-owner mutable** -- `vec`, `mutmap`, `grid`, `ref`, `sized-*` | The owner owns the one buffer it mutates. No shared handles, so no back-pointers into shared nodes. |
| An external resource with exactly-once teardown -- a socket, channel, task group, file | **Linear / affine opaque handle** -- `(defopaque H :linear)` / `:affine` | Single ownership and exactly-once teardown enforced by the type checker. This is the deliberate alternative to refcounting. |
| Genuinely shared: several live owners, none of which is "the" owner, and the last one out must clean up | **`rc<T>`** | Runtime refcount. Prompt, deterministic release when the last strong handle goes. |
| A back-edge, parent pointer, or observer inside an `rc<T>` graph | **`weak<T>`** ([stdlib/weak.tur](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/weak.tur)) | Holds no strong count, so it cannot close a cycle. The forward edges alone decide the lifetime. |

## Where the stdlib actually sits

**The standard library builds no `rc<T>` graphs at all** -- cyclic or
otherwise -- and therefore needs
`weak<T>` nowhere. `rc<T>` appears only in `stdlib/rc.tur` (the module that
defines it), the opt-in `stdlib/rcchain.tur`, and the generated
`stdlib/docstrings.tur`.

That is not because cycles were carefully broken. It is because the library
almost never takes the fourth row of the table: it lives in the first three.
Relative to Rust's ideal -- "use `Rc`/`Arc` only when ownership is genuinely
shared, and break every cycle with `Weak`" -- the stdlib clears the bar by
rarely needing shared ownership in the first place.

The property is protected by a tripwire rather than left to vigilance: the
`tur_stdlib_no_rc_cycles` ctest (`tests/check-stdlib-no-rc-cycles.sh`) fails if
a stdlib type annotation introduces `rc<...>` without an explicit
`rc-cycle-ok` review marker. See the "Ownership across the stdlib" section of
[gc-guide.md](gc-guide.md) for the full finding, including the one reviewed
exception (`stdlib/rcchain.tur`).

## The smell: an `rc<T>` that is not pulling its weight

`rc<T>` is not free. It costs a control-block allocation per value, an
increment and a decrement per handle, and -- this is the part that bites -- it
is the only ownership strategy in the table that can leak, because it is the
only one that can form a cycle.

So it has to earn its place. Ask whether the `rc<T>` you are about to write is
really carrying shared ownership, or whether it is standing in for something
cheaper:

- **Could it be by-value?** If every use site reads the value and none outlives
  the others, pass the value. A refcount that is only ever 1 is a control block
  and two atomic-shaped operations bought for nothing.
- **Could it be a borrow?** If the callee only reads during the call, take
  `^borrow`. A clone-into-the-callee turns a read into an ownership event.
- **Could it be persistent?** If the "sharing" is several readers over a
  structure that is updated by replacement, a `map`/`list`/`set` already gives
  you sharing -- refcounted below the language, with no cycle risk above it.
- **Is there really no single owner?** Parent/child, owner/observer, and
  registry/entry all *look* shared and usually are not: one side owns, the
  other observes. That is `rc<T>` one way and `weak<T>` the other, not `rc<T>`
  both ways.
- **Is it a resource rather than a value?** A socket or a task group wants
  exactly-once teardown, which is what `:linear` / `:affine` gives you. A
  refcount gets you teardown at an unpredictable last-drop instead.

If none of those apply, you have genuine shared ownership -- use `rc<T>`, and
read on.

## Reassigning an `rc<T>` binding with `set!`

`set!` on an `^mut` binding that holds an `rc<T>` **releases the value it
overwrites**, so the idiomatic reassign-in-a-loop does not leak. What decides
whether you get that for free is the ownership of the value going *in*:

- A fresh `(rc/of ...)` or an explicit `(rc/clone x)` carries its own `+1`.
- A bare **variable** carries one too -- the elaborator treats it as a move and
  suppresses the source binding's auto-drop.
- A bare rc **field read** (`(set! h (.next h))`) carries nothing of its own, so
  it is wrapped in a clone-on-read -- the same treatment a `let` initializer
  already gets for the same borrow shape.

The new value is fully evaluated before the old one is released, so a `set!`
whose value reads the very slot being overwritten is safe, and
`(set! h (rc/of ... (rc/clone h) ...))` takes its `+1` while the old value is
still alive. The release is gated on exactly the scope-exit auto-drop predicate,
so the two can never disagree: a binding whose ownership you manage by hand gets
neither.

`set!` of an `rc<T>` through a `&mut` borrow never reaches codegen -- the type
checker rejects it (`set! type mismatch: cannot assign rc<...> through
&mut rc<?> borrow`).

**Two shapes still leak, deliberately.** Both suppress the auto-drop entirely,
and releasing would be worse than leaking:

- `(set! h h)` -- self-assignment lowers to `h = h` with no auto-drop; a release
  would leave the binding dangling.
- `(rc/drop h)` then `(set! h v)` -- the explicit drop already suppressed the
  auto-drop, so `v` is never released. Releasing the old value here would
  double-free instead. A genuine (if unusual) remaining gap.

## Breaking the cycle

Two `rc<T>` values pointing at each other is the textbook leak: each keeps the
other's strong count above zero, so neither is ever reclaimed. Turmeric gives
you two ways out, and they are not equivalent.

**Prefer `weak<T>`.** Give the back-edge to a weak reference. It holds no
strong count, so the forward edges alone decide the lifetime, and the whole
structure is reclaimed the moment the last strong handle goes -- promptly,
deterministically, with the collector off.

```turmeric
(load "stdlib/weak.tur")

;; parent --.child--> child   (strong: the parent owns the child)
;; child  --.parent-> parent  (weak:   the back-edge owns nothing)
(defstruct Node :move [tag : int child : rc<Node> parent : weak<Node>])

(defn build-pair [] : int
  (let [child  (rc/of (make-struct Node 2 (null-child) (null-parent)))
        parent (rc/of (make-struct Node 1 (rc/clone child) (null-parent)))]
    (set! (.parent child) (rc/downgrade parent))
    0))
```
```sweet-exp
load "stdlib/weak.tur"

;; parent --.child--> child   (strong: the parent owns the child)
;; child  --.parent-> parent  (weak:   the back-edge owns nothing)
defstruct Node :move [tag : int child : rc<Node> parent : weak<Node>]

defn build-pair [] : int
  let [child  rc/of(make-struct(Node 2 null-child() null-parent()))
        parent rc/of(make-struct(Node 1 rc/clone(child) null-parent()))]
    set!(.parent(child) rc/downgrade(parent))
    0
```

Reading through the back-edge goes via `weak/upgrade`, which is what makes weak
observation safe rather than merely cheap -- there is no way to read without
first asking whether the value is still there:

```turmeric
(let [o (weak/upgrade w)]
  (if (some? o)
    (let [s (weak/unwrap o)]      ; a NEW strong handle -- ours to release
      (rc/drop s))
    (println "gone")))
```
```sweet-exp
let [o weak/upgrade(w)]
  if some?(o)
    let [s weak/unwrap(o)]      ; a NEW strong handle -- ours to release
      rc/drop(s)
    println("gone")
```

The full surface is `rc/downgrade`, `weak/upgrade`, `weak/unwrap`,
`weak/alive?`, and `weak/drop`, documented in
[stdlib/weak.tur](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/weak.tur). Two rules are easy to miss:

- The rc handed back by `weak/upgrade` is a **new** strong reference. Drop it.
- A `weak<T>` in a local binding is **not** released at scope exit -- call
  `weak/drop`. A `weak<T>` stored in a struct field *is* released for you, by
  the owning value's drop glue.

**The cycle collector is the fallback, not the plan.** `(gc-enable!)` plus
`(gc!)` will reclaim a strong cycle (see `tests/fixtures/gc-collects-strong-cycle`),
but it is opt-in, manually driven, and only sees what the walker sees -- a cycle
routed through an opaque handle or a flat element buffer is invisible to it.
Reach for it when the cycles are genuinely unstructured; reach for `weak<T>`
when you can name which edge is the back-edge, which is most of the time.

## Related

- [gc-guide.md](gc-guide.md) -- reference counting, the cycle collector, and the
  stdlib ownership audit
- [stdlib/weak.tur](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/weak.tur) -- the `weak<T>` API
- [stdlib/rcchain.tur](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/rcchain.tur) -- a collection of `rc<A>` the
  cycle collector can trace through, for when the elements can cycle

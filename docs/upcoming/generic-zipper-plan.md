# Plan: Generic zipper (`stdlib/zip.tur`)

> **Status:** Proposed 2026-10-13. Not started. The core design was
> prototyped against `main` at v0.64.4 (`704cc531`) and runs, both compiled
> (gcc 13 and clang 18, no warnings) and under `tur interpret`. See
> [Appendix A](#appendix-a----the-zp1-prototype-verified-2026-10-13).
> **Type:** Stdlib (pure Turmeric, no inline C), plus five compiler defects
> found while prototyping it (section 5).
> **Track:** Additive and v1-compatible. ZP1, ZP2 and ZP4 land on today's
> compiler. ZP3 waits on one emitter fix (D4).
> **Builds on:** `stdlib/list.tur`'s `:heap` `(Cons A)`, `stdlib/lens.tur`'s
> record encoding of optics, and regions
> ([gc-guide](../guides/gc-guide.md), [memory-usage-guide](../guides/memory-usage-guide.md)).

## 0. Summary

Turmeric's only zipper today, `stdlib/zipper.tur`, is a one-dimensional
cursor over an array of `int64` words. This plan adds a zipper that works
over **any tree-shaped type**, and a cursor over **the elements of any
container**. Those are two different things that both get called a "generic
zipper":

1. **The structural zipper, `(ZipLoc T)`.** This is Clojure's
   `clojure.zip`, typed. You supply three functions for a node type `T`:
   `branch?`, `children` and `make-node`. In return you get `down`, `up`,
   `left`, `right`, `next`, `prev`, `replace`, `edit`, `insert-*`, `remove`
   and `root` over any homogeneous recursive type: ASTs, rose trees, binary
   trees, scene graphs, menus. Haskell's version of the same idea is
   Uniplate's `Data.Generics.Uniplate.Zipper` (and lens's `Plated`), so the
   typeclass layer (ZP3) uses that shape.
2. **The element cursor, `(ZipCursor S A)`.** This is Haskell's "zipper over
   any `Traversable`" (Kiselyov), built the eager way, like lens's `partsOf`
   (Jay's shape and contents). It walks the `A`-elements of any container
   `S`: a tree's labels, a list, a record's fields. You edit them in place,
   then close the cursor back into an `S` of the same shape.

**Recommendation:**

- Use Clojure's design for the core. It is the only one that fits the type
  system today, and the prototype runs.
- Add Uniplate/`Plated` as typeclass sugar once defect D4 is fixed.
- Build the element cursor from a record-encoded `Traversal`.
- Defer the derivative-of-a-type approaches (McBride, `syz`) and Kmett's
  typed level stacks (section 2).

Every location is an immutable value. Moving or editing returns a new
location and leaves the old one valid. Unedited subtrees are shared rather
than copied, and `(zip/root (zip/zipper ops t))` returns `t` itself, not a
rebuilt copy.

## 1. What exists today (verified 2026-10-13)

- **`stdlib/zipper.tur`** is auto-loaded. Its `(defopaque Zipper [A] :int)`
  is a handle to a malloc'd `{int64_t *left; size_t left_len; int64_t focus;
  int64_t *right; size_t right_len;}`.
  - Each move mallocs a new struct and **memcpys both arrays**, so a move
    costs O(n), not the O(1) a zipper is for.
  - Elements are erased to a word: `(:: focus :int)`.
  - The caller must call `zipper-free` on every zipper.
  - Users: the cellular-automata comonad tutorial, `zipper-basic`,
    `typed/zipper-basic` and `zipper-comonad`. The comonad fixture
    reimplements the struct in its own inline C.
  - This plan leaves it alone until ZP5.
- **Typeclasses.** `Functor`, `Foldable` and `Monad` are typed. `Traversable`
  is still the v1 stub `(traverse [ta fn] : int)`, and `Comonad` is
  `:int`-typed. So neither can carry a generic zipper yet.
- **`(Cons A)`** (`stdlib/list.tur`) is a `:heap` typed persistent list whose
  nil is the NULL link. It is the natural spine for siblings and for the
  path.
- **`:heap :copy`** `defstruct`/`defdata` values are shared and freely
  aliased. Only a region reclaims them. Persistent structural sharing needs
  exactly this. A `:move` (default) ADT rejects the aliasing a zipper does on
  every step (`TUR-E0201`).
- **Effects.** A handler's `k` is one-shot by default; `^multishot` is opt-in
  per case. Generators cannot recurse (generators-guide, "Limitations").
- **`stdlib/lens.tur`** ships optics as records (`(Lens S A)` = getter +
  setter), because the van Laarhoven form needs dictionary passing. That
  form is expressible but hand-written. ZP4 follows the record precedent.

## 2. Survey: generic zippers in Clojure and Haskell

| Approach | What it navigates | Generic over | Fits Turmeric today? | Verdict |
|---|---|---|---|---|
| Huet (1997), hand-written per type | one type's one-hole contexts | nothing -- written per type | yes | What users write today. This plan removes the need. |
| **Clojure `clojure.zip`** | sub-nodes of one node type | any homogeneous tree, via `branch?`/`children`/`make-node` | **yes** -- prototyped | **ZP1 core** |
| **Uniplate `...Uniplate.Zipper`; lens `Plated`** | the same | any type with an instance | after **D4** | **ZP3**: typeclass sugar over ZP1 |
| Kiselyov, zipper from any `Traversable` (delimited continuations) | elements, in traversal order | any traversable | partly: `k` is one-shot, so the cursor is forward-only and affine | deferred (ZP-D3) |
| **`partsOf` / Jay's shapely types (shape + contents)** | elements, both directions, persistent | any container with a traversal | **yes** -- prototyped | **ZP4 element cursor** |
| McBride, *The Derivative of a Regular Type is its Type of One-Hole Contexts*; `syz` (Adams, *Scrap Your Zippers*); generic-deriving | every field position, heterogeneous | any ADT | no generic reflection; would need a compiler `derive` | deferred (ZP-D1) |
| Kmett `zippers` (`Top :>> a :>> b` over lenses) | heterogeneous levels, typed | any lens/traversal chain | no type-level stacks | rejected for now (ZP-D2) |

Why Clojure's design rather than a "more Haskell" one:

- **It is the only one that needs nothing new from the type system.** It is
  first-order: a record of three closures over one type `T`.
- **Homogeneity is the real cost, and it is usually acceptable.** The type
  of every node must be the same `T`. Clojure gets heterogeneity for free
  because it is untyped. In a typed language, the usual answer is to
  navigate a sum type: an AST whose nodes are one `defdata`. Uniplate makes
  the same trade.
- **The ops record allows several views of one type.** Clojure ships
  `seq-zip` and `vector-zip` over overlapping data. A typeclass allows one
  instance per type, so in Haskell a second view needs a newtype. The record
  comes first; the class (ZP3) is a convenience that builds the record.
- **The Haskell solution worth borrowing is for the other problem.** That is
  the element cursor: "visit every `A` inside an `S`". Kiselyov's
  continuation zipper is elegant but forward-only here. The eager
  shape-and-contents version gives the same API, both directions, and
  persistence (section 3.2).

## 3. Design

### 3.1 The structural zipper

```turmeric
(defstruct ZipOps :heap :copy [T]
  (branch?   (fn [T] bool))            ; can this node have children?
  (children  (fn [T] (Cons T)))        ; its children, in order
  (make-node (fn [T (Cons T)] T)))     ; rebuild it with new children

(defstruct ZipFrame :heap :copy [T]    ; one level of the path (internal)
  (lefts   (Cons T))                   ; left siblings, NEAREST FIRST
  (pnode   T)                          ; the parent as it was on the way down
  (up      (ZipFrame T))               ; next frame out; the NULL link at the root
  (rights  (Cons T))                   ; right siblings, nearest first
  (changed bool))                      ; has anything below this frame been edited?

(defstruct ZipLoc :heap :copy [T]
  (node T)
  (path (ZipFrame T))                  ; the NULL link at the root
  (end  bool)                          ; Clojure's :end marker, for next/end?
  (ops  (ZipOps T)))
```

Each choice, and why:

- **`pnode` plus `changed`, as in Clojure.** `up` rebuilds the parent with
  `make-node` only when something below it was edited. Otherwise it returns
  the stored parent unchanged. So a walk with no edits allocates no nodes,
  unedited subtrees are shared between versions, and pointer identity
  survives a round trip. An edit marks its own frame; `up` passes the mark
  to the next frame out.
- **`lefts` is reversed** (nearest sibling first), so `left` and `right` are
  O(1) `Cons` operations. `up` and `lefts` reverse it when they need
  document order.
- **The path link is a nullable typed pointer, not a `defdata` and not an
  `Option`.** The obvious encodings are a sum `(Top) | (Frame ...)` or a
  field `(Option (ZipFrame T))`. Each hits an open codegen defect when a
  generic body reads or rebuilds it (D1, D2, D3 in section 5). The nullable
  link is what `(Cons A)` itself uses for nil, it compiles warning-clean, and
  regions do not note its null test. When D1--D3 are fixed this can become a
  sum; the public API does not expose it either way.
- **`ZipOps` is `:heap :copy`.** Each location then carries the ops as one
  word instead of three closures. It also avoids defect D5, seen when a
  by-value ops record was built inside a constrained function.
- **`end` is a flag on the location.** `next` past the last node returns a
  location at the root with `end = true`, which `end?` tests. This keeps
  Clojure's standard loop working unchanged:

  ```turmeric
  (defn edit-all [T] [z : (ZipLoc T) f : (fn [T] T)] : T
    (if (zip/end? z)
      (zip/root z)
      (edit-all (zip/next (zip/edit z f)) f)))
  ```

#### API: Clojure name -> Turmeric signature

Moves that Clojure answers with `nil` at a boundary return `Option`. Edits
that Clojure throws on panic with Clojure's own message. `branch?` and
`up` let a caller check first.

| Clojure | Turmeric | Boundary behaviour |
|---|---|---|
| `zipper` | `(zip/zipper [T] [ops : (ZipOps T) root : T] : (ZipLoc T))` | -- |
| -- | `(zip/ops [T] [branch? children make-node] : (ZipOps T))` | -- |
| `node` | `(zip/node z) : T` | -- |
| `branch?` | `(zip/branch? z) : bool` | -- |
| `children` | `(zip/children z) : (Cons T)` | panics "called children on a leaf node" |
| `make-node` | `(zip/make-node z node kids) : T` | -- |
| `path` | `(zip/ancestors z) : (Cons T)` (root first) | `(tnil)` at the root |
| `lefts` / `rights` | `(zip/lefts z)` / `(zip/rights z) : (Cons T)`, document order | `(tnil)` at the root |
| `down` / `up` | `(zip/down z)` / `(zip/up z) : (Option (ZipLoc T))` | `None` on a leaf or an empty branch / at the root |
| `left` / `right` | `(zip/left z)` / `(zip/right z) : (Option (ZipLoc T))` | `None` at an end or at the root |
| `leftmost` / `rightmost` | `(zip/leftmost z)` / `(zip/rightmost z) : (ZipLoc T)` | returns `z` when already there |
| `root` | `(zip/root z) : T` | rebuilds every changed level |
| `next` | `(zip/next z) : (ZipLoc T)` | pre-order; past the last node, an `end` location at the root |
| `prev` | `(zip/prev z) : (Option (ZipLoc T))` | `None` at the root |
| `end?` | `(zip/end? z) : bool` | -- |
| `replace` | `(zip/replace z node) : (ZipLoc T)` | -- |
| `edit` | `(zip/edit z (fn [T] T)) : (ZipLoc T)` | -- |
| `insert-left` / `insert-right` | `(zip/insert-left z node)` / `(zip/insert-right z node) : (ZipLoc T)` | panics "Insert at top" |
| `insert-child` / `append-child` | `(zip/insert-child z node)` / `(zip/append-child z node) : (ZipLoc T)` | panics "called children on a leaf node" |
| `remove` | `(zip/remove z) : (ZipLoc T)` -- the pre-order predecessor | panics "Remove at top" |

Clojure's `seq-zip`, `vector-zip` and `xml-zip` are covered in section 3.3.
`zip/depth : int` is the one addition in ZP1, and a count is a legitimate
`:int`. Section 4 lists the ZP2 additions.

#### Cost

`k` is the number of children of the node involved; `d` is the depth.

| Operation | Cost |
|---|---|
| `right`, `left`, `replace`, `edit`, `insert-left`/`insert-right` | O(1) |
| `down` | one `children` call (O(1) when children are stored as a `Cons`) |
| `up`, unchanged | O(1), no allocation |
| `up`, changed | O(k): reverse the lefts and call `make-node` |
| `leftmost`, `rightmost` | O(k) |
| `root` | O(d) unchanged; O(d * k) after edits |
| `next` / `prev` | amortised O(1) per node over a full walk |

### 3.2 The element cursor

```turmeric
(defstruct Traversal :copy [S A]
  (parts (fn [S] (Cons A)))          ; the A-elements of an S, in order
  (fill  (fn [S (Cons A)] S)))       ; the same S with its elements replaced, in order

(defstruct ZipCursor :heap :copy [S A]
  (shape S)                          ; the container, as opened
  (lefts (Cons A))                   ; elements before the focus, nearest first
  (focus A)
  (rights (Cons A))
  (trav (Traversal S A)))
```

- **Laws** (the record mirrors `stdlib/lens.tur`'s `Lens`):
  - `(fill s (parts s)) = s`
  - `(parts (fill s xs)) = xs`, when `xs` has as many elements as
    `(parts s)`
- **API.** `zip/cursor` returns `(Option (ZipCursor S A))`, `None` when there
  are no elements. Then:
  - `zip/cursor-next` and `zip/cursor-prev` move, returning `Option`.
  - `zip/cursor-focus`, `zip/cursor-set` and `zip/cursor-edit` read and
    replace the focus.
  - `zip/cursor-close : S` writes the elements back.
- **Cost.** Opening and closing are O(n); every move and edit is O(1). Old
  cursors stay valid.
- **Why a record, not a class.** In Haskell this is
  `Traversable t => t a -> Zipper`. Here `Traversable` is an `:int` stub,
  and a higher-kinded `Shapely [^t]` class whose methods return `(Cons a)`
  compiles to a program that aborts at runtime (D2: "a call reached the
  dead base generic path").
  Once `Traversable` is typed, `traverse` with a state applicative (that is,
  `mapAccumL`) is exactly what is needed to derive a `Traversal`. Then
  `zip/traversal-of` can build the record from an instance, as ZP3 does for
  `ZipOps`.
- **Why not Kiselyov's continuation zipper.** That zipper suspends the
  traversal at each element (`perform Visit`) and keeps `k`. With one-shot
  `k` the cursor is affine and forward-only: you cannot keep two positions
  or step back. Going backwards would need every case to be `^multishot`,
  or re-running the traversal. It wins only when the traversal is lazy,
  infinite or effectful. That is real, but not v1 (ZP-D3).
- **A list is the trivial case.** `zip/cons-traversal` has `parts` as the
  identity and `fill` returning the new list. A `ZipCursor` over it **is** a
  persistent O(1) list zipper. That is the list zipper ZP5 can rebase
  `stdlib/zipper.tur` onto.

### 3.3 Stock instances

- **`(Rose A)` with `zip/rose-ops` and `zip/rose-zip`.**
  `(defstruct Rose :heap :copy [A] (label A) (kids (Cons (Rose A))))` is
  Haskell's `Data.Tree` and the typed counterpart of `seq-zip`. Its `kids`
  is a `Cons`, so `down` is O(1). `zip/rose-labels` is its element
  `Traversal`. The prototype used a monomorphic `Rose`; ZP2 confirms the
  parametric one. It is a `defstruct` rather than a `defdata` so that
  D1/D2 cannot arise.
- **Not shipped: `vector-zip`.** Clojure's `vector-zip` navigates untyped
  nested vectors. The typed counterpart is a user ADT, and the stdlib `Vec`
  is mutable, so `make-node` would have to allocate a new `Vec` per rebuilt
  level.
- **Not shipped: `json-zip`.** It is the natural counterpart of `xml-zip`,
  but `stdlib/json.tur` nodes are mutable C trees that own their children,
  so a persistent `make-node` would alias owned children. Revisit if JSON
  gains a persistent node type.

### 3.4 Memory

Every location, frame, rebuilt node and sibling list is a `:heap` node, as
`(Cons A)` is:

- **Outside a region,** they live until the process exits.
- **Inside `with-region`,** a session whose result is a scalar (a count, a
  sum, a found index) rewinds in one step.
- **A session that returns the edited tree** cannot rewind. The result
  reaches into the generation, so the generation is retired. That is
  correct; it just saves nothing.

There is no `zip/free`, unlike `zipper-free`; a zipper does not need one.
The module contains **no inline C**, so the region store-hook rule (CLAUDE.md)
is met by construction: every store is a compiler-emitted constructor, and
those are already hooked. Any later inline-C fast path (for example, an
in-place `rev-onto`) must call `TUR_REGION_NOTE` on every word it stores.

### 3.5 Naming, loading, gating

- **File.** `stdlib/zip.tur`, loaded with `(load "stdlib/zip.tur")`. It is
  **not** auto-loaded. Every function carries the `zip/` prefix, as `hamt/`
  does, so `zip/down` reads exactly like Clojure's `(zip/down loc)`. Bare
  `up`, `down`, `next`, `remove` and `replace` would collide everywhere.
- **Type names.** `ZipLoc`, `ZipOps`, `ZipFrame`, `ZipCursor`, `Traversal`
  and `Rose` were all unused in `stdlib/` on 2026-10-13. `Zipper` stays with
  `zipper.tur`.
- **Not `lens.tur`.** `zip.tur` must not load `lens.tur`, because that would
  pull `set`, `view` and `over` into every user's namespace. `Traversal`
  therefore lives in `zip.tur` (see section 7, Q1).
- **No `--enable` gate.** The experimental-flag rule covers in-flight
  *compiler* features. This plan adds a stdlib module. The deferred `derive`
  (ZP-D1) is a compiler feature, and would get an `EXPERIMENTS[]` row and
  its own plan.

## 4. Phases

Each phase lands on its own, in its own PR against `main`.

### ZP0 -- compiler defects (in parallel; ZP1, ZP2 and ZP4 do not wait)

Fix D1--D5 and I1--I4 from section 5, each pinned by its repro as a fixture.
Only **D4** blocks a phase here (ZP3). D1--D3 block the nicer internal
encodings, which ZP1 avoids. The I-items each cost an ascription inside
`zip.tur`. Fixing them removes those ascriptions, and the same papercuts
also hit users writing their own `ZipOps`.

### ZP1 -- core structural zipper

- **Code.** `stdlib/zip.tur` with the types in section 3.1 and every row of
  the API table, with full `;;;` docstrings. Pure Turmeric. Add it to
  Bucket B of `tests/run-stdlib-checks.sh`, then regenerate `docs/api/` and
  `stdlib/docstrings.tur` (`tur run docs`).
- **Fixtures.** Each one is a minimal `Rose`-like `defdata` in the fixture
  itself (ZP2 adds the stock one). All run under both `tests/run.sh` and
  `tests/run-turi.sh`; none contain inline C, so `run-turi.sh` runs them
  rather than skipping them.
  - `zip-navigate` -- `down`/`right`/`left`/`up`/`leftmost`/`rightmost`,
    including every `None` boundary.
  - `zip-edit-root` -- `replace`/`edit` deep in the tree, then `root`. The
    *old* root is unchanged afterwards (persistence).
  - `zip-identity-sharing` -- `(root (zipper ops t))` and a
    down/right/up/up round trip return `t` itself, compared as pointers.
    This pins the `changed` optimisation.
  - `zip-next-walk` -- a pre-order walk with `next` until `end?` visits
    every node once in pre-order, and `edit-all` rewrites them all. `prev`
    walks back.
  - `zip-insert-remove` -- `insert-left`/`insert-right`/`insert-child`/
    `append-child`, and `remove` returning Clojure's pre-order predecessor.
    The expected trees are the ones Clojure gives for the same edits.
  - `errors/zip-remove-at-top`, `errors/zip-insert-at-top`,
    `errors/zip-children-of-leaf` -- the panics and their messages.
- **Acceptance.** Every fixture passes on gcc. The same programs also
  compile under `CC=clang` with no errors, because `run.sh` treats a
  `-Wint-conversion` as a failure and the macOS leg uses clang.

### ZP2 -- `Rose`, search, bulk rewrite

- **Code.**
  - `(Rose A)`, `zip/rose-ops`, `zip/rose-zip`.
  - `zip/find-next [T] [z : (ZipLoc T) p : (fn [T] bool)] : (Option (ZipLoc T))`:
    pre-order search from `z`.
  - `zip/edit-all [T] [z : (ZipLoc T) f : (fn [T] T)] : T`: the Clojure loop
    above, shipped. It is the zipper form of Uniplate's `transform`.
- **Fixtures.** `zip-rose` with a parametric `Rose` over `int` and over a
  struct, `zip-find-next`, and `zip-edit-all-ast`. The last one
  constant-folds a small arithmetic AST `defdata` and is the "why would I
  want this" example for the guide.

### ZP3 -- `Plated` typeclass sugar (blocked on D4)

```turmeric
(defclass Plated [a]
  (plate-branch?  [x : a] : bool)
  (plate-children [x : a] : (Cons a))
  (plate-rebuild  [x : a kids : (Cons a)] : a))

(defn zip/plated [^Plated T root : T] : (ZipLoc T) ...)   ; builds the ZipOps from the instance
```

`Plated` is lens's name for "has children of its own type". It is also the
right place for Uniplate's other combinators: `universe`, `transform` and
`rewrite` need no zipper at all. Those are out of scope here but cheap to
add on top. The prototype of exactly this class compiles except for D4.
With the ops record made `:heap` (as in 3.1), D4 is the only remaining
error.

### ZP4 -- element cursor

- **Code.** `Traversal`, `ZipCursor`, the cursor API from section 3.2,
  `zip/cons-traversal` and `zip/rose-labels`.
- **Fixtures.**
  - `zip-cursor-tree`: a user binary tree's `Traversal`, step and edit,
    close. The original is unchanged afterwards.
  - `zip-cursor-list`: the `Cons` case as a list zipper.
  - `zip-cursor-empty`: `None` when there are no elements.
- The prototype of this phase (a binary tree, compiled under clang and
  interpreted) prints the expected values.

### ZP5 -- `stdlib/zipper.tur` (decision point, optional)

After ZP4, decide whether to rebase `(Zipper A)` onto a `ZipCursor` over
`(Cons A)`. That would make moves O(1) instead of O(n) and remove
`zipper-free`.

- **Constraints.** The public signatures take `ptr<void>` arrays and
  lengths, the module is auto-loaded, and the CA tutorial and
  `zipper-comonad` use it. A rebase is cheap only if those signatures stay.
- **Fallback.** If they cannot stay, add one docstring line to
  `zipper.tur` ("moves are O(n); for a persistent zipper see
  `stdlib/zip.tur`") and stop.
- **Do not churn it without a reason.** Changing the auto-loaded module
  moves `expected.c` snapshots across the suite.

### ZP6 -- documentation

- Write `docs/guides/zipper-guide.md`:
  - when to reach for a zipper versus plain recursion;
  - the three functions, `edit-all` on an AST, and the element cursor;
  - a Clojure-to-Turmeric table (section 3.1);
  - the memory note (section 3.4).
- Add a cross-link from the CA comonad tutorial.

### Deferred (each needs its own plan)

- **ZP-D1: derived one-hole contexts.** This is McBride's derivative, made
  concrete: `derive` a `TreeCtx` from a `defdata` so that navigation is
  per-field (`down-left` / `down-right` by constructor field) and
  heterogeneous field types are allowed. It is a compiler feature, so it
  would go behind `--enable`.
- **ZP-D2: heterogeneous typed paths.** This is Kmett's `zippers`. It needs
  type-level stacks, which Turmeric does not have.
- **ZP-D3: continuation cursor over lazy or effectful traversals.** This is
  Kiselyov's. A `^multishot` handler would give a persistent cursor over a
  traversal that is never materialised.
- **ZP-D4: `Comonad` for `ZipLoc`.** `extend` gives every node its context,
  which is useful for attribute-grammar-style passes. It waits on a typed
  `Comonad`.

## 5. Compiler defects found while prototyping (2026-10-13, `704cc531`)

D1--D4 are filed under `docs/reported/` (linked below), with root causes
and fix directions. D5 and the inference papercuts are recorded only here.
Every repro is complete and was run as shown.

**D1 -- generic match-and-rebuild of a parametric `:heap` sum leaves `(Cons
T)` payload binders as `int64_t`**
([heap-sum-match-binder-of-app-payload-unbridged](../archive/heap-sum-match-binder-of-app-payload-unbridged.md)).
The specialised constructor takes the typed pointer. gcc 13 warns (and `run.sh` fails on the warning); clang 15+
and gcc 14 reject it. **Fixed 2026-10-14**, pinned by
`tests/fixtures/heap-sum-match-binder-app-payload`. The *effectful* version
of the same rebuild is fixed too, the same day
([heap-sum-rebuild-in-effectful-generic-merge-temp-ice](../archive/heap-sum-rebuild-in-effectful-generic-merge-temp-ice.md)).
It was three defects, and two of them matter to this plan beyond D1. A
generic function that performs could not take a `:heap` struct or `(Vec T)`
parameter, nor perform only inside a `match` arm; both now compile, pinned
by `tests/fixtures/generic-performer-heap-handle-param`. Matching directly on
a `handle` result, or a `handle` in a non-tail branch, compiles too
([handle-under-match-in-main-has-no-lowering](../archive/handle-under-match-in-main-has-no-lowering.md)),
so a zipper walk may handle an effect mid-branch.

```turmeric
(defdata R :heap :copy (Lf int) (Br (Cons R)))
(defdata Fr :heap :copy [T] (Nil) (Frame (Cons T) T))
(defstruct Box :heap :copy [T] (fr (Fr T)))
(defn val [r : R] : int (match r (Lf v) v (Br _) 0))
(defn bump [T] [b : (Box T)] : (Box T)
  (make-struct Box :fr (match (.fr b)
                         (Nil) (.fr b)
                         (Frame l x) (Frame l x))))
(defn main [] : int
  (let [b (make-struct Box :fr (Frame (tcons-of (Lf 1) (tnil)) (Lf 7)))]
    (match (.fr (bump b))
      (Nil) (println 0)
      (Frame l x) (println (+ (val x) (val (thead l))))))
  0)
;; CC=clang tur run: error: incompatible integer to pointer conversion passing
;; 'int64_t' to parameter of type 'tur_adt_Cons__R *' [-Wint-conversion]
```

**D2 -- a generic over a parametric `:heap` sum is never specialized, so a
constructor in its body aborts at runtime**
([heap-sum-generic-never-specialized](../reported/heap-sum-generic-never-specialized.md)).
It compiles cleanly. The interpreter prints `6`, and so does the compiled
program without `:heap`.

```turmeric
(defdata Fr :heap [T] (Nil) (Frame T T))
(defn bump [T] [f : (Fr T)] : (Fr T)
  (match f
    (Nil) (Nil)
    (Frame a b) (Frame b a)))
(defn main [] : int
  (match (bump (Frame 1 7))
    (Nil) (println 0)
    (Frame a b) (println (- a b)))
  0)
;; at runtime: tur: internal error: base constructor 'ctor_Fr_Frame' of a parametric
;; type has no definition; a call reached the dead base generic path instead of a
;; per-spec clone
```

**Why.** `type_c_name` spells `(Fr T)` and `(Fr int)` alike as `int64_t`,
so `emit_abi_register_call`'s C-name comparison never mints a clone. A
higher-kinded `(defclass Shapely [^t] (contents [ta : (t a)] : (Cons a)) ...)`,
whose `Tree` instance calls generic helpers, aborts the same way
(`ctor_Cons_Cons`).

**D3 -- a by-value `(Option (S T))` field of a `:heap` struct, passed to a
generic function, is read as a carrier word**
([byvalue-option-field-read-as-carrier-at-generic-call](../archive/byvalue-option-field-read-as-carrier-at-generic-call.md)).
The emitted C is
`(int64_t)(intptr_t)(h->p)` on an aggregate, which both gcc and clang
reject. **Fixed 2026-10-14**, pinned by
`tests/fixtures/byvalue-field-of-param-app-as-generic-arg`. The fix also
covers an `(Option T)` field and a non-`Option` by-value sum field, so an
`(Option (ZipFrame T))` path link is available to this plan again. Making
the link a sum is still blocked, by D2 alone now that D1 is fixed.

```turmeric
(defstruct Fr :heap :copy [T] (x T))
(defstruct Holder :heap :copy [T] (p (Option (Fr T))))
(defn peek [T] [o : (Option (Fr T))] : bool (match o (None) false (Some _) true))
(defn probe [T] [h : (Holder T)] : bool (peek (.p h)))
(defn main [] : int
  (println (probe (make-struct Holder :p (Some (make-struct Fr :x 7)))))
  0)
;; error: aggregate value used where an integer was expected
```

**D4 -- an instance method whose declared return is `(Cons a)`, with a
`match` body, misses the pointer-to-word bridge on the fall-through
`return`**
([instance-method-match-fallthrough-return-unbridged](../reported/instance-method-match-fallthrough-return-unbridged.md)).
The arm returns are bridged; the trailing `return __tN;` after
the `switch` is not.

```turmeric
(defdata R :heap :copy (Lf int) (Br (Cons R)))
(defclass Kids [a] (kids [x : a] : (Cons a)))
(definstance Kids [R]
  (kids [r] (match r (Lf _) (tnil) (Br cs) cs)))
(defn main [] : int
  (println (tlength (kids (Br (tcons-of (Lf 1) (tnil))))))
  0)
;; CC=clang: error: incompatible pointer to integer conversion returning
;; 'tur_adt_Cons__R *' from a function with result type 'int64_t' [-Wint-conversion]
```

**D5 -- seen only in the full prototype; did not reproduce in isolation.** A
by-value `:copy` record of three closures was built inside a `^Plated T`
constrained function and passed to a generic function. It was dereferenced
as a carrier pointer: `(*(tur_adt_ZipOps__Rose *)(intptr_t)(__ps))`. A
one-closure reduction compiles correctly. Making the record `:heap :copy`
sidesteps it, and section 3.1 does that for its own reasons.

**Inference papercuts.** Each has a one-ascription workaround, used in
Appendix A.

- **I1.** `(tnil)` as an argument in a generic body, where the parameter is
  `(Cons T)`, does not take `T` from the other arguments. It defaults to
  `int` and the next argument fails with `TUR-E0001` ("expected int, got
  T"). Workaround: `(:: (tnil) (Cons T))`.
- **I2.** A polymorphic self-recursive call in a `match` arm,
  `(Some p) (zroot p)` in `(defn zroot [T] [z : (ZipLoc T)] : T ...)`, is
  typed `int`: "match: arm types are incompatible -- expected tyvar (from
  earlier arm), got int". Workaround: `(:: (zroot p) T)`.
- **I3.** A nullary generic, `(tree-traversal)` returning
  `(Traversal (Tree A) A)`, does not take `A` from the parameter type it is
  passed to. Workaround: ascribe the call.
- **I4.** Inside an instance body, `(tnil)` does not unify with the method's
  `(Cons a)` ("expected (Cons a), got (Cons A)"). Workaround: call a
  top-level generic helper.

## 6. Testing notes

- **Both harnesses.** The prototype runs identically under `tur run` and
  `tur interpret`, so every fixture belongs in `run.sh` *and* `run-turi.sh`.
  Do not add `requires.compiled`.
- **clang.** `run.sh` already fails a fixture whose emitted C raises
  `-Wint-conversion`. Under gcc 13 (the compiler in this container) D1 and
  D4 only *warn*, and `run.sh` turns that warning into a failure. Under
  clang (the macOS leg) they are hard errors. Run new fixtures locally with
  `CC=clang` before opening a PR.
- **Leaks.** Do not add `requires.leak-check`. The zipper's nodes are
  process-lifetime outside a region by design (section 3.4), so a leak check
  would only report that design.
- **Regions.** To check that a scalar-result session rewinds, do it once by
  hand with `TUR_REGION_STATS=1` and record the counts in this plan when
  ZP2 lands. It is not a fixture, because the harness does not assert
  region statistics.

## 7. Open questions (with defaults)

- **Q1. Where does `Traversal` live?** Default: in `zip.tur`. If `lens.tur`
  later grows traversals (`to-list-of`, `over-each`), move it to a small
  `stdlib/optic-traversal.tur` that both modules load. It must not go into
  `lens.tur` itself, because of `set`/`view`/`over` (section 3.5).
- **Q2. Panics, or `Option`, for `insert-*` / `remove` at the top?**
  Default: panic, matching Clojure. A boundary is an expected outcome; an
  insert at the top is a bug. Revisit if a real caller wants to branch on
  it.
- **Q3. Does `zip/children` panic on a leaf, or return `(tnil)`?** Default:
  panic, matching Clojure. Otherwise an empty branch and a leaf would be
  indistinguishable to a caller about to `append-child`.

## Appendix A -- the ZP1 prototype (verified 2026-10-13)

This is the core of the prototype, which runs on `main` at `704cc531`
(compiled under gcc 13 and clang 18 with no warnings, and under
`tur interpret`). The full program adds a `main` that builds
`(Br [(Leaf 1) (Br [(Leaf 2) (Leaf 3)])])`, moves down, right, down, and
replaces `(Leaf 2)` with `(Leaf 40)`. It prints `6`, `44`, `6`: the original
tree's sum, the sum after the edit, and the original again, unchanged. Names
are the prototype's, not the final API. The ascriptions marked I1 and I2 are
the section 5 workarounds.

```turmeric
(defstruct ZipOps :copy [T]
  (branch? (fn [T] bool)) (kids (fn [T] (Cons T))) (mk (fn [T (Cons T)] T)))

(defstruct Frame :heap :copy [T]
  (l (Cons T)) (pnode T) (up (Frame T)) (r (Cons T)) (changed bool))

(defstruct Loc :heap :copy [T] (node T) (path (Frame T)) (ops (ZipOps T)))

(defn top? [T] [f : (Frame T)] : bool (= (:: f :int) 0))
(defn no-frame [T] [] : (Frame T) (:: 0 (Frame T)))

(defn rev-onto [T] [xs : (Cons T) acc : (Cons T)] : (Cons T)
  (if (tnil? xs) acc (rev-onto (ttail xs) (tcons-of (thead xs) acc))))

(defn zipper [T] [ops : (ZipOps T) root : T] : (Loc T)
  (make-struct Loc :node root :path (no-frame) :ops ops))

(defn zdown [T] [z : (Loc T)] : (Option (Loc T))
  (let [ops (.ops z)
        n   (.node z)]
    (if ((.branch? ops) n)
      (let [cs ((.kids ops) n)]
        (if (tnil? cs)
          (None)
          (Some (make-struct Loc
                  :node (thead cs)
                  :path (make-struct Frame :l (:: (tnil) (Cons T))      ; I1
                                           :pnode n :up (.path z)
                                           :r (ttail cs) :changed false)
                  :ops ops))))
      (None))))

(defn zright [T] [z : (Loc T)] : (Option (Loc T))
  (let [f (.path z)]
    (if (or (top? f) (tnil? (.r f)))
      (None)
      (Some (make-struct Loc
              :node (thead (.r f))
              :path (make-struct Frame :l (tcons-of (.node z) (.l f)) :pnode (.pnode f)
                                       :up (.up f) :r (ttail (.r f)) :changed (.changed f))
              :ops (.ops z))))))

(defn mark-changed [T] [f : (Frame T)] : (Frame T)
  (if (top? f)
    f
    (make-struct Frame :l (.l f) :pnode (.pnode f) :up (.up f) :r (.r f) :changed true)))

(defn zup [T] [z : (Loc T)] : (Option (Loc T))
  (let [f (.path z)]
    (if (top? f)
      (None)
      (let [ops (.ops z)
            n   (if (.changed f)
                  ((.mk ops) (.pnode f) (rev-onto (.l f) (tcons-of (.node z) (.r f))))
                  (.pnode f))]
        (Some (make-struct Loc :node n
                               :path (if (.changed f) (mark-changed (.up f)) (.up f))
                               :ops ops))))))

(defn zreplace [T] [z : (Loc T) n : T] : (Loc T)
  (make-struct Loc :node n :path (mark-changed (.path z)) :ops (.ops z)))

(defn zroot [T] [z : (Loc T)] : T
  (match (zup z)
    (None) (.node z)
    (Some p) (:: (zroot p) T)))                                         ; I2

;; A node type: any homogeneous recursive :heap :copy type works.
(defdata Rose :heap :copy (Leaf int) (Br (Cons Rose)))

(defn rose-ops [] : (ZipOps Rose)
  (make-struct ZipOps
    :branch? (fn [r : Rose] : bool (match r (Leaf _) false (Br _) true))
    :kids    (fn [r : Rose] : (Cons Rose) (match r (Leaf _) (tnil) (Br cs) cs))
    :mk      (fn [r : Rose cs : (Cons Rose)] : Rose (Br cs))))
```

The ZP4 prototype used the same pieces: a `:copy` `Traversal` record, a
`:heap :copy` cursor and a `Supply` cell threaded through `fill`. Over the
tree `((1) 2 (3))`, it steps to the second element, sets it to 200, and
closes. It prints `204` for the new tree and `6` for the old one, compiled
under clang and interpreted.

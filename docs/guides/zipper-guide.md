---
title: Zipper Guide
category: Functional Patterns
description: Generic zippers with stdlib/zip.tur -- a typed clojure.zip over any tree (ZipLoc), the Plated typeclass, and an element cursor over any container (ZipCursor).
tags:
  - zippers
  - iteration
  - persistent-data
---

# Zipper Guide

A **zipper** is a tree with a cursor in it. You move the cursor down, up and
sideways, edit the node under it, and zip back up to get the edited tree. Every
step is a new immutable value, so old positions stay valid and unedited
subtrees are shared rather than copied.

[`stdlib/zip.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/zip.tur)
ships two zippers:

- **`(ZipLoc T)`, the structural zipper.** This is Clojure's `clojure.zip`,
  typed. It navigates any tree whose nodes are all one type `T`: an AST, a
  rose tree, a scene graph, a menu.
- **`(ZipCursor S A)`, the element cursor.** This walks the `A`-elements of a
  container `S` (a tree's labels, a list, a record's fields), lets you edit
  them in place, and closes back into an `S` of the same shape.

It is not auto-loaded:

```turmeric
(load "stdlib/zip.tur")
```

Every function has a `zip/` prefix, so `(zip/down z)` reads like Clojure's
`(zip/down loc)`.

## When to reach for a zipper

Plain recursion is the right tool when one pass over the tree computes the
answer: a sum, a pretty-printer, an evaluator. Reach for a zipper when:

- **the edit site is found by searching**, and the edit has to be put back
  into the tree around it (`zip/find-next`, then `zip/replace`, then
  `zip/root`);
- **you make several edits in different places**, and want each one to be a
  local operation rather than a rebuild from the root;
- **you need context at a node**: its siblings (`zip/lefts`, `zip/rights`),
  its ancestors (`zip/ancestors`), its depth;
- **you walk back and forth** (`zip/next`, `zip/prev`), which a recursive
  function cannot do;
- **you want the old tree too.** Every location is persistent, so undo is
  just keeping the previous value.

## The structural zipper

### Describing a tree: three functions

You tell the zipper how to navigate your type with three functions, the same
three `clojure.zip/zipper` takes:

| Function | Type | Says |
|---|---|---|
| `branch?` | `(fn [T] bool)` | can this node have children? |
| `children` | `(fn [T] (Cons T))` | its children, in order |
| `make-node` | `(fn [T (Cons T)] T)` | the node rebuilt with new children |

`zip/ops` bundles them, and `zip/zipper` opens a zipper at the root:

```turmeric
(load "stdlib/zip.tur")

;; A leaf holds an int; a node holds its children.
(defdata Tree :heap :copy (Leaf int) (Node (Cons Tree)))

(defn tree-ops [] : (ZipOps Tree)
  (zip/ops (fn [t : Tree] : bool (match t (Leaf _) false (Node _) true))
           (fn [t : Tree] : (Cons Tree) (match t (Leaf _) (tnil) (Node cs) cs))
           (fn [t : Tree cs : (Cons Tree)] : Tree (Node cs))))

(defn must [o : (Option (ZipLoc Tree))] : (ZipLoc Tree)
  (match o
    (Some z) z
    (None)   (panic "no such location")))

(defn kids-sum [cs : (Cons Tree)] : int
  (if (tnil? cs) 0 (+ (leaf-sum (thead cs)) (kids-sum (ttail cs)))))

(defn leaf-sum [t : Tree] : int
  (match t
    (Leaf v)  v
    (Node cs) (kids-sum cs)))

(defn main [] : int
  ;; [1 [2 3]]
  (let [t   (Node (tcons-of (Leaf 1)
                            (tcons-of (Node (tcons-of (Leaf 2) (tcons-of (Leaf 3) (tnil))))
                                      (tnil))))
        z   (zip/zipper (tree-ops) t)
        two (must (zip/down (must (zip/right (must (zip/down z))))))
        t2  (zip/root (zip/replace two (Leaf 40)))]
    (println (leaf-sum t2))   ; 44 -- [1 [40 3]]
    (println (leaf-sum t)))   ; 6  -- the original is unchanged
  0)
```

The node type must be a `:heap :copy` type (a `defdata` or a `defstruct`): the
zipper keeps the same node in several locations at once, which a `:move` type
forbids. A tree whose nodes are of different types is navigated as one sum
type -- an AST whose nodes are one `defdata` -- exactly as with Haskell's
Uniplate.

`branch?` distinguishes a **leaf** from an **empty branch**. `zip/down` gives
`None` for both, but `zip/children` of a leaf panics, and `zip/append-child`
works only on a branch.

### Moving

Moves return `(Option (ZipLoc T))`: `None` where Clojure answers `nil`, at a
boundary.

| Move | `None` when |
|---|---|
| `zip/down` | the node is a leaf or an empty branch |
| `zip/up` | at the root |
| `zip/left` / `zip/right` | at the first / last sibling, or at the root |
| `zip/prev` | at the root |

`zip/leftmost`, `zip/rightmost` and `zip/next` always return a location.
`zip/next` walks depth-first in pre-order; after the last node it returns the
**end marker**, a location at the root for which `zip/end?` is true. That keeps
Clojure's standard loop working unchanged:

```turmeric
(defn visit-all [z : (ZipLoc Tree)] : int
  (if (zip/end? z)
    0
    (+ 1 (visit-all (zip/next z)))))
```

### Editing

`zip/replace`, `zip/edit`, `zip/insert-left`, `zip/insert-right`,
`zip/insert-child` and `zip/append-child` return the same position with the
change made. `zip/remove` deletes the node and returns the location that came
before it in pre-order. None of them change the location you passed in.

`zip/up` rebuilds a parent with `make-node` only when something below it was
edited; otherwise it returns the stored parent. So a walk that edits nothing
allocates no nodes, and

```turmeric
(zip/root (zip/zipper ops t))   ; => t itself, the same pointer
```

After an edit, only the path from the edit to the root is rebuilt. Every other
subtree is shared between the old tree and the new one.

### Search and bulk rewrite

`zip/find-next` searches forward in pre-order, starting at the location itself:

```turmeric
(defn leaf=? [n : int] : (fn [Tree] bool)
  (fn [t : Tree] : bool (match t (Leaf v) (= v n) (Node _) false)))

;; replace the first 4 with 40
(match (zip/find-next (zip/zipper (tree-ops) t) (leaf=? 4))
  (None)   t
  (Some f) (zip/root (zip/replace f (Leaf 40))))
```

To find the next match after `f`, search from `(zip/next f)`.

`zip/edit-all` applies a function to every node and returns the root. It is
Clojure's edit loop, shipped, and the zipper form of Uniplate's `transform`. It
is **top-down**: a node is edited before its children are visited, and the
walk then descends into whatever the edit returned.

### Example: constant folding an AST

The program only says what to do at one node; the zipper does the traversal
and the rebuild. This fold replaces every closed subtree -- one with no
variables -- by its value. Top-down, the largest closed subtrees fold first:

```turmeric
(defdata Ast :heap :copy (Num int) (Var int) (Add (Cons Ast)) (Mul (Cons Ast)))

;; ast-ops: Add and Mul are branches; make-node keeps the operator.
;; closed? and value are ordinary recursive functions.

(defn fold [a : Ast] : Ast
  (match a
    (Num _)  a
    (Var _)  a
    (Add _)  (if (closed? a) (Num (value a)) a)
    (Mul _)  (if (closed? a) (Num (value a)) a)))

(zip/edit-all (zip/zipper (ast-ops) e) fold)
;; (+ x (* 2 3) (+ 1 (* y 4) (* 5 6)) 7)  =>  (+ x 6 (+ 1 (* y 4) 30) 7)
```

The full program is
[`tests/fixtures/zip-edit-all-ast/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/zip-edit-all-ast/).

### The stock rose tree

`(Rose A)` is a label and a list of child trees (Haskell's `Data.Tree`, the
typed counterpart of Clojure's `seq-zip`):

```turmeric
(defstruct Rose :heap :copy [A] (label A) (kids (Cons (Rose A))))
```

`zip/rose` builds a node and `zip/rose-zip` opens a zipper on one. Every rose
node is a branch -- a leaf is a node with no children -- so
`zip/append-child` works anywhere.

### Plated: the ops as an instance

`Plated` (lens's name for "has children of its own type") makes the three
functions an instance, so `zip/plated` opens a zipper with no ops argument:

```turmeric
(definstance Plated [Tree]
  (plate-branch? [t] (match t (Leaf _) false (Node _) true))
  (plate-children [t] (match t (Leaf _) (tnil) (Node cs) cs))
  (plate-rebuild [t cs] (Node cs)))

(zip/edit-all (zip/plated t) double)
```

`Rose` has a stock instance. An instance gives one canonical view of a type;
for a second view of the same type, build a `ZipOps` with `zip/ops` instead.

## The element cursor

A `(Traversal S A)` says how to visit the `A`-elements of an `S` and put them
back:

- `parts : (fn [S] (Cons A))` lists the elements, in order;
- `fill : (fn [S (Cons A)] S)` returns the same `S` with its elements
  replaced, in order.

It must satisfy `(fill s (parts s)) = s`, and `(parts (fill s xs)) = xs` when
`xs` has as many elements as `(parts s)`.

`zip/cursor` opens a cursor on the first element (`None` when there are none);
`zip/cursor-next` and `zip/cursor-prev` move; `zip/cursor-focus`,
`zip/cursor-set` and `zip/cursor-edit` read and replace the focus; and
`zip/cursor-close` writes the elements back:

```turmeric
(let [xs (tcons-of 1.5 (tcons-of 2.25 (tcons-of 3.75 (tnil))))]
  (match (zip/cursor (zip/cons-traversal) xs)
    (None)   xs
    (Some c) (match (zip/cursor-next c)
               (None)    xs
               (Some c2) (zip/cursor-close (zip/cursor-set c2 30.5)))))
;; => 1.5 30.5 3.75; xs itself is unchanged
```

Opening and closing are O(n); every move and edit is O(1), and every cursor
stays valid.

Two traversals are stock:

- **`zip/cons-traversal`**: a list's elements are itself. A cursor over it is
  a persistent list zipper.
- **`zip/rose-labels`**: a rose tree's labels, in pre-order.

For your own container, write the two functions. `fill` usually threads the
unused elements through the rebuild; see
[`tests/fixtures/zip-cursor-tree/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/zip-cursor-tree/)
for a binary tree.

## Clojure to Turmeric

| `clojure.zip` | `stdlib/zip.tur` | At a boundary |
|---|---|---|
| `zipper` | `(zip/zipper ops root)`, with `(zip/ops branch? children make-node)` | -- |
| `node` | `(zip/node z)` | -- |
| `branch?` | `(zip/branch? z)` | -- |
| `children` | `(zip/children z) : (Cons T)` | panics "called children on a leaf node" |
| `make-node` | `(zip/make-node z node kids)` | -- |
| `path` | `(zip/ancestors z)`, root first | `(tnil)` at the root |
| `lefts` / `rights` | `(zip/lefts z)` / `(zip/rights z)`, in document order | `(tnil)` at the root |
| `down` / `up` | `(zip/down z)` / `(zip/up z) : (Option (ZipLoc T))` | `None` |
| `left` / `right` | `(zip/left z)` / `(zip/right z) : (Option (ZipLoc T))` | `None` |
| `leftmost` / `rightmost` | `(zip/leftmost z)` / `(zip/rightmost z)` | `z` itself |
| `root` | `(zip/root z) : T` | -- |
| `next` / `end?` | `(zip/next z)` / `(zip/end? z)` | the end marker |
| `prev` | `(zip/prev z) : (Option (ZipLoc T))` | `None` at the root |
| `replace` / `edit` | `(zip/replace z node)` / `(zip/edit z f)` | -- |
| `insert-left` / `insert-right` | `(zip/insert-left z node)` / `(zip/insert-right z node)` | panics "Insert at top" |
| `insert-child` / `append-child` | `(zip/insert-child z node)` / `(zip/append-child z node)` | panics "called children on a leaf node" |
| `remove` | `(zip/remove z)`, the pre-order predecessor | panics "Remove at top" |
| -- | `(zip/depth z) : int`, `(zip/find-next z p)`, `(zip/edit-all z f)` | -- |

Clojure's `edit` takes extra arguments for `f`; `zip/edit` takes a one-argument
function, so close over them instead. Clojure's `seq-zip` corresponds to
`zip/rose-zip`. There is no `vector-zip` or `xml-zip`: the stdlib `Vec` and the
JSON nodes are mutable, so write the ops for a persistent node type of your
own.

## Cost

`k` is the number of children of the node involved; `d` is the depth.

| Operation | Cost |
|---|---|
| `right`, `left`, `replace`, `edit`, `insert-left`, `insert-right` | O(1) |
| `down` | one `children` call |
| `up` with no edit below | O(1), no allocation |
| `up` after an edit | O(k): reverse the left siblings and call `make-node` |
| `leftmost`, `rightmost` | O(k) |
| `root` | O(d) with no edits; O(d * k) after edits |
| `next`, `prev` | amortised O(1) per node over a full walk |

## Memory

Locations, path frames and rebuilt nodes are `:heap` values, like `(Cons A)`
cells. There is no `zip/free`, and none is needed:

- **Outside a region**, they live until the process exits.
- **Inside `with-region`**, a session whose result is a scalar (a count, a
  sum, a found index) rewinds in one step. Fifty sessions that each build a
  127-node tree, rewrite every node with `zip/edit-all`, and sum the labels
  report `region-stats: pushes=50 rewinds=50 retires=0` under
  `TUR_REGION_STATS=1`.
- **A session that returns the edited tree** cannot rewind: the result
  reaches into the generation, so the generation is retired (the same fifty
  sessions returning the tree report `rewinds=0 retires=50`). That is correct;
  it just saves nothing.

See [gc-guide.md](gc-guide.md) (regions) and
[memory-usage-guide.md](memory-usage-guide.md).

## See also

- [lens-guide.md](lens-guide.md) -- `Traversal` follows `Lens`'s record encoding.
- [cellular-automata-comonad-tutorial.md](cellular-automata-comonad-tutorial.md)
  -- `stdlib/zipper.tur`'s array-backed 1D zipper as a comonad.
- [generic-zipper-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/generic-zipper-plan.md) -- the design survey (Huet, Clojure,
  Uniplate, Kiselyov, McBride) and what is deferred.

---
title: Cellular Automata with Comonads
category: Tutorials
description: Cellular automata with comonads
---

# Cellular Automata with Comonads in Turmeric

This tutorial builds Conway's Game of Life using **comonads** -- a functional abstraction that makes cellular automata rules both composable and easy to reason about.

We start with the theory, build the abstractions step by step, and end with a fully working Game of Life implementation.

---

## Part 1 -- Why Comonads?

A cellular automaton has one key operation: given the current state of the whole grid and a particular cell's position, compute the next state of that cell.

In Haskell, this is captured by the `Comonad` typeclass:

```haskell
class Comonad w where
  extract   :: w a -> a                    -- "read the focused cell"
  extend    :: (w a -> b) -> w a -> w b    -- "apply rule to every cell"
  duplicate :: w a -> w (w a)              -- derived: extend id
```

The key insight is:

> `extend f grid` applies `f` to the grid at **every possible focus position** simultaneously, producing a new grid.

For Game of Life, `f` is just "count Moore neighbours and apply the four rules". No indices to manage, no off-by-one bugs.

---

## Part 2 -- The Comonad Typeclass in Turmeric

Turmeric declares typeclasses with `defclass`. Higher-kinded type constructor parameters use the `[^w]` syntax:

```turmeric
(defclass Comonad [^w]
  (extract   [wa]     : int)
  (extend    [wa fn]  : int)
  (duplicate [wa]     : int))
```

```sweet-exp
defclass Comonad [^w]
  extract   [wa]     :int
  extend    [wa fn]  :int
  duplicate [wa]     :int
```

This comonad library represents its container values as `int64_t` heap handles, so every method returns `:int`. The `fn` argument to `extend` is a Turmeric closure: `(fn [wa] ...)`.

The implementation lives in [`stdlib/comonad.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/comonad.tur), which also provides Identity and Pair comonad instances as examples.

---

## Part 3 -- The Zipper: 1D Cellular Automata

Before tackling 2D grids, it helps to understand the simpler 1D case. A **Zipper** is a list with a cursor:

```turmeric no-check
left  = [prev, prev-prev, ...]   -- nearest first
focus = current element
right = [next, next-next, ...]   -- nearest first
```

```sweet-exp
left  = [prev, prev-prev, ...]   -- nearest first
focus = current element
right = [next, next-next, ...]   -- nearest first
```

The Comonad instance for Zipper:

- **`extract z`** -- returns `z.focus`
- **`extend f z`** -- creates a new Zipper where each position holds `f(z_i)`, where `z_i` is the Zipper shifted to position `i`
- **`duplicate z`** -- each position holds the Zipper focused there (extend identity)

### 1D XOR automaton

```turmeric
(defn ca-rule [z] :int
  ```c
  typedef struct { int64_t *left; size_t left_len; int64_t focus; int64_t *right; size_t right_len; } Zip;
  Zip *p = (Zip *)(intptr_t)z;
  int64_t l = p->left_len  > 0 ? p->left[0]  : 0;
  int64_t r = p->right_len > 0 ? p->right[0] : 0;
  return (l + p->focus + r) & 1;   /* XOR of three neighbours */
  ```)

(defn main [] :int
  (let [arr (zap-array 9)]
    (zap-set arr 4 1)
    (let [z0 (zipper-from-array arr 9 4)]
      (print-row z0)                       ;; ....#....
      (let [z1 (.extend z0 ca-rule)]
        (print-row z1)                     ;; ...###...
        (let [z2 (.extend z1 ca-rule)]
          (print-row z2)                   ;; ..#.#.#..
          0)))))
```

```sweet-exp
defn ca-rule [z] :int
  ```c
  typedef struct { int64_t *left; size_t left_len; int64_t focus; int64_t *right; size_t right_len; } Zip;
  Zip *p = (Zip *)(intptr_t)z;
  int64_t l = p->left_len  > 0 ? p->left[0]  : 0;
  int64_t r = p->right_len > 0 ? p->right[0] : 0;
  return (l + p->focus + r) & 1;   /* XOR of three neighbours */
  ```

defn main [] :int
  let [arr zap-array(9)]
    zap-set(arr 4 1)
    let [z0 zipper-from-array(arr 9 4)]
      print-row(z0)                       ;; ....#....
      let [z1 .extend(z0 ca-rule)]
        print-row(z1)                     ;; ...###...
        let [z2 .extend(z1 ca-rule)]
          print-row(z2)                   ;; ..#.#.#..
          0
```

One `(.extend z ca-rule)` call steps the entire automaton forward.

The full Zipper implementation lives in [`stdlib/zipper.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/zipper.tur). See [`tests/fixtures/zipper-comonad/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/zipper-comonad/) for a runnable version.

---

## Part 4 -- GridCtx: 2D Cellular Automata

For a 2D grid we use a flat row-major array with a focus position `(cx, cy)`:

```c
typedef struct {
  int64_t *data;            /* flat array, width * height elements */
  int64_t  width, height;
  int64_t  cx, cy;          /* current focus */
} GCtx;
```

Index formula: `data[y * width + x]`.

### Key primitives

```turmeric
;; Allocate a zero-filled grid; focus starts at (0, 0).
(defn grid-new [width height] :int ...)

;; Get / set a value by (x, y); out-of-bounds returns 0 / is a no-op.
(defn grid-get [g x y] :int ...)
(defn grid-set [g x y val] :int ...)

;; Return the value at the current focus.
(defn grid-focus [g] :int ...)

;; Deep-copy the grid (new data array, same cx/cy).
(defn grid-clone [g] :int ...)

;; Count live Moore 8-neighbours at (cx, cy).
(defn grid-count-neighbors [g] :int ...)

;; Print '#' / '.' ASCII, one row per line.
(defn grid-print [g] :int ...)
```

```sweet-exp
;; Allocate a zero-filled grid; focus starts at (0, 0).
defn grid-new [width height] :int ...

;; Get / set a value by (x, y); out-of-bounds returns 0 / is a no-op.
defn grid-get [g x y] :int ...
defn grid-set [g x y val] :int ...

;; Return the value at the current focus.
defn grid-focus [g] :int ...

;; Deep-copy the grid (new data array, same cx/cy).
defn grid-clone [g] :int ...

;; Count live Moore 8-neighbours at (cx, cy).
defn grid-count-neighbors [g] :int ...

;; Print '#' / '.' ASCII, one row per line.
defn grid-print [g] :int ...
```

### The Comonad instance

```turmeric
(defn __gridctx_extend [g fn] :int
  ```c
  typedef struct { int64_t *data; int64_t width; int64_t height; int64_t cx; int64_t cy; } GCtx;
  GCtx *src = (GCtx *)(intptr_t)g;
  int64_t w = src->width, h = src->height;
  int64_t *out_data = malloc((size_t)(w * h) * sizeof(int64_t));
  GCtx tmp;
  tmp.data = src->data; tmp.width = w; tmp.height = h;
  for (int64_t y = 0; y < h; y++) {
    for (int64_t x = 0; x < w; x++) {
      tmp.cx = x; tmp.cy = y;
      out_data[y * w + x] = ((int64_t(*)(int64_t))(intptr_t)fn)((int64_t)(intptr_t)&tmp);
    }
  }
  GCtx *out = malloc(sizeof(GCtx));
  out->data = out_data; out->width = w; out->height = h;
  out->cx = src->cx; out->cy = src->cy;
  return (int64_t)(intptr_t)out;
  ```)

(definstance Comonad [gridctx]
  (extract   [wa]    (grid-focus wa))
  (extend    [wa fn] (__gridctx_extend wa fn))
  (duplicate [wa]    (__gridctx_extend wa (fn [g] g))))
```

```sweet-exp
defn __gridctx_extend [g fn] :int
  ```c
  typedef struct { int64_t *data; int64_t width; int64_t height; int64_t cx; int64_t cy; } GCtx;
  GCtx *src = (GCtx *)(intptr_t)g;
  int64_t w = src->width, h = src->height;
  int64_t *out_data = malloc((size_t)(w * h) * sizeof(int64_t));
  GCtx tmp;
  tmp.data = src->data; tmp.width = w; tmp.height = h;
  for (int64_t y = 0; y < h; y++) {
    for (int64_t x = 0; x < w; x++) {
      tmp.cx = x; tmp.cy = y;
      out_data[y * w + x] = ((int64_t(*)(int64_t))(intptr_t)fn)((int64_t)(intptr_t)&tmp);
    }
  }
  GCtx *out = malloc(sizeof(GCtx));
  out->data = out_data; out->width = w; out->height = h;
  out->cx = src->cx; out->cy = src->cy;
  return (int64_t)(intptr_t)out;
  ```

definstance Comonad [gridctx]
  extract   [wa]    grid-focus(wa)
  extend    [wa fn] __gridctx_extend(wa fn)
  duplicate [wa]    __gridctx_extend(wa (fn [g] g))
```

Notice: `tmp` is a stack-allocated scratch struct that shares `src->data`. Because `fn` is called synchronously and never stores the pointer, this is safe. Each call gets a fresh `(cx, cy)`.

The full implementation lives in [`stdlib/grid.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/grid.tur).

---

## Part 5 -- Conway's Game of Life

The Game of Life rule is a pure function from a focused grid to the next cell state:

```turmeric
(defn conway-rule [g] :int
  ```c
  typedef struct { int64_t *data; int64_t width; int64_t height; int64_t cx; int64_t cy; } GCtx;
  GCtx *p = (GCtx *)(intptr_t)g;
  int64_t alive = p->data[p->cy * p->width + p->cx];
  int n = 0;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) continue;
      int nx = (int)p->cx + dx, ny = (int)p->cy + dy;
      if (nx >= 0 && nx < (int)p->width && ny >= 0 && ny < (int)p->height)
        if (p->data[ny * p->width + nx]) n++;
    }
  }
  if (alive && n < 2) return 0;   /* underpopulation */
  if (alive && n > 3) return 0;   /* overpopulation  */
  if (alive)          return 1;   /* survival        */
  if (n == 3)         return 1;   /* reproduction    */
  return 0;
  ```)
```

```sweet-exp
defn conway-rule [g] :int
  ```c
  typedef struct { int64_t *data; int64_t width; int64_t height; int64_t cx; int64_t cy; } GCtx;
  GCtx *p = (GCtx *)(intptr_t)g;
  int64_t alive = p->data[p->cy * p->width + p->cx];
  int n = 0;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) continue;
      int nx = (int)p->cx + dx, ny = (int)p->cy + dy;
      if (nx >= 0 && nx < (int)p->width && ny >= 0 && ny < (int)p->height)
        if (p->data[ny * p->width + nx]) n++;
    }
  }
  if (alive && n < 2) return 0;   /* underpopulation */
  if (alive && n > 3) return 0;   /* overpopulation  */
  if (alive)          return 1;   /* survival        */
  if (n == 3)         return 1;   /* reproduction    */
  return 0;
  ```
```

A full generation step is then just:

```
(let [next (.extend current conway-rule)]
  ...)
```

That's it -- the Comonad abstracts away all index management.

### Blinker demo

```turmeric
(let [g0 (grid-new 5 5)]
  (grid-set g0 1 2 1)
  (grid-set g0 2 2 1)
  (grid-set g0 3 2 1)
  (grid-print g0)                         ;; .....  .....  .###.  .....  .....
  (let [g1 (.extend g0 conway-rule)]
    (grid-print g1)                       ;; .....  ..#..  ..#..  ..#..  .....
    (let [g2 (.extend g1 conway-rule)]
      (grid-print g2)                     ;; same as g0 -- period 2
      0)))
```

```sweet-exp
let [g0 grid-new(5 5)]
  grid-set(g0 1 2 1)
  grid-set(g0 2 2 1)
  grid-set(g0 3 2 1)
  grid-print(g0)                         ;; .....  .....  .###.  .....  .....
  let [g1 .extend(g0 conway-rule)]
    grid-print(g1)                       ;; .....  ..#..  ..#..  ..#..  .....
    let [g2 .extend(g1 conway-rule)]
      grid-print(g2)                     ;; same as g0 -- period 2
      0
```

---

## Part 6 -- Putting It Together

The full runnable example is in [`examples/cellular-automata.tur`](https://github.com/turmeric-lang/turmeric/blob/main/examples/cellular-automata.tur). It contains both the imperative version (Part 1) and the comonadic version (Part 2) side by side, so you can compare approaches.

To run it:

```bash
./build/tur run examples/cellular-automata.tur
```

### Test fixtures

| Fixture | What it verifies |
|---------|-----------------|
| [`tests/fixtures/grid-basic/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/grid-basic/) | `grid-new`, `grid-get`, `grid-set`, `grid-clone`, `grid-print` |
| [`tests/fixtures/grid-comonad/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/grid-comonad/) | `extend conway-rule` on blinker (3 generations) |
| [`tests/fixtures/game-of-life-blinker/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/game-of-life-blinker/) | Period-2 blinker oscillation |
| [`tests/fixtures/game-of-life-block/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/game-of-life-block/) | Block still-life stability |
| [`tests/fixtures/zipper-comonad/`](https://github.com/turmeric-lang/turmeric/tree/main/tests/fixtures/zipper-comonad/) | Zipper comonad, 1D XOR automaton |

Run all CA-related tests:

```bash
TUR_TEST_FILTER="grid|game-of-life|zipper-comonad|comonad-identity|flat-array" bash tests/run.sh
```

---

## Comonad Laws

For correctness, `extend` and `extract` must satisfy three laws:

1. **Left identity:** `extract (extend f w) = f w`
2. **Right identity:** `extend extract w = w`
3. **Associativity:** `extend f (extend g w) = extend (fn [w'] (f (extend g w'))) w`

These mirror the Monad laws (with arrows reversed). They guarantee that `extend` composes predictably -- multi-step CA simulations chain as expected.

---

## Summary

| Concept | Turmeric | Meaning |
|---------|----------|---------|
| `defclass Comonad [^w]` | HKT typeclass | `w` is a type constructor `* -> *` |
| `(extract wa)` | `.extract w` | Read the focused element |
| `(extend wa fn)` | `.extend grid rule` | Apply rule at every position |
| `(duplicate wa)` | `.duplicate grid` | Grid of grids, each focused differently |
| `definstance Comonad [gridctx]` | 2D grid instance | `extend` = walk all `(x, y)` positions |

Comonads let you write the rule once, at a single cell, and apply it everywhere. No loops, no index arithmetic -- just `extend`.

# HAMT bitmap and leaf nodes are allocated at the size of a full 32-slot node

**Severity: medium (memory, not correctness).** A persistent `Map int int`
holds ~360 bytes of live heap per entry, against ~78 for `MutableMap` and ~19
for a `Vec int`. Most of it is padding no node ever uses.

## Repro

```turmeric
(load "stdlib/map.tur")
(defn fill [m : (Map int int) i : int n : int] : (Map int int)
  (if (= i n) m
    (let [m2 (map-assoc m i i)]
      (map-free m)
      (fill m2 (+ i 1) n))))
(defn main [] : int
  (let [m (fill (:: #map{} (Map int int)) 0 10000)]
    (println (map-count m))
    (map-free m)
    0))
```

Peak heap under `valgrind --tool=massif`, minus the same program at n = 0,
divided by 10000: **361.5 B/entry**. Allocation traffic is 6.9 allocations
and ~1650 B per insert (path copying, expected for a persistent map).

## Root cause

`struct HamtNode` (`src/runtime/hamt.h:106`) is a tagged union whose largest
arm is `HamtNodeArray` (32 child pointers), so `sizeof(HamtNode)` is 264.

- `bitmap_node_create` (`src/runtime/hamt.c:206`) allocates
  `sizeof(HamtNode) + sizeof(HamtNode *) * (child_count - 1)`: the
  variable-length tail is added *on top of* the full array arm, so a 1-child
  bitmap node is 264 B and a 2-child one is 272 B.
- Every entry lives in a collision node (`collision_node_create`,
  `src/runtime/hamt.c:224`), also `sizeof(HamtNode)` = 264 B, plus a separate
  32-byte `HamtEntry` -- two mallocs and ~296 B per key before malloc overhead.

## Fix directions

- Size a bitmap node from `offsetof(HamtNode, as.bitmap.children) +
  child_count * sizeof(HamtNode *)` (and the copies at `hamt.c:480`).
- Size a collision node from `offsetof(HamtNode, as) + sizeof(HamtNodeCollision)`,
  or store a single-entry leaf inline instead of a node plus a chained entry.
- `src/runtime/hamt.c` is the one implementation (the archive and the emitted
  preamble share it), so the fix moves every fixture snapshot that embeds it.

Found writing `docs/guides/memory-usage-guide.md` (2026-10-05).

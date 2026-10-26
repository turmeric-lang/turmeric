---
title: "MiniKanren Part 1: Relations and Queries"
category: Tutorials
description: Logic programming with miniKanren -- relations, composition, and bidirectional queries
tags:
  - logic-programming
  - queries
---

# MiniKanren Part 1: Relations and Queries

A practical introduction to relational programming in Turmeric using a small, runnable example project in `examples/minikanren`.

> **Note:** This guide uses a miniKanren-style approach (relations + bidirectional queries) implemented directly in Turmeric with explicit loops. The real miniKanren engine ships in `stdlib/logic.tur` -- see [tur-logic-guide.md](tur-logic-guide.md).

---

## What you will build

The example models a tiny family-graph relation and demonstrates:

- `parento` relation (`parent`, `child`)
- `grandparento` relation composed from `parento`
- Bidirectional queries:
  - known child, unknown parent
  - known parent, unknown child
  - both variables unknown (enumeration)

Code location:

- `examples/minikanren/src/main.tur`

---

## Build and run

From the repository root:

```sh
cmake -S . -B build
cmake --build build -j --target minikanren
./build/examples/minikanren/minikanren
```

---

## Step 1: Define domain facts

The example encodes people as integer IDs and converts IDs to names with `person-name`.

`parento` acts like a relation predicate:

- returns `1` when a `(parent, child)` pair is true
- returns `0` otherwise

This mirrors the "goal succeeds/fails" shape used in miniKanren.

---

## Step 2: Compose relations

`grandparento` is defined in terms of `parento`:

- iterate over possible middle person (`mid`)
- succeed when `parento(grand, mid)` and `parento(mid, child)` both hold

This demonstrates relational composition (building larger relations from smaller ones).

---

## Step 3: Run directional and bidirectional queries

The example includes:

- `query-parents-of child`
- `query-children-of parent`
- `query-grandparents-of child`
- `print-all-parent-facts` (enumerates all matching pairs)

Although the implementation is explicit loops, the usage pattern matches miniKanren thinking: ask the same relation in different directions.

---

## Example output (abridged)

```text
miniKanren-style relational example
----------------------------------
Parents of:
bart
homer
marge
Children of:
homer
bart
lisa
Grandparents of:
bart
abe
mona
```

---

## Next steps

Continue to the [`tur/logic` guide](tur-logic-guide.md), which covers the real
miniKanren engine in `stdlib/logic.tur` -- term representation, unification,
`fresh`, disjunction/conjunction, `run-logic`, and reification.

# Linear consumption is not reconciled at a loop body, a literal `match`, or `and` / `or`

> **Status: fixed 2026-11-27.** All three are rejected where the paths
> disagree, the rule `if` has. See [Resolution](#resolution).

**Severity:** high (a double free in a program the compiler accepts; the
other shapes leak, skip a must-consume, or refuse valid code).

`if` (both forms, since 2026-11-27) and a `match` on constructors check that
the paths meeting after them agree on which linear values were consumed
(TUR-E0104). Three other places where paths meet do not look at linear
consumption at all, so a `ref<T>` or a `^linear` value consumed on one path is
read as consumed -- or not -- on all of them.

Found 2026-11-27 while fixing
[ref-moved-in-if-without-else-double-frees](ref-moved-in-if-without-else-double-frees.md),
which closed the `if` with no else. Every shape below behaves the same on the
pre-#1486 `main` (`d4ad1b0`).

## Repro

All run with `TUR_SANITIZE_PROGRAMS=1 tur run`; `tur check` is silent on each
except where noted. `sink` and `close` are:

```turmeric
(defn sink [r : ref<int>] : int
  (let [v @r]
    (drop! r)
    v))

(defopaque ^linear H :int)
(defn mk [] : H (:: 7 :H))
(defn close [h : H] : int
  ```c
  (void)h; printf("close\n"); return 0;
  ```)
```

### 1. A loop body

```turmeric
(defn f [n : int] : int
  (let [r      (ref 41)
        ^mut i 0]
    (while (< i n)
      (sink r)
      (set! i (+ i 1)))
    1))
```

`(f 2)`:

```
ERROR: AddressSanitizer: attempting double-free on 0x502000000010 in thread T0:
```

With `h (mk)` and `(close h)` in the body, `(f 2)` prints `close` twice and
`(f 0)` never closes it: a `^linear` value consumed twice, or not at all.

### 2. A `match` on literal patterns

```turmeric
(defn skipped [c : int] : int
  (let [h (mk)]
    (match c
      0 (close h)
      _ 0)
    1))
```

Accepted; `(skipped 1)` never closes `h`. With a `ref<T>` and `sink` in the
first arm, the same path leaks the cell (8 bytes under LeakSanitizer). And
the valid form is refused:

```turmeric
(defn both [c : int] : int
  (let [h (mk)]
    (match c
      0 (close h)
      _ (close h))))
```

```
error [TUR-E0101]: linear value 'h' used after being consumed
```

### 3. `and` / `or`

```turmeric
(defn f [c : bool] : int
  (let [r (ref 41)]
    (and c (= 41 (sink r)))
    1))
```

`(f false)` skips the right operand and leaks the cell: after the `and` the
ref reads as consumed, so the scope exit drops nothing. `(or c ...)` is the
same with `(f true)`.

| Shape | Result |
|---|---|
| `(while ... (sink r))` | double free |
| `(while ... (close h))`, `h` `^linear` | consumed once per iteration, or never |
| literal `match`, one arm consumes | the other arm's path never consumes it (leak for a ref) |
| literal `match`, every arm consumes | TUR-E0101, a false rejection |
| `(and c (sink r))`, `(or c (sink r))` | leak on the short-circuit path |
| `match` on constructors, one arm consumes | TUR-E0104, as it should be |

## Root cause

- **Loop.** `elab_while_inner` (`src/compiler/elab_forms.c:5556`) snapshots
  the move state around the body and reports a local "moved inside this
  loop's body" (TUR-E0005) -- for a local with `scope_drop_inst` only
  (`:5656`), which is the Drop opaques #1486 tracks. There is no
  `linear_state_snapshot_bindings` around the body, so `is_linear_consumed`
  set by the first iteration is all the let sees.
- **Literal `match`.** The literal path (`src/compiler/elab_structs.c:4297`)
  rewinds and merges the move state per arm (`arm_moves_*`) and returns
  before the LT1 block the constructor path runs (`:5245`). Linear state runs
  straight through the arms as if they were a sequence, which is both
  halves of the table: the first arm's consumption is still set when the
  second arm is elaborated (TUR-E0101), and whatever is set at the end is
  the answer for every arm.
- **`and` / `or`.** Not traced; the right operand is elaborated as an
  ordinary argument, with no snapshot around it.

## Fix directions

- Loop: snapshot linear state before the body; a linear binding newly
  consumed by a body that does not leave is an error, with the wording the
  move check beside it uses. `elab_tail_leaves` (`elab_forms.c`) answers
  "does not leave".
- Literal `match`: restore before each arm, capture after it, and run the
  same agreement check and merge the constructor path does. Hoisting that
  block into a helper both paths call keeps them from drifting again.
- `and` / `or`: the right operand is a branch with no else. Give it the rule
  the no-else `if` now has.
- Fixtures: the double free and the leaks need `requires.leak-check`; the
  plain suite compares printed output only.

## Resolution

Fixed 2026-11-27, each by rejecting the disagreement -- the route chosen for
the `if` with no else, over releasing the value on the path that kept it.

- **Loop** (`elab_while`, `src/compiler/elab_forms.c`). A linear value the
  condition or the body consumes is TUR-E0101: "consumed inside this loop, so
  a second iteration would consume it again and a loop that never runs would
  not consume it". A body that always leaves is exempt, and what it consumed
  belongs to the path that left.
- **Literal `match`** (`elab_match`, `src/compiler/elab_structs.c`). The
  literal path now rewinds linear state before each arm and merges after
  (`ArmLinear`, beside `ArmMoves`). Arms that carry on must agree
  (TUR-E0104, the constructor path's message); an arm that leaves does not
  vote. Every arm consuming compiles, where it was TUR-E0101.
- **`and` / `or`** (`elab_call_inner`, `src/compiler/elab_call.c`). An
  operand after the first is a branch with no else: a linear value it
  consumes is TUR-E0104, unless the operand leaves.

Fixtures: `errors/ref-consumed-in-loop`, `errors/linear-consumed-in-loop`,
`errors/linear-literal-match-arm-mismatch`,
`errors/ref-consumed-in-and-operand`, `errors/ref-consumed-in-or-operand`;
and `linear-join-paths-agree` for the shapes that are sound, under
`requires.leak-check`.

Writing that last fixture turned up an emitter defect the exemption for a
leaving arm depends on: a `return` in a `match` arm did not return. It is
fixed in the same change --
[return-in-match-arm-does-not-return](return-in-match-arm-does-not-return.md).

Not changed:

- The constructor path of `elab_match` keeps its own copy of the agreement
  check; it was not moved onto `ArmLinear`. Its "does this arm leave" test is
  still the top node only
  ([do-ending-in-return-is-not-seen-as-leaving](../reported/do-ending-in-return-is-not-seen-as-leaving.md)).
- After a `while` whose body always leaves, a value the body moved still
  reads as moved (`(while c (return (sink r)))` then `@r` is TUR-E0201). The
  linear state is rewound there; the move state is not. A false rejection,
  the same family as the report above.

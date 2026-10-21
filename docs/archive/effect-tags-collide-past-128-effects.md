# Effect tags collide once a program uses more than 128 effects

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by `effect-tags-past-128`.

**Severity:** medium, but rare in practice. A `perform` can be caught by a
handler for a **different** effect, silently. It needs more than 128 distinct
effects reaching the CPS backend in one program.

## Repro

130 effects, each performed and handled once, then a probe that installs a
`D129` handler inside a `D0` handler and performs `D0`:

```python
# gen.py -- writes tags.tur
n = 130
for i in range(n): print(f"(defeffect D{i} [] : int)")
for i in range(n):
    print(f"(defn use{i} [] : int (handle (perform (D{i})) (D{i} [] k) (resume k {i})))")
print("(defn probe [] : int\n  (handle (handle (perform (D0))\n"
      "            (D129 [] k) (resume k 999))\n    (D0 [] k) (resume k 1)))")
print("(defn main [] : int\n  (println (+ " + " ".join(f"(use{i})" for i in range(n)) + "))")
print("  (println (probe))\n  0)")
```

`tur run tags.tur` prints `8385` then **`999`**. The `D0` perform was handled
by the `D129` handler; the correct output is `1`. In the emitted C,
`dk_perform(2, ...)` appears four times: the tag is shared.

## Root cause

`src/compiler/emit_cps_ir.c:effect_tag` memoizes tags in
`g_eff_syms[CPS_MAX_EFFECTS]` (128 entries), starting at tag 2. Once the table
is full it returns `2` for every further effect, which is the first effect's
tag. `dk_perform` matches handlers by tag alone, so the collision is
invisible at run time.

`CPS_MAX_EFFECTS` also sizes the per-effect `path_nodes` array in `ensure_S`,
so effects past the limit may also be missing from the taint analysis.

## Fix directions

- Make the tag table growable (an arena-backed array or a hash map from
  `Symbol *` to tag), and size `path_nodes` from the real count.
- At minimum, raise a located internal error when the table fills, instead of
  returning a shared tag.

## Resolution (2026-10-21)

`effect_tag` (`src/compiler/emit_cps_ir.c`) grows its table, so every effect
gets its own `dk_perform` tag however many a program has.

The 128-entry cap was the visible half; the classifier's effect sets were the
other. Every per-function set (`SEnt`'s performs / handles / both, the
net-escaping table, the base and permanent taint) was a pair of `uint64_t`s,
and `mark_effect` dropped an effect past bit 127 on the floor, so effect 130
could never taint. They are one `EffSet` type now: 1024 bits, with the per-bit
loops bounded by the number of tags actually issued. An effect past bit 1022
shares the last bit with every other such effect. That bit only
over-approximates -- two functions touching different overflow effects look
like they share one, which is more taint, never less -- and a `handle` never
subtracts it from a net escaping set, so a handler for one overflow effect
cannot clear another's perform. A 1,100-effect variant of the repro (its probe
nests the two highest effects) prints the right sum and `1`.

The fixture is the repro with a second probe that performs `D129` itself, so
both handlers are reachable: it prints `8385`, `1`, `999`.

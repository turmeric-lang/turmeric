# Walking a stdlib `(Cons A)` inside `with-region` retires the generation

**RESOLVED 2026-10-07** by the second fix direction: an erasing ascription
whose only use is a comparison with a literal `0` -- `(= (:: l :int) 0)` or
`(not= ...)` -- is not noted. The erased word is compared and dropped; it
reaches no store and no result, so it cannot escape. `emit_builtin`
(`src/compiler/emit_core.c`) marks exactly that operand with
`EmitCtx.region_erasure_compare_only`, and the `EX_ASCRIBE` emit
(`src/compiler/emit_expr.c`) reads and clears the flag before emitting its
inner, so an erasure nested inside the compared expression is still noted.

Re-measured: the repro now reads `pushes=50 rewinds=50 retires=0`, and so
does a bracket that only calls `tnil?`. The one emitted change is the
`TUR_REGION_NOTE_WORDS(&(l), sizeof(l));` at the top of `tnil?`, gone from
157 fixture snapshots and nothing else; the full suite was otherwise green.
The stdlib needed no change, and user code with the same null test benefits.

**Still erasing, on purpose:** `tcons`, whose tail parameter is the
carrier-level `:int` -- passing a typed list to it is the implicit typed-node
-> `:int` argument the region hooks note. Inside a region, build with
`tcons-of`; `docs/guides/memory-usage-guide.md` says so. **Not covered:** the
CPS-IR emitter (`emit_cps_ir.c`) compares ANF atoms, so the same null test in
a function body that goes through CPS still notes -- a lost saving there,
never a correctness issue. The stdlib walkers take the direct path.

**Severity: low (a lost saving, never a correctness bug).** `tnil?` and
`tlength` test for the empty list with `(= (:: l :int) 0)`. That is an
erasing ascription of a node to `:int`, which notes the node as an escape
(see `docs/guides/gc-guide.md`, regions), so every bracket that walks a stdlib
list retires instead of rewinding -- even though the erased word is only
compared with 0.

## Repro

```turmeric
(defn build [n : int acc : (Cons int)] : (Cons int)
  (if (= n 0) acc (build (- n 1) (tcons-of n acc))))
(defn rounds [k : int acc : int] : int
  (if (= k 0) acc
    (rounds (- k 1) (+ acc (with-region (fn [] : int (tlength (build 10000 (tnil)))))))))
(defn main [] : int (println (rounds 50 0)) 0)
```

`TUR_REGION_STATS=1`: `pushes=50 rewinds=0 retires=50`. Replace `tlength`
with `thead` and it reads `rewinds=50 retires=0`; a user `defdata :heap`
list walked with `match` also rewinds every time.

`tcons` (`stdlib/list.tur:52`) has the same shape: its tail parameter is an
erased `:int`, so building with it retires too. `tcons-of` does not.

## Fix directions

- Give `stdlib/list.tur` a non-erasing null test (a typed `(Cons A)` compared
  with `(tnil)`, or a compiler-known null check) and use it in `tnil?`,
  `tlength-acc__` and friends.
- Or have the escape-note pass skip an erasing ascription whose only use is a
  comparison against a literal.

Found writing `docs/guides/memory-usage-guide.md` (2026-10-05).

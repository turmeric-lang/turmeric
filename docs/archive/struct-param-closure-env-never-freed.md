# A capturing closure whose parameter is a by-value struct leaks its env

**RESOLVED 2026-10-22**; see Resolution at the end.

**Severity: low (leak; 24 B per closure built).** Found 2026-10-21 rewriting
the datalog examples onto typed data: every query predicate there is a
`(fn [Datum] bool)` closure, and LeakSanitizer reports each one's env.

## Repro

```turmeric
(defstruct D :copy [e : int x : int])
(defn count-if [n : int pred : (fn [D] bool)] : int
  (let [^mut i 0 ^mut c 0]
    (while (< i n) (when (pred (D i 0)) (set! c (+ c 1))) (set! i (+ i 1)))
    c))
(defn main [] : int
  (let [k 3]
    (println (count-if 10 (fn [d : D] : bool (= (.e d) k)))))
  0)
```

Built with `TUR_CC_FLAGS="-O1 -g -fsanitize=address"` and run with
`ASAN_OPTIONS=detect_leaks=1`: prints `1`, then
`24 byte(s) leaked in 1 allocation(s)` -- the closure's env. The same leak
shows for a closure returned by a factory
(`(defn mk [e : int] : (fn [D] bool) (fn [d : D] : bool (= (.e d) e)))`),
passed directly or let-bound first.

The same program with `int` in place of `D` reports no leak.

## What differs (partly traced)

With an `int` parameter, `count-if`'s parameter is the typed poly carrier
(`tur_poly_fn_t`), and the call passes `(tur_poly_fn_t){ env, adapter }`.
With a struct parameter it stays the one-word fat handle
(`count_hyif(int64_t, int64_t)`), and the call passes
`(int64_t)(intptr_t)(env)`. In both, `main` emits no `TUR_CLOSURE_DROP` for
the env after the call, so the clean `int` run may only mean LeakSanitizer
still finds the env pointer conservatively (a copy in a dead stack slot or
register), not that something frees it. Establish that first: if neither
shape is freed, this is the general "a capturing closure handed to a
non-retaining fn parameter is never dropped" gap, and the closure-drop-glue
S1c inference (`nonretain_param_mask`, `elab_infer_nonretain_masks` in
`src/compiler/elab_fns.c`) is where to look for why the call site gets no
drop.

## Fix directions

1. Check the `int` variant under a non-conservative measure (a counter in
   the env's drop glue, or `gc-live-blocks`-style accounting) to see which of
   the two shapes is actually freed.
2. Give the call a drop of the closure's env after the call returns when the
   parameter is inferred non-retaining, for the fat-handle ABI as for the
   carrier.
3. A `requires.leak-check` fixture with a struct-parameter predicate passed
   inline, from a factory, and let-bound.

## Resolution (2026-10-22)

Fix direction 1 settled it: neither shape was freed. Under
`ASAN_OPTIONS=use_stacks=0:use_registers=0` the `int` twin leaks the same 24 B,
and its emitted `main` has no `TUR_CLOSURE_DROP` either -- LeakSanitizer had
found the env pointer in a dead stack slot. So this was the general gap, and its
cause was one step earlier than either guess in "What differs": `count-if`'s
`nonretain_param_mask` was 0, so the call site neither hoisted the inline lambda
into a `__borrowc` let nor let the let-bound one count as non-escaping.

The mask was 0 because the inference never ran. `elab_infer_nonretain_masks`
skips a body `expr_subtree_has_inline_c` says may hide inline C, and that
predicate had no arm for `EX_SET` or `EX_WHILE`: every body with a `set!` or a
`while` -- any counting loop, `count-if` included -- fell to its conservative
`default`. A two-line arm each (`src/compiler/emit_core.c`) turns the inference
on for them; the escape walks it feeds already model both nodes. It is the
fourth missing arm in that predicate to have switched the inference off (after
the `any` readers, the dynamic nodes, and `EX_UNION_INJECT`).

`struct-param-closure-env-freed` (`requires.leak-check`) pins an inline lambda,
a let-bound one, a factory call passed directly and let-bound, and the `int`
twin. `tests/run.sh`, `tests/run-turi.sh` and `tests/run-leak-check.sh` are
green with the wider inference, no snapshot moved.

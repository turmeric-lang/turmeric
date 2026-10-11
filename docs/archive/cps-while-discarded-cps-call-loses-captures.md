# A `while` loop that discards a CPS call's result loses its function's parameters

**Severity: medium** (compile failure in the emitted C).

**Status: RESOLVED 2026-10-11** -- see [Resolution](#resolution). Found 2026-10-10 implementing
[xml-html-parsers-plan](../upcoming/spices/xml-html-parsers-plan.md) on `tur`
v0.64.0. Worked around in turmeric-spices `spices/html/tests/sax_tokens.tur`
(`feed-all` binds the result and matches on it).

## Repro (needs a turmeric-spices checkout)

From `spices/html`:

```turmeric
(defmodule f2 (export)
(import dom/event :refer [SaxEvent SaxControl Continue])
(import html/sax :refer [HtmlSaxParser html-sax-parser-new html-sax-feed!
                         html-sax-finish! html-sax-parser-free])
(defn feed-n [p : HtmlSaxParser n : int] : nil
  (let [^mut i 0]
    (while (< i n)
      (html-sax-feed! p "<a>")          ;; result discarded
      (set! i (+ i 1)))))
(defn main [] : int
  (match (html-sax-parser-new (fn [e : SaxEvent] : SaxControl (Continue)))
    (Ok p)  (do (feed-n p 3) (html-sax-finish! p) (html-sax-parser-free p))
    (Err _) nil)
  0))
```

```
error: use of undeclared identifier 'n'
error: use of undeclared identifier 'p'
```

`html-sax-feed!` returns a `(Result nil ParseError)` and runs the handler
closure inside `with-region`, so `feed-n` is CPS-converted; the loop's join
continuation does not carry `p` and `n`. Binding the result
(`(let [r (html-sax-feed! p "<a>")] (match r ...))`) compiles. Smaller
single-file versions -- a local `step` returning a `Result` and calling a
closure, or a cross-module one taking a struct with a closure field -- do not
reproduce, so the trigger involves more of `html-sax-feed!`'s shape (the
region bracket, or the `:heap` events delivered under it).

## Resolution

Not the discarded result, and not the region bracket: the loop's
INVARIANTS. A `while` in a CPS-colored function is lowered to a
tail-recursive helper `<fn>_loop<N>__cps(<carried>, <invariants>, kont)`,
and `emit_loop` (`src/compiler/emit_cps_ir.c`) collects the invariants --
the enclosing function's values the body reads -- with the capture walker.
It ran under the multi-shot capture gate, which refuses an owning value it
cannot clone. `p : HtmlSaxParser` is such a value: a `:copy` record whose
opaque `raw` field makes it a one-word carrier record, admitted as a CPS
parameter (`fn_carrier_param_ok`) but not as a multi-shot capture. One
refusal bails the whole set (`if (!inv.ok) inv.n = 0`), so the helper was
declared without `p` and `n` and its body read both. (Binding the result
moved the CPS call out of the loop's straight-line body, so the join that
captured `p` used the single-shot rule and was fine.)

Invariants are threaded linearly -- the entry passes them once, each
back-edge passes them on once, and a frame that carries the back-edge merges
them into a single-shot env (`collect_caps`) -- so `emit_loop` now collects
them under the single-shot rule a join env uses (`g_cap_single_shot`). The
spice's `feed-n` emits `f2__feed_hyn_loop0__cps(int64_t i, int64_t n,
int64_t p, DK *)`, and its C compiles (only the native lexbor header, a
build dependency, is missing here).

**Pinned by** `cps-while-owning-invariant` -- the report's shape (a CPS call
in a `while`, result discarded, reading a carrier-record invariant) and the
record's `(fn ...)` field called inside the loop beside two int invariants,
standalone (no spices needed).

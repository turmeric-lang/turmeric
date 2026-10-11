# A `while` loop that discards a CPS call's result loses its function's parameters

**Severity: medium** (compile failure in the emitted C).

**Status: OPEN.** Found 2026-10-10 implementing
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

# CPS coloring resolved a call through a parameter to a same-named global

**RESOLVED 2026-10-07.** Found and fixed the same day, while working
[r7rs-prelude-library-object-varies-with-the-program](r7rs-prelude-library-object-varies-with-the-program.md).

**Severity was:** medium. A function that calls a function-typed parameter or
local was left uncolored whenever the program also had a top-level function of
the same name. An effectful function value passed to it was then refused at
compile time. Separately, any program defining a global `f`, `g`, `k`, `body`
or `alg` changed how stdlib functions taking a callback of that name were
compiled.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn f [x : int] : int (+ x 1))
(defn app [f : (fn [int] int) x : int] : int (f x))
(defn main [] : int
  (println (handle (app (fn [x : int] : int (+ x (perform (Ask)))) 1)
             (Ask [] k) (resume k 10)))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset ...
```

Renaming the global `f` to `g` made the same program print `11`.

## Root cause

`cps_find_node` (`src/passes/cps.c`) maps a call's callee binding to a
top-level function node. After the pointer compare, it falls back to the C
symbol, so a named-let alias (a local binding carrying the lifted function's
`c_export_name`) resolves to its lambda. But the fallback used the BARE NAME
of any binding without a `c_export_name`, parameters and locals included. So
`(f x)` through the parameter `f` became an edge to the global `f`. The edge
replaced the "unresolved call" that colors a function (CPS0.1 rule 3), and a
pure global left the caller uncolored.

A sweep of the corpus with the fallback instrumented found 117 fixtures where
it matched a non-global binding, all parameters or locals. By name: `f` in 92
fixtures, `g` in 51, `k` in 8, `body` and `alg` in one each, and a local
deliberately shadowing `vec-get`. Not one was a named-let alias. Most hits were the stdlib's `__cons-fmap`
(`stdlib/list.tur`), whose callback parameter is `f`.

## Fix

A local or parameter now names a function only through `c_export_name`
(`cps_call_c_symbol`). Its bare name is never looked up, so a call through it
stays unresolved and colors its caller as rule 3 intends. The node index
still registers every node by name, so the named-let alias resolves as before.

## Effect on the r7rs library cache

This was most of
[r7rs-prelude-library-object-varies-with-the-program](r7rs-prelude-library-object-varies-with-the-program.md).
That report's two variant-forking programs both define `f`
(`(define f (lambda ...))`, `(define (f x) ...)`), so `__cons-fmap` lost its
CPS twin, and the fresh-name counters shifted after it. Its "a lambda forks the
object" reading was this name. After the fix the report's `a.tur` and `b.tur`
link one library object.

Pinned by `tests/fixtures/cps-param-named-like-a-global-fn` (compile error
before, `11 2 2` after).

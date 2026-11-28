# A performing `while` followed by a bare variable the loop does not set is refused

**Status: RESOLVED 2026-11-28** -- see [Resolution](#resolution). The text
below is the report as it stood while open.

**Severity: medium (compile failure).** A function whose body is a `while`
that performs, followed by a bare variable as its result, fails to compile
with "this effect operation has no lowering here" when the loop does not
assign that variable. Returning a parameter after the loop is the common
shape.

Found 2026-11-24 while executing
[cps-byvalue-struct-with-closure-field](cps-byvalue-struct-with-closure-field.md)
(the probe returned the struct parameter after the loop). Reproduces on `main`
at `1227412b`, with any result type.

## Repro

```turmeric
(defeffect Log [x : int] : nil)
(defn feed! [r : int s : int] : (Result int int)
  (perform (Log (+ r s)))
  (ok 0))
(defn feed-n [base : int n : int] : int
  (let [^mut i 0]
    (while (< i n)
      (feed! base i)
      (set! i (+ i 1))))
  base)
(defn main [] : int
  (handle (println (feed-n 100 3))
    (Log [x] k) (do (println x) (resume k nil)))
  0)
```

```
repro.tur:3:3: error: this effect operation has no lowering here: ...
```

`TUR_TRACE_EVICT=1` reads `BODY-UNSUPPORTED feed-n unsupported form:
EX_WHILE`, and `TUR_TRACE_CORE=1` names the rule:

```
[LOOP-REJECT] cps_ir.c:5186
```

Writing the result as `(+ base 0)` compiles and prints `100 101 102 100`.

## Root cause

`build_loop` (`src/passes/cps_ir.c:5082`) has two ways to continue after a
loop:

- **A result variable.** When the continuation is a trivial delivery of one
  variable to the function's return, the loop helper delivers that variable
  itself. It must be loop-carried.
- **A join.** Anything richer is reified as a join that runs the code after
  the loop.

A bare variable after the loop matches the first shape on its form alone, so
the builder commits to it. It then finds the variable is not carried and
rejects (`cps_ir.c:5186`, "a non-carried result is not this shape"), where the
join would have handled it. `(+ base 0)` is not a bare variable, takes the
join, and works.

## Fix direction

Decide the shape after the carried set is known: when the delivered variable
is not loop-carried, fall to join mode. The carried set is computed a few
lines below the choice (`loop_collect_carried`), so the choice has to move
below it, or the result-variable branch has to test membership first. Join
mode's own refusal (a `return` in the loop body) still applies.

## Workaround

Make the result an expression: `(+ base 0)`, or for a struct bind the loop in
a helper that returns nil and return the struct from the caller.

## Resolution

Fix direction taken as written. `build_loop` (`src/passes/cps_ir.c`) now
collects the loop-carried set (`loop_collect_carried`) before choosing the
continuation shape, and the result-variable shape requires the delivered
variable to be in it. A bare variable the loop never sets -- a parameter, a
by-value struct parameter, an outer `let` local -- falls to join mode like any
other code after the loop, and join mode's own refusals (a control-free loop
keeps whole-form delegation; a `return` in the body) still apply. The repro
prints `100 101 102 100` on both engines.

Fixture: `tests/fixtures/cps-while-then-uncarried-var-result` (an int
parameter, a by-value struct parameter, a let-bound local, and the carried
counter as the still-working result-variable case).

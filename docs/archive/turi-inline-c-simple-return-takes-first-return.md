# The interpreter's `simple-return` matcher answers with the first `return` it finds

> **FIXED (2026-10-02).** Pattern 7 of `try_exec_simple_inline_c`
> (`src/turi/eval.c`) now declines a body with a loop (`for` / `while` / `do`),
> a `goto`, or more than one `return`, the same refuse-rather-than-guess rule
> W4 gave the accessor matcher. Such a body now gets the clean "inline-C not
> supported" error under `--interpret`, or the compiled answer with
> `--enable=repl-jit-inline-c` (aot-compiled-repl-plan C1). Found while testing
> C1, whose `all-even` case printed `false` for an input that is all even: the
> body never reached C1, because this matcher claimed it first. Interpreter
> suite unchanged (2521 passed / 0 failed), compiled suite unchanged (3489 / 0).
> Test: `tests/turi/repl-jit-inline-c.sh`, the `all-even` cases.

**Summary:** The interpreter's inline-C pattern executor
(`try_exec_simple_inline_c`, `src/turi/eval.c`) models a body of the shape
`return <simple-expr>;` (pattern 7, `simple-return`). It found the first
`return ` in the body and evaluated whatever followed. It did not check whether
that `return` was reachable on every path. When control flow guarded it, the
matcher answered with the guarded value for every input.

**Severity:** High (silent wrong answer). The compiled path was correct; under
`tur --interpret` or at `tur repl` the call returned rc=0 with the wrong value
and no warning.

## Minimal repro

````turmeric
(defn all-even [a : int b : int] : bool
  ```c
  for (int i = 0; i < 2; i++) { if (a % 2 != 0) return 0; if (b % 2 != 0) return 0; }
  return 1;
  ```)
(defn main [] : int
  (println (all-even 4 8))   ; tur run: true    tur --interpret: false
  (println (all-even 4 7))   ; tur run: false   tur --interpret: false
  0)
````

`TUR_IC_TRACE=1` showed the claim: `all-even  claimed by simple-return -> int=0`.

## Root cause

Pattern 7 already declined `printf` side effects, `__TUR_CNAME_` sibling
splices, and function-pointer calls (W4, 2026-06-12). It had no guard for
control flow, although the accessor matcher next to it had declined `>1`
`return` since W4. A body whose first `return` sits inside a loop or behind an
`if` was evaluated as if that `return` were the only path.

## Fix

Decline in pattern 7 when the body contains the whole word `for`, `while`, `do`
or `goto`, or more than one `return ` (`ic_body_has_word` /
`ic_body_count_sub`, the helpers W4 added for the constructor matcher).

An `if` without a second `return` is not on the list, and need not be: a
single `return` of a local the `if` assigns conditionally
(`int64_t r = a; if (b > a) r = b; return r;`) was measured to decline already
-- the local resolver cannot read `r` back -- so it gets the clean error, not a
wrong answer. Declining every `if` would also have changed which stdlib bodies
the matcher claims (a scan found 35 stdlib inline-C bodies with an `if`) for no
correctness gain.

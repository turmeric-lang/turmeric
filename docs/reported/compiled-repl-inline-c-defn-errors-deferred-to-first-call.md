# Compiled REPL: an inline-C defn's C errors surface at its first call, not its definition

**Severity:** low (REPL diagnostics; nothing runs wrong). Found 2026-10-29
while testing the JIT preamble cache.

## Repro

~~~sh
printf '(defn bad [] : int ```c this is not C; ```)\n(+ 1 2)\n(bad)\n' \
  | TUR_NO_AUTO_SPICE=1 ./build/tur repl --enable=compiled-repl
~~~

~~~
=> #<fn bad>
=> 3
<tur-jit>:...: syntax error on identifier (expected ';'):
error: the turn did not compile
~~~

The definition turn and the next turn succeed; the C error appears only when
`bad` is called. Name the function `h` instead and the definition turn
itself fails with the syntax error.

## Root cause

A turn's program is the whole definition table plus the turn's `main`, and
`jit_prune` (`src/compiler/jit_prune.c`) drops every function nothing live
names before c2mir sees the text. A defn no turn has called yet is not live, so
its inline C is never compiled. A one-letter name such as `h` is kept only
because the pruner matches identifiers, and some other `h` in the program
(a parameter, a local) names it -- which is why
`tests/turi/repl-compiled.sh`'s "a turn that fails to elaborate or compile
leaves the table as it was" sees the rejection: it passes by that accident,
with `(defn h ...)`.

## Fix directions

- In a definition turn, root the defined function (and its `__ffi` shim) for
  the pruner, so its C is compiled -- and rejected -- in the turn that
  defines it. Cheap: one more root, in the turns that define something.
- Then give the existing test a name that cannot collide (`(defn bad ...)`),
  so it asserts the behaviour rather than the accident.

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

## Resolution (2026-10-29)

The first fix direction. `turi_compiled_turn_run` (`src/turi/compiled_turn.c`)
collects the C names of the functions the turn defines (its `EX_FN_DEF`
items, named by `emit_c_function_name`, the emitter's own
`raw_name_for_binding`). It hands them to the JIT pruner as extra roots for
that turn's compile only (`jit_prune_set_extra_roots`,
`src/compiler/jit_prune.c`). A defn's inline C is compiled, and a mistake in
it reported, in the turn that defines it, whatever its name. Functions
defined in earlier turns are still pruned when nothing reaches them, so a
session pays for its own new code, not its whole table.

The root check reads the function's name from the chunk's own text: the
pruner's name map grows as it fills, so the `NameEnt` a chunk points at may
have moved. The first draft read it through that pointer and ASan caught it.

`tests/turi/repl-compiled.sh`'s rejection check now uses `(defn bad ...)`,
and a new case checks that a hyphenated name (`get-it`) is rejected where it
is defined and that the session goes on (33/33).
`tests/check-jit-prune-baggage.sh` is unchanged (8/8).

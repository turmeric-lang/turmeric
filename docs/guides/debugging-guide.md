---
title: Debugging Turmeric Programs
category: Troubleshooting
description: Finding out why a program does not compile, panics, crashes or prints the wrong thing -- reading diagnostics, `tur explain`, the `tur debug` stepper, editor debugging over DAP, `tur build --debug` under lldb/gdb, sanitizers for inline C, and `tur demangle`
---

# Debugging Turmeric Programs

This guide is for the moment something is wrong with **your program**: it does
not compile, it panics, it crashes, or it prints the wrong answer. If you
suspect the compiler itself is wrong, read
[Diagnosing the Compiler](diagnosing-the-compiler-guide.md) instead.

## Which tool, for which symptom

| Symptom | Start with |
|---|---|
| A compile error you do not understand | [`tur explain <code>`](#reading-a-compile-error) |
| A macro expands to something surprising | [`tur expand`](#macros-and-types) |
| `panic at ...` and exit code 134 | [`tur debug`](#stepping-through-a-program-tur-debug) or [lldb](#native-debugging-with-lldb-or-gdb) |
| Wrong output, no crash | [`tur debug`](#stepping-through-a-program-tur-debug) with a conditional breakpoint |
| "How did this value get here?" | [`tur trace`](#going-backwards-tur-trace) -- step backwards |
| Segfault, bus error, or memory corruption | [`--debug` plus lldb](#native-debugging-with-lldb-or-gdb), then [sanitizers](#memory-errors-in-inline-c-sanitizers) |
| Works interpreted, fails compiled (or the reverse) | [The two back ends](#compiled-versus-interpreted) |

Every example below uses this program, saved as `boom.tur`:

```turmeric
(defn checked-div [a : int b : int] : int
  (if (= b 0)
    (panic "division by zero")
    (/ a b)))

(defn average [total : int count : int] : int
  (+ 0 (checked-div total count)))

(defn main [] : int
  (println (average 10 2))
  (println (average 10 0))
  0)
```
```sweet-exp
defn checked-div [a : int b : int] : int
  if =(b 0)
    panic("division by zero")
    /(a b)

defn average [total : int count : int] : int
  +(0 checked-div(total count))

defn main [] : int
  println(average(10 2))
  println(average(10 0))
  0
```

(`average` adds `0` only so that its call to `checked-div` is not a tail call;
[Tail calls leave no frame](#tail-calls-leave-no-frame) explains why that
matters for a backtrace.)

---

## Reading a compile error

`tur check` type-checks a file without generating or compiling any C, so it is
the fastest way to see every error:

```sh
tur check boom.tur
```

An error names the file, line and column, carries a code, and points at the
expression:

```
typo.tur:5:20: error [TUR-E0001]: function 'area' arg 2: expected int, got cstr
4 | (defn main [] : int
5 |   (println (area 3 "four"))
  |                    ^^^^^^
```

`tur explain` prints the long form of any code, with an example of the mistake
and how to fix it. Pass the whole code, including the `TUR-` prefix:

```sh
tur explain TUR-E0001
```

For tooling, `tur check --json-diagnostics boom.tur` prints each diagnostic
as a JSON object with `severity`, `code`, `message`, `file` and a span. An editor running the language server shows the same
diagnostics as you type -- see the [LSP guide](lsp-guide.md).

## Macros and types

When a macro does something you did not expect, look at what it produced.
`tur expand` prints every expansion in the file, inner-first, each under a
`;; <macro-name> @ line:col` header:

```sh
tur expand boom.tur
```

In the REPL, `:expand <form>` expands a single form and `:type <expr>` shows
the type the checker infers -- often the quickest way to find out why a call
does not type-check. See the [REPL guide](repl.md).

## When a program panics

A panic prints a message and stops the program with exit code 134 (`SIGABRT`):

```
$ tur run boom.tur
5
panic at boom.tur:3: division by zero
```

The location is the `(panic ...)` that fired: the source file's name and its
line. `tur --interpret` prints the same line, with exit code 1. A failed
`cast` names the cast the same way: `panic at boom.tur:7: cast: any holds
Point, not Other`. A panic raised inside a runtime helper (a Saffron dynamic
operator, say) still names a line in the generated C. The location does not say how your code got there. For the
calls that led to it, use either of the next two sections: `tur debug`
shows the Turmeric call stack directly, and a `--debug` build under lldb
stops at the panic with the full stack (see
[the panic recipe](#finding-where-a-panic-came-from)).

A failed contract is a panic too. Its location is the line the predicate is
written on, and its message names the kind of check, the function and the
predicate: `panic at boom.tur:2: Precondition failed in safe-div: (not= b 0)`.
The same two tools find how the call got there.
Contracts and how to turn them off are covered in the
[contract types guide](contract-types-guide.md).

## Stepping through a program: `tur debug`

`tur debug` runs the program under the interpreter and stops at the first form
of `main` (or at the file's first form, if it has no `main`), with a
`(tur-dbg)` prompt:

```
$ tur debug boom.tur
stopped at boom.tur:9
=> 9    (defn main [] : int
(tur-dbg) break 3 if (= b 0)
breakpoint 1 set at 3 if (= b 0)
(tur-dbg) continue
5

stopped at boom.tur:3
=> 3        (panic "division by zero")
(tur-dbg) backtrace
  #0  checked-div  at boom.tur:3
  #1  average  at boom.tur:7
  #2  main  at boom.tur:11
(tur-dbg) locals
  b = 0
  a = 10
(tur-dbg) eval (+ a 1)
11
```

The commands:

| Command | Short | Does |
|---|---|---|
| `break <line> [if <expr>]` | `b` | Set a breakpoint on a line of the file, optionally conditional |
| `delete [n]` | | Clear breakpoint `n`, or all of them |
| `continue` | `c` | Run to the next breakpoint |
| `step` | `s` | Step into calls |
| `next` | `n` | Step over calls |
| `finish` | `fin` | Run until the current function returns |
| `backtrace` | `bt`, `where` | Print the call stack |
| `locals` | `l` | Print every binding in scope |
| `print <name>` | `p` | Print one binding |
| `eval <expr>` | `e` | Evaluate any expression in the current frame |
| `list` | `ls` | Show the source around the current line |
| `quit` | `q` | Abort the program |

Breakpoints take a **line number**, not a function name. The condition is any
Turmeric expression, evaluated in the frame that reaches the line.

### Tail calls leave no frame

A call in tail position replaces its caller's frame rather than pushing a new
one. If `average` were written `(checked-div total count)` -- a tail call --
the backtrace above would go straight from `checked-div` to `main`. A frame
missing from a backtrace is almost always this, and it applies to compiled
programs under lldb too.

`tur debug` runs on the interpreter. Inline-C blocks and the few features the
interpreter does not support (see [the two back ends](#compiled-versus-interpreted))
need the native route below.

## Debugging in your editor

`tur dap` speaks the Debug Adapter Protocol, so any DAP client can drive the
same debugger with breakpoints in the gutter, a call-stack view, and locals.
The repository ships a VS Code extension for it; its
[README](https://github.com/turmeric-lang/turmeric/blob/main/editors/vscode-turmeric/README.md)
covers installation and `launch.json`. With the extension installed, open a
`.tur` file and press **F5**.

## Going backwards: `tur trace`

Sometimes the useful question is not "what is this value" but "how did it
become this value". `tur trace` records an interpreted run, and `tur dap` can
replay a recording so a debugger steps **backwards**:

```sh
tur trace boom.tur -o run.turtrace
```

The [time-travel tracing guide](time-travel-tracing-guide.md) covers the
format, the step cap, and reverse execution in VS Code.

## Native debugging with lldb or gdb

For a compiled program -- including crashes inside inline C, and anything the
interpreter cannot run -- build with `--debug`:

```sh
tur build --debug boom.tur -o boom
lldb ./boom
```

`--debug` writes `#line` directives into the generated C, so the debug
information points at your `.tur` file, and compiles with `-g -Og`. You can
set breakpoints by `.tur` file and line, and frames show Turmeric source:

```
(lldb) breakpoint set --file boom.tur --line 3
(lldb) run
* thread #1, stop reason = breakpoint 1.1
    frame #0: boom`checked_hydiv(a=10, b=0) at boom.tur:3:13 [opt] [inlined]
-> 3           (panic "division by zero")
(lldb) bt
  * frame #0: boom`checked_hydiv(a=10, b=0) at boom.tur:3:13 [opt] [inlined]
    frame #1: boom`average(total=10, count=0) at boom.tur:6:29 [opt] [inlined]
    frame #2: boom`main(argc=<unavailable>, argv=<unavailable>) at boom.tur:11:29 [opt]
(lldb) frame variable
(int64_t) a = 10
(int64_t) b = 0
```

`-Og` (not `-O0`) is deliberate: the optimizer is needed to drop unused
standard-library code from a single-file build. Expect lldb's `[opt]` marker,
`[inlined]` frames, line numbers that land on a function's first line rather
than the exact call, and the occasional `<unavailable>` variable. gdb works
the same way, with `break boom.tur:3`.

### Finding where a panic came from

A panic ends in `abort()`, so a debugger stops right there with the whole
stack. No breakpoint is needed -- run the program, then ask for a backtrace:

```
$ lldb ./boom
(lldb) run
panic at boom.tur:3: division by zero
* thread #1, stop reason = signal SIGABRT
(lldb) bt
    frame #3: boom`tur_panic_at(file="boom.tur", line=3, msg=...) at boom_tur.c:...
    frame #4: boom`checked_hydiv(a=10, b=0) at boom.tur:3:13 [opt] [inlined]
    frame #5: boom`average(total=10, count=0) at boom.tur:6:29 [opt] [inlined]
    frame #6: boom`main(...) at boom.tur:11:29 [opt]
```

The first frame below `tur_panic_at` is the call that panicked. In gdb, `run` then
`bt`.

### Reading the names: `tur demangle`

Turmeric names are spelled as C identifiers in a compiled binary, so
`checked-div` appears as `checked_hydiv` and `geom/vector/add2` as
`geom__vector__add2`. `tur demangle` turns them back:

```sh
tur demangle checked_hydiv            # -> checked-div
nm -g ./boom | tur demangle           # filter any tool's output
```

`--annotate` prints `source[mangled]` instead, so the C name stays greppable.
The scheme itself is in the [name mangling guide](name-mangling-guide.md).

### Pretty-printers

A source checkout includes pretty-printers that show `Option`, `Result` and
cons lists as Turmeric values (`(some 42)`, `(ok 14)`) rather than raw C
structs:

```
(lldb) command script import tools/debug/turmeric_lldb.py
(gdb)  source tools/debug/turmeric_gdb.py
```

## Memory errors in inline C: sanitizers

An out-of-bounds write in an inline-C block may not crash where it happens, or
at all. AddressSanitizer and UndefinedBehaviorSanitizer catch it at the faulting
access. `TUR_CC_FLAGS` replaces the flags `tur` passes to the C compiler, so
keep the defaults you need and add the sanitizers:

```sh
TUR_CC_FLAGS="-g -O1 -std=c99 -fno-strict-aliasing -fsanitize=address,undefined -fno-omit-frame-pointer" \
  tur build --debug oob.tur -o oob
./oob
```

With `--debug`, the report points into your `.tur` file:

```
==51650==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x603000000fc0 ...
    #0 0x000100e163fc in poke oob.tur:3
    #1 0x000100e163fc in main oob.tur:11
```

A line inside an inline-C block is attributed to the block, so read the block
around the reported line.

## Compiled versus interpreted

Turmeric has two back ends. `tur run` and `tur build` compile through C;
`tur interpret`, `tur debug`, `tur trace`, `tur dap` and the REPL use the
tree-walking interpreter. They run the same language, but a program can
behave differently on the two -- usually because of inline C, which the
interpreter does not run, or a feature with a documented interpreter carve-out.

When a program works on one and not the other, run both:

```sh
tur run boom.tur
tur interpret boom.tur
```

and check the [interpreter parity guide](turi-parity-guide.md) for the feature
involved. If neither explains the difference, it is likely a compiler bug --
see [Diagnosing the Compiler](diagnosing-the-compiler-guide.md).

## Looking at the generated C

For the compiled back end, `tur emit-c` prints the C that `tur build` compiles:

```sh
tur emit-c boom.tur > boom.c
```

It is long (the runtime is included), but searching it for a function's
demangled name, `checked_hydiv`, finds your code quickly. This is mostly useful
when a crash is in inline C, or when filing a bug.

## Print debugging

`println` works anywhere, prints `int`, `float`, `bool` and `cstr` values, and
is often the fastest tool. Print a label with the value so the output is
readable:

```turmeric
(defn average [total : int count : int] : int
  (println "average: count =")
  (println count)
  (+ 0 (checked-div total count)))
```
```sweet-exp
defn average [total : int count : int] : int
  println("average: count =")
  println(count)
  +(0 checked-div(total count))
```

## Before you dig: is it the right `tur`?

A surprising number of "impossible" errors come from running a different
compiler, or a different standard library, than you think:

- `tur --version` and `command -v tur` show which binary is on your `PATH`.
  A package-manager install and a source build can both be present.
- `TUR_STDLIB_DIR` overrides where the standard library is loaded from. Some
  version managers (mise and asdf shims among them) export it, which can pair a
  new compiler with an old standard library. If errors mention stdlib
  functions you did not call, check `echo $TUR_STDLIB_DIR`.

## See also

- [Error handling guide](error-handling-guide.md) -- `Result`, `Option` and
  `panic`, for designing failures rather than finding them
- [Compiler flags](compiler-flags-guide.md) -- diagnostic flags such as
  `--strict-effects` and `--warn-unused-result`
- [Diagnosing the Compiler](diagnosing-the-compiler-guide.md) -- when the bug
  is in Turmeric, not your program

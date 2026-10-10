---
title: tur repl -- REPL Reference
category: Getting Started
description: Reference guide for the `tur repl` interactive read-eval-print loop, covering startup, expression evaluation, meta-commands, and configuration
---

# tur repl -- REPL Reference

`tur repl` launches an interactive Turmeric read-eval-print loop.

```sh
tur repl              # interactive Turmeric prompt
tur repl --watch      # also auto-reload spice exports when source changes
                      # (see "Working with spices in the REPL" below)
tur repl --engine jit # build the enclosing spice in process via MIR
                      # (see "Building the spice in process" below)
```

---

## Starting and stopping

```
$ tur repl
Turmeric v0.x.0  (type :help for help, :quit to exit)
> 
```

Exit with `:quit`, `:q`, or `Ctrl-D`.

---

## Evaluating expressions

Type any Turmeric expression and press Enter.  The result is printed on the
next line.

```
> (+ 1 2)
3
> (* 6 7)
42
> "hello"
"hello"
```

Top-level definitions persist across expressions:

```
> (defn square [x :int] :int (* x x))
> (square 9)
81
```

---

## Multi-line input

If the parentheses in the current input are not balanced, the REPL keeps
reading additional lines (shown with a `  ` indent prompt) until the
expression is complete:

```
> (let [x 10
        y 20]
    (+ x y))
30
```

To abandon an incomplete expression, enter a blank line.

---

## Inline-C at the prompt (experimental)

The REPL is the tree-walking interpreter, so it runs an inline-C body only when
it recognises the shape: a simple `return` of an arithmetic expression, a field
accessor, a flat constructor. Any other body -- a loop, a branch with two
returns, a library call -- is `inline-C not supported in interpreter mode`.

On a build with the JIT engine (the default on 64-bit x86-64 and arm64),
`--enable=repl-jit-inline-c` compiles such a defn instead, the first time it
is called. Its own source goes through the real compiler and the in-process
MIR engine, so `#include`s, the C library, and loops all work:

````
$ tur repl --enable=repl-jit-inline-c
> (defn count-upper [s : cstr] : int
    ```c
    #include <ctype.h>
    int64_t n = 0;
    for (const char *p = s; *p; p++) if (isupper((unsigned char)*p)) n++;
    return n;
    ```)
=> #<fn count-upper>
> (count-upper "Hello Turmeric World")
=> 3
````

The first call costs one compile (~135 ms on a Release build); later calls go
straight to the compiled function, and redefining the defn compiles the new
body on its next call. `tur --interpret` honours the same flag.

Limits, each reported as an error value:

- Only the defn itself is compiled, so its body cannot call another Turmeric
  definition.
- Every parameter and the result must be an `int`-class, `float`, `cstr`,
  `bool` or `ptr<void>` value; the result may also be unit.
- Interpreter values stay on the interpreter's side. A `ptr<void>` parameter
  accepts nil or a handle that an earlier compiled call returned, so a
  `make-cell` / `cell-get` pair works. An argument that is memory the
  interpreter allocated (a vec it built, say) is refused, whatever the
  parameter's type.
- A `cstr` argument is borrowed: the C body may read it, but must not `free` it
  or keep it after it returns. An `:int` that smuggles a handle in from
  somewhere else is not checked; give the handle a real type.
- The inline-C must be the defn's whole body, and the defn must be written
  out, not produced by a macro.

The design, and the plan to compile whole turns, are in
[aot-compiled-repl-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/aot-compiled-repl-plan.md).

---

## Switching readers with `#lang`

A line beginning with `#lang ` is handled before evaluation and switches the
reader for the rest of the session:

```
> #lang turmeric/sweet
; reader set to sweet-exp (session reset)
```

The switch **resets the session** -- the source you have accumulated so far is
discarded, because it was read under the old reader and may not parse under the
new one.  Repeating the reader you are already in prints
`; reader already set to ...` and changes nothing.

The preloaded stdlib survives the reset.  It is pinned as a prelude prefix when
the REPL starts, and the reset rewinds to that pin rather than to zero, so
macros (`when`) and collection literals (`#map{...}`, `#set{...}`, `[...]`)
still resolve immediately after a switch.  That works only because the preload
is plain s-expressions, which parse under every reader; without the pin the
first literal after a switch failed with `unknown function or operator
'hamt-of'`.

---

## Meta-commands

Meta-commands begin with `:` and are processed before evaluation.

### `:help`

Prints a summary of all meta-commands.

```
> :help
Meta-commands:
  :help               show this help
  :quit  :q           exit the REPL
  :type <expr>        print inferred type without evaluating
  :expand <form>      expand the form's head macro once and print it
  :doc  <sym>         print documentation for a symbol or builtin
  :reload <file>      evaluate a .tur file into the current session
  :load-string "<src>"  evaluate source directly (\n for newlines)
  :run <file>         reset session, load file, auto-invoke (main)
  :reset              clear session and start fresh
  :pwd                print the working directory
  :cd [dir]           change the working directory (bare :cd goes home)
  :explain [code]     explain the most recent error, or a TUR-E#### code

Tutorial commands:
  :tutorial              list available tutorials
  :tutorial <name>       start a tutorial
  :tutorial <name> <n>   start a tutorial at step n
  :next                  go to next step
  :prev                  go to previous step
  :hint                  show hint for current step
  :skip                  skip current step
  :quit-tutorial         exit tutorial mode
  :tutorial-progress     show progress in current tutorial
```

### `:quit` / `:q`

Exits the REPL.  Equivalent to `Ctrl-D`.

### `:pwd` and `:cd [dir]`

`:pwd` prints the working directory; `:cd` changes it, resolving relative
paths against the current one.  Bare `:cd` goes to `$HOME`.

```
> :pwd
/Users/you/projects/demo
> :cd src
/Users/you/projects/demo/src
> :cd /nope
:cd /nope: No such file or directory
```

This moves the **running** process, so everything you have defined stays
in scope -- relative paths in a later `:reload` or `(load ...)` simply
resolve against the new directory.

When shell integration is active (see
[Shell integration](#shell-integration-osc-markers)), a successful `:cd` also
emits an OSC 7 `file://` report, the same notification terminals use to track
the working directory.  A host editor can follow along without restarting the
REPL or scraping its output.

**The reported path is logical, not resolved.**  `getcwd(3)` always answers
with symlinks expanded, so a REPL launched in `/tmp/demo` reports
`/private/tmp/demo` on macOS -- a path that never string-matches the one the
host holds.  `:pwd` and the OSC 7 report therefore use `pwd -L` semantics:
`$PWD` when it still names the current directory, falling back to `getcwd()`
when it does not.  The fallback is guarded by a `(device, inode)` comparison,
so the answer is never a path that is not this directory:

```
$ cd /tmp/demo          # /tmp is a symlink to /private/tmp on macOS
> :pwd
/tmp/demo               # matches the host's own path
```

Note that `:cd` itself still moves *physically*: `:cd ..` from a symlinked
directory lands in the real parent, where a shell's logical `cd` would go to
the symlink's parent.  When the two disagree, the reported path describes
where the process actually is.

### `:load-string "<src>"`

Evaluates source handed over directly, with no file involved.  The argument is
a single double-quoted literal with C-style escapes, so a whole multi-line
region collapses onto the one line the prompt reads:

```
> :load-string "(defn dbl [x :int] :int (* x 2))\n(dbl 21)"
=> 42
```

This exists for host editors implementing "run selection".  Without it the
only route was writing the region to a scratch file and sending
`(load "...")`, which pays a disk round-trip and leaves temp files behind for
something the evaluator does from memory anyway.

Results, `_` binding, Show-instance rendering, and error reporting are
identical to typing the same forms at the prompt -- it runs the same
evaluation path, not a separate one.  Definitions persist into the session.

### `:type <expr>`

Elaborates `<expr>` and prints the inferred type without evaluating it.

```
> :type (+ 1 2)
:int
> :type (fn [x :int] :int x)
(:int -> :int)
```

### `:expand <form>`

Expands the form's head macro exactly ONCE and prints the result --
template and `defmacro*` macros alike, including macros defined at
earlier prompts.

```
> (defmacro plus1 [x] `(+ ~x 1))
> :expand (plus1 41)
(+ 41 1)
> :expand (cond a 1 else 2)
(if a 1 (cond else 2))
```

One step at a time: run `:expand` again on the printed result to walk a
recursive macro's unfolding (or use `tur expand <file>` for the full
trace of a whole file).

### `:doc <sym>`

Prints brief documentation for a known builtin or user-defined symbol.

```
> :doc println
println -- print a value followed by a newline
> :doc +
+ -- integer or float addition
```

For user-defined functions the signature is printed:

```
> (defn square [x :int] :int (* x x))
> :doc square
square : (Int -> Int)
```

### `:reload <file>`

Evaluates the contents of `<file>` into the current session.  Useful for
loading a script into the REPL without restarting.

```
> :reload src/utils.tur
reloaded src/utils.tur
```

If the file cannot be opened or contains an error, a diagnostic is printed and
the session continues.

`:reload` and `:run` never change the session's own reader. A file written for
another reader -- a `.tur.sweet` file in a plain Turmeric session, say -- is
read under its own reader, exactly as `(load "file")` would read it, and the
prompt keeps reading what you type the way it did before. A file in another
*language* is declined by name: a `.scm` file needs a Scheme session, which
`tur repl --lang r7rs` starts.

> **Not to be confused with `(reload)`.**
> `(reload)` (a Turmeric form, no leading colon) rebuilds the *enclosing
> spice* and refreshes its FFI bindings -- a different mechanism from
> `:reload <file>` for loading a one-off `.tur` script.
> See [Working with spices in the REPL](#working-with-spices-in-the-repl).

---

## Output format

| Value type | Example output |
|-----------|----------------|
| Integer | `42` |
| Float | `3.14` |
| Boolean | `true` / `false` |
| String (`:cstr`) | `"hello"` |
| Nil | (nothing printed -- nil results are silent) |
| Closure | `#<fn square>` |
| Struct | `#<struct Point>` |
| Future | `#<future pending>` / `#<future resolved>` |
| Error | `error: <message>` (to stderr) |

---

## Colour output

ANSI colour is enabled automatically when both stdout and stderr are
connected to a terminal.  Diagnostics (type errors, parse errors) are
highlighted in colour when enabled.

To force-disable colour, redirect output through a pipe:

```
tur repl 2>/dev/null | cat
```

---

## Shell integration (OSC markers)

The REPL can emit the OSC escape sequences terminals and host editors use to
follow along without scraping output:

| Marker | Meaning |
|---|---|
| `OSC 133;A` | a prompt is about to be written -- the REPL is **idle** |
| `OSC 133;C` | input accepted, evaluation starting -- the REPL is **busy** |
| `OSC 133;D;<status>` | evaluation finished; `0` = ok, `1` = the form errored |
| `OSC 7` | working directory report, emitted at startup and after `:cd` |

`A` -> `C` -> `D` is the full idle/busy cycle, which is what lets a host show
a spinner, disable its "run" button while a form is evaluating, and tell a
failed form from a successful one without parsing stderr.

`OSC 133;B` (end of prompt, start of the typed command) is deliberately **not**
emitted.  Placing it correctly means writing it between the prompt string and
the user's keystrokes -- inside editline's own output -- which would require
embedding it in the prompt behind `\1..\2` non-printing guards that libedit
does not reliably honour, risking a visibly corrupted prompt.  `B` only serves
command extraction; busy/idle needs only `A`/`C`/`D`.

Markers are on automatically when stdout is a TTY.  Two environment variables
override that:

| Variable | Effect |
|---|---|
| `TUR_SHELL_INTEGRATION=1` | force **on** even when stdout is not a TTY |
| `TUR_NO_SHELL_INTEGRATION=1` | force **off** (wins if both are set) |

The force-on switch is what a GUI host wants.  Driving the REPL over pipes
rather than a pty is the common case for an embedding editor, and it is
precisely the case that gets no markers by default -- leaving the host unable
to distinguish "evaluating" from "waiting at the prompt", with no safe option
but to assume busy forever:

```sh
TUR_SHELL_INTEGRATION=1 tur repl    # markers even when stdout is a pipe
```

The default stays off over a pipe on purpose: emitting escapes into a
redirect nobody asked for would corrupt captured output.

---

## Editing and history

If the binary was built with `editline` support (the default on macOS and
most Linux distributions), the REPL provides:

- **Line editing** -- arrow keys, `Ctrl-A`/`Ctrl-E`, `Ctrl-K`, etc.
- **History** -- `Up`/`Down` arrows cycle through previous entries.
- **Completion** -- `Tab` completes known symbol names (when implemented).

Without editline, raw `fgets` input is used with no history or editing.

---

## Working with spices in the REPL

When you launch `tur repl` from inside a spice project (any directory
whose ancestor contains a `build.tur`), the REPL auto-discovers and
compiles the project into a shared library, then makes every exported
defn callable from the prompt:

```
$ cd ~/projects/my-spice
$ tur repl
Loaded spice from /home/me/projects/my-spice (5 exports)
turmeric> (add42 100)
=> 142
turmeric> (sh/mul 6 7)
=> 42
```

Each export is bound under **two** names so you can call it either way:

- **Bare** -- `(add42 100)`
- **Qualified** -- `(<module>/<defn> ...)`, e.g. `(sh/add42 100)`

The qualified form avoids collisions when two modules in the same
project export the same name.

### Cache layout

The compiled library and its symbol manifest live under
`.tur-repl-cache/` next to your `build.tur`:

```ascii
my-spice/
|-- build.tur
|-- src/
|   `-- lib.tur
`-- .tur-repl-cache/        <- auto-generated, gitignored
    |-- lib-0.so            <- shared library (one per process generation)
    `-- exports.manifest    <- module/defn -> mangled C symbol :: signature
```

The first time the cache directory is created, `.tur-repl-cache/` is
appended to your project's existing `.gitignore` (idempotently; no
`.gitignore` is created if one didn't already exist).

The `lib-<N>.so` filenames are generation-tagged so re-loads always
see a fresh `dlopen` handle. The cache is fully reproducible from
source -- deleting it just costs one rebuild on the next REPL start.

### Skipping the auto-build

If sources haven't changed since the last REPL invocation, the loader
sees the cached `.so` is newer than every `.tur` under `src/` and
skips the rebuild entirely:

```
$ tur repl                    # rebuild + load (~1s)
$ tur repl                    # nothing changed -- instant load
```

### Building the spice in process (`--engine jit`)

By default the loader shells out to `tur build --shared --bundle-deps` on
the spice root and `dlopen`s the result -- the `.tur-repl-cache/` flow above.
`--bundle-deps` links each `:spices` dep's modules into the library, so a
spice whose sources import a dep loads on its own; only the spice's own
exports are bound at the prompt. On a binary built with
`-DTUR_JIT=ON`, the whole spice can instead be compiled as one translation
unit **in process** through the MIR engine, skipping the subprocess, the
`.so`, and the `dlopen`:

```sh
tur repl --engine jit            # this session only
TUR_ENGINE=jit tur repl          # every session in this shell
```

...or `:engine "jit"` in the project's `build.tur`, which makes it the default
for anyone working in that project. Precedence is the same ladder `tur run`
uses: `--engine` > `TUR_ENGINE` > `build.tur :engine` > `"cc"`.

Cold spice load is roughly 3x faster this way, and every load compiles fresh
so there is no cached artifact to go stale. The trade is that it is a
different execution engine -- see
[jit-guide.md](jit-guide.md) for what differs from the `cc` path. On a binary
built without `-DTUR_JIT=ON`, asking for the `jit` engine is a hard error
rather than a silent fallback, because the two engines differ in semantics and
guessing which one you got is worse than being told.

> `tur --enable=jit repl` (the retired experiment spelling) is a hard
> `TUR-E0310` as of 0.38.0 -- drop the flag. Engine selection is the supported
> spelling.

### (reload) -- pick up edits without restarting

The REPL exposes a `(reload)` form that re-runs the build for the
current spice, swaps in the fresh library, and refreshes the symbol
bindings against the new function pointers:

```
turmeric> (add42 0)
=> 42
[...you edit src/lib.tur to change the body of add42...]
turmeric> (reload)
(reload) rebuilt 1 export
=> nil
turmeric> (add42 0)
=> 100
```

`(reload)` is well-behaved in every scenario:

| Situation | Output |
|---|---|
| No spice loaded, no project here | `(reload) no spice project here; nothing to reload` |
| No spice loaded, build.tur found | `(reload) loaded N exports from /path` (self-heal) |
| Spice loaded, no source changes | `(reload) no changes` |
| Spice loaded, source changed | `(reload) rebuilt N exports` |
| Build failed (e.g. compile error) | `(reload) failed; previous spice image left in place` |

The self-heal case matters most when your startup build failed (e.g.
a compile error in your spice): instead of restarting the REPL after
fixing the source, just type `(reload)`.

`definstance` is **idempotent** across `(reload)`: re-running a `definstance C [T]`
replaces the existing singleton entry instead of appending a duplicate, so a
typeclass-heavy session can `(reload)` freely without `instance already defined`
errors or ambiguous resolution from stale entries.

### --watch -- auto-reload on edit

`tur repl --watch` checks source freshness between every prompt and
fires `(reload)` automatically when any `.tur` file's mtime advances:

```
$ tur repl --watch
turmeric> (add42 0)
=> 42
[...you edit src/lib.tur...]
turmeric> (add42 0)
(reload) rebuilt 1 export
=> 100
```

The check is synchronous and runs right before each eval (one `stat`
call per `.tur` file in the build dir). There's no background thread
and no platform-specific filesystem watcher; polling at the prompt
cadence is sufficient because the user has to type *something* to
advance the loop anyway.

### Environment knobs

| Variable | Effect |
|---|---|
| `TUR_NO_AUTO_SPICE=1` | Skip discovery entirely. Useful for a pure-Turmeric REPL inside a project directory. |
| `TUR_BIN=<path>` | Override the executable used for the rebuild subprocess. Defaults to `tur` (PATH lookup). Helpful when running an in-tree dev build. |

### Calling spice defns: type marshaling

Arguments are marshaled per the defn's signature recorded in
`exports.manifest`. Each parameter falls into one of two **classes**:

- **`:int` class** -- `:int`, `:bool`, `:cstr`, `:ptr`, sized integer
  types (`:int8`, `:uint32`, ...). All passed in a 64-bit integer
  register.
- **`:float` class** -- `:float`, `:float32`, `:float64`. Passed in a
  vector register.

The marshaler accepts compatible Turmeric values:

```
turmeric> (sh/add42 100)         ; :int -> :int            OK
=> 142
turmeric> (sh/scale 2.5 4.0)     ; :float :float -> :float OK
=> 10
turmeric> (sh/add42 1.5)         ; :float into :int slot   REJECTED
error: ffi: 'sh/add42' arg 0: expected :int-class, got float
```

Auto-widening from `:int` to `:float` is allowed; the reverse is not
(it would lose precision). Arity mismatches surface as:

```
turmeric> (sh/add42)
error: ffi: 'sh/add42' expects 1 arg, got 0
```

### Current limits (v1)

- **Variadic exports** (`& rest :type`) are recognised but not
  callable from the REPL. The error message is explicit; the marshaling
  code for cons-list rest args lives in a later phase.
- **Struct / ADT returns** can't yet be reconstructed on the
  interpreter side. The error suggests sticking to primitive
  (`:int` / `:float` / `:cstr` / etc.) returns for now.
- **Spice exports are pre-bound**, so you don't need to import them --
  call them directly. A top-level `(import M :refer [...])` (or `:as`)
  does work at the prompt since 2026-10-10, for a module the REPL can
  find: one under the cwd, or in the stdlib.

### Troubleshooting

**`stale exports.manifest`** -- the loader found a symbol in
`exports.manifest` that isn't in `lib-N.so`. Either type `(reload)`
to rebuild against the current source, or delete the cache and
restart:

```sh
rm -rf .tur-repl-cache
tur repl
```

**`spice rebuild failed`** -- the underlying `tur build --shared`
subprocess reported a compile error. The full output is replayed.
Fix the source and type `(reload)` (no need to restart the REPL).

**`no dispatcher for shape`** -- a defn's arity exceeds the FFI
dispatcher table's coverage (default: arity 0..6). Regenerate with a
larger bound:

```sh
python3 tools/gen_ffi_dispatch.py --max-arity 8
# rebuild the tur binary
```

---

## Non-interactive use

`:reload` can be piped in to run a script and then drop to the REPL, or to
run a script non-interactively:

```sh
# Run a file and exit
printf ':reload myfile.tur\n:quit\n' | tur repl

# Capture output only (discard banner)
printf ':reload myfile.tur\n:quit\n' | tur repl 2>/dev/null \
    | sed '1d'   # strip the banner line
```

To run source without putting it on disk first, use `:load-string` -- escape
the newlines so the whole region arrives as one line:

```sh
printf ':load-string "(defn f [] 7)\\n(f)"\n:quit\n' | tur repl
```

### `TUR_STDLIB_DIR`

`TUR_STDLIB_DIR` overrides where the stdlib is loaded from.  Because it is an
ordinary environment variable it is inherited by every child process and
outlives the install that set it, so a stale value can point a freshly built
`tur` at a stdlib that has since moved or been deleted.

It is validated before use: if `$TUR_STDLIB_DIR/macros.tur` is not
readable, `tur` prints one line naming the variable, unsets it, and falls back
to the stdlib beside the binary.

```
$ TUR_STDLIB_DIR=/gone tur repl
tur: ignoring TUR_STDLIB_DIR=/gone (no readable macros.tur there); falling back to the stdlib beside the binary
```

A directory that *does* contain a stdlib is honoured silently -- an explicit
override remains an override, so pinning a host's bundled stdlib works as
expected.

---

## See also

- [eval-api.md](eval-api.md) -- C embedding API for programmatic use of libturi.
- `man tur-repl` -- man page with a concise option reference.

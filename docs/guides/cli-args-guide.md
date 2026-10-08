---
title: CLI Arguments Guide
category: Language Basics
description: Passing arguments to scripts with `*args*` and parsing them with `stdlib/args.tur`
---

# CLI Arguments Guide

Turmeric scripts can receive command-line arguments via the built-in `*args*`
cons list. For structured parsing -- flags, named options, subcommands, and
type coercion -- the standard library provides `stdlib/args.tur` with a
builder-pattern API.

## Passing Arguments to a Script

The program's own name is a second pre-declared global, `*argv0*` (a
`:cstr`): `argv[0]` of a compiled binary, the script path under
`tur --interpret`. `*args*` never includes it.

Use `--` to separate `tur run` flags from your script's arguments:

```sh
tur run script.tur -- input.txt output.txt --verbose
```

Everything after `--` is forwarded to the script as a cons list bound to
`*args*`.

## Raw Access via `*args*`

`*args*` is a pre-declared cons list of `:cstr` values. Each element is one
argument string, in order. The binding itself is typed `:int` (the elaborator
declares it as a global `:int` carrying the cons-cell pointer), so walking it
means `list-head` / `list-tail` from `stdlib/list.tur` plus a `::` ascription
to recover the element's `:cstr` type:

```turmeric
;; script.tur
(load "stdlib/list.tur")
(load "stdlib/str-build.tur")   ;; str-concat / int->str

(defn main [] : int
  (println (str-concat "arg 0: " (:: (list-head *args*) :cstr)))
  (println (str-concat "arg 1: " (:: (list-head (list-tail *args*)) :cstr)))
  0)
```
```sweet-exp
;; script.tur
load("stdlib/list.tur")
load("stdlib/str-build.tur")

defn main [] :int
  println $ str-concat "arg 0: " (:: list-head(*args*) :cstr)
  println $ str-concat "arg 1: " (:: list-head(list-tail(*args*)) :cstr)
  0
```

```turmeric
;; Walk all arguments.  `when` takes exactly one body form -- wrap two in `do`.
(load "stdlib/list.tur")

(defn print-args [args : int] : void
  (when (not (= args 0))
    (do
      (println (:: (list-head args) :cstr))
      (print-args (list-tail args)))))

(print-args *args*)
```
```sweet-exp
;; Walk all arguments
load("stdlib/list.tur")

defn print-args [args :int] :void
  when not({args = 0})
    do
      println (:: list-head(args) :cstr)
      print-args(list-tail(args))
print-args(*args*)
```

`cstr->parse-int` (from `stdlib/str.tur`) turns one of those strings into an
`:int`. It takes the raw `:int` cell value, so no `::` ascription is needed:

```turmeric
(load "stdlib/list.tur")
(load "stdlib/str.tur")
(load "stdlib/str-build.tur")

(defn main [] : int
  (let [n (cstr->parse-int (list-head *args*))]
    (println (str-concat "count: " (int->str n)))
    0))
```
```sweet-exp
load("stdlib/list.tur")
load("stdlib/str.tur")
load("stdlib/str-build.tur")

defn main [] :int
  let [n cstr->parse-int(list-head(*args*))]
    println $ str-concat "count: " int->str(n)
    0
```

> **Note:** `head` / `tail` are **not** bound in the compiled path -- a self-contained script that does
> not load `stdlib/list.tur` has to define its own inline-C `head`/`tail`
> stubs, which the interpreter then overrides with its natives (see the
> CLI-argument rule in `CLAUDE.md`). Note the interpreter's `head` yields the
> element as an `:int`, so the same `::` ascription is what makes `println`
> print the string rather than the pointer.

## Structured Parsing with `stdlib/args.tur`

For anything more complex than a positional argument or two, use
`stdlib/args.tur`. It provides:

- **Flags** -- boolean `--verbose`, `--help`
- **Options** -- `--input=file.txt` or `--input file.txt` with type coercion
- **Defaults** -- option values used when the flag is absent
- **Positional args** -- ordered non-flag arguments
- **Subcommands** -- arbitrarily nested `build`/`test`/`push` dispatch
- **Help generation** -- automatic `--help` output from the spec

### Import

```turmeric
(load "stdlib/args.tur")
(load "stdlib/str-build.tur")   ;; str-concat / int->str, for the examples below
```

```sweet-exp
load("stdlib/args.tur")
load("stdlib/str-build.tur")   ; str-concat / int->str, for the examples below
```

### Building a Spec

Build a spec with the `args/spec-*` functions, then call `args/parse`.
Two conventions to know:

- Registration names **include** the dashes (`"--verbose"`); result accessors
  (`args/has?`, `args/get-*`) take the name **without** dashes (`"verbose"`).
- An option's default is an `(Option cstr)`: `(some "1")` for a default of
  `1`, `(none)` to make the option required.
- A spec handle is an `ArgSpec` and a parse result is an `ArgResult` -- two
  distinct `defopaque` newtypes, so passing one where the other belongs is a
  compile error rather than a silently wrong pointer.

```turmeric
(defn main [] : int
  (let [spec (-> (args/spec-new)
                 (args/spec-prog "mytool")
                 (args/spec-flag "--verbose")
                 (args/spec-option "--input"  "string" (none))       ; required
                 (args/spec-option "--count"  "int"    (some "1"))    ; default 1
                 (args/spec-option "--output" "string" (some "out.txt")))
        result (args/parse spec *args*)]
    (if (args/error? result)
      (do
        (println (str-concat "error: " (args/error-msg result)))
        1)
      (do
        (when (args/has? result "verbose")
          (println "verbose mode on"))
        (println (str-concat "input:  " (args/get-str result "input")))
        (println (str-concat "count:  " (int->str (args/get-int result "count"))))
        (println (str-concat "output: " (args/get-str result "output")))
        0))))
```
```sweet-exp
defn main [] :int
  let [spec (-> (args/spec-new)
                 (args/spec-prog "mytool")
                 (args/spec-flag "--verbose")
                 (args/spec-option "--input"  "string" (none))       ; required
                 (args/spec-option "--count"  "int"    (some "1"))    ; default 1
                 (args/spec-option "--output" "string" (some "out.txt")))
        result (args/parse spec *args*)]
    if args/error?(result)
      do
        println $ str-concat "error: " args/error-msg(result)
        1
      do
        when args/has?(result "verbose")
          println("verbose mode on")
        println $ str-concat "input:  " args/get-str(result "input")
        println $ str-concat "count:  " int->str(args/get-int(result "count"))
        println $ str-concat "output: " args/get-str(result "output")
        0
```

```sh
tur run mytool.tur -- --input=data.csv --count=5
```

### Option Types

| Type string | Accessor | Notes |
|---|---|---|
| `"string"` | `args/get-str` | Returned as `:cstr` |
| `"int"` | `args/get-int` | Parsed to `:int` |
| `"float"` | `args/get-int` | Parsed to `:float` |
| `"bool"` | `args/get-bool` | `"true"`/`"1"` => true |

### Flags

Flags are boolean switches -- present means true, absent means false:

```turmeric
(args/spec-flag spec "--verbose")
(args/spec-flag spec "--dry-run")

;; At runtime:
(args/has? result "verbose")  ; => true/false
```
```sweet-exp
args/spec-flag(spec "--verbose")
args/spec-flag(spec "--dry-run")
;; At runtime:
args/has?(result "verbose")
; => true/false
```

### Positional Arguments

Positional arguments are everything that is not a flag or option value.
Access them as a cons list. **The list is in reverse order** -- the LAST
positional argument is the head:

```turmeric
(load "stdlib/list.tur")

(let [pos (args/positional result)]
  (println (str-concat "last positional:   " (:: (list-head pos) :cstr)))
  (println (str-concat "second from last:  " (:: (list-head (list-tail pos)) :cstr))))
```
```sweet-exp
load("stdlib/list.tur")

let [pos (args/positional result)]
  println $ str-concat "last positional:   " (:: list-head(pos) :cstr)
  println $ str-concat "second from last:  " (:: list-head(list-tail(pos)) :cstr)
```

```sh
tur run copy.tur -- src.txt dst.txt
```

### Subcommands

Register subcommands with their own specs. Each subcommand can have its own
flags, options, and nested subcommands:

```turmeric
(defn main [] : int
  (let [build-spec (-> (args/spec-new)
                       (args/spec-flag "--release")
                       (args/spec-option "--output" "string" (some "a.out")))
        test-spec  (-> (args/spec-new)
                       (args/spec-flag "--verbose")
                       (args/spec-option "--filter" "string" (none)))
        spec       (-> (args/spec-new)
                       (args/spec-prog "myapp")
                       (args/spec-subcommand "build" build-spec)
                       (args/spec-subcommand "test"  test-spec))
        result     (args/parse spec *args*)]
    (if (args/error? result)
      (do
        (args/print-help spec)
        1)
      (let [sub (args/subcommand result)]
        (cond
          (cstr-eq? sub "build")
            (let [r (args/sub-result result)]
              (println (str-concat "building, release: "
                                   (if (args/has? (.value r) "release")
                                     "yes"
                                     "no")))
              0)
          (cstr-eq? sub "test")
            (let [r (args/sub-result result)]
              (println (str-concat "testing, filter: "
                                   (args/get-str (.value r) "filter")))
              0)
          else
            (do
              (args/print-help spec)
              1))))))
```
```sweet-exp
defn main [] :int
  let [build-spec (-> (args/spec-new)
                       (args/spec-flag "--release")
                       (args/spec-option "--output" "string" (some "a.out")))
        test-spec  (-> (args/spec-new)
                       (args/spec-flag "--verbose")
                       (args/spec-option "--filter" "string" (none)))
        spec       (-> (args/spec-new)
                       (args/spec-prog "myapp")
                       (args/spec-subcommand "build" build-spec)
                       (args/spec-subcommand "test"  test-spec))
        result     (args/parse spec *args*)]
    if args/error?(result)
      do
        args/print-help(spec)
        1
      let [sub (args/subcommand result)]
        cond
          cstr-eq?(sub "build")
          let
            [r (args/sub-result result)]
            println $ str-concat "building, release: " (if args/has?(.value(r) "release") "yes" "no")
            0
          cstr-eq?(sub "test")
          let
            [r (args/sub-result result)]
            println $ str-concat "testing, filter: " args/get-str(.value(r) "filter")
            0
          else
          do
            args/print-help(spec)
            1
```

`cstr-eq?` is byte-wise content equality on `cstr`, from
`(import cstr :refer [cstr-eq?])`. It is not optional sugar: `=` has **no**
`cstr` overload at all, so `(= sub "build")` is a `TUR-E0006` operator-lookup
error rather than a comparison. `(eq? sub "build")` is the same comparison
reached through the auto-loaded `Eq[cstr]` instance, if you would rather not
import `cstr`.

```sh
tur run myapp.tur -- build --release
tur run myapp.tur -- test --filter=core --verbose
```

`args/sub-result` returns an `(Option ArgResult)` -- `(none)` when no
subcommand matched. The `cond` arms above have already established which
subcommand ran, so they take `(.value r)` directly; a caller that has not
should test `(.is-some r)` first.

Nested subcommands work the same way: call `args/sub-result` on the outer
result, then `args/subcommand` on the inner to walk the chain.

### Help Output

`args/print-help` writes usage text to stdout based on the spec:

```turmeric
(when (args/has? result "help")
  (args/print-help spec)
  (exit 0))
```
```sweet-exp
when args/has?(result "help")
  args/print-help(spec)
  exit(0)
```

Or just let the parser handle missing required options and check
`args/error?`:

```turmeric
(when (args/error? result)
  (println (args/error-msg result))
  (args/print-help spec)
  (exit 1))
```
```sweet-exp
when args/error?(result)
  println(args/error-msg(result))
  args/print-help(spec)
  exit(1)
```

### Cleanup

Both spec and result are heap-allocated. Free them when done:

```turmeric
(args/spec-free   spec)
(args/result-free result)
```
```sweet-exp
args/spec-free(spec)
args/result-free(result)
```

## API Reference

| Function | Signature | Description |
|---|---|---|
| `args/spec-new` | `-> ArgSpec` | Create an empty arg spec |
| `args/spec-prog` | `spec :ArgSpec name :cstr -> ArgSpec` | Set program name for help |
| `args/spec-flag` | `spec :ArgSpec name :cstr -> ArgSpec` | Register a boolean flag |
| `args/spec-option` | `spec :ArgSpec name :cstr type :cstr dflt :(Option cstr) -> ArgSpec` | Register a named option |
| `args/spec-subcommand` | `spec :ArgSpec name :cstr sub :ArgSpec -> ArgSpec` | Register a subcommand |
| `args/parse` | `spec :ArgSpec argv :int -> ArgResult` | Parse `*args*` against spec |
| `args/has?` | `result :ArgResult key :cstr -> :bool` | True if flag/option was supplied |
| `args/get-str` | `result :ArgResult key :cstr -> :cstr` | Get option value as string |
| `args/get-int` | `result :ArgResult key :cstr -> :int` | Get option value as int |
| `args/get-bool` | `result :ArgResult key :cstr -> :bool` | Get option value as bool |
| `args/positional` | `result :ArgResult -> :int` | Cons list of positional args |
| `args/subcommand` | `result :ArgResult -> :cstr` | Matched subcommand name |
| `args/sub-result` | `result :ArgResult -> (Option ArgResult)` | Parse result for subcommand |
| `args/error?` | `result :ArgResult -> :bool` | True if parsing failed |
| `args/error-msg` | `result :ArgResult -> :cstr` | Error description string |
| `args/print-help` | `spec :ArgSpec -> :void` | Print usage to stdout |
| `args/spec-free` | `spec :ArgSpec -> :void` | Free spec memory |
| `args/result-free` | `result :ArgResult -> :void` | Free result memory |

## CLAUDE.md Rule

The `CLAUDE.md` file for this project enforces a strict rule: **CLI arguments
must only be read via `*args*` or `stdlib/args.tur`**. Reading from
`g_tur_args` via raw inline C is forbidden. See `CLAUDE.md` for details.

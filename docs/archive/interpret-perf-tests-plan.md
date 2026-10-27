# Plan: `tur --interpret` Support for Performance Benchmarks

**Status:** Not started.

**Goal:** Make every benchmark under `performance-comparison/benchmarks/*/turi/`
runnable via `tur --interpret` (the tree-walking interpreter) so the benchmark
harness can collect "turi" timings without requiring a compile step.

---

## Background and Current State

The performance benchmark suite compares Turmeric (compiled), turi (interpreted),
C, Rust, Python, Clojure, and Racket.  The intended invocation for the turi row is:

```
build-rel/tur --interpret benchmarks/<category>/turi/<name>.tur <arg>
```

Running any turi benchmark today fails in two ways:

1. The interpreter never sets `*args*`, so CLI arguments are invisible to the
   benchmark (it always uses its `parse-first-arg` fallback value).
2. Every benchmark calls `parse-first-arg`, whose body is inline-C referencing
   the C global `g_tur_args` -- a symbol that does not exist at interpreter
   runtime.

There are four independent gaps that all need to be closed:

| # | Gap | Where |
|---|-----|--------|
| 1 | Inline-C bodies in benchmarks are interpreter-incompatible | Benchmark source files + reader/elaborator |
| 2 | CLI args (`*args*`) not injected into interpreter | `src/main.c` `cmd_eval` |
| 3 | `tur run file.tur <arg>` silently drops the trailing arg | `src/main.c` `cmd_run` |
| 4 | `build-rel/tur` is stale; `--interpret` not in that binary | build infrastructure |

Gaps 1 and 2 are blocking.

---

## Benchmark Authorship Principle

**Benchmark files must be written in Turmeric, not in C.**

Inline-C in a benchmark body means the benchmark is measuring C performance,
not Turmeric performance.  A sieve implemented as a thin Turmeric wrapper
around `calloc` / direct array indexing / `free` is a C sieve.  That defeats
the entire purpose of a language comparison.

Rules:
- The algorithm logic must be expressed in Turmeric using stdlib types
  (`vec`, `list`, `map`, etc.).
- Inline-C is only permitted where the benchmark is explicitly testing FFI
  or platform I/O (e.g. a file-I/O benchmark that calls `fopen`).
- If a stdlib function is missing that the benchmark needs (e.g. `vec/new`,
  `str->int`), add it to stdlib instead of inlining C in the benchmark.

Reader conditionals exist to handle the small number of cases where the
compiler and interpreter paths genuinely diverge (e.g. a platform syscall that
the interpreter needs a native shim for).  They are not a workaround for
writing the algorithm in C.

---

## Gap 0 -- Missing Stdlib Functions

Before the benchmark files can be rewritten in Turmeric, the stdlib gaps they
expose must be filled.  The table below lists every function needed by at least
one benchmark that does not yet exist (verified against `stdlib/vec.tur`,
`stdlib/list.tur`, `stdlib/str.tur`).

| Function | Signature | Needed by | Notes |
|----------|-----------|-----------|-------|
| `cstr->parse-int` | `[s :cstr] :int` | all (parsing `*args*`) | `cstr->int` exists but reinterprets the pointer; this one calls `strtoll` |
| `vec-new-filled` | `[size :int init :int] :int` | primes, matrix_multiply | `vec-new` exists but takes no args and starts empty |
| `vec-set!` | `[v :int i :int val :int] :nil` | primes, matrix_multiply | `vec-push!`/`vec-pop!` exist; random-access write does not |
| `bit-shr` | `[x :int n :int] :int` | monte_carlo_pi | Logical (unsigned) right shift; `>>` is arithmetic in Turmeric |
| `println-float` | `[x :float decimals :int] :nil` | monte_carlo_pi | `println` uses default float formatting; benchmark needs `%.6f` |

**Not** needed as new stdlib:
- `int->unit-float`: express as `(/ (as float64 x) 9007199254740992.0)` once `as float64` is confirmed working
- `vec-get`: already exists
- matrix multiply's triple loop: expressed as tail recursion over a flat `:float` vec (storing doubles bitcast to int64 via `(as :int x)` / `(as float64 x)`)

Functions that require inline-C and therefore also need a native interpreter
shim (registered in `src/turi/builtins.c`): `cstr->parse-int`, `bit-shr`,
`println-float`.  The others (`vec-new-filled`, `vec-set!`) are pure inline-C
wrappers that can also be handled by the interpreter's existing pattern
executor if the bodies are simple enough -- verify during implementation.

---

## Gap 1 -- Reader Conditionals for Compiler vs. Interpreter

### Problem

Some benchmark operations have no pure-Turmeric expression today because the
required stdlib functions don't exist yet (Gap 0) or because the compiled and
interpreted paths genuinely differ.  Reader conditionals let a single source
file provide both paths where they truly diverge -- but the goal is to
minimise their use by filling stdlib gaps first.

### Syntax

```turmeric
#?(:tur  <compiled-expression>
   :turi <interpreted-expression>)
```

`#?` is read as a special list.  The compiler picks the `:tur` branch and
discards `:turi`; the interpreter picks `:turi` and discards `:tur`.  Either
branch may be omitted (the form evaluates to `nil` in the omitted case).

Reader conditionals may appear anywhere a normal expression is legal: as a
function body, inside `let`, at top level, etc.

### Where to implement

**Parse phase** (`src/parser/`): The reader sees `#?(` and collects alternating
keyword/expression pairs into a new AST node `EX_READER_COND`.  The reader does
not need to know which mode is active -- it just captures both branches.

**Elaboration phase** (`src/compiler/elab_core.c`): A new elaboration rule for
`EX_READER_COND` discards the `:turi` branch and elaborates only the `:tur`
branch.  The compiled AST never sees the interpreter branch.

**Interpreter** (`src/turi/eval.c`): The `eval_expr` switch adds a case for
`EX_READER_COND` that picks the `:turi` branch and ignores `:tur`.

### `*args*` type change

`*args*` is currently declared as `:int` in the elaborator -- a raw int64_t
that happens to be a cons-cell pointer.  That type must change to a proper list
of strings so that ordinary stdlib list operations work on it in both the
compiled and interpreted paths.

The elaborator declaration in `elab_core.c` becomes:

```c
/* *args* : (list :cstr) */
b_args->type = type_list(TY_CSTR);
```

The compiled `main()` emitted by `emit_module.c` already builds a cons-cell
list; it just needs the cell layout to match stdlib `list.tur`'s `Cons` struct
rather than the ad-hoc `__tur_cons` it uses today.

Once that is done, the benchmarks need no helper and no reader conditional for
argument parsing -- they just use `*args*` as the list it is:

```turmeric
(let [n (if (nil? *args*) 1000 (str->int (first *args*)))]
  (println (fib n)))
```

`parse-first-arg` is deleted from every benchmark file.  No reader conditional
is needed for this case because `*args*` has the same type and the same runtime
layout in both paths.

### Example: sieve operations (primes benchmark)

The current `primes.tur` wraps `calloc`, direct array indexing, and `free` in
inline-C.  That is a C sieve.  It should be rewritten to use `vec` from
stdlib, which works in both the compiled and interpreted paths with no reader
conditional:

```turmeric
;; mark-multiples and count-primes-loop stay as pure Turmeric tail recursion.
;; The sieve is a vec<:int> of 0/1 flags.

(defn count-primes [limit : int] : int
  (let [s (vec/new (+ limit 1) 0)]
    (count-primes-loop s 2 limit 0)))
```

`vec/new`, `vec/get`, and `vec/set!` replace every inline-C helper.  No
`ptr<void>`, no `calloc`, no `free`, no reader conditional.  If any of those
`vec` functions are missing from stdlib they must be added there -- not
inlined as C in the benchmark file.

---

## Gap 2 -- CLI Argument Injection into the Interpreter

### Problem

`cmd_eval` in `main.c` has signature `static int cmd_eval(const char *path, bool use_color)`.
CLI arguments after the file path are never passed into the interpreter, so
`*args*` is always 0 (nil list).

### Fix: Inject `*args*` as a list of `:cstr` values

With the type change from Gap 1, `*args*` is now `(list :cstr)`.  The
interpreter builds the same cons-cell structure that the compiled `main()` now
emits (after the `emit_module.c` cell-layout fix) and writes it into the env:

```c
TuriValue args_list = turi_nil();
for (int i = extra_argc - 1; i >= 0; i--) {
    TuriValue s = turi_cstr(extra_argv[i]);  /* :cstr element */
    args_list = turi_cons(s, args_list);      /* prepend to list */
}
turi_env_set_global(env, "*args*", args_list);
```

`turi_cons` / `turi_cstr` use whatever cell layout stdlib `list.tur` expects,
so `(first *args*)` and `(nil? *args*)` work without any special-casing in the
benchmark files.

Change `cmd_eval`'s signature:

```c
static int cmd_eval(const char *path, bool use_color,
                    char **extra_argv, int extra_argc);
```

Callers (in `main.c`):

```c
/* Before: */
return cmd_eval(argv[2], !no_color && stderr_is_tty());

/* After: */
return cmd_eval(argv[2], !no_color && stderr_is_tty(),
                argv + 3, argc - 3);
```

---

## Gap 3 -- `tur run` Argument Forwarding Without `--`

### Problem

`run_all.sh` builds commands like:

```sh
cmd_arr=("tur" "run" "benchmarks/numerical/turi/fibonacci.tur" "1000")
```

In `cmd_run`, arguments that don't start with `-` after the first non-option
positional are silently ignored because `explicit_file` is already set:

```c
} else if (argv[i][0] != '-') {
    if (!explicit_file) explicit_file = argv[i];   /* "1000" is dropped here */
}
```

### Fix: Collect trailing positional args into passthrough

In `cmd_run`, after setting `explicit_file`, treat subsequent non-option
arguments as passthrough args (equivalent to `--`):

```c
} else if (argv[i][0] != '-') {
    if (!explicit_file) {
        explicit_file = argv[i];
    } else if (passthrough_start < 0) {
        passthrough_start = i;
    }
}
```

---

## Gap 4 -- Stale `build-rel` Binary and `run_all.sh` Binary Path

### Problem

`build-rel/tur` is dated May 13 and does not contain `--interpret` support.
The benchmark script uses `TUR="$(pwd)/../build/tur"` (debug build).

### Fix

1. Run `just release` to rebuild `build-rel/tur` after implementing Gaps 1--3.

2. Update `run_all.sh` to accept a `TUR` env-var override:

   ```sh
   TUR="${TUR:-$(pwd)/../build-rel/tur}"
   ```

---

## Gap 5 -- Add `tur run --interpret` Subcommand (Optional / Nice-to-Have)

Unify the two invocation forms so `run_all.sh` can use a single command
template for both the compiled and interpreted rows.

In `cmd_run`, detect `--interpret` among the flags and, when set, delegate to
`cmd_eval` instead of the compile+execute path:

```c
if (use_interpret) {
    return cmd_eval(explicit_file, !no_color && stderr_is_tty(),
                    passthrough_start >= 0 ? argv + passthrough_start : NULL,
                    passthrough_start >= 0 ? argc - passthrough_start : 0);
}
```

Update `run_all.sh` to use `$TUR run --interpret` for the turi row.

---

## Phase Order

| Phase | Change | Depends on |
|-------|--------|-----------|
| **INT-0a** | Vim + VSCode syntax rules for `#?(...)` (Gap 6) | -- |
| **INT-0b** | stdlib: `cstr->parse-int`, `vec-new-filled`, `vec-set!`, `bit-shr`, `println-float` (Gap 0) | -- |
| **INT-0c** | Interpreter native shims for `cstr->parse-int`, `bit-shr`, `println-float` | INT-0b |
| **INT-1** | Parser: `EX_READER_COND` node | -- |
| **INT-2** | Elaborator: discard `:turi` branch; change `*args*` type to `(list :cstr)` | INT-1 |
| **INT-3** | Interpreter: pick `:turi` branch in `eval_expr` | INT-1 |
| **INT-4** | CLI arg injection in `cmd_eval` (Gap 2) | -- |
| **INT-5** | `tur run` trailing-arg passthrough (Gap 3) | -- |
| **INT-6** | Benchmark files rewritten: remove `parse-first-arg`, use `*args*` + stdlib | INT-2, INT-3, INT-0b, INT-0c, INT-4 |
| **INT-7** | `tur run --interpret` flag (Gap 5) | INT-4 |
| **INT-8** | Rebuild `build-rel/tur`, update `run_all.sh` TUR path | INT-7 |

INT-0b through INT-6 are the minimum needed to make `tur --interpret` work for
all benchmarks.

---

## Testing

### Smoke tests

```sh
build/tur --interpret performance-comparison/benchmarks/numerical/turi/fibonacci.tur 10
# expected: 55

build/tur --interpret performance-comparison/benchmarks/numerical/turi/primes.tur 20
# expected: 8
```

### Reader conditional unit test (new fixture)

`tests/fixtures/reader-cond/input.tur`:

```turmeric
(println #?(:tur "compiled" :turi "interpreted"))
```

- Compiled expected output: `compiled`
- Interpreted expected output: `interpreted`

### Full benchmark run

```sh
cd performance-comparison
./scripts/run_all.sh numerical small
# All turi rows should have timing_s.median > 0 and no SKIP lines
```

---

## Gap 6 -- Syntax Highlighting for Reader Conditionals

The reader conditional form `#?(:tur ... :turi ...)` needs to be recognized by
both editor integrations so it is highlighted as structure rather than appearing
as an error or plain text.

### Neovim (`vim-syntax/syntax/turmeric.vim`)

Add a match for the `#?` dispatch prefix and a region for the conditional list:

```vim
" Reader conditional: #?(:tur ... :turi ...)
syn match turmericReaderCondPrefix /#?/ contained
syn region turmericReaderCond matchgroup=turmericReaderCondPrefix
    \ start=/#?(/ end=/)/
    \ contains=turmericReaderCondKey,turmericForm,@turmericExpr
syn match turmericReaderCondKey /:\(tur\|turi\)/ contained
    \ nextgroup=@turmericExpr skipwhite

hi def link turmericReaderCondPrefix Special
hi def link turmericReaderCondKey    Keyword
```

The `:tur` and `:turi` keys should be styled differently from regular keywords
(e.g. `Special` or a dedicated highlight group) so they stand out visually.

### VSCode (`vscode-syntax-ext/syntaxes/turmeric.tmLanguage.json`)

Add a new rule to the `repository` section:

```json
"reader-conditional": {
  "name": "meta.reader-conditional.turmeric",
  "begin": "#\\?\\(",
  "beginCaptures": {
    "0": { "name": "punctuation.definition.reader-conditional.turmeric" }
  },
  "end": "\\)",
  "endCaptures": {
    "0": { "name": "punctuation.definition.reader-conditional.turmeric" }
  },
  "patterns": [
    {
      "match": ":(tur|turi)\\b",
      "name": "keyword.other.reader-conditional-key.turmeric"
    },
    { "include": "#forms" }
  ]
}
```

Reference `"reader-conditional"` from the top-level `patterns` array (before
the general `forms` rule so it takes precedence).

Also add `#?` to `language-configuration.json`'s bracket/autoclosing pairs if
the file already lists `(` so the editor auto-completes `#?(` -> `#?()`.

### Phase order

Add **INT-0** before INT-1: editor syntax support can land independently and
does not block any other phase.

| Phase | Change | Depends on |
|-------|--------|-----------|
| **INT-0** | Vim + VSCode syntax rules for `#?(...)` | -- |

---

## Out of Scope

- JIT compilation or `dlopen` of C snippets from the interpreter.
- Making the interpreter competitive with the compiled path in performance --
  the point is a valid comparison row, not a fast interpreter.
- WASM / Emscripten support for reader conditionals (straightforward extension,
  not needed for the benchmark goal).

---
title: C Integration Guide
category: Interoperability
description: Foreign function interface (FFI) and C interop
---

# Turmeric <-> C Integration Guide

Turmeric compiles to C99. This means C integration is not a plugin API -- it is
the compilation target itself. You write Turmeric code that reaches into C
(and vice-versa) by making the generated C source do what you need; `tur build`
links the small Turmeric runtime for you automatically.

This guide covers the *static* story -- the library is known at build time
and the generated C calls it by name. For loading libraries at **runtime**
(`dlopen`/`dlsym`, the experimental `call-ptr` form) and for how C calls
work under `--interpret` and the REPL, see the
[Dynamic FFI Guide](ffi-guide.md).

This guide covers the two directions:

1. **Calling C from Turmeric** -- importing symbols with `extern-c`, writing
   inline C blocks, and understanding how the generated code interacts with
   your C headers.
2. **Calling Turmeric from C** -- using the generated C output as a static
   library or including the emitted source directly in a larger C project.

---

## The Compilation Model

Running `./build/tur build path/to/file.tur` internally does:

```text
Source -> Reader -> Elaborator -> Effect-lower -> CPS transform
        -> Borrow-checker -> Emit C99 -> cc -> executable
```

The emitter (`src/compiler/emit_*.c`) writes a self-contained `.c` file. For
multi-file builds it also emits a `_main.c` that pulls in the generated module
headers and defines `main()`. The runtime (defer frames, rc, panics, ...) is
carried by the compiler and linked automatically -- from the prebuilt
`libturt_runtime.a` / `libturi.a` archive under `--runtime=lib`, or compiled
alongside the generated code (set `TUR_RUNTIME_LIB` to point at the archive
explicitly).

To inspect the emitted C without building, use:

```sh
./build/tur emit-c path/to/file.tur
```

This makes debugging integration problems much easier because you can see
exactly what the C side of the equation looks like.

### Interpreter-only natives are not available to compiled code

A handful of list primitives -- `cons`, `head`, `tail`, `nil-value`, and
`cstr->parse-int` -- are **interpreter natives** registered by `tur run` / the
REPL (in `src/main.c`), *not* compiled stdlib functions. They are unbound when
you `tur build` / `tur emit-c` a file: you will see

```text
error: unknown function or operator 'cons'
```

even though the same program runs fine under `tur run`. This is why docstring
examples that read `(cons "/bin/ls" (cons "-l" 0))` work at the REPL but fail to
compile verbatim.

In compiled programs, build raw cons lists by one of:

- **Define local inline-C stubs** at the top of the file (the interpreter will
  shadow them with its natives automatically, so the file still runs both ways):

  ```turmeric
  (defn cons [value : int next : int] : int
    ```c typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
    __tur_cons_cell *c = malloc(sizeof(*c));
    c->head = (int64_t)value; c->tail = (int64_t)next;
    return (int64_t)(intptr_t)c;
    ```)
  ```

- **Use a stdlib helper** that *is* compiled -- e.g. `stdlib/args.tur`'s
  `args/parse` linearises the pre-declared `*args*` cons list for you, and
  `stdlib/list.tur` provides typed `Cons`/`tcons` cells.
- **Pass `0` (the empty list)** where an API accepts an empty cons list.
  (`stdlib/process.tur` no longer needs this: since stdlib-os-surface P1 its
  argv is variadic -- `(process/run "/bin/ls" "-l" "/tmp")`.)

See the "CLI Argument Parsing" rule in `CLAUDE.md` for the `*args*` conventions.

---

## Calling C from Turmeric

### `extern-c` -- Importing a C symbol

```turmeric
(extern-c function-name [arg-types...] return-type)
```

```sweet-exp
extern-c function-name [arg-types...] return-type
```

`extern-c` declares that a C function (or global) with the given name is
available at link time. The elaborator trusts the signature entirely -- there
is no validation against an actual header file.

**Examples from the stdlib:**

```turmeric
;; libc file I/O (stdlib/io.tur)
(extern-c fopen  [^cstr ^cstr] :ptr)
(extern-c fclose [^ptr]        :int)
(extern-c fread  [^ptr ^int ^int ^ptr] :int)

;; libc memory (stdlib/io.tur)
(extern-c malloc [^ptr size]  :ptr)
(extern-c free   [^ptr p]     :void)

;; libc RNG (stdlib/random.tur)
(extern-c rand  [^]       :int)
(extern-c srand [^int]    :void)
(extern-c time  [^ptr]    :ptr)
```

```sweet-exp
;; libc file I/O (stdlib/io.tur)
extern-c fopen  [^cstr ^cstr] :ptr
extern-c fclose [^ptr]        :int
extern-c fread  [^ptr ^int ^int ^ptr] :int

;; libc memory (stdlib/io.tur)
extern-c malloc [^ptr size]  :ptr
extern-c free   [^ptr p]     :void

;; libc RNG (stdlib/random.tur)
extern-c rand  [^]       :int
extern-c srand [^int]    :void
extern-c time  [^ptr]    :ptr
```

**Type annotation reference:**

| Turmeric type | Generated C type | Notes |
|---------------|-----------------|-------|
| `:int`        | `int64_t`       | Alias for `int64` |
| `:int8`       | `int8_t`        | -128 .. 127 |
| `:int16`      | `int16_t`       | -32 768 .. 32 767 |
| `:int32`      | `int32_t`       | -2 147 483 648 .. 2 147 483 647 |
| `:int64`      | `int64_t`       | Alias for `int` |
| `:uint8`      | `uint8_t`       | 0 .. 255 |
| `:uint16`     | `uint16_t`      | 0 .. 65 535 |
| `:uint32`     | `uint32_t`      | 0 .. 4 294 967 295 |
| `:uint64`     | `uint64_t`      | 0 .. 18 446 744 073 709 551 615 |
| `:float`      | `double`        | Alias for `float64` |
| `:float32`    | `float`         | IEEE 754 single-precision |
| `:float64`    | `double`        | IEEE 754 double-precision |
| `:bool`       | `bool`          | `<stdbool.h>` |
| `:cstr`       | `const char *`  | Null-terminated, borrowed |
| `:ptr`        | `void *`        | Untyped pointer |
| `:void`       | `void`          | For return types only |

When a C function is variadic (e.g. `printf`), declare it with just the fixed
arguments. The elaborator does not validate the variadic portion:

```turmeric
(extern-c printf [^cstr] :int)
(printf "count=%lld\n" count)   ;; extra args pass through unchecked
```

```sweet-exp
extern-c printf [^cstr] :int
printf("count=%lld\n" count)   ;; extra args pass through unchecked
```

**Globals and zero-argument functions:**

Use an empty arg list `[^]` for globals that are accessed as function calls
or zero-argument functions:

```turmeric
(extern-c stderr [^] :ptr)   ;; FILE* stderr -- accessed as (stderr)
(extern-c rand   [^] :int)   ;; int rand(void)
```

```sweet-exp
extern-c stderr [^] :ptr   ;; FILE* stderr -- accessed as stderr()
extern-c rand   [^] :int   ;; int rand(void)
```

### Inline C blocks -- Arbitrary C inside a Turmeric expression

Surround C source with triple backticks and a `c` tag:

```turmeric
(defn file-size [f]
  ```c
  FILE* file = (FILE*)f;
  long pos = ftell(file);
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, pos, SEEK_SET);
  return (int)size;
  ```)
```

```sweet-exp
defn file-size [f]
  ```c
  FILE* file = (FILE*)f;
  long pos = ftell(file);
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, pos, SEEK_SET);
  return (int)size;
  ```
```

The block is pasted verbatim into the generated function body. Turmeric
parameters are available by name as C local variables with their translated
types. You **must** provide an explicit `return` if the function has a
non-`void` return type.

### Naming `let`-bound locals from inline C

An inline-C block does not have to be a whole `defn` body -- it can sit in
statement position inside a mixed Turmeric body, and there it can name the
enclosing `let`-bound locals the same way it names parameters:

```turmeric
(defn history [db : int] : ptr<void>
  (let [raw (db-q db)]
    (let [^mut i 1]
      (let [key (rvec-get raw i)]
        ```c
        { struct { int64_t *data; size_t len; size_t cap; } *vec = (void*)raw;
          vec->data[i] = key; }
        ```))))
```

The local **is** the C variable, not a copy of it: reads see whatever the
Turmeric code last stored, and a write from inline C is visible to the
Turmeric code that follows.

Two rules decide whether a local is reachable this way, and both are about
there being exactly one thing the name could mean:

- The source name must already be a valid C identifier. `raw` and `key`
  qualify; `raw-vec`, `key?`, and `db/raw` do not -- they go through the
  mangler, and the mangled spelling is an unstable implementation detail you
  must not write down.
- The name must be unambiguous within the function. If two `let`s in the same
  `defn` both bind `key`, or a local shares a name with a parameter, neither
  is reachable -- there is no single spelling that could mean one and not the
  other. Rename one of them.

A local that fails either rule is not an error; it is simply not declared
under that name, so the C compiler reports it as undeclared. When that
happens, rename the local rather than guessing at a mangled spelling.

Locals bound by `match` arms follow the same rules. A `^atomic`,
`^thread-local`, or captured-by-a-lifted-body local does not participate --
those carry their own access protocol (an accessor call, an indirection) that
a bare name could not express.

Inline C is the escape hatch for anything the type system cannot yet express:
struct definitions, platform intrinsics, `#include`s for system headers inside
a function scope, etc.

**Style rule**: always place the closing ` ``` ` and the enclosing `)` on the
same line (` ```) `). Putting ` ``` ` on its own line causes Markdown renderers
to interpret it as the end of any surrounding code fence, breaking rendered
documentation.

**Important constraints:**

- The generated code is `c99 -pedantic`. Avoid GCC/Clang extensions unless
  you know the target will always use an extension-compatible compiler.
- Local `typedef`s and `struct` definitions inside inline C are fine (see
  `stdlib/random.tur` -- it defines `typedef struct Random Random` inside the
  inline block). They are scoped to that function.
- Do not rely on identifier names that look like Turmeric-mangled names
  (e.g. `tur__0`) -- these are unstable implementation details.
- `static` helpers *at file scope* (in an `extern-c` block, or a whole-file
  C shim) are fine, but be aware of ODR if the same function name is used in
  multiple inline blocks across files.
- **Do not define a helper *function* inside a `defn`'s inline-C body.** That
  body is spliced *inside* the emitted C function, so a function definition
  there becomes a C **nested function** -- a non-standard GNU extension (ISO C
  forbids nested functions), and a `static` storage class on one is rejected
  outright (`error: invalid storage class for function ...`) by clang and
  standard toolchains. Local `typedef`s and `struct` definitions are fine; a
  *function* is not. Hoist the helper into its own sibling `defn` (which emits
  a real file-scope function) and call it from the inline body via
  `__TUR_CNAME_<name>__` (see below). This is how `stdlib/digest.tur` factors
  its SHA-256 / MD5 block transforms out of the per-digest bodies.

**Hoisting a payload to file scope -- `/* __tur_include__: ... */`:**

An inline-C body is spliced *inside* the emitted C function, so anything that has
to sit at file scope -- a system `#include` whose header declares top-level
`static inline` functions (mbedTLS, anything pulling `psa/crypto.h`), a
`typedef`, a file-scope `static` helper -- goes in a `__tur_include__` marker
comment instead. `tur build` scans the generated TU for those markers and
prepends each payload to the top of the file:

```c
/* __tur_include__: #include <stdlib.h> */
/* __tur_include__: typedef struct { char *s; } Owned; */
/* __tur_include__: static char *own(Owned *o, char *s) { ... } */
```

Payloads are sorted into two buckets -- preprocessor directives (`#include`,
`#define`, `#undef`, `#pragma`) ahead of **all** code payloads -- with source
order preserved within each bucket. So an `#include` supplied by one block covers
code hoisted from another regardless of which block appears first in the file,
and a feature-test `#define` still precedes the include it conditions. Spice
authors do not have to order their blocks defensively.

Because that ordering is fixed, no cc invocation site passes
`-Wno-error=implicit-function-declaration`: a missing prototype in inline C is a
hard build error, not a silently-wrong implicit declaration. Do not add the flag
back to paper over one -- fix the declaration.

Hoisting runs on the `tur build` path only. `tur emit-c` leaves the markers where
they sit, so an `emit-c` dump (or a fixture snapshot) is not the place to check
what the compiled TU actually looks like.

**Leading `#include` lines are hoisted automatically:**

You rarely need a `__tur_include__` marker for a plain header. An inline-C body's
*leading* directives -- blank lines, `//` comments, `#include <...>` /
`#include "..."`, and object-like `#define`s -- are lifted to file scope on their
own. This exists because a function-scope `#include` is swallowed by the header's
own include guard in the *second* function that needs it, leaving an implicit
declaration behind.

The scan is deliberately conservative: it stops at the first line that is not one
of those, so an `#include` sitting after real C code, or inside an `#ifdef`, is
left exactly where you wrote it.

```c
#include <sys/stat.h>     /* hoisted -- leading directive        */
#ifdef _WIN32
  #include <shlwapi.h>    /* NOT hoisted -- the scan stopped at the #ifdef */
#endif
```

**What happens when a hoisted header is not found:**

A hoisted `#include <X>` lands at file scope with no `#ifdef` around it, so an
angle include is wrapped in `#if __has_include(<X>)` to keep the emitted C
portable across targets. When the header *is* missing, the wrap does not rescue
the build -- the body that needed it is still compiled -- it only decides how the
failure reads. So:

- **Unmarked** (the default): the `#else` emits a `#pragma message` naming the
  header. You get `tur: inline-C requested <raygui.h>, not found on the include
  path` at the top of the cc output, ahead of the implicit-declaration and
  link errors it causes. Fix the `-I`.
- **Marked `tur:optional`**: skipped silently. Use this *only* for a header whose
  absence is expected on some target -- a per-platform alternative whose users
  are behind an `#ifdef`:

  ```c
  #include <direct.h>  /* tur:optional -- Windows only */
  #ifdef _WIN32
    return _mkdir(path);
  #else
    return mkdir(path, 0755);
  #endif
  ```

  This is the `stdlib/fs.tur`, `stdlib/term.tur` and `stdlib/image.tur` shape:
  the platform header goes at the top, bare, *outside* the `#ifdef` that uses it,
  so the hoister lifts it (avoiding the include-guard problem above) and the wrap
  drops it everywhere else. Any site marking a header optional makes it optional
  for the whole TU.

Quoted `#include "X"` is never wrapped. Those are project or vendored headers
where missing is a genuine error, and cc's own `No such file or directory` is
already the right diagnostic.

**Declare a prototype for anything you call:**

An unprototyped call in inline C is not a style nit, it is wrong code, and it is
wrong in two independent ways.

The implicit declaration returns `int`, so a 64-bit result is truncated on every
host -- that is how an emitted `tur_hamt_hash_xxh64` call quietly produced a
32-bit hash. And under `tur jit` it is worse: c2mir lowers a call with no
prototype in scope as all-anonymous-variadic, which on Apple arm64 passes the
arguments on the stack instead of in registers, so the callee reads whatever was
left in `x0`/`x1`. That is not a crash you get to notice -- `htons(8080)`
returned `0xb8f6` instead of `0x901f`, silently corrupting ports and header
lengths. (An explicit variadic with named parameters -- `f(const void *, size_t,
...)` -- is fine; it is specifically the fully unprototyped `f()` form that
breaks.) The same reasoning is written out at the `JIT_PRELUDE` / `_OSSwapInt16`
shims in `src/jit_engine.c`.

So `#include` the header, or write the `extern` declaration yourself -- through
`__tur_include__` when it has to be at file scope.

**Calling a sibling `defn` from inline C -- `__TUR_CNAME_<name>__`:**

When an inline-C body needs to call (or take the address of) another Turmeric
`defn`, do **not** hand-write that defn's mangled C identifier -- the mangling
scheme (`mangle.c`) is an internal detail that can change, and a stale spelling
fails at the C-compile stage (`implicit declaration of function ...`, a hard
error) with no Turmeric-level warning. Instead, splice the name with the
`__TUR_CNAME_<source-name>__` placeholder. The emitter expands it through the
same mangler the rest of the compiler uses, so the reference always tracks the
current scheme:

```turmeric
(defn tur-int-carrier-eq? [a : int b : int] : bool (= a b))

(definstance MapKey [int] (mk-box [x] x)
  ;; expands to the current mangled spelling of `tur-int-carrier-eq?`
  (mk-cmp [x] : int ```c return (int64_t)(intptr_t)__TUR_CNAME_tur-int-carrier-eq?__; ```)
  (mk-owned? [x] 0))
```

The source name between `__TUR_CNAME_` and the trailing `__` may contain sigils
(`-`, `?`, `!`, `=`, ...); it is terminated by the first `__`.

The splice resolves to the callee's **exact emitted C name**. When the name
resolves to a binding visible in scope, the expansion matches that binding's
full C identifier -- including the module prefix a global defined inside a named
module carries (`geom__helper_qu`), and any `(export-as "...")` C alias. So a
sibling `defn` inside `(defmodule geom ...)` is spliced correctly:

```turmeric
(defmodule geom
  (defn helper? [a : int b : int] : int (if (= a b) 1 0))
  (defn use-it [] : int
    ;; expands to geom__helper_qu (prefix included)
    ```c return (int)__TUR_CNAME_helper?__(7, 7); ```))
```

When the name does **not** resolve to a visible binding, the splice falls back
to the mangle-only spelling (no module prefix). This preserves the escape hatch
for referencing an unprefixed global in another translation unit that the
current module does not import -- e.g. stdlib carrier helpers referenced across
files without an explicit `import`.

### Capability structs -- The idiomatic pattern for C APIs

The stdlib uses **capability structs** to wrap C APIs behind a Turmeric-visible
interface. This pattern keeps the unsafe pointer juggling isolated:

```turmeric
;; stdlib/random.tur -- capability struct wrapping libc rand()
(defn Real-Random []
  ```c
  typedef struct Random Random;
  struct Random {
      int (*next_int)(int min, int max);
      int (*next_float)(void);
  };

  static int random_next_int(int min, int max) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return min + rand() % (max - min + 1);
  }

  static int random_next_float(void) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return rand() % 10000;
  }

  Random* rng = (Random*)malloc(sizeof(Random));
  rng->next_int  = random_next_int;
  rng->next_float = random_next_float;
  return (void*)rng;
  ```)

(defn Real-Random-free [rng]
  ```c free(rng); ```)
```

```sweet-exp
;; stdlib/random.tur -- capability struct wrapping libc rand()
defn Real-Random []
  ```c
  typedef struct Random Random;
  struct Random {
      int (*next_int)(int min, int max);
      int (*next_float)(void);
  };

  static int random_next_int(int min, int max) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return min + rand() % (max - min + 1);
  }

  static int random_next_float(void) {
      static int seeded = 0;
      if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }
      return rand() % 10000;
  }

  Random* rng = (Random*)malloc(sizeof(Random));
  rng->next_int  = random_next_int;
  rng->next_float = random_next_float;
  return (void*)rng;
  ```

defn Real-Random-free [rng]
  ```c free(rng); ```
```

The struct is returned as `:ptr` (opaque `void *`) and freed explicitly. This
is intentionally manual -- `rc<T>` and `weak<T>` cannot track arbitrary C heap
memory yet, so the caller is responsible for cleanup.

### Returning `result` / `option` from inline-C -- use the preamble helpers

When a C constructor is **fallible** -- it allocates or acquires a handle in C
and can fail (open a device, connect a socket, parse a file) -- the right
return type is a real `(Result Handle E)` or `(Option Handle)`, **not** a
`:ptr<void>` and **not** a magic-sentinel `:int` (`-1`, `0`-as-absent,
`INT64_MIN`).

You do not need to hand-roll the result struct. Every emitted translation unit
carries a small set of preamble helpers that build Option/Result values through
the **canonical** heap layout (the same one `stdlib/option.tur` and
`stdlib/result.tur` use), so a value built in C flows straight into the stdlib
accessors and vice versa. Prefer the **typed** builders -- they spell out the
payload's cast direction, so a pointer handle needs no hand-written
`(int64_t)(intptr_t)` widening:

| Helper | Builds | Payload |
|--------|--------|---------|
| `tur_ok_ptr(void *p)` / `tur_err_ptr(void *p)`   | ok / err `(Result A B)` | pointer handle, widened for you |
| `tur_ok_int(int64_t v)` / `tur_err_int(int64_t e)` | ok / err `(Result A B)` | integer code, as-is |
| `tur_some_ptr(void *p)` / `tur_some_int(int64_t x)` | some `(Option A)` | pointer / integer payload |
| `tur_none()` (or the `TUR_NONE` macro) | none `(Option A)` | -- (NULL) |

The carrier-level builders `tur_box_ok` / `tur_box_err` / `tur_box_some` (which
take the raw `int64_t` and need the explicit pointer cast) remain valid for
forwarding a payload that is already a carrier. The inspectors
`tur_is_ok` / `tur_ok_value` / `tur_err_value` / `tur_is_some` / `tur_opt_value`
read a Result/Option from inside an inline-C consumer.

```turmeric
(defopaque Device :ptr<void>)

;; Fallible C constructor: a *typed* (Result Device int), built with the typed
;; builders. No re-declaration of the result struct layout, no hand cast.
(defn open-device [id : int] : (Result Device int)
  ```c
  #include <stdlib.h>
  if (id < 0) return tur_err_int(22);      /* EINVAL */
  void *h = malloc(device_size());
  return tur_ok_ptr(h);
  ```)

;; And the consumer is plain Turmeric -- ok?/err?/ok-val all work:
(defn use-device [id : int] : int
  (let [r (open-device id)]
    (if (ok? r) (device-tag (ok-val r)) -1)))
```

The same shape works for `(Option Device)` via `tur_some_ptr` / `tur_none`.

**See [inline-c-results-guide.md](inline-c-results-guide.md)** for the full
helper table, a worked rtmidi-shaped example, the `_Static_assert` layout
guard, the two anti-patterns this replaces, and the `_int`/`_ptr`-only
limitation.

See `tests/fixtures/inline-c-result-builder/` (typed builders) and
`tests/fixtures/inline-c-typed-result-option/` (the carrier-level `tur_box_*`
builders) for end-to-end examples.

---

## Calling Turmeric from C

There are several ways to use compiled Turmeric code inside a larger C project:

- **`tur build --shared <dir>`** builds the project as a shared library.
- **`tur emit-cmake`** publishes a Turmeric library for consumption by C and
  C++ projects via CMake or CPM -- see
  [using-turmeric-from-cmake.md](using-turmeric-from-cmake.md).
- **`libturi.a`** provides a C embedding API for evaluating Turmeric
  expressions and calling Turmeric functions from a C host -- see
  [eval-api.md](eval-api.md).
- Or include the emitted `.c` directly, as below.

### Include the emitted `.c` directly

```
./build/tur emit-c mylib.tur > generated/mylib.c
```

Then add `generated/mylib.c` (and `src/runtime.c`) to your C build. Declare
the Turmeric-emitted top-level `defn` functions with `extern` in a hand-written
header, and call them from your C code.

Name mangling is **reversible and injective**: a top-level
`(defn my-function ...)` becomes `my_function` in C, but sigils encode through
escape digraphs -- `-` -> `_hy`, `/` -> `_sl`, `_` -> `_un`, with `?`, `!`, `=`
and friends covered analogously -- so any Turmeric global name round-trips
cleanly to C and back. Closures and anonymous functions get mangled names
like `tur__closure_N`. See [name-mangling-guide.md](name-mangling-guide.md)
for the full table and the demangler. **Inside an inline-C body, prefer the
`__TUR_CNAME_<source-name>__` splice** (Inline C blocks) over hand-spelling the
mangled name; the splice tracks the live mangler so a future scheme change
does not silently break your code.

### `#[used]` -- retaining a symbol reached only from C

The compiler keeps a definition with external C linkage when it is reachable
through the Turmeric export/import + call graph. A defn whose mangled symbol is
reached **only** through a raw `extern` reference is invisible to that analysis:

- a hand-written cross-module inline-C bridge that calls another module's
  unexported helper by its mangled name, or
- a C-ABI callback taken **by address** (an Arrow C Data Interface `release`
  function, a `qsort` comparator, a signal handler) and stored in a struct
  field or passed to a C API -- never called from Turmeric.

Such a defn is demoted to `static` under separate compilation (and dropped
entirely by the single-main whole-program build shortcut, which inlines only
the entry module's transitive Turmeric imports), so the `extern` reference
dangles at link time. Mark it `#[used]` to force external linkage and keep its
module in the build:

```turmeric
;; sort.tur -- unexported helper reached by a bridge in group.tur
(defn #[used] __so-take [col : int perm : int n : int] : int
  ```c
  ... ```)

;; interop.tur -- C-ABI callback stored by address, never called from Turmeric
(defn #[used] ip-release-schema [schema : int] : void
  ```c
  ... ```)
```

`#[used]` goes before the name, like `#[no-unwind]`, and the two compose in
either order: `(defn #[used] #[no-unwind] name [...] ...)`. Prefer the
`__TUR_CNAME_<source-name>__` splice (above) over hand-spelling the mangled
name in the `extern` declaration that reaches a `#[used]` symbol.

### Subprocess / build-step integration

Use `./build/tur build` as a build step that produces an executable, then have
your C application invoke it as a subprocess. This is the zero-coupling option:
the Turmeric binary handles I/O independently.

### Linking the runtime

`tur build` links the runtime automatically. If you embed the emitted `.c` in
your own build instead, link the runtime archive (`libturt_runtime.a` or
`libturi.a` from the compiler's build tree; `TUR_RUNTIME_LIB` points `tur` at
it too). The defer-frame surface, declared in `src/runtime/runtime.h`, is
small:

```c
/* src/runtime/runtime.h (abridged) */

typedef void (*defer_fn_t)(void *env);

#define TUR_FRAME_MAX_DEFERS 32

typedef struct tur_frame {
    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];
    void *envs[TUR_FRAME_MAX_DEFERS];
    DeferMode modes[TUR_FRAME_MAX_DEFERS];  /* NORMAL / SUSPENDED / REPLAY */
    int n;
    struct tur_frame *parent;
    bool may_capture;
    struct EffectRow *effect_row;
} tur_frame;

void tur_frame_init(tur_frame *f, tur_frame *parent);
int  tur_frame_push_defer(tur_frame *f, defer_fn_t thunk, void *env);
void tur_frame_fire_lifo(tur_frame *f);
void tur_frame_fire_chain(tur_frame *f);
```

---

## Memory Management

Turmeric has three memory tiers. Understanding which tier a value lives in is
essential when crossing the C boundary.

### Arena (compile-time only)

The compiler itself uses a bump-allocator arena (`src/runtime/arena.h`). This is
**compiler-internal only** -- generated programs do not use it.

### Reference counting -- `rc<T>`

`rc<T>` is Turmeric's primary heap type. In generated C it is represented as a
pointer to an `RcControlBlock` followed immediately by the value. The control
block holds a strong count and a weak count.

```c
/* src/runtime/rc.h (abridged) */
struct RcControlBlock {
    uint64_t strong_count;
    uint64_t weak_count;
    void    *value;
    RcDropFn drop_fn;         /* NULL -> use free() */
    RcWalkFn walk_fn;         /* enumerates rc children for the cycle collector */
    uint8_t  value_type_kind; /* fixed-width TypeKind byte */
    uint8_t  color;           /* Bacon-Rajan cycle collector (GcColor) */
    bool     may_contain_cycles;
    /* ... cycle-collector bookkeeping (gc_index, gc_buffered, ...) */
};
```

**Pitfall:** If you receive an `rc<T>` across the C boundary (as a `void *`),
you are holding a raw pointer into Turmeric's reference-counting machinery.
Calling `free()` on it directly will corrupt the control block. Always let
Turmeric code manage `rc<T>` lifetimes; pass scalars or opaque `void *`
capability structs across the boundary instead.

**Pitfall:** Cycles in `rc<T>` graphs are broken by the Bacon-Rajan cycle
collector, but only Turmeric-managed `rc<T>` nodes are tracked. If you create
a cycle that involves a raw C pointer (e.g. a C struct that holds a `void *`
back to an `rc<T>`), the cycle collector will not see it and memory will leak.

**If you do drive a control block from C** (`src/runtime/rc.h`), two rules cover
the whole surface:

- **Never repoint `cb->value` by raw assignment.** Go through
  `rc_set_value(cb, val, glue)`, and pass the drop glue **explicitly**. A bare
  `NULL` there does not mean "leave it alone": it re-derives the free-capable
  default for the block's value type, which clobbers an explicit struct drop
  glue and strands everything that glue would have released. This is the
  codegen's own path -- `rc/of` allocates, then repoints through `rc_set_value`
  with the alloc's glue passed through unchanged.
- **A scalar payload needs no glue at all.** `rc_cb_alloc(size, kind, NULL)`
  places the value inline at `(cb + 1)`, inside the header's own allocation, so
  `free(cb)` in `rc_cb_free` reclaims it. Scalar types therefore default to a
  no-op glue (`inline_scalar_drop_fn`) -- freeing that interior pointer is an
  invalid free and aborts. Only a *separately* allocated payload, installed by
  `rc_set_value` or `tur_rc_from_ref`, wants a freeing drop function.

### Weak pointers -- `weak<T>`

A `weak<T>` holds only the control block pointer (strong count = 0 is allowed).
`upgrade` returns a value wrapped in `Option`; if the strong count has reached
zero it returns `nil`. Weak pointers crossing the C boundary have the same
concern as `rc<T>` -- do not `free()` them directly.

### Manual heap (`malloc`/`free` via `extern-c`)

When an inline C block or `extern-c` call allocates memory with `malloc`, that
memory is invisible to the cycle collector and the borrow checker. You must
`free()` it manually, typically with a matching `extern-c free` call or an
inline block. The stdlib consistently pairs allocating functions with a
corresponding `*-free` function (see `Real-Random-free` above).

**`defer` is the right tool here:**

```turmeric
(let [buf (malloc 1024)]
  (defer (free buf))
  ;; ... use buf ...
  )  ;; free fires here, even if an exception is thrown
```

```sweet-exp
let [buf malloc(1024)]
  defer free(buf)
  ;; ... use buf ...
  ;; free fires here, even if an exception is thrown
```

---

## The `defer` System

`defer` registers a cleanup thunk that fires in LIFO order at scope exit,
including on exception unwind. This maps directly to `tur_frame_fire_lifo` in
the runtime.

```turmeric
(let [f (fopen "data.bin" "rb")]
  (defer (fclose f))
  ;; ... read from f ...
  )   ;; fclose(f) called here
```

```sweet-exp
let [f fopen("data.bin" "rb")]
  defer fclose(f)
  ;; ... read from f ...
  ;; fclose(f) called here
```

**Maximum defers per frame:** `TUR_FRAME_MAX_DEFERS` = 32. Exceeding this at
runtime returns `-1` from `tur_frame_push_defer` (the generated code silently
ignores the error). Keep the number of defers per lexical scope under 32.

**`defer` and panics:** defer thunks registered before a `panic` fire in
reverse order during unwinding. A defer that itself panics during a panic
trips the double-panic guard, which prints `double panic: aborting` and calls
`abort()` immediately.

**`defer` and `return`:** Defers fire before `return` via `tur_frame_fire_chain`.
This means you can safely return from the middle of a scope that has registered
defers.

---

## Panics and Error Signaling

Turmeric has no throw/catch exception system. Recoverable failures are modeled
with `Result` / `Option`; unrecoverable ones go through `panic`:

```turmeric
(panic "something went wrong")   ;; prints to stderr, fires defers, aborts
```

```sweet-exp
panic("something went wrong")   ;; prints to stderr, fires defers, aborts
```

A panic can be intercepted at a controlled boundary with `catch-unwind`, which
runs a nullary thunk and returns an ordinary `Result` -- `(ok value)` on normal
return, `(err Panic)` if the thunk panicked. The unwind is implemented with
`setjmp`/`longjmp` under the hood. See
[error-handling-guide.md](error-handling-guide.md) for the full story
(`catch-panic-of`, `stdlib/panic.tur`, `--lint-panic`).

**From C:** If your inline C block or `extern-c` function needs to signal an
error, return a real `(Result T E)` or `(Option T)` built with the preamble
helpers (`tur_err_int`, `tur_ok_ptr`, ... -- see above) and check it in
Turmeric. `tur_panic(msg)` is callable from C too, but it unwinds only to a
`catch-unwind` boundary and aborts the process otherwise -- reserve it for
genuinely unrecoverable states.

**FFI boundaries and panics:** a panic must not unwind through C stack frames.
When Turmeric code is invoked as a callback from C, wrap the callback body in
`catch-unwind` so a panic is converted to a value before it reaches the C
caller.

---

## Type System Boundary Rules

| Turmeric concept | Safe to pass to C? | Notes |
|-----------------|--------------------|-------|
| `int`, `float`, `bool` | Yes | Map to `int64_t`, `double`, `bool` |
| `cstr` | Yes (read-only) | `const char *`; Turmeric owns the string data |
| `ptr` | Yes | `void *`; you manage the lifetime |
| `ref<T>` | No | Borrow-checker-managed; do not store across call |
| `rc<T>` | No | Contains control block; use `ptr` wrappers instead |
| `weak<T>` | No | Same issue as `rc<T>` |
| closures (annotated `^fat`) | Yes (as `int64_t`) | Unified-representation handle; see Callbacks |
| closures (unannotated) | No | Compiler chooses bare vs. fat; carrier is not stable across positions |
| structs (copy) | Yes (by value) | Passed as C value types |
| structs (move) | With care | Passing implies ownership transfer |

The golden rule: **use `ptr` (opaque `void *`) for any C-allocated resource
that crosses the boundary**, and keep `rc<T>`/`ref<T>` on the Turmeric side.

---

## Inline C and the Type Checker

The elaborator (`src/compiler/elab_*.c`) does not parse inline C. It treats an
inline block as a black box and trusts the annotated return type. This means:

- **Type mismatches in inline C are silent.** A block annotated `:int` that
  actually returns a `double *` will compile and then corrupt memory at runtime.
- **Undefined behavior is not caught by the borrow checker.** The borrow
  checker stops at the boundary of an inline block.
- **No `#include` is injected.** If your inline C calls `memcpy`, you need to
  either add an `extern-c memcpy` declaration or put `#include <string.h>` at
  the top of the inline block. The latter is valid C99 (an `#include` can
  appear anywhere a declaration can appear).

### Callbacks: `^fat` parameters are `int64_t` in inline-C

Under the unified closure representation, a function-typed parameter marked
`^fat` is emitted in the generated C signature as **`int64_t`** -- the
closure handle. Inside an inline-C body you can therefore name it directly
and dispatch it with the standard `TUR_APPLY*` macros:

```turmeric
(defn run-twice [^fat f x : int] : int
  ```c
  /* f is int64_t; TUR_APPLY1 reads the thunk from slot 0 of the
     fat-closure box and invokes it with x. */
  int64_t a = TUR_APPLY1(f, (int64_t)x);
  int64_t b = TUR_APPLY1(f, a);
  return (int)b;
  ```)
```

If your C side is a plain `extern-c` callback (a hand-written `int64_t (*)(int64_t)`
function pointer), declare it normally and pass it in -- the `^fat` parameter
auto-shims a bare fn-pointer into a one-cell fat box on the way in. Do not
spell the handle as `void *` in inline-C; the codegen agrees on `int64_t`,
and the unsafe-block capture scan inspects ascriptions, so a wrong
carrier type can hide a real capture from the checker. See
[fat-closure-annotation-guide.md](fat-closure-annotation-guide.md) for the
deeper rationale and `^fat` on return types.

`:fn` values are first-class: closures cross the boundary as `int64_t` and
must be annotated at the boundary.

### Calling a typed `fn` parameter from inline C -- `tur_poly_fn_t`

A function-typed parameter that is *not* marked `^fat` -- an ordinary
`f : (fn [int int] int)` -- arrives in inline C as a **`tur_poly_fn_t` struct**,
declared in the emitted preamble as:

```c
typedef struct { void *env;
                 int64_t (*fn)(void *, int64_t);
                 int64_t (*fn_cps)(void *, int64_t, struct DK *); } tur_poly_fn_t;
```

Call it **env first, then every argument, uncurried**. The declared `fn` field
type is the one-argument spelling, so cast the slot to the real arity:

```turmeric
(defn apply2 [f : (fn [int int] int) a : int b : int] : int
  ```c
  return ((int64_t (*)(void *, int64_t, int64_t))f.fn)(f.env, a, b);
  ```)
```

That is exactly what the compiler emits for `(defn apply2 [...] (f a b))`. The
pre-HKT spelling `(int64_t(*)(int64_t,int64_t))(intptr_t)f` is a hard C error
under the by-value HKT path (`aggregate value used where an integer was
expected`) -- `f` is a struct, not a handle.

Better still, do not hand-write the convention at all. A stdlib or spice copy of
a calling convention is a silent miscompile waiting for the convention to move,
and it stays broken for as long as no fixture loads the module. If the body can
be written in Turmeric (`(f init x)`), write it in Turmeric and let the compiler
generate the ABI.

---

## Build and Linking

### Building the compiler

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j --config Debug
```

The compiler binary is `build/tur`. A Release build uses
`-DCMAKE_BUILD_TYPE=Release` into a separate build dir.

### Compiler flags for generated code

The compiler invokes `$CC` (defaulting to `cc`) with:

```sh
-O2 -std=c99 -Wall -fno-strict-aliasing
```

(`-g -Og` instead of `-O2` under `--debug`.) The `TUR_CC_FLAGS` environment
variable **replaces** the default flag set entirely -- include the defaults if
you only mean to add to them.

**Pitfall:** modern clang/gcc treat an implicit function declaration as a hard
error in C99 mode, and the build pipeline deliberately does not pass
`-Wno-error=implicit-function-declaration`. Common sources of trouble in
inline C:

- Implicit function declarations (missing `#include`).
- Signed/unsigned comparisons when mixing `int64_t` with `size_t`.
- `int` vs `long` mismatches when calling libc functions that return `size_t`.

Cast liberally and include headers explicitly.

### Linking external libraries

`extern-c` imports must be resolvable at link time. In a project with a
`build.tur` manifest, declare libraries under `:build-opts`:

```
:build-opts {:link-libs ["m" "raylib"]}   ;; -> -lm -lraylib on the link line
```

A spice can also vendor C sources and include dirs with `:c-sources` /
`:c-includes` (see
[developing-spices-guide.md](developing-spices-guide.md)). For a one-off
single-file build, put `-L`/`-l` flags in `TUR_CC_FLAGS` (remembering it
replaces the default flags).

### Multi-file builds

```sh
./build/tur build src/main.tur   # compiles main.tur and any (import ...) deps
```

Each required module emits its own `.c` + `.h` pair. A generated `_main.c`
`#include`s all modules and defines `main()`. `extern-c` declarations in one
module are visible to C but not automatically shared between Turmeric modules --
repeat the declaration in each module that needs it, or factor them into a
shared `.tur` file.

---

## Common Pitfalls Summary

| Pitfall | Consequence | Fix |
|---------|-------------|-----|
| Calling `free()` on an `rc<T>` pointer | Heap corruption | Never cross this boundary; use `:ptr` instead |
| Annotating inline C with wrong return type | Silent type confusion or memory corruption | Run with `emit-c` and inspect the generated code |
| Missing `#include` in inline C | Implicit function declaration -> hard build error | Add `#include` at top of inline block (or via `__tur_include__`) |
| Creating a C<->rc cycle | Memory leak (cycle collector can't see C pointers) | Keep cycles entirely on one side |
| More than 32 defers in a single scope | Silent drop of excess defers | Split scope or refactor |
| `defer` panicking during a panic | Double-panic guard aborts | Keep defer bodies simple and non-panicking |
| Inline C that calls `longjmp` unexpectedly | Skips Turmeric defer/rc cleanup | Only use `longjmp` if you know the full unwind path |
| Storing a `ref<T>` across an `extern-c` call | Borrow checker does not track C call boundaries | Use copy or `rc<T>` for data that outlives a single call |
| Varadic `extern-c` with wrong arg types | UB at runtime | Check generated C with `emit-c`; cast explicitly in callers |
| `static` name collision in multiple inline blocks | ODR violation / linker error | Prefix static helper names with a module-specific prefix |
| Defining a helper *function* inside a `defn`'s inline-C body | `error: invalid storage class for function` / non-portable nested-function extension | Hoist it to a sibling `defn` (file-scope function), call via `__TUR_CNAME_<name>__` |
| Using `cons`/`head`/`tail` in compiled code | `error: unknown function or operator 'cons'` (they are interpreter-only natives) | Define inline-C stubs, use a stdlib list helper, or pass `0` for an empty list (see Interpreter-only natives) |

---

## Worked Example -- Wrapping a C Library

This example wraps a hypothetical `libmath` C library with a Turmeric module.

**libmath.h** (your C header):

```c
typedef struct Vec2 { double x, y; } Vec2;
Vec2  vec2_add(Vec2 a, Vec2 b);
double vec2_len(Vec2 v);
Vec2 *vec2_alloc(double x, double y);  /* heap-allocated, caller frees */
void  vec2_free(Vec2 *v);
```

**math_wrap.tur** (Turmeric wrapper):

```turmeric
;; Declare the functions we need
(extern-c vec2_alloc [^float ^float] :ptr)
(extern-c vec2_free  [^ptr]          :void)

;; vec2_add and vec2_len operate on C struct values, which we pass through
;; inline C since C-struct-by-value parameters are not expressible in extern-c
(defn vec2-add [a b]
  ```c
  #include "libmath.h"
  Vec2 *pa = (Vec2 *)a;
  Vec2 *pb = (Vec2 *)b;
  Vec2 *result = (Vec2 *)malloc(sizeof(Vec2));
  *result = vec2_add(*pa, *pb);
  return (void *)result;
  ```)

(defn vec2-len [v]
  ```c
  #include "libmath.h"
  Vec2 *pv = (Vec2 *)v;
  return vec2_len(*pv);
  ```)

(defn demo []
  (let [a (vec2_alloc 3.0 4.0)]
    (defer (vec2_free a))
    (let [len (vec2-len a)]
      (println len))))   ;; prints 5.0
```

```sweet-exp
;; Declare the functions we need
extern-c vec2_alloc [^float ^float] :ptr
extern-c vec2_free  [^ptr]          :void

;; vec2_add and vec2_len operate on C struct values, which we pass through
;; inline C since C-struct-by-value parameters are not expressible in extern-c
defn vec2-add [a b]
  ```c
  #include "libmath.h"
  Vec2 *pa = (Vec2 *)a;
  Vec2 *pb = (Vec2 *)b;
  Vec2 *result = (Vec2 *)malloc(sizeof(Vec2));
  *result = vec2_add(*pa, *pb);
  return (void *)result;
  ```

defn vec2-len [v]
  ```c
  #include "libmath.h"
  Vec2 *pv = (Vec2 *)v;
  return vec2_len(*pv);
  ```

defn demo []
  let [a vec2_alloc(3.0 4.0)]
    defer vec2_free(a)
    let [len vec2-len(a)]
      println(len)   ;; prints 5.0
```

Build with:

```sh
TUR_CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -L. -lmath" \
  ./build/tur build math_wrap.tur
```

(or, in a manifest-rooted project, declare `:build-opts {:link-libs ["math"]}`
in `build.tur` and run `tur build .`).

---

## Future Directions

These are not yet available but are plausible extensions:

- **`extern-struct`** -- import a C struct layout into the Turmeric type system,
  eliminating the need for opaque `:ptr` wrappers.
- **`rc<T>` with custom drop registration from user code** -- the `RcDropFn`
  field in `RcControlBlock` is wired and used internally; a user-facing way to
  register a custom destructor would let an `rc<T>` own a C-allocated resource
  directly.
- **Algebraic effects across the boundary** -- effects (`perform`/`handle`) are
  implemented using delimited continuations. Crossing the C boundary inside a
  `handle` block is not safe.

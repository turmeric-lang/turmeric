---
title: Dynamic FFI
category: Interoperability
description: Calling C libraries at runtime -- dlopen/dlsym, call-ptr, extern-c under the interpreter, and the JIT thunk engine
---

# Dynamic FFI Guide

Turmeric has two ways to call C. The **static** way -- `extern-c`
declarations and inline-C blocks compiled into the generated C -- is
covered by the [C Integration Guide](c-integration-guide.md). This guide
covers the **dynamic** way: loading a shared library at runtime with
`dlopen`, resolving a function with `dlsym`, and calling the resulting
pointer with `call-ptr` -- plus what happens to all of this under
`--interpret` and the REPL, where a JIT-enabled build synthesizes call
thunks at runtime with c2mir
([docs/archive/jit-ffi-c2mir-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/jit-ffi-c2mir-plan.md)).

Quick orientation:

| You want to... | Reach for | Build-time needs |
|---|---|---|
| Call a C library you can link against | `extern-c` (+ autolink / `:cmake-deps`) | headers + `-l` flag |
| Call a library chosen/found at runtime | `dlopen` / `dlsym` / `call-ptr` | nothing |
| Give a C library a callback into Turmeric | `callback-ptr` | nothing |
| Call C from the REPL / `--interpret` | either; both route through JIT thunks | a `-DTUR_JIT=ON` build |
| Wrap a C library as a reusable package | a spice with `:cmake-deps` | see [Developing Spices](developing-spices-guide.md) |

Every form on this page is stable -- `call-ptr` and `callback-ptr` graduated
from `--enable=jit-ffi` in 0.38.0 and need no flag. Everything here that
touches raw pointers still lives inside `(unsafe ...)` blocks and behind the
`TURI_CAP_FFI` capability in sandboxed interpreter environments.

---

## The dynamic loop: dlopen -> dlsym -> call-ptr

```turmeric
(defn main [] : int
  (unsafe
    (let [h (dlopen "libm.so.6")          ;; ptr<void> library handle
          p (dlsym h "cbrt")]             ;; ptr<void> function address
      (println (call-ptr p [:float -> :float] 27.0))))   ;; => 3
  0)
```

- **`(dlopen path)`** loads a shared library (`RTLD_LAZY`) and returns its
  handle. The path spelling is the platform's: `"libm.so.6"` on Linux,
  `"libm.dylib"` on macOS.
- **`(dlsym handle name)`** resolves a symbol address, or a null pointer
  when absent.
- **`(dlclose handle)`** unloads. Any pointer previously resolved from
  that handle is dangling afterwards -- `call-ptr` cannot save you from
  calling into an unmapped page, so close only when nothing will call in
  again.
- **`(call-ptr p [T1 T2 -> R] args...)`** invokes `p` as a C function of
  the stated signature. The signature vector is positional parameter
  types, `->`, one return type.

All four require an enclosing `(unsafe ...)` block. Under `--interpret` and in
the REPL, `call-ptr` and `callback-ptr` additionally need a `-DTUR_JIT=ON`
build, because that is where the c2mir thunk provider lives; a build without
the engine says so rather than misbehaving. The compiled path needs nothing --
it lowers to a plain cast-and-call.

### Signature vocabulary

| Type token | C type used | Register class |
|---|---|---|
| `:int`, `:int64`, `:uint64` | `long long` | integer |
| `:bool`, `:int8` / `:uint8` | `signed char` / `unsigned char` | integer |
| `:int16` / `:uint16` | `short` / `unsigned short` | integer |
| `:int32` / `:uint32` | `int` / `unsigned int` | integer |
| `:cstr` | `const char *` | integer |
| `:ptr` | `void *` | integer |
| `:float` | `double` | float |
| `:float32` | `float` (exact -- not widened) | float |
| `:void` / `:nil` | return position only | -- |
| a record type name | that record, by value | see below |

Widths are **exact**, and that matters most in **return** position: a C
function returning `int` leaves the upper half of the return register
unspecified, so declaring such a callee `-> :int` (64-bit) reads garbage
for negative results -- `-1234` comes back as `4294966062`. Declare the
width the callee actually returns (`-> :int32`) and the value is extended
correctly, by signedness. Argument position is more forgiving: any
integer-class value may be passed to any integer-class slot, and the cast
to the declared width happens at the boundary.

The **pointer expression** may be a `dlsym` result (`ptr<void>`) or a raw
`:int` address (e.g. one you got from inline C).

### Struct-by-value

A signature slot may name a `defstruct` (or any single-constructor record),
in parameter or return position:

```turmeric
(defstruct Point [x : int32 y : int32])

(unsafe
  (let [d (call-ptr distance-fn [Point Point -> :float] a b)
        p (call-ptr origin-fn   [-> Point])]
    ...))
```

There is no marshalling step in compiled code: a `defstruct` already emits
as the exact by-value C struct with the declared field types, so the
signature just names it. Requirements on the record:

- one constructor, record-style (a multi-variant sum has a tag, which no C
  API declares);
- not `:heap` (its ABI is a pointer -- declare the slot `:ptr`);
- monomorphic (a parametric record has no single layout);
- every field a scalar from the table above, or itself such a record
  (a nested by-value record is inlined, exactly as the emitted C inlines
  it; a field of concrete *parametric* monomorph type is refused under
  `--interpret` today).

Arguments are matched by **type identity**, not by shape: two records with
the same field types are still different C types, and passing one where the
other is declared is an error.

An aggregate whose fields are all the same floating-point type (an AAPCS64
*HFA* -- `{float, float}`, `{double, double, double}`, and so on) crosses
correctly in both directions on arm64, as does every other aggregate shape.

### Callbacks: `callback-ptr`

`(callback-ptr f [T1 T2 -> R])` goes the other way -- it hands C a function
pointer that runs Turmeric. The signature vector reads exactly like
`call-ptr`'s, and describes the callback's own parameters and return:

```turmeric
;; qsort reads only the SIGN of the result, and ptr-deref yields a unique
;; value usable once -- so subtract rather than compare twice.
(defn intcmp [a : ptr b : ptr] : int
  (unsafe (- (ptr-deref a) (ptr-deref b))))

(unsafe
  (let [q   (dlsym libc "qsort")
        cmp (callback-ptr intcmp [:ptr :ptr -> :int])]
    (call-ptr q [:ptr :int :int :ptr -> :void] buf 5 8 cmp)))
```

The callback must be a **top-level function**, named directly or written
inline with no captures. A C callback slot is a bare function pointer with
no room for a captured environment, so a closure cannot become one; a
capturing lambda, or a local holding a function value, is a compile-time
error.

Callbacks are **process-lifetime**. A C library holding a function pointer
has no way to announce that it is finished with it, so there is no safe
moment to reclaim one and no `callback-free!`.

A Turmeric error raised inside a callback cannot propagate: there is no
error channel through a C callback slot, and unwinding through the foreign
frames in between is not safe. The error is printed to stderr and the
callback returns a zero result, so handle failure inside the callback.

Aggregates cross in both directions: a callback may take a record by value
(the C caller's struct is rebuilt into a record before your function sees
it) and return one (packed back into the C caller's return slot).  The
same record requirements as `call-ptr` apply, including the aarch64 HFA
refusal under `--interpret` -- inbound, the native caller writes the SIMD
registers and MIR-generated code would read the general-purpose ones, the
same mismatch mirrored.

### How it executes

- **Compiled (`tur build` / `tur run`) and `tur jit`:** pure codegen. The
  emitted C is a cast-and-call --
  `((double (*)(double))(intptr_t)p)(27.0)` -- with each argument cast to
  its declared parameter type. Works in every build.
- **Interpreted (`--interpret`, REPL):** the signature is rendered to a
  ~10-line C thunk, compiled in-process by c2mir (the C front end of the
  MIR JIT the tree already vendors), cached per unique signature, and
  called with the marshalled arguments. `callback-ptr` works the same way
  in reverse: c2mir synthesizes a function with the stated C signature that
  calls back into the interpreter. Both require a **JIT-enabled build**
  (`-DTUR_JIT=ON`); an engine-less interpreter reports
  `... under --interpret requires a JIT-enabled build` -- a clean error,
  never a nil.

Thunk compilation costs milliseconds per *unique signature*, once per
process -- noise against tree-walking dispatch. A c2mir compile failure
surfaces as an error value with the diagnostic on stderr.

---

## Worked example: libzmq

[ZeroMQ](https://zeromq.org/)'s C API is flat, scalar-heavy, and
string-oriented -- a perfect fit for the current signature vocabulary.
Here is the zguide hello-world shape (REQ/REP ping-pong, both ends in one
process), written twice.

### Variant 1: fully dynamic (no libzmq at build time)

No headers, no link flags -- the library, and even the receive buffer's
`calloc`, are resolved at runtime. Runs identically compiled, under
`tur jit`, and under `--interpret` in a JIT build:

```turmeric no-check
;; Run with: tur run zmq-dyn.tur
(defn main [] : int
  (unsafe
    (let [z        (dlopen "libzmq.so.5")
          libc     (dlopen "libc.so.6")
          ctx-new  (dlsym z "zmq_ctx_new")
          socket   (dlsym z "zmq_socket")
          bind     (dlsym z "zmq_bind")
          connect  (dlsym z "zmq_connect")
          send     (dlsym z "zmq_send")
          recv     (dlsym z "zmq_recv")
          close    (dlsym z "zmq_close")
          term     (dlsym z "zmq_ctx_term")
          calloc-p (dlsym libc "calloc")
          free-p   (dlsym libc "free")
          puts-p   (dlsym libc "puts")
          ctx      (call-ptr ctx-new [-> :int])
          rep      (call-ptr socket [:int :int -> :int] ctx 4)   ;; ZMQ_REP
          req      (call-ptr socket [:int :int -> :int] ctx 3)   ;; ZMQ_REQ
          buf      (call-ptr calloc-p [:int :int -> :int] 1 64)]
      (call-ptr bind    [:int :cstr -> :int] rep "tcp://127.0.0.1:5555")
      (call-ptr connect [:int :cstr -> :int] req "tcp://127.0.0.1:5555")

      ;; REQ sends, REP receives + prints
      (call-ptr send [:int :cstr :int :int -> :int] req "ping" 4 0)
      (call-ptr recv [:int :int :int :int -> :int] rep buf 63 0)
      (call-ptr puts-p [:int -> :int] buf)                       ;; => ping

      ;; REP replies, REQ receives + prints
      (call-ptr send [:int :cstr :int :int -> :int] rep "pong" 4 0)
      (call-ptr recv [:int :int :int :int -> :int] req buf 63 0)
      (call-ptr puts-p [:int -> :int] buf)                       ;; => pong

      (call-ptr free-p [:int -> :void] buf)
      (call-ptr close [:int -> :int] rep)
      (call-ptr close [:int -> :int] req)
      (call-ptr term  [:int -> :int] ctx)))
  0)
```

Reading notes:

- Opaque C pointers (`ctx`, sockets, the buffer) ride as `:int` -- they
  are addresses, never dereferenced on the Turmeric side. `:cstr` is used
  where zmq reads a NUL-terminated string (`zmq_bind`'s endpoint, the
  outgoing message bytes).
- `ZMQ_REQ`/`ZMQ_REP` are `3`/`4`: C `#define`s do not survive into a
  dynamic symbol table, so constants must be restated (or wrapped once in
  a spice).
- The 64-byte buffer is calloc'd (zeroed) and the receive is capped at
  63, so it stays NUL-terminated for `puts`.
- Both messages are the same length; a reused buffer with mixed lengths
  would need re-zeroing between receives.

### Variant 2: extern-c (libzmq at build time)

When you can link against the library, declare the signatures once and
call the names directly -- arities and types are then checked at every
call site, and there is no experiment flag. The autolink hint embeds
`-lzmq` in the emitted C, so plain `tur run` works with no extra flags:

```turmeric no-check
(defn zmq/autolink-hint [] : int
  ```c
  /* __tur_autolink__: -lzmq */
  return 0;
  ```)

(extern-c zmq_ctx_new  []                                     :ptr)
(extern-c zmq_socket   [ctx :ptr typ :int]                    :ptr)
(extern-c zmq_bind     [s :ptr ep :cstr]                      :int)
(extern-c zmq_connect  [s :ptr ep :cstr]                      :int)
(extern-c zmq_send     [s :ptr buf :cstr len :int flags :int] :int)
(extern-c zmq_recv     [s :ptr buf :ptr len :int flags :int]  :int)
(extern-c zmq_close    [s :ptr]                               :int)
(extern-c zmq_ctx_term [ctx :ptr]                             :int)
(extern-c puts         [s :ptr]                               :int)

;; An inline-C allocation (unlike raw-malloc) is not move-typed, so the
;; binding can be passed to as many calls as needed.
(defn make-buf [n : int] : ptr<void>
  ```c
  return calloc(1, (size_t)n);
  ```)

(defn free-buf [p : ptr<void>] : nil
  ```c
  free(p);
  ```)

(defn main [] : int
  (unsafe
    (let [ctx (zmq_ctx_new)
          rep (zmq_socket ctx 4)    ;; ZMQ_REP
          req (zmq_socket ctx 3)    ;; ZMQ_REQ
          buf (make-buf 64)]
      (zmq_bind rep "tcp://127.0.0.1:5556")
      (zmq_connect req "tcp://127.0.0.1:5556")
      (zmq_send req "ping" 4 0)
      (zmq_recv rep buf 63 0)
      (puts buf)                    ;; => ping
      (zmq_send rep "pong" 4 0)
      (zmq_recv req buf 63 0)
      (puts buf)                    ;; => pong
      (free-buf buf)
      (zmq_close rep)
      (zmq_close req)
      (zmq_ctx_term ctx)))
  0)
```

Which to choose: `extern-c` when the dependency is known at build time
(better checking, no gate); the dynamic loop when it is not -- plugin
systems, optional accelerators, probing for a library that may not be
installed, or REPL exploration of a `.so` you just built. A packaged
wrapper should be a spice with `:cmake-deps`
([Developing Spices](developing-spices-guide.md#wrapping-a-c-library-with-cmake-deps)).

One footgun both variants share: a value from `raw-malloc` is
**move-typed** (unique) -- passing it to a function call consumes it, so
threading one buffer through several `extern-c` calls trips TUR-E0201.
Allocate long-lived C buffers from C (an inline-C helper as above, or
libc's `calloc` via `call-ptr`), which yields an ordinary non-unique
value.

---

## extern-c under the interpreter

In a JIT-enabled build, `--interpret` and the REPL give `extern-c` its
full meaning: on declaration, the symbol is resolved against the process
(`dlsym(RTLD_DEFAULT)`) and bound to a native that calls it for real
through the same thunk engine.

```turmeric
(extern-c strtol [s :cstr endp :int base :int] :int)
(defn main [] : int
  (println (strtol "123abc" 0 10))   ;; => 123 under --interpret (JIT build)
  0)
```

In an engine-less build, a small table of
known names (`exit`, `free`, `strlen`, `getenv`, `printf`, `puts`) works,
and everything else silently evaluates to nil. The known table also
*overrides* the thunk path in JIT builds, deliberately -- `free` must stay
a no-op in the interpreter (inline-C allocations are reproduced from the
interpreter's value arena, not raw malloc), `exit` must flush, and
`printf` marshals interpreter values rather than trusting a variadic ABI.

**Symbol resolution order** for interpreter extern-c: the process image --
`tur`'s own exported runtime (JIT builds link with `ENABLE_EXPORTS`) --
then anything loaded with `RTLD_GLOBAL`, which includes libraries the JIT
autolink step dlopened. A symbol from a library the process never linked
needs an explicit `(dlopen ...)` first (a handle-less global load is all
it takes; the handle can be ignored).

Struct-by-value crosses extern-c too: annotate the slot with a record name
(`[v : InAddr]`, or `: DivT` in return position) and the same requirements
as `call-ptr` apply -- the interpreter packs/rebuilds the record through
the thunk engine, and compiled code emits the record's C type in the
prototype.  Variadic declarations still fall back to the nil stub --
representable signatures only, loudly documented over silently wrong.

```turmeric
(defstruct DivT [quot : int32 rem : int32])
(extern-c div [a :int b :int] : DivT)      ;; div_t div(int, int)
;; (.quot (div 47 10)) => 4 under --interpret
```

(Redeclaring a symbol a libc header already declares -- `div` above --
still trips the C compiler on the *compiled* path, as with any extern-c;
use the real header type or a symbol of your own there.)

Floating-point aggregates cross by value on every backend, including
`struct { float x, y; }`-flavoured vector APIs. On aarch64 such a record is an
AAPCS64 HFA and travels in `v0..v7` rather than the general-purpose registers;
the pinned MIR fork implements that rule, so `tur jit`, `tur run`/`tur build`
and the interpreter's `call-ptr`/`callback-ptr` all agree with a natively
compiled callee:

```turmeric
(defstruct Vec2 [x : float y : float])
(extern-c length [v : Vec2] : float)   ;; float length(Vec2)
```

---

## Where the thunks come from (and the fallback ladder)

Under the hood, one provider serves every dynamic call in the
interpreter. When the REPL calls a **spice export**, the resolution order
is:

1. **JIT thunk** -- any signature, any arity; JIT builds only.
2. **Per-export `__ffi` shim** -- baked into the spice's `.so` at build
   time; any arity, all builds.
3. **Generated shape table** -- pre-generated trampolines up to
   `--max-arity` (6 by default); the legacy floor for old spices in
   non-JIT builds.

A JIT build is therefore never *worse* than a non-JIT build -- if thunk
compilation fails for a signature, the call falls through to the rungs
below.

Notes for the curious:

- Thunks compile through the same engine (`c2mir` -> `MIR_link` ->
  `MIR_gen`) that backs `tur jit`; one resident module per unique
  signature, process-lifetime, negative-cached on failure.
- Platform coverage is MIR's: x86-64 Linux and arm64 macOS are solid;
  Windows/WASM interpreters have no thunk engine (the compiled `call-ptr`
  path still works wherever the C compiles). The provider interface is
  the swap point if dyncall/libffi ever need to slot in.
- ASan cannot see inside thunked calls -- the MIR engine is deliberately
  built unsanitized, same status quo as the JIT.

---

## Capability gating

Every dynamic-FFI operation -- `dlopen`/`dlsym`/`dlclose`, `call-ptr`,
and thunk-backed `extern-c` calls -- sits behind the `TURI_CAP_FFI`
capability bit in sandboxed interpreter environments, alongside the
existing gates for I/O and unsafe memory. A sandboxed env
(`turi_env_new_sandboxed`) gets a clean refusal, not a call.

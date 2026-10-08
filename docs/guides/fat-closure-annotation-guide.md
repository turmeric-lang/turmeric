---
title: Fat-Closure Annotation Guide (`^fat`)
category: Language Basics
description: When and why to mark function-typed parameters and return positions `^fat`, and what breaks without it
---

# Fat-Closure Annotation Guide (`^fat`)

`^fat` is a one-token type annotation that controls how the compiler
represents a function value at a specific source position. It is
**purely a representation hint** -- it does not change runtime semantics,
arity, or signature compatibility. But omitting it where it belongs
causes a SEGV at the call site, so it is worth understanding.

This guide explains the two closure representations Turmeric uses, why
the language can't always pick the right one automatically, what
`^fat` does at parameter and return positions, and when you need to
reach for it.

---

## Two closure representations

A function value in Turmeric compiles to one of two ABIs:

| Representation | Layout | Call site |
|---|---|---|
| **Bare fn-pointer** | `int64_t (*)(int64_t...)` -- a raw C function pointer | `f(arg)` |
| **Fat closure** | Heap `int64_t[]` -- slot 0 is a thunk pointer, slots 1..N are captured free vars | `apply-fat(f, arg)` / `TUR_APPLY1(fat, arg)` |

The compiler picks the bare representation whenever a `(fn ...)` body
captures **no** free variables, because there is nothing to put in
slots 1..N and a bare pointer is cheaper than a heap allocation. A
`(fn ...)` that captures one or more enclosing locals is always fat.

This is a real type-system distinction, tracked by `arg_fat[]` and
`result_fat` bits on the function `Type`. A captureless `fn` and a
captureful `fn` are *not* interchangeable.

## Why this matters at call sites

`apply-fat` (and its `TUR_APPLY1`/`TUR_APPLY2`/... macros) load the
thunk from slot 0 of a fat closure and pass the closure itself as the
environment pointer. Feeding a **bare** fn-pointer to `apply-fat`
reads the first instruction byte of the function as a "thunk address"
and segfaults.

Any combinator that dispatches its callback through `apply-fat` is
therefore a **fat-expecting sink**:

```turmeric
(defn bind-parser [p f] : ptr<void>
  ;; ... eventually calls (apply-fat f x) ...
  ...)
```
```sweet-exp
defn bind-parser [p f] : ptr<void>
  ;; ... eventually calls (apply-fat f x) ...
  ...
```

If a caller passes a captureless lambda here:

```turmeric
(bind-parser p (fn [x] (mreturn (transform x))))   ;; captures nothing
```
```sweet-exp
bind-parser(p (fn [x] mreturn(transform(x))))   ;; captures nothing
```

...the lambda lowers to a bare fn-pointer, `apply-fat` reads it as a fat
closure, and the program crashes. Do not force a capture by hand (the
`(let [sentinel 0] (fn [x] (let [_ sentinel] ...)))` trick) -- the right
tool is `^fat`.

## `^fat` on a parameter

Annotating a function-typed parameter `^fat` declares that the body
calls it through the fat-closure ABI. The elaborator then auto-shims
any captureless `fn` passed at the call site, boxing it into a
one-cell fat closure (`{ __tur_fatshim<arity>, orig_fn }`) before the
call.

```turmeric
(defn bind-parser [p ^fat f] : ptr<void>
  ;; (apply-fat f x) is now safe for any caller
  ...)
```
```sweet-exp
defn bind-parser [p ^fat f] : ptr<void>
  ;; (apply-fat f x) is now safe for any caller
  ...
```

- A capturing `fn` argument is already fat and passes through unchanged.
- A bare `extern-c` C function flowing in is also auto-shimmed.
- Plain `:int`/`:ptr<void>` arguments are unaffected -- the marker only
  fires on function-typed parameters.

Reach for `^fat` on a parameter whenever your function body invokes the
parameter via `apply-fat`, `TUR_APPLY1`, or any other fat-ABI form.

### `^fat` parameters inside inline-C

A `^fat` function-typed parameter is materialised in the
generated C signature as plain **`int64_t`** -- the unified closure handle
(a pointer to the fat box, or, for the bare-shimmed case, a one-cell box
whose slot 0 is the original fn-pointer). That means you can name it
directly inside an inline-C body and feed it back to the `TUR_APPLY*`
macros:

```turmeric
(defn run-once [^fat f x : int] : int
  ```c
  /* f is int64_t at the C level; pass it through TUR_APPLY1
     to invoke the closure under the unified ABI. */
  return (int64_t)TUR_APPLY1(f, x);
  ```)
```
```sweet-exp
defn run-once [^fat f x : int] : int
  ```c
  /* f is int64_t at the C level; pass it through TUR_APPLY1
     to invoke the closure under the unified ABI. */
  return (int64_t)TUR_APPLY1(f, x);
  ```
```

The same rule applies in either direction: a Turmeric global declared
`^fat` is callable from inline-C as `int64_t`, and an inline-C function
that wants to *receive* a closure handle from Turmeric should take it as
`int64_t`. Do not spell it as `void *` -- inline-C ascription is scanned
for captures and the carrier type is what the codegen agrees on.

## `^fat` on a return type

The parameter form only fires at **call-argument** sites. A different
situation arises when a *factory* function returns a fat closure built
from a captureless inner lambda:

```turmeric
(defn pfail [] ^fat :ptr<void>          ;; <- ^fat on the return
  (fn [inp] (pfail-impl inp)))          ;; inner lambda captures nothing
```
```sweet-exp
defn pfail [] ^fat :ptr<void>          ;; <- ^fat on the return
  (fn [inp] pfail-impl(inp))          ;; inner lambda captures nothing
```

Without `^fat` on the return type, the inner `(fn ...)` lowers to a
bare pointer and any downstream `apply-fat` on the returned value
segfaults. `^fat` on the return tells the compiler to box the
captureless tail lambda into a one-cell fat closure before returning.

A capturing tail lambda or a forwarded value that is already fat
passes through unchanged -- the shim only fires when the tail is a
bare `TY_FN`.

Reach for `^fat` on a return type whenever your function constructs a
function value that downstream code will fat-call. In a parser-
combinator library, that is essentially every public constructor
(`pfail`, `item`, `pure`, `or-parser`, the `*-ref` thunks for
mutually recursive non-terminals).

## Result type: bare `^fat` is int-carrier only

There are two ways to spell a `^fat` *parameter*, and they differ in what
they know about the closure's **result type**:

```turmeric
(defn run-with [^fat g x : int] : int (g x))                     ;; bare
(defn run-with [^fat g :(fn [float] #{} float) x : float] : float (g x))  ;; annotated
```
```sweet-exp
defn run-with [^fat g x : int] : int g(x)                     ;; bare
defn run-with [^fat g :(fn [float] #{} float) x : float] : float g(x)  ;; annotated
```

Under the unified closure representation, every
closure value -- bare or fat, captureless or capturing -- is carried as a
single `int64_t` handle. The remaining distinction is **what the call site
knows about the result type** at the point where it dispatches the handle.

A **bare** `^fat g` records no result signature, so a direct call `(g x)`
falls back to the int-carrier dispatch path: the result is read out of the
integer return register as an `int64_t`. That is correct for the int-carrier
generic combinators (`>>>`, `option-map`, and friends), where every value
is already an `int64` at runtime.

But a closure that returns a **non-int register class** -- `:float` is the
one that bites -- cannot go through the bare form unless the tail-position
inference (below) can recover the type. A `double` is returned in a
floating-point register (xmm0), while the bare-`^fat` int-carrier dispatch
reads the integer register (rax). The bits do not line up, and you get
garbage. (A `:cstr` or `:ptr<void>` result happens to share the integer
register, so it round-trips -- but do not rely on that; annotate non-int
results. For the integer/pointer carrier specifically, the SF-application
call sites bridge `int <-> ptr<void>`, so mixing the two
requires no manual `(::)` cast at the boundary.)

For any non-int result, use the **annotated** form -- it carries the
result type, and the compiler dispatches through a typed thunk that uses
the right register:

```turmeric
(defn run-with [^fat g :(fn [float] #{} float) x : float] : float
  (g x))                       ;; returns a real double
```
```sweet-exp
defn run-with [^fat g :(fn [float] #{} float) x : float] : float
  g(x)                       ;; returns a real double
```

The tail-position retype pass lets a bare `^fat g` in the
result *tail* of a `:float` function infer the result type from the
declared return -- no annotation required. The one case still left to the
annotated form is a bare-`^fat` non-int result consumed in a **non-tail**
position; that is tracked in
`docs/archive/history/bare-fat-result-monomorphization-plan.md` (deferred until
a real consumer exists). The annotated form remains the *checked* path in
all positions.

## When you do *not* need `^fat`

- The function value is called with the normal `f(arg)` syntax (direct
  invocation, not `apply-fat`). The compiler dispatches against the
  known representation.
- The function value is fully erased into inline C and the C code
  treats it as an opaque `int64_t` payload.
- The function value comes from an `extern-c` declaration and never
  flows through a fat-ABI sink.

If unsure: try without `^fat` first; if the program SEGVs at an
`apply-fat`/`TUR_APPLY1` site, the missing annotation is almost
certainly the cause.

## Arity bound

The auto-shim ships with `__tur_fatshim0` through `__tur_fatshim5`,
covering 0-5 argument lambdas. Higher arities are rejected at
elaboration time. In practice every combinator in the stdlib that
uses `^fat` is 1- or 2-ary, so this rarely binds.

## Interaction with other annotations

- `^fat` is independent of `#fx{Unsafe}`, `:linear`, and effect rows.
  It is a representation marker, not a discipline or capability.
- A **named effectful `defn` may be used as the value** of a `^fat`
  fn-typed parameter in any position -- inside a `do`, nested in another
  call, in tail position. Taking a function's address is not performing
  its effect, so boxing one into a fat closure costs the caller nothing.
- A `^fat` parameter typed `(fn [T] #fx{E} R)` with a **non-empty effect
  row is callable**. Effectful callbacks through `^fat` parameters --
  middleware, logging hooks, visitors that may `perform` -- work, whether
  the argument is an inline lambda or a named top-level `defn`.

  A shape worth knowing if you are working on the compiler: a `^fat`
  parameter holds a boxed `{ shim, direct-entry }` record, not a bare fn,
  so anything that keys off "the function" -- an effect registry lookup,
  an identity comparison, a table of known entry points -- must read the
  **direct entry** out of the box, not the box itself.
- `^fat` on a parameter and `^fat` on the return type compose: a
  factory that takes a captureless callback *and* returns a captureless
  fat closure marks both positions.
- The fat-ABI layout itself is unchanged; `^fat` only changes how the
  compiler decides to **produce** a fat vs. bare value at the marked
  position.

## Quick reference

| Situation | Annotation |
|---|---|
| Your function calls a callback via `apply-fat` / `TUR_APPLY1` | `[cb ^fat fn-type]` on the parameter |
| The callback returns a non-int type (`:float`, ...) | `[cb ^fat :(fn [argtypes] #fx{} :RetType)]` -- annotate so the result type is threaded |
| The callback is int-carrier (generic combinator) | bare `[cb ^fat]` is fine |
| Your function returns a `(fn ...)` that downstream code will fat-call | `^fat :ptr<void>` on the return type |
| Both of the above | Mark both positions |
| Pure direct invocation only | No annotation needed |

For a worked example end-to-end, see
[parser-combinators-tutorial.md](parser-combinators-tutorial.md)
sections 4, 5, and 8. The historical design rationale lives in
[`docs/archive/history/captureless-lambda-abi-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/captureless-lambda-abi-plan.md) and
[`docs/archive/history/fat-closure-return-position-plan.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/fat-closure-return-position-plan.md).

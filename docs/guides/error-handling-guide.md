---
title: Error Handling Guide
category: Error Handling
description: `Result`, `Option`, `panic`, contract macros (`assert!`, `require!`, `ensure!`)
---

# Error Handling Guide

This guide covers the Turmeric error handling story: `Result`, `Option`, `panic`,
unwrap helpers, and contract macros. It describes what is implemented today.

## Overview

Turmeric offers two primary error handling strategies:

1. **`Result`** -- recoverable errors; use when callers should handle failure.
2. **`panic`** -- unrecoverable errors; use for programming mistakes and invariant violations.

`Option` is a separate type for nullable / optional values and has its own unwrap helpers.
Contract macros (`assert!`, `require!`, `ensure!`, `invariant!`) provide structured
precondition and postcondition checking built on top of `panic`.

> **No exceptions:** Turmeric has no `throw` / `try` / `catch`. Use `Result`
> for recoverable failures and `panic` / `catch-unwind` for unrecoverable
> ones. Fiber rejection (from `await`, `with-timeout`, `task-cancel`)
> surfaces as `TURI_REJECTION` and is observed with `(error? r)` /
> `(error-message r)`.

---

## `Result`

A result value is either `(ok value)` or `(err error)`. `Result` is a real sum
(SR2b): the runtime representation is the tagged monomorph
`{ int tag; union { A ok_val; B err_val; } as; }` -- 16 bytes, tag 0 = Ok,
tag 1 = Err, payload at offset 8. A concrete monomorph such as
`(Result int cstr)` flows by value; an erased generic base still rides the
int64 carrier as a heap pointer to the same layout. Inline-C code should
build and read these through the preamble helpers (`tur_box_ok` /
`tur_is_ok` / `tur_ok_value` / ...) rather than spelling the struct by hand --
see the [inline-C results guide](inline-c-results-guide.md).

An inline-C function that hands back a carrier value declared `: int` needs an
ascription at the boundary for the typed accessors to take it -- `(ok? (:: r
(Result int cstr)))`, the same thing `some?` has always wanted. `ok?` and
`err?` take `(Result A B)` rather than `:int` as of the by-value default.

### Constructors

```turmeric
(ok 42)        ;; ok result
(err 99)       ;; err result
```
```sweet-exp
ok(42)         ;; ok result
err(99)        ;; err result
```

### Predicates

```turmeric
(ok?  r)       ;; => bool
(err? r)       ;; => bool
```
```sweet-exp
ok?(r)         ;; => bool
err?(r)        ;; => bool
```

### Extractors

These are unsafe -- check `ok?` / `err?` first:

```turmeric
(ok-val  r)    ;; => int  (undefined behaviour if r is err)
(err-val r)    ;; => int  (undefined behaviour if r is ok)
```
```sweet-exp
ok-val(r)      ;; => int  (undefined behaviour if r is err)
err-val(r)     ;; => int  (undefined behaviour if r is ok)
```

### Unwrapping

```turmeric
(result-unwrap     r)            ;; ok value, or abort() with message to stderr
(result-unwrap-or  r default)    ;; ok value, or default
(result-expect     r "message")  ;; ok value, or abort() printing "message"
(result-must       r)            ;; ok value, or panic "result-must: called on err"
(result-must-msg   r "msg")      ;; ok value, or panic with "msg"
```
```sweet-exp
result-unwrap(r)               ;; ok value, or abort() with message to stderr
result-unwrap-or(r default)    ;; ok value, or default
result-expect(r "message")     ;; ok value, or abort() printing "message"
result-must(r)                 ;; ok value, or panic "result-must: called on err"
result-must-msg(r "msg")       ;; ok value, or panic with "msg"
```

> **Note:** `result-unwrap` and `result-expect` call `abort()` directly (not via
> `tur_panic`). Prefer `result-must` / `result-must-msg` when you want the standard
> panic message format and double-panic guard.

### Combinators

```turmeric
(result-map      r f)            ;; apply f to ok value; propagate err unchanged
(result-map-err  r f)            ;; apply f to err value; propagate ok unchanged
(result-flat-map r f)            ;; f receives ok value and returns a new result
(result-or       r alt)          ;; r if ok, else alt
(result-or-else  r f)            ;; r if ok, else (f err-value)
```
```sweet-exp
result-map(r f)                ;; apply f to ok value; propagate err unchanged
result-map-err(r f)            ;; apply f to err value; propagate ok unchanged
result-flat-map(r f)           ;; f receives ok value and returns a new result
result-or(r alt)               ;; r if ok, else alt
result-or-else(r f)            ;; r if ok, else f(err-value)
```

### Equality

```turmeric
(result-eq? r1 r2 ok-cmp err-cmp)
;; ok-cmp  -- fn [a b :int] :bool  used when both are ok
;; err-cmp -- fn [a b :int] :bool  used when both are err
;; => bool; false if variants differ
```
```sweet-exp
result-eq?(r1 r2 ok-cmp err-cmp)
;; ok-cmp  -- fn [a b :int] :bool  used when both are ok
;; err-cmp -- fn [a b :int] :bool  used when both are err
;; => bool; false if variants differ
```

The `Eq` typeclass instance uses `=` for both sides:

```turmeric
(eq? (ok 1) (ok 1))   ;; => true
(eq? (ok 1) (err 1))  ;; => false
```
```sweet-exp
eq?(ok(1) ok(1))    ;; => true
eq?(ok(1) err(1))   ;; => false
```

### Option interop

```turmeric
(ok-or   opt err-val)    ;; some(v) -> ok(v), none -> err(err-val)
(err-context r "prefix") ;; prepend "prefix: " to the err string; ok passes through
```
```sweet-exp
ok-or(opt err-val)       ;; some(v) -> ok(v), none -> err(err-val)
err-context(r "prefix")  ;; prepend "prefix: " to the err string; ok passes through
```

### Collection utilities

```turmeric
(result-collect    vec)    ;; (vec result) -> result<vec, E>; first err wins
(result-partition  vec)    ;; -> pair; separate ok and err elements
(result-partition-ok  pair)  ;; -> vec of ok values from the pair
(result-partition-err pair)  ;; -> vec of err values from the pair
```
```sweet-exp
result-collect(vec)          ;; (vec result) -> result<vec, E>; first err wins
result-partition(vec)        ;; -> pair; separate ok and err elements
result-partition-ok(pair)    ;; -> vec of ok values from the pair
result-partition-err(pair)   ;; -> vec of err values from the pair
```

### Memory

```turmeric
(result-free r)    ;; free the heap struct (does not free the contained value)
```
```sweet-exp
result-free(r)     ;; free the heap struct (does not free the contained value)
```

### Typeclass instances

`result` (as `ptr<void>`) implements `Functor`, `Applicative`, `Monad`,
`Foldable`, `Bifunctor`, and `Eq`. These are defined in `stdlib/result.tur`.

| Typeclass    | Behaviour on `ok` / `err` |
|---|---|
| `Functor`    | `fmap` applies to ok value; err propagates |
| `Applicative`| `pure x = ok(x)`; `ap` short-circuits on first err |
| `Monad`      | `bind` applies f to ok value; err propagates |
| `Foldable`   | `foldl` / `foldr` operate on ok value; err is skipped |
| `Bifunctor`  | `bimap fn-left fn-right` -- fn-left over err, fn-right over ok |
| `Eq`         | structural equality via `result-eq?` with `=` comparator |

---

## Query operator: `?`

The `?` operator unwraps an ok `Result` in place, or returns the err `Result`
early from the enclosing function. It is the ergonomic counterpart to manual
`(if (err? r) (return r) (ok-val r))` threading.

```turmeric no-check
(? expr)            ;; s-expression
```
```sweet-exp
?(expr)             ;; neoteric -- composes inside other calls
(? expr)            ;; traditional spelling also works under #lang sweet-exp
```

### Lowering

`(? expr)` lowers to a single-evaluation `let` so `expr` runs exactly once:

```turmeric
(let [__q expr]
  (if (err? __q)
      (return __q)        ;; propagate the err Result unchanged
      (ok-val __q)))      ;; otherwise yield the unwrapped ok value
```
```sweet-exp
let [__q expr]
  if err?(__q)
      return(__q)        ;; propagate the err Result unchanged
      ok-val(__q)      ;; otherwise yield the unwrapped ok value
```

The lowering routes through the `#fx{}`-safe stdlib helpers `__tur-q-is-err?`
and `__tur-q-ok-val` (in `stdlib/result.tur`), so call sites need no `(unsafe
...)` wrapper.

### Scope rule

`?` is only valid **inside a function body** -- using it at the top level is a
hard error (`? operator is only allowed inside a function body`). Because it
expands to `(return <err>)`, the enclosing function must itself return a
`Result` so the propagated err is well-typed.

Applying `?` to a non-`Result` literal is rejected up front:

```turmeric
(? 5)   ;; error [TUR-E0001]: ? operator requires a Result value, got int
```

Computed non-`Result` operands are caught by the same `TUR-E0001` check when
the `__tur-q-is-err?` helper fails to accept them.

### Worked example

```turmeric
;; parse-config threads three fallible steps; any err short-circuits.
(defn parse-config [src : ptr<void>] : ptr<void>
  (let [raw    (? (read-source  src))]
    (let [toks (? (tokenize     raw))]
      (let [ast (? (parse-forms toks))]
        (ok ast)))))
```
```sweet-exp
;; parse-config threads three fallible steps; any err short-circuits.
defn parse-config [src : ptr<void>] : ptr<void>
  let [raw    ?(read-source(src))]
    let [toks ?(tokenize(raw))]
      let [ast ?(parse-forms(toks))]
        ok(ast)
```

If `read-source`, `tokenize`, or `parse-forms` returns an err, `parse-config`
returns that err immediately; otherwise it returns `(ok ast)`.

For the "transform the error, then propagate" use case, combine `?` with
[`result-or-else`](#combinators) to rewrite the err payload before it bubbles
up.

---

## `Option`

An option is either `(some value)` or `(none)`. `none` is represented as `NULL`.

### Constructors

```turmeric
(some 42)    ;; some option
(none)       ;; none option (NULL)
```
```sweet-exp
some(42)     ;; some option
none()       ;; none option (NULL)
```

### Predicates

```turmeric
(some? o)    ;; => bool
```
```sweet-exp
some?(o)     ;; => bool
```

### Unwrapping

```turmeric
(option-unwrap o)            ;; value, or exit(1) with message to stderr
(option-must   o)            ;; value, or panic "option-must: called on none"
(option-expect o "message")  ;; value, or panic with "message"
```
```sweet-exp
option-unwrap(o)             ;; value, or exit(1) with message to stderr
option-must(o)               ;; value, or panic "option-must: called on none"
option-expect(o "message")   ;; value, or panic with "message"
```

> **Note:** `option-unwrap` calls `exit(1)` directly. Prefer `option-must` /
> `option-expect` for consistent panic semantics.

### Option interop with Result

```turmeric
(ok-or opt err-val)    ;; some(v) -> ok(v), none -> err(err-val)
```
```sweet-exp
ok-or(opt err-val)     ;; some(v) -> ok(v), none -> err(err-val)
```

### Equality

```turmeric
(option-eq? o1 o2 cmp-fn)
;; cmp-fn -- fn [a b :int] :bool
;; => true if both none, or both some with cmp-fn returning true
```
```sweet-exp
option-eq?(o1 o2 cmp-fn)
;; cmp-fn -- fn [a b :int] :bool
;; => true if both none, or both some with cmp-fn returning true
```

The `Eq` typeclass instance uses `=` for the contained value.

### Memory

```turmeric
(option-free o)    ;; free the heap struct
```
```sweet-exp
option-free(o)     ;; free the heap struct
```

### Typeclass instances

`option` implements `Functor`, `Applicative`, `Monad`, `Foldable`, `Traversable`,
`Alternative`, `Eq`, `Clone`, and `Show`. These are defined in `stdlib/option.tur`.

---

## `panic`

`panic` terminates the program unconditionally. Use it for:

- Bugs and invariant violations that cannot be recovered from.
- Assertions in test code.
- Unreachable code branches.

```turmeric
(panic "something went wrong")
```
```sweet-exp
panic("something went wrong")
```

This prints `panic: something went wrong` to stderr and calls `abort()`.

### Double-panic guard

If `tur_panic` is called while a panic is already in progress (e.g. in a `defer`
chain), it prints `double panic: aborting` and calls `abort()` immediately.

### `defer` during panic

Defer thunks registered before the panic are fired in reverse order during
unwinding. If a defer thunk itself panics, the double-panic guard triggers `abort()`.

### Auditing panic call sites

Pass `--lint-panic` to have the compiler emit `TUR-W0038` at every panic call
site so a codebase can audit (or gate CI on) where panics can originate. The
flagged sites are:

- `panic` / `tur_panic`
- the contract macros `assert!` / `require!` / `ensure!` / `invariant!`
  (and their `-msg!` variants)
- `result-unwrap` / `option-unwrap` -- these additionally carry a
  soft-deprecation hint to prefer `result-must` / `option-must`, which have
  consistent panic semantics (`*-unwrap` go straight to `abort()`/`exit(1)`).

The lint is **off by default**. Silence intentional sites with a
`;; #lint-panic-allow` comment:

- as a **top-of-file** comment (in the leading comment block) it silences the
  whole file;
- on the line **immediately preceding** a call it silences just that call.

```turmeric
(defn supervisor [] : int
  ;; #lint-panic-allow
  (panic "unrecoverable")   ;; not flagged
  0)
```

### Catching a panic at a boundary: `catch-unwind`

`catch-unwind` runs a nullary thunk behind a panic boundary. If the thunk
returns normally, the result is `(ok value)`; if the thunk panics, the panic is
intercepted and the result is `(err panic)`, where the err slot carries an
opaque `Panic` handle. Execution then continues after the boundary instead of
aborting.

```turmeric
(let [r (catch-unwind (fn [] : int (risky)))]
  (if (err? r)
    (recover)
    (use (ok-val r))))
```
```sweet-exp
let [r catch-unwind((fn [] :int risky()))]
  if err?(r)
    recover()
    use(ok-val(r))
```

The value is an ordinary `Result` (carried as the `:int` result box), so it
composes with `ok?` / `err?` / `ok-val` and the `?` operator.

**Use it for:** FFI boundaries (a panic must not unwind into C), test harnesses,
and supervisor loops that keep running after a failed unit of work. It is **not**
a substitute for `result` -- model expected, recoverable failures with `result`
and reserve `catch-unwind` for turning a genuine panic into a value at a
controlled boundary.

**Inspecting the panic.** Load `stdlib/panic.tur` to read the payload through the
opaque `Panic` wrapper:

```turmeric
(load "stdlib/panic.tur")

(let [r (catch-unwind (fn [] : int (panic "boom")))]
  (when (err? r)
    (println (panic-message (result-panic r)))))  ; => boom
```
```sweet-exp
load "stdlib/panic.tur"

let [r catch-unwind((fn [] : int panic("boom")))]
  when err?(r)
    println(panic-message(result-panic(r)))  ; => boom
```

`stdlib/panic` exposes `result-panic` (extract the `Panic` from an err result),
`panic-message`, `panic-file`, `panic-line`, and `panic-type`.

**Interaction with `defer`.** Defer thunks registered inside the thunk fire
during unwinding, *before* `catch-unwind` produces its `err` result -- a defer
can observe that a panic is in progress. Because the thunk runs in its own call
frame, only its defers fire; defers above the boundary are untouched (partial
unwind).

**Double panic.** If a defer thunk that fires during unwinding itself panics,
the double-panic guard triggers `abort()` -- the inner panic is fatal and is not
caught (matching Rust's `catch_unwind` + `Drop`-panic semantics).

**Typed catches: `catch-panic-of`.** `(catch-panic-of Type thunk)` is like
`catch-unwind` but only catches panics whose payload type matches `Type`; panics
of any other type are re-raised to the next outer boundary.

```turmeric
(catch-panic-of :cstr (fn [] : int (panic "string panic")))  ;; caught
(catch-panic-of :int  (fn [] : int (panic "string panic")))  ;; re-raised
```

> See [docs/design/error-handling-rationale.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/design/error-handling-rationale.md)
> for the exception-vs-panic boundary `catch-unwind` is meant to sit on.

---

## Unwrap helpers: `must!` and `must-msg!`

These macros unwrap an **option** value, panicking on `none`:

```turmeric
(must!     (some 42))                   ;; => 42
(must!     (none))                      ;; panic: option-must: called on none

(must-msg! (some 42) "expected value")  ;; => 42
(must-msg! (none)    "expected value")  ;; panic: expected value
```
```sweet-exp
must!(some(42))                         ;; => 42
must!(none())                           ;; panic: option-must: called on none

must-msg!(some(42) "expected value")    ;; => 42
must-msg!(none()   "expected value")    ;; panic: expected value
```

> `must!` expands to `(option-must expr)`. `must-msg!` expands to
> `(option-expect expr msg)`. For `result` values use `result-must` /
> `result-must-msg` directly -- the macros do not dispatch generically.

---

## `option-must` and `option-expect`

These are the underlying functions used by `must!` and `must-msg!`:

```turmeric
(option-must   (some 42))               ;; => 42
(option-must   (none))                  ;; panic: option-must: called on none
(option-expect (some 42) "want value")  ;; => 42
(option-expect (none)    "want value")  ;; panic: want value
```
```sweet-exp
option-must(some(42))               ;; => 42
option-must(none())                 ;; panic: option-must: called on none
option-expect(some(42) "want value") ;; => 42
option-expect(none()   "want value") ;; panic: want value
```

---

## `ignore!`

Explicitly discard a result or option value to silence unused-value warnings
(when linting is enabled):

```turmeric
(ignore! (some-fn-returning-result))
```
```sweet-exp
ignore!(some-fn-returning-result())
```

`ignore!` expands to `(do expr nil)` -- the expression is evaluated for its side
effects and the value is dropped.

### Linting unused results

Pass `--warn-unused-result` to have the compiler warn when a `result`-shaped
value (a `ptr<void>`) is computed in statement position and its value is thrown
away:

```turmeric
(defn main [] : int
  (write-record r)   ;; warning: discarded result value of type ptr<void>;
                     ;;          use ignore! to suppress this warning
  0)
```
```sweet-exp
defn main [] : int
  write-record(r)   ;; warning: discarded result value of type ptr<void>;
                     ;;          use ignore! to suppress this warning
  0
```

The warning is **off by default** and is silenced three ways:

- **`ignore!`** -- `(ignore! (write-record r))` expands to `(do expr nil)`, so
  the discarded value is `nil`, not a result. Use this when discarding is
  intentional.
- **Binding it** -- `(let [_ (write-record r)] ...)`. A `let` binding (even to
  `_`) is an explicit discard and never warns.
- **Using the value** -- e.g. propagating it with the [`?` operator](#query-operator-).
  Because `(? expr)` binds its operand in a `let`, a `?`-wrapped call is never
  in statement position and never triggers the warning.

---

## Contract macros

Contract macros live in `stdlib/macros.tur` (module `tur/macros`) and are
auto-imported. They all expand to `tur-contract-check` or `tur-contract-check-inv`
from `stdlib/contract.tur`, which call `tur_panic` on failure.

By default contracts are enabled (`contract-enabled?` returns `true`). Pass
`--no-contracts` to strip them from a release build: every `assert!` /
`require!` / `ensure!` / `invariant!` is dropped **at elaboration time**, so the
contract's predicate expression is never evaluated, and `contract-enabled?`
folds to `false`.

> **Side-effect warning.** Because the predicate is removed entirely, any side
> effect inside a contract condition disappears under `--no-contracts` -- this
> matches the C / Rust `assert` convention. Keep contract predicates pure;
> never rely on a side effect that lives inside `(assert! ...)`.

The codegen preamble also defines `TUR_CONTRACTS_ENABLED` (`1` normally, `0`
under `--no-contracts`) so inline-C blocks can branch on the build mode. See
the [compiler flags guide](compiler-flags-guide.md) for the release-build
recipe.

### `assert!` and `assert-msg!`

Unconditional sanity check. Use anywhere you want to verify an intermediate
condition holds:

```turmeric
(assert! (= x 1))
;; panics with "Assertion failed" if x != 1

(assert-msg! (= x 1) "x must be 1")
;; panics with "x must be 1" if x != 1
```
```sweet-exp
assert!({x = 1})
;; panics with "Assertion failed" if x != 1

assert-msg!({x = 1} "x must be 1")
;; panics with "x must be 1" if x != 1
```

### `require!` and `require-msg!`

Precondition check at function entry:

```turmeric
(defn sqrt [n : int] : int
  (require! (>= n 0))
  ...)
;; panics with "Precondition failed" if n < 0

(defn sqrt [n : int] : int
  (require-msg! (>= n 0) "sqrt: n must be non-negative")
  ...)
```
```sweet-exp
defn sqrt [n :int] :int
  require!({n >= 0})
  ...
;; panics with "Precondition failed" if n < 0

defn sqrt [n :int] :int
  require-msg!({n >= 0} "sqrt: n must be non-negative")
  ...
```

### `ensure!` and `ensure-msg!`

Postcondition check before returning from a function:

```turmeric
(defn abs [n : int] : int
  (let [result (if (< n 0) (- 0 n) n)]
    (ensure! (>= result 0))
    result))
;; panics with "Postcondition failed" if the result is somehow negative
```
```sweet-exp
defn abs [n :int] :int
  let [result (if {n < 0} {0 - n} n)]
    ensure!({result >= 0})
    result
;; panics with "Postcondition failed" if the result is somehow negative
```

### `invariant!` and `invariant-msg!`

Structural invariant check -- passes the value to a predicate function:

```turmeric
(invariant! my-list non-empty?)
;; panics with "Invariant failed" if (non-empty? my-list) is false

(invariant-msg! my-list non-empty? "list must not be empty")
```
```sweet-exp
invariant!(my-list non-empty?)
;; panics with "Invariant failed" if non-empty?(my-list) is false

invariant-msg!(my-list non-empty? "list must not be empty")
```

---

## When to use what

| Situation | Approach |
|---|---|
| Caller should handle the failure (e.g. file not found) | `result` |
| Value may or may not be present | `option` / `some` / `none` |
| Programming error / violated invariant | `panic` |
| Unwrapping a result you are confident is ok | `result-must` / `result-must-msg` |
| Unwrapping an option you are confident is some | `option-must` / `must!` |
| Checking a precondition at function entry | `require!` / `require-msg!` |
| Checking a postcondition before returning | `ensure!` / `ensure-msg!` |
| Verifying a structural invariant | `invariant!` / `invariant-msg!` |
| General sanity check in the middle of code | `assert!` / `assert-msg!` |
| Discarding a result intentionally | `ignore!` |

---

## Panic inside async tasks

> **Today.** Every `(async ...)` task has its own panic boundary. `(async ...)`
> still runs its body on the caller's stack until the body's first suspension
> point, but a panic there is caught at the task boundary rather than unwinding
> whoever spawned it: the task's future is **rejected** carrying the panic
> message, and the spawn returns normally. (Fibers spawned into a *task group*
> keep their own boundary: a panic there auto-cancels the group.)
>
> The rejection is re-raised at the point that demands the result. `(await f)`
> on a rejected task raises the task's own panic *at the await*, so a
> `catch-unwind` around the await catches it -- and with no handler in scope it
> prints the task's message and aborts, exactly as an uncaught panic does
> anywhere else. A task whose panic nobody ever awaits does not terminate the
> program.

```turmeric
(defn boom [] : int (panic "task exploded"))

(defn main [] : int
  (let [fut (async boom)]
    (println "the spawn itself does not unwind"))   ; runs

  (let [fut (async boom)
        r   (catch-unwind (fn [] : int (await fut)))]
    (if (err? r)
      (println "recovered from the task's panic")   ; taken
      (println "no panic")))
  0)
```
```sweet-exp
defn boom [] : int panic("task exploded")

defn main [] : int
  let [fut async(boom)]
    println("the spawn itself does not unwind")   ; runs

  let [fut async(boom)
        r   catch-unwind((fn [] : int await(fut)))]
    if err?(r)
      println("recovered from the task's panic")   ; taken
      println("no panic")
  0
```

> **Still planned.** Three parts of the task-boundary design are not built yet:
>
> 1. If a task is cancelled while a panic is in progress, the panic takes
>    precedence.
> 2. An uncaught panic in async main terminates the process with a nonzero exit
>    code after all defer thunks have fired.
> 3. On the WASM target, panics lower to the WebAssembly `unreachable`
>    instruction.
>
> A panic raised *after* a task re-parks on a pending `await` is also outside
> the boundary today: the boundary is the spawn-side frame, so it is gone by
> the time the parked continuation resumes.

---

## Panic inside effect handlers and continuations

A panic interacts with the effect/continuation machinery as follows:

1. **Effect handlers**: a panic inside a `(perform ...)` or inside a handler body
   propagates normally through the handler chain; it is only intercepted by a
   `catch-unwind` boundary.
2. **Continuations**: a panic unwinds to the enclosing `reset` boundary. Once a
   panic has been recovered at a `catch-unwind`, the panic-in-progress state is
   cleared, so a later panic is again catchable (recovering one panic never
   leaves a sticky state that turns the next panic into a spurious double-panic
   abort). Resuming a captured continuation after a recovered panic does not
   re-panic.
3. **Defer**: defer thunks fire in reverse registration order during unwinding. A
   defer thunk that itself panics triggers the double-panic guard and calls
   `abort()`.
4. **Cleanup ordering**: defer thunks fire *before* `catch-unwind` populates its
   `err` result, so a defer can observe that a panic is in progress.

---

## See Also

- [error-handling-rationale.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/design/error-handling-rationale.md) --
  exceptions vs. panic design rationale
- [effects-system-guide.md](effects-system-guide.md) -- effect handler
  semantics referenced above
- [compiler-flags-guide.md](compiler-flags-guide.md) -- `--no-contracts`,
  `--warn-unused-result`, and `--lint-panic`
- [cps-transform-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/cps-transform-plan.md) -- the CPS
  substrate the async runtime is built on

# `nil` passed to a `ptr<void>` parameter type-checks, then emits `((void)0)`

**RESOLVED 2026-10-10.** Fix direction 2: a literal `nil` at a `ptr<void>`
parameter is the null pointer, as the checker always meant. Fixing it exposed
a second defect behind the same reactor-guide examples: a captureless
callback crashed the reactor. Both are fixed. See "Resolution" at the end.

**Severity: low-medium (compile failure in cc, not a wrong answer).**
**Discovered:** 2026-10-10, spiking `docs/upcoming/spices/nng-async-plan.md`
against v0.63.9 (the prebuilt release `turmeric-spices/scripts/install-tur.sh`
fetches, which matches `main` at `f53e73ed`).

`tur check` accepts a literal `nil` as the argument for a `ptr<void>`
parameter. The emitter then lowers that `nil` to its unit placeholder
`((void)0)`, and cc rejects the call with `invalid use of void expression`. No
Turmeric diagnostic is printed. The failure needs no inline C: a pure Turmeric
callee shows it too.

It matters beyond a toy because **the reactor guide's own examples hit it**.
Every `reactor-add-timer` / `reactor-add-fd` / `reactor-add-chan` /
`local-spawn` example in `docs/guides/reactor-guide.md` passes `nil` as its
`user-data : ptr<void>` argument: 12 sites, counting the s-expression and
sweet-exp forms of each example. The draft nng async plan's reactor loop did
the same.

## Repro

```turmeric
(defn is-null? [p : ptr<void>] : bool (= (:: p :int) 0))
(defn main [] : int
  (println (is-null? nil))
  0)
```

```
$ tur check d.tur        # exit 0, no diagnostic
$ tur run d.tur
d_tur.c: error: invalid use of void expression
        bool __ps_173 = (is_hynull_qu(((void)0)));
```

The same call through `reactor-add-fd ... nil` fails the same way
(`reactor_hyadd_hyfd(..., ((void)0))`).

Binding the `nil` first gets a real diagnostic, which is the behaviour the
argument position should match:

```turmeric
(let [q : ptr<void> nil] ...)
;; error [TUR-E0023]: cannot bind 'q' to an expression of type :void
```

**Workaround (before the fix):** pass a null pointer explicitly: `(:: 0 :ptr<void>)`. With that
spelling, the reactor fd / timer / fiber-group calls compile and run (checked in the
same spike).

## Root cause (where to look)

- `atom_nil()` (`src/compiler/emit_core.c:4447`) is the `((void)0)` placeholder
  every unit-valued expression lowers to. That is right for statement position
  and wrong as a call argument.
- The argument check does not reject a `:void`/`nil`-typed argument against a
  `ptr<void>` parameter, although the `let` path does (`TUR-E0023`). So the
  value reaches the call emitter, which splices the placeholder in as-is.

## Fix directions

1. **Reject it in the checker** (smallest): a `nil`-typed argument to a
   non-unit parameter is a type error. Name `(:: 0 :ptr<void>)` in the
   message, as `TUR-E0023` names `do`.
2. **Or give `nil` a meaning at pointer type**: lower a literal `nil` to
   `NULL` when the expected type is `ptr<T>`. That makes the guide examples
   correct as written, but it adds a second meaning to `nil`.
3. Either way, change the 12 reactor-guide sites to whatever the fix
   blesses, and add a fixture that passes `nil` (or the blessed spelling) as
   `reactor-add-fd`'s user-data, so the guide's shape is under test.

## Resolution (2026-10-10)

### The `nil` argument

The checker already meant `nil` to be a null pointer here: the arm in
`elab_call_fn_inner` (`src/compiler/elab_call.c`) said "Allow nil as a null
pointer for ptr<void> parameters" and set `arg_ok`. Nothing told the back
ends. Measured before the fix, on every call shape:

| Shape | Before |
| --- | --- |
| Turmeric callee, inline-C callee, extern-c, capturing closure call, `reactor-add-*`, `local-spawn` | cc: `invalid use of void expression` |
| Constructor field `(Holder 7 nil)` | gcc warns `-Wint-conversion` and runs. clang rejects the `((void)(((void)0)), INT64_C(0))` comma expression, which is not a null pointer constant |
| `tur --interpret` | `(is-null? nil)` printed `false`; the interpreter's nil is not 0 |

The arm now rewrites a literal `nil` to the `0` integer literal. The sibling
arm directly below already accepts `0` as a null pointer, and every back end
handles it: `(void *)(intptr_t)(INT64_C(0))` at a call, a bare `INT64_C(0)`
(a null pointer constant) in a constructor, and 0 in the interpreter. So one
rewrite fixes all three.

Only the literal is accepted. Any other `:void`-typed argument (a void call,
a `do` ending in nil) is a statement, not a null pointer. It used to be
accepted and fail in cc; it now gets `TUR-E0001: function 'f' arg 1: expected
ptr<void>, got nil`. Nothing that compiled before is rejected.

### The second defect: captureless reactor callbacks

With the `nil` fixed, the guide's Quick start compiled and then segfaulted in
`call_tur_timer_cb` (`src/async/reactor.c:318`). It did the same on v0.63.9
with `(:: 0 :ptr<void>)` spelled out. So this was independent of `nil`, and
it was hidden only because nothing got that far.

The `reactor-add-fd` / `-timer` / `-interval` / `-signal` / `-chan` wrappers
in `stdlib/reactor.tur` took their callback as a plain `cb : int`. A
captureless lambda therefore arrived as a bare C function pointer, and
reactor.c called it through "its fat box's slot 0": through the function's
own code bytes. A capturing lambda is already a fat box, which is why every
existing reactor fixture captures a sentinel. None of them used the wrappers
at all: they declare the `tur_reactor_*` externs themselves. Nothing in
either repo called the wrappers, so the guide was their only user.

`local-spawn` had the same bug and was fixed by typing its body `^fat body :
(fn [ptr<void>] nil)`. That auto-shims a captureless lambda into a heap
`{ fatshim, fn }` box, which the owner frees at teardown. The five callbacks
now get the same treatment: `^fat cb : (fn [int int ptr<void>] nil)` for fd,
signal and chan, and `(fn [int ptr<void>] nil)` for timer and interval. That
also lets the checker hold a callback to the arity reactor.c calls it with.

### Guide

`docs/guides/reactor-guide.md` also opened its Quick start and Styles 1 and 2
with a top-level `(import reactor)`, which is `import is only allowed inside
defmodule`. They now `(load "stdlib/reactor.tur")`, as Style 3 already did.
Both Quick start blocks, s-expression and sweet-exp, were run verbatim from
the guide and print `hello from timer`. The released v0.63.9 fails on the
same text.

### Pinned by

All four positive fixtures carry `requires.leak-check` and are clean under
LeakSanitizer (`tests/run-leak-check.sh`), so the boxed callbacks are freed at
`reactor-free`.

**Correction (same day):** this section first said the fnsan run proved the
shims match reactor.c's `TurFdCbFn` / `TurTimerCbFn`. It did not prove that.
`tests/run-fnsan.sh` links an uninstrumented `libturi.a`, both locally and in
CI, and the reactor's calls into callbacks are made from inside libturi, so
nothing checked them. Re-run against a libturi built with clang and
`-fsanitize=function -fsanitize-trap=function`, the reactor fixtures, old and
new, are still clean. A deliberately mismatched callback traps there, so the
check is live. Against the uninstrumented libturi, the same mismatch runs and
prints a wrong value. See the reactor-add-chan follow-up in
[stdlib-int-stand-in-audit](../reported/stdlib-int-stand-in-audit.md). The gate
itself was fixed the same day: the fnsan job's libturi is now built under
`-fsanitize=function`
([fnsan-job-does-not-instrument-libturi](fnsan-job-does-not-instrument-libturi.md)).

- `nil-arg-to-ptr-param`: Turmeric callee, constructor field, capturing
  closure. Pure Turmeric, so `run-turi.sh` runs it too (it prints `true`
  there now).
- `nil-arg-to-ptr-param-inline-c`: inline-C callee, and `free(nil)` through
  extern-c.
- `errors/nil-nonliteral-arg-to-ptr-param`: a void call at a `ptr<void>`
  parameter is `TUR-E0001`.
- `reactor-captureless-callbacks`: the guide's Quick start, an interval, and
  a channel watcher, each with a captureless callback and `nil` user-data,
  through the stdlib wrappers.
- `reactor-fd-captureless` (`requires.posix-apis`): `reactor-add-fd`, the
  same way.

### Left as is, then fixed

`reactor-add-chan` took its channel as a raw `ptr<void>`, and converting a
linear `(Chan A)` to one consumes it. So a caller had to hold the raw view and
re-type it for `chan-send` / `chan-free`. Fixed the same day: it now borrows a
`(Chan A)`, and `reactor-add-async-chan` borrows an `(AsyncChan A)`.
`reactor-captureless-callbacks` passes its channel directly. See
[stdlib-int-stand-in-audit](../reported/stdlib-int-stand-in-audit.md).

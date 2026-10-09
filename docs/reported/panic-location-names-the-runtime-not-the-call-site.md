# A panic's "at" location names the runtime, not the Turmeric call site

**Narrowed again 2026-10-09: an index out of bounds is a panic.** `vec-get`,
`vec-set!`, `vec-get-byval`, `slice-get`, `grid-get`/`grid-set!`,
`sized-buf-get` and the `sized-bitvec-*` accessors printed their message and
called `exit(1)`: no `panic at` line, nothing for a `catch-unwind` to catch,
and the interpreter's natives did the same with `_exit(1)`. Each raises a
panic now -- `tur_panic` in the stdlib inline C, `turi_runtime_panic` in the
`vec-get` / `vec-set!` / `slice-get` natives -- so a `catch-unwind` in scope
catches it and, with none, the process ends as for any panic (`panic at
...: vec index out of bounds`, then abort). Pinned by
`tests/fixtures/vec-index-out-of-bounds-panics` on both engines. The
location the compiled panic names is the runtime's (`tur_panic`'s own
`__FILE__`/`__LINE__`), and the interpreter's is `panic at` with no site: a
call made from inline C has no `_at` entry to hand its site to, which is the
open direction below.

**Narrowed again 2026-10-09: a failed `cast` names the cast.**  Each tag
check the emitter writes for a `cast` out of `any` now calls
`__tur_any_cast_check_at(have, want, "<file>", <line>)` with the cast's own
basename and line (`emit_any_cast_bind_check`, `src/compiler/emit_expr.c`),
and the preamble's check panics through `tur_panic_at`; the location-free
`__tur_any_cast_check` stays for the runtime's own callers (the r7rs raising
check).  The interpreter's cast failure sets the same site
(`g_panic_site`, `src/turi/eval.c`), so both engines print `panic at
input.tur:13: cast: any holds Point, not Other`.  157 `expected.c`
snapshots moved (the check's call and the preamble's two new lines);
`tests/fixtures/any-cast-wrong-type-panics` pins the line on both engines.
**Still open:** a panic raised inside a runtime helper the program calls
rather than writes -- a Saffron dynamic operator (`ensure_saffron_dyn_runtime`),
a dynamic method dispatch on an `any` (`__tur_inst_slot`), the panics in
stdlib inline-C bodies (`json/decode-file!`, the arrow loop cell) -- still
names the helper's line in the generated C.  A vec index out of bounds
prints `vec index out of bounds` and exits, with no `panic at` line at all.
Each needs the call site handed to the helper (an `_at` entry, as the cast
check has), or a thread-local site the emitter sets before the call.

**Narrowed again 2026-10-08: direction 4 is done -- `--panic-trace` is
retired.**  The flag is accepted and ignored with `TUR-W0050`
(`src/main.c`), the way `--lint-effects` is; the preamble no longer carries
`g_panic_trace` or `tur_panic_print_scope_chain`, and no `main` sets it.  It
printed defer-frame pointers, which named no function and no line; the panic
line now names the site, and `--debug` + `tur debug` gives a stack.  The
compiler-flags guide says so.  **Still open:** a panic raised inside the
runtime (a bounds check) names the runtime helper that called `tur_panic`.

**Narrowed again 2026-10-08: a failed contract's `panic at` names the
predicate's own site.**  The `:pre`, `:post`, return-refinement, parameter-
refinement, class-result and loop-invariant checks the elaborator injects now
call `tur-contract-check-at` (stdlib/contract.tur) with the basename and line
the predicate is written at, and it panics through `tur_panic_at` -- so the
location is `boom.tur:2` rather than a line of the generated C, and the
message no longer repeats it: `panic at boom.tur:2: Precondition failed in
safe-div: (not= b 0)`.  One builder, `rt_contract_check_call`
(`src/compiler/elab_fns.c`), makes every such call; `rt_contract_message`
keeps the location in the message only if no -at helper is bound.  The
interpreter's native for it (`native_contract_check_at`) prints the same line
where it used to print a bare `panic at`.  Pinned by
`tests/fixtures/contract-failure-names-its-predicate` (`:pre`),
`contract-post-failure-names-its-site` and
`contract-param-refinement-names-its-site`, on both engines.

The contract MACROS too: `assert!`, `require!`, `ensure!` and the `-msg!`
forms expand to `(tur-contract-check cond msg)` written in contract.tur's
template, so the expanded call's own span is the template's.  `elab_call`
rewrites that call to the -at form naming the macro use the program wrote
(`elab_macro_use_site`, over a stack of the expansions in progress, each frame
its call site and its macro's `defmacro` span): the innermost use that is not
itself inside the template of the macro expanding around it.  So an `assert!`
handed to `when` names the `assert!`, and a user macro wrapping
`require-msg!` names the user macro's use.  Pinned by
`tests/fixtures/contract-macro-names-its-use`.

**Still open:** a panic raised inside the runtime (a bounds check), and
direction 4 (`--panic-trace`).

**Narrowed again 2026-10-07: direction 2 is done.** A failed contract's
message names the kind of check, the function, where the predicate is
written, and the predicate's source:
`Precondition failed in safe-div at boom.tur:2: (not= b 0)`, and likewise
`Postcondition failed in ...`, `Return contract violated in ...`, `Class
result contract violated in ...` and `Contract violated by parameter 'd' in
...` (`rt_contract_message`, `src/compiler/elab_fns.c`, used by the `:pre`
site, `rt_wrap_return_check` -- early returns included -- and
`rt_inject_param_checks`; a lambda is `in fn`, an instance method its method
name).  The old prefix is kept, so every `expected.stderr` that matched it
still does.  Compiled and `--interpret` print the same message.  Pinned by
`tests/fixtures/contract-failure-names-its-predicate`; the contract types and
debugging guides show the new text.  **Still open:** direction 4
(`--panic-trace`), and the `panic at` prefix of a contract failure and of a
runtime-raised panic (a bounds check), which still names the runtime helper
that called `tur_panic` -- the contract's own location is now inside its
message.

**Narrowed 2026-10-04: a `(panic ...)` names its own call site** (fix
directions 1 and 3). Compiled: the preamble's body is now `tur_panic_at(file,
line, msg)`, and each `panic` site the emitter writes passes its own source
basename and line as literals (`emit_panic_call`, `src/compiler/emit_expr.c`).
So `tur run boom.tur` prints `panic at boom.tur:3: division by zero` in every
build, `--debug` or not. `tur_panic(msg)` stays as the runtime's own entry, and
its location is still the runtime's. Interpreted: EX_PANIC hands its span to
`turi_runtime_panic`, which prints the same line, and a typed `panic-with` with
no catcher prints `panic at boom.tur:N`. Pinned by
`tests/fixtures/panic-names-its-call-site` (`expected.stderr`, on both back
ends), and the location literal is in every `expected.c` that has a panic. The
debugging guide now says the message names the call site. **Still open:**
direction 2 (a failed `:pre`/`:post` still says only `Precondition failed`,
without the function or the predicate) and direction 4 (what `--panic-trace` is
for). A panic raised inside the runtime (a bounds check) still names a
generated-C line.

**Severity:** medium (user experience). Every compiled panic reports the same
location -- a line inside the runtime preamble's own `tur_panic` -- whatever
code panicked. The interpreter prints `panic at` with no location at all, and a
failed `:pre` / `:post` says only `Precondition failed`, without the function or
the predicate. A user reading the message learns nothing about where to look.
Found 2026-10-04 while writing `docs/guides/debugging-guide.md`, against
`./build/tur` v0.62.0 on macOS/arm64.

## Repro

```turmeric
(defn checked-div [a : int b : int] : int
  (if (= b 0)
    (panic "division by zero")
    (/ a b)))

(defn main [] : int
  (println (checked-div 10 0))
  0)
```

```
$ tur run boom.tur
panic at /var/folders/.../T/tur-build/boom_tur.c:1127: division by zero

$ tur interpret boom.tur
panic at
panic: division by zero
```

The `1127` is the same for every program built against the same preamble: it is
the `fprintf` line inside `tur_panic` itself. `tur build --debug` does not
change it -- the `#line` directives map the *call site* to `boom.tur:3`, but the
location is taken inside the callee.

A contract failure has the same location problem and less text:

```turmeric
(defn safe-div [a : int b : int] : int
  :pre (not= b 0)
  (/ a b))
```

```
panic at /var/folders/.../T/tur-build/contract_tur.c:1127: Precondition failed
```

`--panic-trace` does not help either: it prints the defer-frame chain as raw
pointers (`at frame 0x... (parent: 0x..., n_defers: N)`), and only when a
`defer` frame is live. It is not a call stack, and no guide documents it.

## Root cause

`src/compiler/emit_module.c:14423` emits

```c
fprintf(stderr, "panic at %s:%d: %s\n", __FILE__, __LINE__, msg ? ...);
```

inside the body of `static void tur_panic(const char *msg)`. `__FILE__` /
`__LINE__` expand where they are written -- in the runtime -- so they can never
name the caller. `panic-with` (`emit_module.c:14971`) does take `file`/`line`
parameters; plain `tur_panic` does not.

## Fix directions

1. Make `tur_panic` a macro (or give it `file`/`line` parameters, as
   `panic-with` already has) so the location is captured at the call site.
   Under `--debug` the `#line` directives then make that location a `.tur`
   file and line; without `--debug`, have the emitter pass the elaborated
   span's file and line as literals so the location is right in every build.
2. Have contract checks pass the function name and the predicate's source text
   into the message: `precondition of safe-div failed: (not= b 0)`.
3. Give the interpreter's panic the node's span (it has one) instead of an
   empty `panic at`.
4. Decide what `--panic-trace` is for. Either print a symbolized call stack
   (`backtrace(3)` + `tur demangle`), or retire the flag.

## Guide upkeep

`docs/guides/debugging-guide.md` ("When a program panics") tells readers that
the printed location is the runtime's and to use lldb or `tur debug` for the
real one. When this is fixed, rewrite that paragraph to say the message names
the call site, and drop the "is always the runtime's line" sentence.

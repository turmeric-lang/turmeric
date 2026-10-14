# A panic's "at" location names the runtime, not the Turmeric call site

**Follow-up 2026-10-14: the three "known and accepted" gaps are closed.**
The slot discipline changed under all three: a sited call now SAVES the
slot, sets its site, and RESTORES the saved value after (`tur_site_push` /
`tur_site_pop`, `emit_site_wrap` and `emit_site_open_stmt` in
`src/compiler/emit_core.c`) instead of clearing it, so a site outlives the
calls made beneath it. A call the compiler forwards and a call written in
stdlib source KEEP a site already set and set their own only when none is
(`tur_site_keep`, `site_mode_for`); the interpreter's leaf call does the same
(`leaf_panic_site`, `src/turi/eval.c`; stdlib source is `diag_file_is_stdlib`).
On the 100M-turn `vec-get` loop the extra load measured nothing (99 ms
best-of-9 for unsited, set/clear and save/restore alike: the set is sunk into
the panic path). (1) A **partial application** names the call made through
it: the thunk's one call carries `forwards_site` (`elab_partial_apply`) and
keeps the site the call through the value set; a unit with such a partial
sites its fn-value calls (`emit_cps_ir_addr_taken_any` counts the thunks'
targets), a direct call of a let-bound partial's thunk is sited as a call to
its target (`binding_panics_in_inline_c`, the `__papN` scan), and the
interpreter hands the thunk the caller's site (`g_fwd_site`). (2) The **per-unit
slot**: a multi-file build's runtime header gives the owner TU the one slot
(`extern` elsewhere; `emit_panic_site_slot(out, shared)`), and a call into
another module's sited body, whose signature this TU does not record, holds
its value in an `__auto_type` temp -- that build is never c2mir's. (The r7rs
split build already shared its slot through `emit_split_state`.) (3) A
**stdlib HOF**: the program's call to a stdlib function taking a function
value is sited in a unit whose fn-value calls are, and the call through the
value inside it, written in stdlib source, keeps that site -- so
`(option-map (some 9) get10)` names the program's line on both engines. Found
on the way: `run-turi.sh` never checked `expected.stderr` on a positive
fixture, so "both engines" above was asserted compiled only; it checks it
now. And `(option-map (some 1) (vec-get v))` -- a generic partial handed to a
CPS-emitted clone -- did not compile: the return delivery boxed an already
boxed carrier word (`slot_store_reap`). Pinned by
`tests/fixtures/panic-site-through-partial`, `panic-site-let-bound-partial`,
`panic-site-partial-through-stdlib-hof` (both engines),
`panic-site-through-stdlib-hof` (compiled), `option-map-generic-partial`, and
`tests/run-panic-site-across-modules.sh` (ctest
`tur_panic_site_across_modules`). Still not sited, and named as the runtime's
line: a sited body entered from C -- a `--shared` library's host, the REPL's
dlopened spice -- where no Turmeric call exists to name.

**Resolved 2026-10-14: the three shapes left open are closed, and the
report is archived.** A sited inline-C body entered THROUGH A FUNCTION VALUE
names the call through the value: `(f v 9)` inside a HOF prints `panic at
input.tur:8` on both engines where the compiled program printed the runtime's
line. The direct emitter's four fat/thin call-through-a-value arms
(`site_fn_value_call`, `src/compiler/emit_expr.c`) and the CPS backend's two
(`e2a_fallback_stmt`, `fncps_direct_call`, `src/compiler/emit_cps_ir.c`) set
the thread's site around the call in the same statement-expression shape a
direct sited call uses -- but only in a unit that takes a panicking inline-C
function as a value ANYWHERE (`emit_fn_value_calls_sited`, answered once per
unit from the CPS classifier's address-taken pre-pass,
`emit_cps_ir_addr_taken_any`): in any other unit no such body can be reached
through a value, and closure calls pay nothing. Where it is on, a 100M-turn
loop of a call through a fn value measured no difference (5.4-5.6 s sited
against 5.7-5.8 s not, run-to-run spread). A `__poly_N` forwarding wrapper's
own forwarding call is NOT sited in such a unit (`emit_direct_call_sited`,
`ctx->in_forwarding_wrapper`): it would have named the line the program
handed the function over, where the interpreter names the program's `(f x)`.
An INSTANCE METHOD whose inline-C body panics, reached by dispatch, names the
dispatching call: the method's binding carries no `source_fn_def`, so
`binding_panics_in_inline_c` finds its FnDef through its definstance
(`inst_method_binding_panics`, `src/compiler/emit_core.c`). The r7rs raising
check (`__tur_any_cast_check_r7`) takes the cast's file and line and panics
through `__tur_any_cast_check_at` when no hook is installed. Interpreted, the
driver's native leaf call SAVES and restores the site instead of clearing it,
so a native HOF that calls back into a native (`turi_call`) or whose Turmeric
callback's own leaf calls come and go still names the HOF's call when a later
native panics inside it; the native-resume apply (`tvar/modify`, `shift`, the
serial receiver) names the requesting form, carried on its `DK_NATIVE_RESUME`
frame. Pinned by `tests/fixtures/panic-site-through-fn-value` (both engines),
`panic-site-through-poly-fn-value` (the CPS backend and the forwarding
wrapper; compiled) and `panic-site-through-instance-dispatch` (compiled).
No `expected.c` moved. Known and accepted: a partial application
(`(vec-get v)`) names the line the partial was made, on both engines; the
site is per unit, so a call from one unit into another's sited body names the
runtime's line; and a stdlib HOF that is a native under `--interpret` but
Turmeric-bodied compiled names the HOF's call interpreted and the HOF's own
`(f x)` line compiled. `docs/guides/debugging-guide.md` says the location is
the call's in every shape.

**Narrowed a third time 2026-10-09: the Saffron dynamic runtime names the
program's line, on both back ends.** `(+ (* a 2) b)` with `b` a string
printed `panic at <emitted>.c:2888: +: no operator for a cstr argument`
compiled and a bare `panic at` interpreted; both print `panic at input.tur:8:
...` now. Compiled, the operator, comparison, `not` and `println` helpers take
the call's site -- a static `tur_site_t` the emitter interns per node
(`emit_site_ref_text`) -- as a trailing argument and panic through
`tur_panic_at` (`ensure_saffron_dyn_runtime`, `src/compiler/emit_module.c`;
`dyn_sited_call`, `src/compiler/emit_expr.c`): an argument rather than the
thread-local slot, because setting and clearing the slot around every
dynamic operator cost a 10M-turn Saffron arithmetic loop 30% (0.11 s to
0.15 s), where the argument costs it nothing measurable (0.10-0.11 s to
0.11-0.12 s, within the run-to-run spread), and because a nested operator
among the operands would have cleared the slot before the outer call ran.
The call-arity check (`cannot call a ... value`) and the field miss (`no field
'.x' on a ...`) are statements off the hot path: they set the slot around
themselves and the helpers panic through `tur_panic_sited`. The dynamic
method dispatch on an `any` sits in the fixed preamble, ahead of the slot, so
it takes the location as arguments (`__tur_inst_slot_at`, as the cast check
does). Interpreted, each of those panics sets `g_panic_site` to its node's
span first (`src/turi/eval.c`). Found on the way, interpreter only: `(+ 6
"x")` did not panic at all -- the overload was selected from argument 0
alone, so the int `+` ran over the string's address and printed it; every
operand must now be of the kind that selected the overload, named in the
refusal as the compiled runtime names it. Pinned by
`tests/fixtures/saffron-dyn-op-panic-site` (an operand built by a nested
operator), `saffron-dyn-field-panic-site`, `saffron-dyn-call-panic-site` and
`saffron-any-dispatch-panic-site`, each asserting the sited line and the full
message on both engines. **Still open:** the r7rs raising check; a sited
body entered through a function value or an instance-method dispatch (no
site set: the runtime's line, or the enclosing sited call's while its
arguments are being evaluated); and the interpreter's panic from a native
reached other than through the driver's leaf call.

**Narrowed again 2026-10-09: a panic raised in a stdlib inline-C body names
the call.** `(vec-get v 9)` on line 12 prints `panic at input.tur:12: vec
index out of bounds` on both engines, where it printed the runtime's own line
compiled and a bare `panic at` interpreted. Compiled: a direct call to a
function whose inline-C body calls `tur_panic(` (vec-get, vec-set!, slice-get,
grid-get/set!, sized-buf-get, the sized-bitvec accessors, json's decoders,
the arrow loop cell; `fn_def_panics_in_inline_c`) sets the thread's current
site to a static `{ "input.tur", 12 }` the emitter interns per call just
before the call and clears it just after -- `({ tur_site_set(&__tur_site_N);
__auto_type r = vec_get(...); tur_site_clear(); r; })` in the general call
arm of `emit_expr.c`, a set and a clear statement around the call line in
the cps->direct arms of `emit_cps_ir.c` -- and `tur_panic` inside such a
body is `tur_panic_sited`, which names the current site (`emit_fns.c`
`#define`s it for the body's extent). One slot (`tur_cur_site`, host slot
`tur_tls_cur_site` under the JIT), not a stack: a sited call among the
arguments sets and clears its own before the outer call runs, so the outer
then names the runtime's line as before, never another call's --
`tests/fixtures/panic-site-nested-sited-args` has such an argument and the
outer call still names its line, because the inner call's clear precedes
the outer's set. A call written in a macro's TEMPLATE -- `vec-set!` expands
to `vec-set-o!` in stdlib/vec.tur -- carries the macro use the program wrote
(`EX_CALL.site`, set by `elab_call` from `elab_macro_use_site`), so `(vec-set!
v 9 5)` names the program's line, not vec.tur's; a call the program wrote and
handed to a macro keeps its own (`vec-bounds-failure-keeps-prior-output`
pins the macro case). Interpreted: the driver sets `g_panic_site` from the call
node before a native leaf call and clears it after, so `turi_runtime_panic`
in `vec-get`'s native prints the same line (`vec-index-out-of-bounds-panics`
pins both). Cost: two thread-local stores per such call, none at entry -- a
100,000,000-iteration loop of nothing but `vec-get` went from 0.09 s to
0.14 s on a 4-core box (the slot is emitted program-side, after the fixed
preamble, because a thread-local in the fixed region is reached through a
host accessor CALL in a hosted build, which doubled that loop). Still open
after this pass: a panic raised in a runtime helper the program never names
-- a Saffron dynamic operator, a dynamic method dispatch on an `any`, the
r7rs raising check -- named the helper's line (the Saffron ones were fixed
the same day, above); so does a sited body entered through a function value
or an instance-method dispatch, and the interpreter's panic from a native
reached other than through the driver's leaf call.

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

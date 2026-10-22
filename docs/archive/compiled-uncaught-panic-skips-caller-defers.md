# Compiled: an uncaught panic runs only the panicking function's defers

**RESOLVED 2026-10-22** (archived), by fix direction 1, scoped to the panic
forms. The emitted C `main` sets a new thread-local, `tur_root_unwind`
(`emit_main_root_unwind_prologue`; slot `tur_tls_root_unwind` in
`src/runtime/tur_tls.c`, registered in `tools/gen-runtime-split.py`). An
emitted `panic` / `panic-with` site now calls `tur_panic_at_site` /
`tur_panic_with_site`. With no `catch-unwind` live, those still print the
message at once, then raise the signal the caught path uses instead of
aborting. Every frame on the way out fires its defers through its own
propagation check, including a CPS chain's (`__dk_panic_abandon`). `main`'s own
checks, and those of the program's top-level statements (`EmitCtx.in_user_main`;
`main` used to have no propagation at all, its C return type being unrecorded),
end the unwind in `tur_panic_root_exit`, which flushes and aborts. The process
still exits 134, with the same message.

Left as it was, deliberately: a panic raised by a runtime helper (a failed
dynamic operator, a bad `cast`) with no `catch-unwind` live still aborts where
it is raised. Those helpers' callers carry no propagation check, so unwinding
from them would run the rest of the caller (measured on the
`saffron-*-panic-site` fixtures). The same goes for other threads and
task-group fibers, which never set the flag. error-handling-guide says so.

Found on the way: a statement-position call to a `: !` function had no
propagation check (only `: nil` calls had one, nil-call-panic-not-propagated),
so under `catch-unwind` the caller ran on past it (`emit_stmt.c`, EX_CALL). It
does now, pinned by `tests/fixtures/never-call-panic-propagates`. The
exhausted-generator panic of `gen-unwrap` gained the same check.

Pinned by `tests/fixtures/panic-uncaught-unwinds-callers` (a direct function,
a CPS function resumed before the panic, two callers and `main`; run by
`run.sh` and `run-turi.sh`). The runtime preamble changed: 158 snapshots and
`src/runtime/generated/` regenerated.

Suite: `run.sh` 4057 passed, 0 failed; `run-jit.sh` over
panic/defer/catch/effect/handle/gen 588 passed, 0 failed; `run-leak-check.sh`
240 passed, 0 failed, 8 known-open.

**Severity: medium** (engine divergence; contradicts error-handling-guide's
"Defer thunks registered before the panic are fired in reverse order during
unwinding"). It also **blocks [discontinue-plan](../upcoming/discontinue-plan.md)
fixture 1 as written**: a `discontinue` raises inside the body, the body's
`defer` lives in the body function, and the plan says the panic reaches the
top level uncaught after that defer runs. Found 2026-10-22 checking what blocks
that plan; reproduces on `main` (`a0a718f3`) and on #1326's head. No effects are needed to see it.

## Repro

```turmeric
(defn boom [x : int] : int (if (> x 0) (panic "boom") x))
(defn helper [] : int
  (defer (println "helper: defer ran"))
  (+ 1 (boom 1)))
(defn main [] : int
  (defer (println "main: defer ran"))
  (println (helper))
  0)
```

| | stdout | stderr |
| --- | --- | --- |
| `tur run` / `tur jit` | (nothing) | `panic at ...:1: boom`, `Aborted` |
| `tur --interpret` | `helper: defer ran`, `main: defer ran` | `panic at ...:1: boom` |

Neither defer runs compiled. When the `panic` is lexically in the function
that holds the `defer` (`tests/fixtures/panic-defer`), it does run. Under a
`catch-unwind` anywhere above, every defer runs
(`tests/fixtures/defer-cps-caught-panic`), so the gap is only the uncaught
case. A CPS-colored function is the same: a `handle` whose body has a `defer`,
resumes, then panics uncaught prints nothing compiled.

## Root cause

`tur_panic_at` (the preamble, `src/compiler/emit_module.c:15922`) has two
branches. With a `catch-unwind` live (`tur_handler_chain`) it sets
`tur_panicking` and returns, and each frame's emitted
`if (tur_panicking) { ... tur_frame_fire_lifo(...); return ...; }` fires that
function's defers on the way out. With none it fires `global_panic_frame`'s
chain and calls `abort()`. `global_panic_frame` is set only at the `panic`
site (`tur_panic_set_frame`, `emit_expr.c:10404`), to the panicking function's
own frame. A function's outermost frame is initialised with a NULL parent
(`emit_expr.c:6066`, `emit_fns.c:1717`), so that chain never reaches a
caller's frame, and the abort comes before any caller's return path runs.

## Fix directions

1. **Unwind the same way whether or not anything catches.** Install a
   process-level landing in the emitted `main` (and at each thread or task
   entry) so `tur_handler_chain` is never NULL in program code. The
   caught-panic path then does the unwinding, and the landing prints the panic
   and exits 134 after the defers have run. That matches the interpreter and
   the guide, and keeps `#[no-unwind]` (`tur_panic_abort`) as the one
   no-unwind path.
2. **Narrower:** link each function's outermost `tur_frame` to the caller's
   through a thread-local "current frame" (push at entry, pop at exit), so
   `global_panic_frame`'s chain spans the whole stack. That costs every
   function with a defer two stores, and does not cover CPS frames.

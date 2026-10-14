# `(scheme eval)`: a continuation captured inside a call from evaluated code to a program procedure cannot be re-entered after that call returns

**Resolved 2026-10-14** (see Resolution at the end): the evaluator now keeps
what its frames in such a continuation need, and both back ends re-enter it
with the R7RS answer. What stays refused is narrower: a capture inside a
host call made from a text being evaluated, resumed after that `eval` has
returned.

**Severity:** low (a refused program, not a wrong answer). Both back ends
refused it with a named error. Before 2026-10-14, the compiled back end freed
memory twice. The interpreter refused it only by accident, through state the
two envs should not have shared. Found 2026-10-14 while fixing the
`(scheme eval)` half of turi-dropped-closures-kept-until-teardown.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme eval))
(define saved #f)
(define count 0)
(define (run)
  (let* ((env (environment '(scheme base)))
         (r ((eval '(lambda (f) (+ 1 (f 41))) env)
             (lambda (x) (call/cc (lambda (k) (set! saved k) x))))))
    (write (list 'result r)) (newline)
    (set! count (+ count 1))
    (if (< count 3) (saved (* count 100)))
    (write (eval '(+ 1 2) env)) (newline)))
(run)
```

R7RS answers `(result 42)`, `(result 101)`, `(result 201)`, `3`. Both back
ends do now. Before the resolution:

| back end | output |
| --- | --- |
| `tur --interpret` | `(result 42)`, then `call/cc: the continuation was captured inside a call from evaluated code to a program procedure, and that call has returned; the evaluator cannot resume it` |
| compiled | `(result 42)`, then `tur: eval: a continuation captured inside a call ... was invoked after that call returned; the evaluator cannot resume it`, exit 70 |

What works, on both back ends:

- **Escaping** out of such a call through a continuation captured outside
  it.
- **Re-entering** a continuation while the call it was captured in is still
  running, such as a loop written with `call/cc` inside the program
  procedure.

## Root cause

A re-entrant continuation is a copy of the C stack (stdlib/r7rs/prelude.tur,
r7rs-call/cc). Captured inside a host call, it holds a stack of frames:

- the program's frames;
- the bridge's frames (src/turi/r7rs_embed.c, `host_native`, `embed_call`,
  `turi_r7rs_embed_apply`);
- the evaluator env's driver frames, whose state lives on the heap.

Re-entering it after the host call returned resumes all of them. The bridge's
own state survives that since 2026-10-14:

- A host call's arguments are kept in a list by serial, freed by the first
  return that finds them.
- The answer travels through a slot the host function fills as its last act.

The evaluator's state is what does not survive:

- **Compiled.** The program's call/cc does not tell libturi that a capture
  happened. The evaluator's driver frees that call's work-stack temporaries
  when the call first returns (`TURI_DRIVE_FREE_OWN`, src/turi/eval.c). A
  re-entry freed them a second time: ASan's "attempting double-free" at
  eval.c:10873.
- **Interpreted.** The capture pins the epoch, so the frames and temporaries
  stay. But the evaluator env's own dynamic state is not restored: its live
  escapes (`env->escape_live`) and the process-global prompt state
  (`g_r7k_form_depth`). With the bridge's state fixed, the re-entry ran on and
  failed with `cdr: not a pair ()`.

`r7rs-cont-restore__` therefore refuses the continuation in the interpreter
before anything is restored (`R7kCont.bridge_call`,
`turi_r7rs_embed_call_live`). `host_native` stops a compiled program at the
second return, the first point libturi can see it.

Before 2026-10-14, the interpreter's two envs shared one set of
dynamic-environment slots. The program's wind list then held the
evaluator's winds, so re-entering `k` ran an evaluator wind thunk from the
program env. That thunk reached an escape whose prompt had returned:
`continuation invoked after its call/cc prompt returned`.

## Fix directions

1. **Make the evaluator's state part of the continuation.** In the
   interpreter, save the embedded env's `escape_live` and the form-prompt
   globals with a capture (`TuriContState`), and restore them with it. For a
   compiled program, the runtime's call/cc would also have to pin libturi
   (`turi_cont_pin`) at a capture. It reaches libturi only when the program
   imports `(scheme eval)`, so that needs a hook the eval library installs, as
   `tur_r7rs_type_error_hook` is installed.
2. **Refuse it earlier in the compiled back end**, through the same hook: let
   r7rs-cont-check-here__ ask whether the bridge call is still live. The
   program then gets a catchable error before the wind travel, as the
   interpreter does, not an exit.

## Resolution (2026-10-14)

Fix direction 1, with the refusal of direction 2 kept for the one case that
stays out of reach. The evaluator's side of a program continuation is a
handle (`src/turi/r7rs_embed.c`, `turi_r7rs_embed_cont_*`) taken at the
capture and used at the re-entry:

- **At the capture**, the embedded env's dynamic-extent fields (its live
  escapes above all), its dynamic-environment slots, and the host call in
  progress. For a compiled program, whose call/cc keeps no interpreter state
  of its own, also the drivers' work stacks and the interpreter's thread
  globals (`turi_cont_state_capture`), and the interpreter is pinned
  (`turi_cont_pin`), so the frames and temporaries the image holds are not
  freed at the first return. An interpreted program's own capture holds
  those already, so the handle takes the env's fields alone
  (`turi_cont_state_capture_env`).
- **At the re-entry**, after the image is back: the state is restored, the
  host calls and evaluations begun since the capture are dropped, the
  evaluator's wind stack is travelled to the captured one (R7RS 6.10, so an
  evaluated `dynamic-wind`'s `before` runs again and the handlers and
  parameters of that extent are reinstalled), and the slots are put back.
- **The bridge's host calls** live in a list by serial rather than in
  `host_native`'s frame, and the answer in a slot the host fills as its
  last act, so a second return through the same frame finds what it needs.

The interpreter reaches the handle from its own capture
(`native_r7rs_cont_capture`, `R7kCont.embed`). A compiled program reaches it
through four hooks in the prelude's call/cc (`r7k_hook_capture`,
`_resumable`, `_restore`, `_resumed`; stdlib/r7rs/prelude.tur), which
`stdlib/r7rs/eval.tur` installs at startup through
`r7rs-cont-hooks-install__`, the way the type-error hook is installed. A
program that never imports `(scheme eval)` has null hooks and links no
libturi.

**What stays refused.** A capture inside a host call made from a text being
evaluated (`(eval '(f 1))`, as against an applied procedure) also copies the
frames of that evaluation, which end with its `eval`: resumed after that
returned, they would finish the evaluation a second time (seen as a
use-after-free of the evaluation's source buffer). The bridge keeps the
evaluations in progress by serial, and such a continuation can be resumed
only while its evaluation is still in progress -- then the rest of the text
runs again, as R7RS asks. Afterwards `r7rs-cont-check-resumable__` raises
an error object the program can `guard`, before any wind thunk runs, on
both back ends:

    call/cc: the continuation was captured inside an `eval` that has since returned; it cannot be resumed

**Memory.** Nothing tells libturi when a compiled program's continuation
dies, so from the first such capture on the evaluator's pool sweep declines
("a program continuation holds the evaluator's frames"); under the
interpreter it already declined while a program continuation was live. The
pinned temporaries stay until teardown, the interpreter's policy before
the pool sweep.

**Checked.** `tests/fixtures/r7rs-eval-callcc-reentry` runs on both back
ends: the repro above; a loop written with `call/cc` inside the program
procedure (re-entered while the call runs); a re-entry into an evaluated
`parameterize` / `dynamic-wind` / `guard` extent, with a raise from the
re-entered program procedure caught by the evaluator's guard and a later
raise caught by the program's; an escape out of evaluated code; and the
text case, resumed while the evaluation runs and refused after. It passes
under `tur jit`, and under quarantine stress with a sweep every 8 KB,
interpreted and compiled. `run-turi.sh`, `run.sh`, the R7RS conformance
suite and the eval ctests pass.

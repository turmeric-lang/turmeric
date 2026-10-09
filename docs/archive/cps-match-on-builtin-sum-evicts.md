# A colored arm under a `match` on an Option, a Result or a literal evicts the function

**RESOLVED 2026-10-09** (filed the same day). See "Fixed" at the end.

**Severity was: high.** This was a compile-time refusal ("this effect
operation has no lowering here") of the most ordinary way to look at a value
an effectful function is handed: matching an `(Option A)` or
`(Result T E)`, or matching an integer against literals, with an arm that
performs or calls a colored function. `tur --interpret` ran it. A numeric
`(as T ...)` around a colored call was refused the same way. Found
2026-10-09 while executing
[cps-effectful-callback-through-multi-arg-or-untyped-param](../reported/cps-effectful-callback-through-multi-arg-or-untyped-param.md).

## Repro

```turmeric
(defeffect Ask [] :int)
(defn m1 [o : (Option int)] : int (match o (Some v) (+ v (perform (Ask))) (None) 0))
(defn m4 [n : int] : int (match n 1 (perform (Ask)) _ 5))
(defn c1 [] : float (+ 0.5 (as float (perform (Ask)))))
(defn main [] : int
  (println (handle (m1 (some 1)) (Ask [] k) (resume k 10)))   ; 11
  (println (handle (m4 1) (Ask [] k) (resume k 10)))          ; 10
  (println (handle (c1) (Ask [] k) (resume k 7)))             ; 7.5
  0)
```

`TUR_TRACE_EVICT=1`: `BODY-UNSUPPORTED m1 unsupported form: EX_MATCH`, the
same for `m4`, and `unsupported form: EX_CAST` for `c1`.

## Root cause

The CPS translation (`src/passes/cps_ir.c`) lowers a `match` itself only in
`match_dk_ok`'s shape: a tagged ADT scrutinee whose `->tag` and
`->as.Ctor._N` the CPS emitter reads directly
([colored-call-inside-match-evicts-the-cps-backend](colored-call-inside-match-evicts-the-cps-backend.md)
widened that to by-value ADTs). Any other match was translatable only by
delegating it whole to the direct emitter. That is impossible once an arm
carries control, so the function was evicted. The built-in sums are not a
`TY_ADT` scrutinee, and neither are literal and `any`-narrowing arms.

`EX_CAST` had no arm in `cps_tail` or `cps_bind`. A cast whose operand
carried control fell to the default, which can only delegate, so it was
evicted the same way.

## Fixed

**The direct emitter dispatches; the CPS emitter emits the arms.** The
direct match emitter already routes every arm body through one hook,
`EmitCtx.match_tail`, on all its arm shapes but a session offer. That hook
was added for proper tail calls (T3), and `emit_tail` uses it. A new
CT_MATCH form (`match.direct`, `src/passes/cps_ir.h`) takes the hook:

- `build_direct_match_term` binds the scrutinee to an atom (through
  `cvar_expr`, so the copied match names it), unless it is already a
  variable. It CPS-translates each arm body and records each arm's pattern
  binders as its fields, for the capture analyses.
- `emit_direct_match` (`src/compiler/emit_cps_ir.c`) hands the copied match
  to `emit_value` with the hook set. Each callback emits the arm's CPS term
  in place, into the buffer the direct emitter is writing. The arm ends in a
  `return` or a join's `goto`, like every CPS arm. The direct emitter's result
  temp and `goto <end>` stay, unreachable, exactly as on the tail path.
- The emitter checks that every arm came back through the hook exactly once.
  An arm emitted as a plain value would fall out of the match into the
  trailing `abort()`, so a miss is an internal error, not a silent wrong
  answer.
- `match_direct_dispatch_ok` admits any non-`!`-typed match without guards.
  A guard is direct-emitted inside the dispatch, and the capture analyses
  would have to read its free variables, so a guarded match is left as it
  was.

The continuation gates admit a CT_MATCH the way they admit a CT_IF:
`perform_body_ok`, `perform_cont_reset_ok`, `await_cont_reset_ok`,
`shift_body_ok`, `case_loop_body_ok`, `handle_case_ok_rec` and
`joins_closed_rec`. So do the three resume-protocol walks: `case_reopens`,
`term_resumes_nontail` and `term_k_only_resumed`. The B4 tag dispatch gains
from this too; before, a B4 match in a perform's continuation or a handler
clause evicted.

**`as`** binds its operand to a fresh binder and delegates a cast of that
binder (`cps_bind_cast`), as `cps_bind_reinterp` already did for a Tier A
reinterpret.

**Found on the way and fixed: a capture's recorded C type outlived its
helper.** A lifted CPS helper reads each capture out of its env into a local
and records the local's C spelling in the program-wide name -> C-type table
(`emit_localvar_record_ctype`). The carrier bridges consult that table. The
local is named after the SOURCE binding, so a `double x` capture stayed
recorded under `x` for the rest of the program. The prelude's
`vec-eq-loop`, whose `x` is a Vec, then had `x` bit-cast as a double at its
calls (`((union { double s; int64_t d; }){.s = (x)}).d`).
`tests/check-emitted-float-conversions.py` reports it on HEAD for a program
that compiled there: an effectful lambda `(fn [x : float y : int] ...)`
through an annotated-row parameter, with the capture crossing a `perform`.
The read-out now saves each name's previous spelling
(`emit_localvar_save_ctype`) and restores it when the helper's body is done
(`emit_localvar_restore_ctype`). A temp's name is unique in the program and
needs neither.

Measured: the fixture suite is unchanged (3642 passed, no snapshot moved),
and `check-emitted-float-conversions.py --corpus` reports no findings.

Pinned by `tests/fixtures/cps-match-builtin-sum-colored-arm`. It covers:
- an Option and a Result scrutinee, tail and bind position;
- int literal arms with a wildcard;
- a computed scrutinee;
- a nil-typed match;
- a match after a perform, with its scrutinee crossing into the lifted
  continuation;
- a nested match;
- a match in a handler clause that re-performs and resumes.

The `as` shapes are in `effect-row-under-match-and-cast` and
`cps-effectful-callback-scalar-kinds`; the latter also pins the stale
capture record (four findings before). Every line equals `tur --interpret`.
The three fixtures are leak-checked, pass the JIT harness and run clean
under `-fsanitize=function`.

Not covered: a match with a guard is still evicted when an arm is colored.

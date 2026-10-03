# The CPS backend still evicts some `handle`s over effectful fn fields

**Severity: medium.** A legal program is refused at build time with "this
effect operation has no lowering here". This is an expressiveness gap, not a
miscompile. It is what remains of the type fuzzer's `GEN_REJECT`s after
`handle-over-effectful-fn-field-in-arg-let-evicted` (archived) was fixed.

**Narrowed 2026-10-03: item 5 (seed 3333 case 121) is fixed; (3) and (4)
remain.**  It was not the generic-over-rank-2-over-`handle` the shape
suggested.  Reduced, it needed only a `handle` in `main` plus a call to a PURE
generic whose body applies a lambda literal on the spot -- `(let [b x] ((fn []
b)))`.  The coloring pass (`cps_collect_calls`, cps.c) counted that applied
literal as an unresolved call, so the generic was colored; its signature then
kept it off the CPS backend, but `main` still translated the call as
cps->cps, with a join over its fat-closure result that is not
slot-representable (the `CT_LETCONT` `TUR_TRACE_CORE` named).  An applied
literal -- directly, or through the `__call_head_N` temp the elaborator hoists
it into (`closure_head_init`) -- has exactly one callee, its own lifted
lambda, so it is now an edge to that lambda's node: a pure lambda leaves the
generic uncolored, an effectful one still colors it.  Pinned by
`tests/fixtures/handle-beside-applied-lambda-generic`.  Seed 3333: 0
`GEN_REJECT`s in 300 (was 1); seed 4444: 1, item (4).

Found on the way, pre-existing and unchanged: a lambda literal that itself
PERFORMS, applied on the spot inside a function called under a `handle` --
`(defn app [] : cstr ((fn [] (perform (E "hi")))))` -- is "no lowering here"
compiled (it runs under `--interpret`), on the old coloring and the new.

**Narrowed 2026-10-01 (second pass): shapes (1) and (2) are fixed, plus two
neighbours the fixes exposed; (3) and (4) remain.**  Fuzz seeds 3333 and 4444
went from 5 `GEN_REJECT`s in 600 cases to 2 (one per seed): seed 3333 case 121
(below, "What is left") and seed 4444 case 93, shape (4).

## Fixed (2026-10-01)

- **(2) `unsupported form: EX_REINTERPRET`.**  A generic call whose result the
  elaborator re-typed to its instantiation is wrapped in a Tier A reinterpret
  (`int -> cstr`).  `safe_to_delegate` had no arm for it, so even a reinterpret
  over a pure call evicted the function; it is now exactly as delegatable as
  the value it converts.  A reinterpret whose operand carries control
  (`(g2 (handle ...))`) is decomposed: the operand into a binder of its own
  type, then the retype delegated over that binder (`cps_bind_reinterp`).  The
  previous attempt changed `cps_bind`/`cps_tail` only, which is why the trace
  did not move: the refusal came from `safe_to_delegate`.  Pinned by
  `tests/fixtures/handle-under-generic-call-reinterpret`.
- **(1) A rank-2 / fn-value (indirect) call with a `handle` operand.**  An
  indirect call was delegated only with LITERAL arguments.  When an argument
  carries control, the arguments are now bound first, left to right, and the
  call delegated over the binders (`delegate_call_atomized`); a constrained
  forall's dictionary argument (`EX_DICT`) rides inline.  In BIND position an
  indirect call whose arguments are merely non-literal is delegated whole --
  that was what evicted a `main` holding a `handle` for an unrelated
  `(.app s (g "x"))`.  TAIL position keeps the literal rule: delegating a tail
  indirect call turns it into a bind and a return, and the r7rs programs that
  recurse through closures ran out of stack (seen and reverted while landing
  this).  Pinned by `tests/fixtures/handle-in-rank2-call-operand`.
- **A rank-2 argument atomized ahead of a `handle`** -- `(ru ri (handle ...))`
  -- emitted INVALID C (pre-existing, on every compiler): the handle's
  continuation captured the `tur_poly_fn_t` binder and declared its env field
  `void *`.  The capture set now knows the fresh binders a poly-wrap
  `CT_LETRAW` binds (`cvar_is_polyfn`).  Same fixture.
- **A cps->direct call to a resolved clone** passed an atomized carrier word
  where the clone takes `const char *` (a `-Wint-conversion`, a hard error on
  clang / gcc 14); `atoms_csv_call_clone` bridges a variable whose recorded C
  type differs from the clone's parameter.

`TUR_TRACE_EVICT=1` now names a reinterpret's kinds and the form it wraps
(`EX_REINTERPRET int -> cstr of EX_CALL`), and `EX_CALL` / `EX_GET_FIELD` /
`EX_DICT` by name instead of `EX_#n`.

## What is left

3. **A signature-rejected generic HOF** (`l2gbapp11 SIG-REJECT`) on the path
   from the handler to the performer.
4. **A join continuation that would capture the field-load callee.**
   `(handle (.run (make-struct FE fe) (let [c false] (t (fn [] c)))) ...)`
   is `BODY-STRUCT-JOIN` (`needs_heap_join`).  Before the E2c capture fix
   (`CC_ATOM` / `COL_ATOM` of `tailcall.fn_atom`, 2026-10-01) the join read the
   callee atom uncaptured, which was invalid C (`use of undeclared identifier
   '__t2'`).  It is a clean refusal now, not admitted.  Seed 4444 case 93.
5. **FIXED 2026-10-03 (see the top).  Seed 3333 case 121**: `(l0gb8 (l0ru7 l0ri6 (handle ...)))` -- a generic
   call over a rank-2 call over a `handle`.  The reinterpret and the rank-2
   operand both lower now; what still fails is `main`'s structural core check
   (`BODY-STRUCT-CORE`; `TUR_TRACE_CORE=1` names a `CT_LETCONT` whose join
   parameter is not slot-representable -- the innermost reject, with nothing
   failing beneath it).  Not yet reduced further.

## Regression check

The fuzzer's `GEN_REJECT` count on a fixed seed: `python3
tests/type-fuzz-src.py --n 300 --seed 3333`, which is 0 since 2026-10-03
(seed 4444: 1, item 4).

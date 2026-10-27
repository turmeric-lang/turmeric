# A delegated user-fn call in a reified heap-join continuation evicts (BODY-STRUCT-OR-TAINT)

**STATUS: RESOLVED.** Fixed by adding a `letraw_effect_free`-gated `CT_LETRAW` case to
`handle_delim_ok` (src/compiler/emit_cps_ir.c): a PURE delegated op (a call to an
effect-free callee, or a raw rc/field/struct op) inside a handled body's delimited
position is now admitted, so the subsequent `KK_PROMPT` delivery stays the handle's own
result and `term_core_ok` no longer rejects it. An EFFECTFUL delegated call still falls to
`term_core_ok` and evicts (handle-effectful-fn-param-same-fn unchanged). Always-on (a
shipping-backend BODY-* reduction, not flag-gated); default suite 2190/0. Regression
fixture `cps-pure-delegated-call-in-handle`. The minimal repro below now CPS-emits.

NOTE the corpus fixtures listed under "Affected real fixtures" did NOT move off the fiber
yet -- each has a COMPOUND cause (effect-row-poly: a `#{e}` row-variable call reads as
non-effect-free via callee_effect_free; effect-subtype-capability: an effectful-fn-in-
struct-field, E2-adjacent). This fix removes the delegated-call LAYER of their blocker
stack; those other causes remain separate future work.

Retained for the paper trail below (severity/repro/analysis as originally filed).

**Severity:** medium (blocked a class of real-effect fixtures from the CPS/DK backend;
correctness was fine -- they fell back to the fiber and ran correctly).

**One-line:** When a colored function makes a NON-tail call to a colored helper and the
reified continuation contains a *delegated (direct-emitted) user-function call*
(`CT_LETRAW`), the function is dropped from the CPS set as BODY-STRUCT-OR-TAINT. A
*native/builtin* call (`CT_LETPRIM`) in the exact same position is fine.

## Minimal repro

```turmeric
(defeffect Write [msg : cstr] : nil)
(defn greet [name : cstr] #fx{Write} : nil (perform (Write name)))
(defn add-int [x : int y : int] : int (+ x y))          ; a PURE user fn
(defn main [] : int
  (handle
    (do (greet "hi") (add-int 3 4) 0)                    ; <-- delegated user call after greet
    (Write [msg] k) (do (println msg) (resume k 0)))
  0)
```

`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume repro.tur` reports
`[EVICT] BODY-STRUCT-OR-TAINT eff=1 main` (and greet, tainted downstream).

Replace `(add-int 3 4)` with a native/builtin -- `(println 7)`, `(println (+ 3 4))` --
and BOTH greet and main CPS-emit cleanly. The trigger is specifically a call to a
user-defined (non-colored) `defn`, not a native. `(greet "a") (greet "b")` (two colored
calls) is also fine.

## Root cause (dump diff)

`tur emit-c --enable=cps-tramp-resume --dump-cps` shows `main` identical except for one
node in the `j3` continuation reified after the non-tail `tailcall greet("hi" j3)`:

```
  WORKS  (println 7):    let __t1 = (println 7)                 ; CT_LETPRIM (native)
  EVICTS (add-int 3 4):  let __t1 = <owning-op ?>  ; direct-emitted   ; CT_LETRAW (delegated)
```

Both `j3` bodies then `(<prompt> 0)`. So the reified heap-join continuation of a non-tail
colored call carries the post-call work as a `CT_LETRAW` when it is a delegated user call,
and that `CT_LETRAW` is what makes `main` non-`in_s`.

### Pinned root cause (instrumented)

Traced in the eviction categorization (emit_cps_ir.c ~5850, the BODY-STRUCT-OR-TAINT
branch). For the repro, with `--enable=cps-tramp-resume`:

```
G (add-int, EVICTS):  main  needs_heap_join=0  term_core_ok=0  first_unsupported=NULL
                      greet needs_heap_join=0  term_core_ok=1   (tainted down via Write)
E (println, WORKS):   (no drop -- main term_core_ok=1)
```

So the drop is NOT `needs_heap_join` (Rule A) -- it is `term_core_ok(main.term) == false`,
i.e. main is not in the C1 core subset. The initial drop is main; that taints `Write`,
which then drops the symmetric `greet`.

The sharp anomaly: **`term_core_ok` returns false while `first_unsupported` returns NULL --
the two DISAGREE.** `first_unsupported` (emit_cps_ir.c ~418) is meant to name the residual
form, but it finds none, so the categorizer falls through to the unnamed
BODY-STRUCT-OR-TAINT bucket.

**EXACT failing sub-check (instrumented):** `term_core_ok`'s `CT_APPCONT` case
(emit_cps_ir.c ~1685):

```
        case CT_APPCONT:
            return t->as.appcont.kont.kind != KK_PROMPT && atom_ok(&t->as.appcont.v);
```

For the repro it fires `[TCO] APPCONT fail: kont=2 (KK_PROMPT) atom_ok=1`. So a bare
deliver-to-prompt (`(<prompt> 0)`, the handle body's terminator delivering `0`) reaches
`term_core_ok` and is rejected because `kont.kind == KK_PROMPT`. In the WORKING variant
(native `println`) no `CT_APPCONT KK_PROMPT` reaches `term_core_ok` -- the native path
folds the deliver away, whereas the delegated user call (`CT_LETRAW`/`CT_LETCALL`) leaves
the `deliver 0` as a standalone `CT_APPCONT KK_PROMPT` node in the reified `j3` continuation.

So the two threads to pull:
1. Why does the CPS lowering leave a bare `CT_APPCONT{KK_PROMPT}` after a delegated call
   but fold it after a native/`CT_LETPRIM`?  Making the delegated-call continuation lower
   the same way the native one does is the most surgical fix.
2. Is the `kont.kind != KK_PROMPT` reject in `term_core_ok`'s `CT_APPCONT` actually
   necessary for a deliver-to-prompt in a heap-join continuation?  If a `CT_APPCONT
   KK_PROMPT` there is emittable, admit it (and make `first_unsupported` name it either
   way so the two functions stop disagreeing).

`letraw_ok` is NOT the cause (it only rejects owning-alloc/ref ops; `add-int` is neither).
The `<owning-op ?>` dump label is just the dumper's fallback for a `CT_LETRAW` payload it
does not special-case -- not a real owning-op misclassification.

## Affected real fixtures (BODY-STRUCT-OR-TAINT, --enable=cps-tramp-resume)

`effect-row-poly` (greet/main), `effect-reopen` (counted-sum/main),
`effect-subtype-capability` (main -- also has an effectful-fn-in-struct-field, a separate
E2-adjacent issue), `cps-backend-owning-struct-field-op-capture-direct` (f/g). Fixing this
delegated-call-in-continuation case should clear the "-or-taint" half of several of them.

## Fix direction

Teach the CPS heap-join path to admit a `CT_LETRAW` (delegated user call) in a reified
continuation the same way it admits a `CT_LETPRIM`: the lifted frame just direct-emits the
call (`add_int(3,4)`) into the frame body, exactly as the non-lifted direct emitter would.
The operands are already capture-scanned by `collect_caps_rec`'s `CT_LETRAW` case. Confirm
`emit_heap_join` emits the `CT_LETRAW` body, and that whatever `ensure_S` rule drops `main`
is relaxed for this shape. Verify with the repro above (expect `hi` then the program runs),
then the flag-on sweep + default suite.

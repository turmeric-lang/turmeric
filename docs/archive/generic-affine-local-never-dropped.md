# A generic-typed affine local is never dropped

> **RESOLVED 2026-11-05.** A `let` local typed by a generic's type variable,
> initialised by a call (or a block yielding one), whose every use is a direct
> argument at a `^borrow` parameter -- a plain one, or the class signature's
> `^borrow` for a dictionary dispatch -- now gets a per-specialization drop.
> The elaborator appends `(defer nil)` drop glue carrying `spec_drop_of = c`
> (`src/compiler/elab_forms.c`, `binding_is_spec_drop_candidate` /
> `spec_drop_uses_ok`); the emitter resolves `c`'s type in the active spec and,
> when it is a move-only Drop opaque (the rule a concrete local gets), runs
> that instance's drop method there and nothing elsewhere
> (`emit_defer_body`, `src/compiler/emit_core.c` -- every site that runs a
> defer body goes through it). `tur --interpret` does the same from the
> activation's tyvar pin (`turi_spec_drop_body`, `src/turi/eval.c`). A local
> that is returned, rebound, captured, stored or handed to an owning parameter
> keeps the old behaviour (no drop): a type-variable value is not
> move-tracked, so only a body that visibly never hands it on is safe.
>
> Found on the way: the CPS marking pass (`src/passes/cps.c`, `EX_DEFER`)
> rebuilt each defer field by field and dropped `is_drop_glue`; it now copies
> it and `spec_drop_of`.
>
> Pinned by `generic-affine-local-dropped` (every shape above, `Res` and
> `int`, on both engines) and `numvec-class`, which no longer frees
> `na-copy`'s result by hand and stays leak-checked.

**Severity:** medium-low (leak). A value whose type is a generic's type
variable, bound in a `let` inside that generic, is not dropped at scope exit
in any specialization -- even when the specialization's type is `^affine`
with a `Drop` instance. The same code at the concrete type drops it.

## Repro

```turmeric
(defopaque ^affine Res :int)
(defn res-free [r : Res] : nil (println "freed"))
(definstance Drop [Res] (drop [x] : void (res-free x)))
(defclass Mk [v] (mk-copy [^borrow a : v] : v))
(definstance Mk [Res] (mk-copy [a] (:: 7 Res)))

(defn use-copy [V] [(Mk V)] [^borrow a : V] : int
  (let [c (mk-copy a)]
    1))
(defn use-copy-concrete [^borrow a : Res] : int
  (let [c (mk-copy a)]
    1))

(defn main [] : int
  (let [r (:: 3 Res)]
    (println (use-copy-concrete r))   ; prints "freed", then 1
    (println (use-copy r))            ; prints 1 -- c is never freed
    0))
```

Found with `NumArray`'s `na-copy` (`stdlib/numvec.tur`): a generic
`(let [c (na-copy a)] ...)` leaks the copy on all three engines (LeakSanitizer
under `tur jit` reports the header and data block per specialization).

## Cause (as far as traced)

Scope-exit drops are decided where the binding's type is known to have a
`Drop` instance. In the generic body the type is `V`; nothing re-asks per
specialization, where `V` is `Res` / `F64Vec`. Not traced to the line.

## Workaround

`NumArray` has `na-free`; `tests/fixtures/numvec-class` frees its copy with
it. Any class that hands back an owned `v` needs the same until this is fixed.

## Fix directions

Re-run the drop decision per specialization (the emitter knows the spec's
binding of `V`), or have the elaborator record a "drop if the instance type
has `Drop`" obligation on generic-typed affine locals that each spec resolves.

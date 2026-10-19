# A `@Type` witness call skips the argument coercions a receiver dispatch applies

**RESOLVED 2026-10-19** -- see [Resolution](#resolution-2026-10-19). Pinned by
`witness-dispatch-fn-arg` and `comonad-extend-witness`.

**Severity: medium (cc failure).** Calling a typeclass method through the
`@TypeName` witness -- the fix TUR-E0020 itself recommends for an erased
receiver -- hands a lambda to a `: fn` method parameter as a bare `void *`, so
cc rejects the program. For stdlib `Comonad` this means `extend` cannot be
called through the class at all: its two instances (`identity`, `pair`) share
the `:int` carrier, so a bare `(extend w f)` is ambiguous and `(.extend
@identity w f)` does not compile.

Found writing the Comonad API docs (2026-10-19).

## Repro

```turmeric
(defclass Cm [^w] (ex [wa] : int) (ext [wa f : fn] : int))
(definstance Cm [box] (ex [wa] wa) (ext [wa f] (f wa)))
(defn main [] : int
  (println (.ext @box 41 (fn [wa : int] : int (+ wa 1))))
  0)
```

```
error: incompatible type for argument 2 of '__inst_Cm_ext_box'
note: expected 'tur_poly_fn_t' but argument is of type 'void *'
tur: cc invocation failed (status 256)
```

The same call without the witness, `(ext 41 (fn ...))`, prints `42` -- it
dispatches to the sole instance through the receiver path. With stdlib
`Comonad`: `(.extend @identity (__identity_new 42) (fn [wa : int] : int ...))`
fails the same way, while `.extract @identity` and `.duplicate @identity`
(no `: fn` parameter) work.

## Root cause

`elab_method_call`'s witness branch (`src/compiler/elab_typeclasses.c`, the
`Phase D1` block, ~line 7312) elaborates the extra arguments with plain
`elab_form` and builds the direct call. The receiver-dispatch path further down
runs three coercions over the same arguments that the witness path never does
(~lines 9343-9530):

1. `EX_POLY_WRAP` for a `: fn` (`is_poly_fn`) parameter -- the missing one here;
2. `arrow_fat_shim` for an `is_fat` parameter;
3. `elab_coerce_to_any` for an `any` parameter.

## Fix directions

Lift those three loops into one helper taking the method `FnDef`, the receiver
and the argument array (plus the span offset of the first argument for
diagnostics, which differs by one between the two call shapes), and call it
from both paths. The witness path has no abstract-tyvar receiver, so it passes
`obj_is_abstract_tyvar = false`. Pin with the repro above and a stdlib
`Comonad` `extend` through `@identity` and `@pair`.

## Resolution (2026-10-19)

The three coercions now live in one helper, `method_call_coerce_args`
(`src/compiler/elab_typeclasses.c`), lifted verbatim from the receiver path.
Both paths call it: the receiver path with the argument items starting at
index 2 of the call form, the witness branch with them starting at index 3
(after the `@Type`). The witness branch passes `obj_is_abstract_tyvar =
false`, since a witness names a concrete instance.

The repro prints `42`, and so does stdlib `Comonad`: `(.extend @identity w f)`
and `(.extend @pair p f)` compile and run, with a captureless lambda, a
let-bound capturing closure and a closure returned from a function. The
compiled and interpreted engines agree.

- `tests/fixtures/witness-dispatch-fn-arg` -- two instances sharing the
  carrier; an inline lambda, a capturing closure, a captureless let-bound
  lambda and a named function through `@box` / `@twice`. Runs under both
  `run.sh` and `run-turi.sh`.
- `tests/fixtures/comonad-extend-witness` -- stdlib `extend` through
  `@identity` and `@pair`, including the `extract (extend w f) = f w` law.

No existing snapshot moved: every receiver-path call goes through the same
code as before, and no witness call with a coerced argument compiled until
now.

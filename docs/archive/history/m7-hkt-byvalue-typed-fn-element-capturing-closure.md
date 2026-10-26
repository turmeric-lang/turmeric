# M7 by-value HKT: a CAPTURING closure passed as a typed-fn element param drops its env

> **RESOLVED for plain-element shapes (2026-06-19).** A typed `(fn [a] b)`
> element param whose RESULT is a plain element (`fmap`, Bifunctor `bimap`,
> Applicative `ap`, Alternative `<|>`) is now marked `is_poly_fn` under the
> by-value path, so it lowers to the `tur_poly_fn_t` `{env, fn}` carrier and
> `(g x)` preserves a CAPTURING closure's env -- exactly as the regular defn
> path does. Probe `docs/upcoming/v2/m7-hkt-probe-capturing.tur` exits 121
> (capturing mapper through `bimap` AND the partial-app `(Result _ B)` head).
> Suite 1685/0 flag-on AND flag-off (the marking is gated on `g_m7_hkt_enabled`
> and inert on the legacy carrier path).
>
> **Two coordinated pieces (both `elab_typeclasses.c`):**
> 1. Param loop: mark a TY_FN element param `is_poly_fn` -- SCOPED to a plain
>    (non-`TY_APP`) result, so a continuation returning an HKT-applied `(m b)`
>    (`bind`/`traverse`) is excluded (it regresses under the carrier switch --
>    see the experimental finding below).
> 2. Rank-N `params[0]` dispatch path: mirror the args-path closure fallback so a
>    lambda/closure argument that lands in `params[0]` (Bifunctor `bimap [g h x]`,
>    whose first param is a mapper, not the HKT receiver) is wrapped as an
>    `EX_POLY_WRAP` closure instead of erroring "must be a named function".
>
> **Residual (narrower, NOT a migration blocker for Functor/Applicative/
> Alternative):** an HKT-RETURNING continuation -- Monad `bind`'s `k : (fn [a]
> (m b))`, Traversable `traverse` -- is deliberately excluded from the marking, so
> a CAPTURING continuation there still drops its env. The Monad `[Result]`/
> `[Option]` migration will need this reconciled (the continuation's wrapped
> `(m b)` result must be unpacked through `tur_poly_fn_t`); tracked here for that
> later effort. The carrier path (`TUR_M7_HKT=0`) does not support capturing
> through any HKT element param (Phase-5-deprecated).

**Summary.** Under the by-value HKT path (default), an instance method whose
element mapper is a typed function param (`g : (fn [a] b)`) emits that param as a
bare `int64_t` and bare-calls it `((int64_t(*)(int64_t))(intptr_t)g)(x)` in the
by-value spec body. For a NON-capturing function (a top-level `defn` or a
non-capturing lambda) the int64 holds a raw function pointer and the call is
correct. For a **capturing** closure the int64 holds a fat-closure handle whose
`fn(env, x)` entry needs the env -- the bare raw-pointer call drops it and
**segfaults**. Severity: **hard crash / silent miscompile** for the common
`(fmap xs capturing-closure)` shape.

This is the last gate for migrating Functor/Monad/Applicative/Alternative
`[Result]` (and the rest of the by-value HKT stdlib) to pure-Turmeric bodies:
once a class signature is typed (required for by-value element recovery), every
instance body calls the element fn as `(g x)`, and that call must preserve the
env of a capturing closure.

## Minimal repro

```turmeric
;; bimap shape -- a FULLY-APPLIED ctor head, so NOT the partial-app blocker
;; (that one is resolved); this is the orthogonal capturing-closure gate.
(defclass MyBifunctor [^^p]
  (bimap2 [g : (fn [a] c) h : (fn [b] d) x : (p a b)] : (p c d)))
(definstance MyBifunctor [Result]
  (bimap2 [g h x]
    (if (.is-ok x) (ok (g (.ok-val x))) (err (h (.err-val x))))))

(defn main [] : int
  (let [delta 100
        r     (bimap2 (fn [n : int] : int (+ n delta))   ;; CAPTURES delta
                      (fn [n : int] : int n)
                      (:: (ok 21) (Result int int)))]
    (ok-val (:: r (Result int int)))))   ;; TARGET 121
```

```
$ ./build/tur build /tmp/repro.tur -o /tmp/repro && /tmp/repro
Segmentation fault          # the capturing (fn [n] (+ n delta)) called without its env
```

Replacing the capturing lambda with a top-level `defn` (no capture) returns 121
correctly -- confirming the gate is specifically capturing closures.

## Root cause (traced 2026-06-19)

The discrepancy is **instance-method vs regular-defn lowering of a TY_FN param**:

- A REGULAR defn `(defn apply-it [f : (fn [int] int) x : int] : int (f x))`
  lowers `f` to the **`tur_poly_fn_t` `{env, fn}` carrier** and calls it
  `((int64_t(*)(void*,int64_t))f.fn)(f.env, x)`. A capturing closure round-trips
  correctly (verified: returns 121). The call site builds the `tur_poly_fn_t`
  from the closure box.
- A TYPECLASS INSTANCE METHOD lowers the element fn param `g : (fn [a] b)` to the
  bare **`int64_t` carrier** and bare-calls it
  `((int64_t(*)(intptr_t)g)(x)` -- a thin (envless) function-pointer call -- in
  BOTH the carrier base method AND the by-value `__spec` clone. A capturing
  closure's env is dropped -> segfault. The non-capturing case works "by luck"
  because a non-capturing lambda / `defn` lowers to a bare function pointer the
  raw call invokes correctly.

So the regular path already has the right representation (`tur_poly_fn_t`); the
instance-method path does not. The difference is the `is_poly_fn` flag: the
regular path marks a TY_FN param `is_poly_fn` (emit_fns.c lowers it to
`tur_poly_fn_t`); the instance-method param handling only sets `is_poly_fn` for
the `:fn` keyword (`param_is_fn`) or `TY_FORALL`/`TY_EXISTS`, NOT a bare typed
`(fn [a] b)` annotation.

**Constraint: the carrier base method must stay int64.** The polymorphic HKT
dispatch dict (the M6/M7 carve-out) has `int64_t (*method)(int64_t, ...)` slots
and is referenced by indirect/constrained-poly dispatch. So the carrier base's
element-param ABI cannot change to `tur_poly_fn_t`. The fix must thread
`tur_poly_fn_t` through the **by-value `__spec` clone only**: (a) the spec's
typed-fn element param emits as `tur_poly_fn_t`, (b) the spec body's `(g x)`
dispatches via `f.fn(f.env, x)`, (c) the spec call site builds the
`tur_poly_fn_t` from the (thin or fat) argument -- the same shim the regular defn
call site already emits. That is the substantial part: it is emit-side surgery in
the by-value-spec path (param-type emit, body-call emit, call-site arg emit),
not a single elaborator flag.

## Proposed fix directions

1. **Mark the typed-fn element param as fat (`^fat`) in the by-value spec** so the
   body call dispatches through the fat protocol (`TUR_APPLY`), and auto-shim a
   bare function pointer into a fat box at the call site (the same plumbing the
   `^fat` bare-function arrow layer already uses). This is the natural home: the
   element param is exactly a `^fat` sink.
2. Alternatively, lower `(fn [a] b)` element params to the `tur_poly_fn_t`
   `{env, fn}` carrier and call `g.fn(g.env, x)`, but that changes the element ABI
   everywhere and is more invasive.

## Experimental finding (2026-06-19): the naive `is_poly_fn` marking regresses `bind`

Marking every typed `(fn [a] b)` instance-method element param `is_poly_fn` (so
it lowers to `tur_poly_fn_t`, like the regular defn path) -- plus adding the
missing closure-arg fallback to the `params[0]` rank-N path (Bifunctor `bimap`'s
first param is a mapper fn, not the HKT receiver, so it lands in `params[0]`,
whose poly path only accepted a NAMED function, not a lambda/closure) -- DID fix
the capturing case for **`fmap`/partial-app `pmap` (-> 121)** and **`bimap`
(-> 121)**. But it **regressed `bind` (probe -> 2, expected 21)**: bind's
continuation `k : (fn [a] (m b))` RETURNS an HKT-applied type, and switching it to
the `tur_poly_fn_t` carrier changes how that wrapped result is unpacked. So the
marking cannot be blanket -- the HKT-RETURNING continuation shape (`bind`, and
likely `traverse`) needs its result handling reconciled with the
`tur_poly_fn_t` packing, or the marking must be scoped to element params whose
result is a plain element (`fmap`/`bimap`/`ap`/`<|>`), not an `(m b)`.

This is why the fix is per-shape, not a one-line flag. The two pieces that DID
work and are reusable:
- `elab_typeclasses.c` param loop: `param_is_poly |= (param_type.kind == TY_FN)`.
- `elab_typeclasses.c` rank-N `params[0]` path: mirror the args-path closure
  fallback (wrap a `TY_PTR_VOID` / boxed-`TY_FN` arg as an `EX_POLY_WRAP`
  `is_closure` instead of erroring "must be a named function").

## How to validate a fix

- The repro above exits 121 (capturing closure preserved).
- The partial-app probe with a capturing closure
  (`pmap (:: (ok 21) (Result int int)) (fn [x] (+ x delta))`) exits 121.
- `bash tests/run.sh` stays green flag-on AND flag-off; the eight by-value shape
  probes stay green; existing HKT fixtures emit clean.

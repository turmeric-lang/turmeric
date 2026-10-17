# A call to a later-defined function with no return annotation is typed `int`

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`forward-call-unannotated-return`.

**Severity:** high (silent wrong output). Calling a function that is
defined **later** in the file, and whose return type is inferred rather than
written, treats the result as an `int`. A float is truncated; a string is
printed as its address.

## Repro

```turmeric
(defn main [] : int
  (println (half 7.1))
  (println (greet))
  0)

(defn half [x : float] (/ x 2.0))
(defn greet [] "hi")
```

- `tur run`: prints `3` and `94555787698344`.
- `tur --interpret`, or the same file with the two definitions moved above
  `main`: prints `3.55` and `hi`.

`python3 tests/check-emitted-float-conversions.py` on the file reports the
float-to-int conversion.

## Root cause

Pass 1 of the top-level driver pre-declares every `defn` so forward calls
resolve (`src/compiler/elab_toplevel.c:elab_pre_declare_toplevel_defn`). A
`defn` with no return annotation is pre-declared with return type `TY_INT`.
The forward-generation ordering (`fwd_gen_order_init`) defers callers of
callees whose signature is not yet known, but it does not count an
unannotated return as unknown. So `main` is elaborated against the
placeholder `int` before `half`'s body has been inferred.

## Fix directions

- Treat a `defn` with an unannotated return as "signature pending" in the
  forward-generation order, so its callers are elaborated after it; or
- re-check calls against the final signature once the callee is elaborated,
  and error on a mismatch instead of keeping the placeholder.
- Add the repro as a fixture (`3.55` / `hi`).

## Resolution (2026-10-17)

The forward-generation order (`fwd_gen_order_init`,
`src/compiler/elab_toplevel.c`) now counts an inferred return as a pending
signature. `fgo_defn_name` marks such a `defn` lossy through the new
`fgo_return_unannotated`, which reads past what `elab_defn` reads between the
params and the return type -- an `#fx{...}` row, the `#reads` / `#writes`
frames and a `^fat` marker -- and treats a `: T` annotation, or a keyword that
is not the last form, as annotated. The caller is then elaborated after the
callee's body, against its real type. An unannotated mutually recursive pair is
a cycle and is broken at its first member, as before.

Pinned by `forward-call-unannotated-return` (`3.55`, `hi`, and the recursive
pair), which runs on both engines.

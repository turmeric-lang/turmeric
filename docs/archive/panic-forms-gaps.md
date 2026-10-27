# The panic forms: an unfinished downcast, a typed catch that swallows, and wrong locations from `panic-with`

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`panic-payload-forms`, `panic-with-source-location` and
`catch-panic-of-mismatch-aborts` (both engines), `panic-payload-accessors`
(`stdlib/panic.tur`'s wrappers; the interpreter harness skips a fixture that
loads inline-C, so it was run by hand under `--interpret`), and
`errors/catch-unwind-non-thunk` and `errors/catch-unwind-thunk-with-args`.

**Severity: medium.** Found 2026-10-19 checking the examples for the panic
forms' API reference (`stdlib/forms/panics.tur`). The only fixture that
mentions a `panic-payload-*` form is `tests/fixtures/panic-downcast`, whose
body is `(defn main [] : int 0)`, so none of this is pinned.

## 1. `panic-payload-downcast` produces no value

`(panic-payload-downcast p :cstr)` is typed `ptr<void>`, but its emitter
evaluates the payload and returns `nil` (`src/compiler/emit_expr.c`,
`case EX_PANIC_PAYLOAD_DOWNS`, "unused in v1"), though the runtime function it
should call exists (`tur_panic_payload_downcast`, `src/runtime/runtime.c:398`).
Any use of the result is a C error:

```turmeric
(load "stdlib/panic.tur")
(defn main [] #fx{Unsafe IO} : int
  (let [rs (catch-unwind (fn [] : int (panic "boom")))]
    (println (ptr-null? (panic-payload-downcast (result-panic rs) :cstr)))
    0))
```

`tur run`: `invalid use of void expression` from the C compiler.

## 2. `catch-panic-of` swallows a panic of another type when nothing is above it

The guide (error-handling-guide, "Typed catches") says a panic whose payload is
not of the named type "is re-raised to the next outer boundary". Compiled,
that holds only if a boundary exists:

```turmeric
(defn main [] : int
  (let [r (catch-panic-of :int (fn [] : int (panic "s")))]   ; payload is a cstr
    (println (ok? r))                                        ; true -- the panic was swallowed
    (println "after")                                        ; reached
    0))
```

`tur run` prints `true`, `after` and exits 0. Wrapped in an outer
`catch-unwind`, the same call propagates (the outer result is `err`).
`tur --interpret` aborts (`panic: s`, exit 1) in both shapes, which is right.
Root cause not traced; look at what the compiled catch does with a type
mismatch when `tur_panic_*` has no enclosing frame.

## 3. Compiled `panic-with` reports the generated C location

`(panic-with 42)` uncaught prints `panic at /tmp/tur-build/k_tur-....c:2759`
rather than the `.tur` file and line; `panic-payload-line` on a caught one
returns a line in the generated C (2702 for a call on source line 3).
`panic` is right: it passes the source span (`emit_panic_call(body, e->span,
...)`); `panic-with` passes the C compiler's `__FILE__, __LINE__`
(`emit_expr.c`, `case EX_PANIC_WITH`: `tur_panic_with(%d, (void*)%s, __FILE__,
__LINE__)`). Interpreted, the location is the source's.

## 4. `catch-unwind` does not check that its argument is a function

`(catch-unwind 5)` compiles and segfaults at run time. Every other call form
rejects a non-callable.

## 5. The interpreter's payload accessors are placeholders

Under `--interpret`, `panic-payload-type` is 0, `panic-payload-file` is `""`
and `panic-payload-line` is 0 for a caught `(panic "boom")`; compiled they are
5, the program's file name and the line. `stdlib/panic.tur`'s wrapper cannot
be called at all interpreted (it is an inline-C `defn` taking a `Panic`).

## Fix directions

1. Emit `tur_panic_payload_downcast((tur_panic_payload *)p, TYPE)` for
   `EX_PANIC_PAYLOAD_DOWNS`, or retire the form in favour of
   `panic-payload-type`.
2. Make the compiled typed catch re-raise a mismatched panic whether or not a
   boundary exists (abort when none does).
3. Pass the source span to `tur_panic_with`, as `panic` does.
4. Type-check `catch-unwind`'s and `catch-panic-of`'s thunk as a zero-argument
   function.
5. Give the interpreter real payload accessors, and a fixture for each of the
   five on both engines.

## Resolution (2026-10-21)

1. **`panic-payload-downcast` has a value.** The emitter
   (`EX_PANIC_PAYLOAD_DOWNS`, `src/compiler/emit_expr.c`) calls the preamble's
   `tur_panic_payload_downcast`; it evaluated the payload and returned nil.
   The elaborator also read only a bare-symbol type, so the guide's
   `:cstr` left the target `TY_UNKNOWN` and could never match -- a keyword is
   read now, and an unknown type is an error.
2. **A mismatched `catch-panic-of` with no boundary above is an uncaught
   panic.** The compiled boundary staged the signal and returned, and with
   nothing to propagate to, `main`'s return swallowed it (exit 0, the next
   statement ran). `tur_catch_panic_of_box` / `_box_via`
   (`src/compiler/emit_module.c`, and the regenerated split runtime) raise it
   afresh once their node is popped and the handler chain is empty:
   `tur_panic_at` for a message panic, so `panic at input.tur:6: s` still
   names the message, `tur_panic_with` otherwise.
3. **`panic-with` reports the source position.** `emit_panic_with_call`
   passes the `.tur` file's base name and the form's line, as `emit_panic_call`
   does for `panic` (and in the CPS body emitter, `emit_fns.c`),
   instead of the C compiler's `__FILE__` / `__LINE__`.
4. **A catch boundary checks its thunk.** `catch-unwind` and `catch-panic-of`
   (`src/compiler/elab_concurrent.c`) refuse a literal, a float / bool /
   cstr / nil / struct value, and a function that takes arguments. An `int`
   stays accepted -- it is how a closure travels through an unannotated
   parameter, as `resume` accepts it.
5. **The interpreter's payload accessors are real.** The placeholders were
   not the `panic-payload-*` forms but `stdlib/panic.tur`'s wrappers, which
   are inline-C: `result-panic` answered nil, so every accessor read a null
   handle. `result-panic`, `panic-message`, `panic-file`, `panic-line` and
   `panic-type` are natives over the interpreter's `TuriPanicPayload`
   (registered with rows in `native_caps.c` for the capability and
   handle-provenance tables), the payload records the site's file and line
   (it was NULL / 0), a plain panic's message is copied out of the one
   buffer every later panic overwrites, and the mismatch abort names the
   location as the compiled one does.

The preamble changed, so the split runtime artifacts and all 157 codegen
snapshots are regenerated in the same commit.


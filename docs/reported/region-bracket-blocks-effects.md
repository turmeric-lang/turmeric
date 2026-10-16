# A `perform` inside `with-region` / `bt-scope` cannot reach an outer handler

**Severity:** medium-high. A correct program aborts at run time on the default
build. The same program prints the right answer with regions off and under
the interpreter.

## Repro

```turmeric
(defeffect Bail [code : int] : int)

(defn main [] : int
  (println (handle (with-region (fn [] : int (+ 5 (perform (Bail 9)))))
             (Bail [c] k) c))
  0)
```

- `tur run`: `tur: unhandled effect (tag 2)`, `Aborted`.
- `tur --interpret`: `9`.
- `TUR_REGIONS=0 tur run`: `9`. So does a user-written forwarder of the same
  shape as `with-region`.

A handler **inside** the thunk works, and the bracket still rewinds.

## Root cause

`src/compiler/emit_cps_ir.c:emit_term_inner` forces a region-scope callee
(`with-region`, `bt-scope`) onto the cps->direct arm (`force_direct`, with
`tailcall_routes_cps_to_cps` agreeing) so it can wrap the call in
`tur_region_push` / `pop_checked`. A direct call into the bracket's thunk
enters it through the thunk's plain entry wrapper. That wrapper seeds a fresh
DK root, so the `perform` inside the thunk never sees the handler installed
outside the bracket. With regions off the bracket is an ordinary call and
takes the threaded cps->cps route.

## Fix directions

- Route a region-scope call cps->cps. Push the region before the call, and
  move the `pop_checked` and escape note into the continuation frame that
  receives the thunk's result (and into the panic and abandon paths).
- Until then, diagnose it at compile time: a region bracket whose thunk can
  perform an effect not handled inside it should be an error, not an abort.
- Note for the fix: DK continuation frames carry no region note today. Once
  continuations can cross a bracket, a stored continuation becomes a new way
  for a node to escape it. See
  [regions-internals-guide](../guides/regions-internals-guide.md).

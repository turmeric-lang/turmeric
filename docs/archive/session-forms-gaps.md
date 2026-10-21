# Session types: a `send` is not checked against the protocol's payload type, and destructuring arity is not checked

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`errors/session-send-payload-type-mismatch`, `errors/session-destructure-arity`
and `errors/session-mp-destructure-arity` (both engines).

**Severity: medium.** Found 2026-10-19 checking the examples for the session forms'
API reference (`stdlib/forms/sessions.tur`). Each repro was run on `tur run` and
`tur --interpret`.

## 1. The payload of `send` is not compared with the protocol

```turmeric
(load "stdlib/session.tur")
(defn main [] : int
  (let [[s r] (make-session (Send int Close))]
    (let [t (session-spawn (fn [] (let [[n r] (recv r)] (println n) (close r))))]
      (let [s (send s 7.25)]
        (close s)
        (session-join t))))
  0)
```

`tur check` is silent. `tur run` prints `7`, `tur --interpret` `7.25`: a float sent on
a `(Send int Close)` channel is truncated by the compiled runtime and passes through
the interpreter's. session-types-guide lists "payload type mismatch" under `TUR-E0212`, and says a
message crosses as one word with floats "bit-preserved" when the protocol says
`float`; nothing compares the argument's type with the step's. A float on a
`(Send float Close)` channel arrives exactly (7.25 on both). Fix: unify the
argument with the step's payload type in `send` / `send-to`.

## 2. The number of endpoints destructured is not checked

```turmeric
(defn main [] : int
  (let [[a b c] (make-session (Send int Close))] 0))      ; c is ignored
(defprotocol P [A B C] (-> A B int) (-> B C int))
(defn main2 [] : int
  (let [[a b] (make-protocol P)] 0))                      ; the C endpoint is dropped
```

Neither is an arity error. The only diagnostics are `TUR-E0100` for the endpoints
that were bound and not consumed, so `[a b]` against a three-role protocol hides the
missing role behind a message about the two that were named. Fix: compare the
pattern's length with the number of endpoints.

## 3. Stale text in the guide

The first note under "make-session, send, recv, close" ("Payload types: use `int` or
`bool` for now ... a `float` payload is silently truncated; a `cstr` ... fails to build
on macOS") contradicts "Payload types" below it, which says floats, strings, pointer
handles and delegated endpoints all travel intact on both backends. The latter is
what the code does (7.25 and a `cstr` arrive exactly when the protocol names them);
the first note quotes the archived `session-payloads-are-int64-only`.

## Resolution (2026-10-21)

All three items, on both engines (the elaborator is shared, so one check covers
`tur run` and `tur --interpret`).

1. **`send` checks its payload** against the protocol's message type
   (`elab_session_send`, `src/compiler/elab_sessions.c`). The rule is the one
   `perform` already applied to its arguments, factored out of
   `elab_perform` as `elab_word_slot_mismatch` (`src/compiler/elab_effects.c`):
   exact primitives with the `int`/`int64` and `float`/`float64` aliases, no
   scalar into a pointer-shaped slot, nominal identity for aggregates, and
   silence wherever a type variable or `any` leaves the answer to inference.
   `(send s 7.25)` on `(Send int Close)` is now `TUR-E0212: send: the
   protocol's next message is int, but this value has type float`. `send-to`
   already compared kinds and is unchanged.
2. **The pattern's length is checked** where `elab_let` splits a session pair
   (`src/compiler/elab_forms.c`): two names for `make-session` and `recv`, one
   per role for `make-protocol`. Any other count is `TUR-E0002` naming the
   form and both numbers, and the misleading `TUR-E0100` is gone
   (`unexpected.diag` pins its absence).
3. **The stale note** under "make-session, send, recv, close" in
   session-types-guide is replaced by the two rules above and a pointer to
   "Payload types". The `make-session` / `make-protocol` reference pages
   (`stdlib/forms/sessions.tur`) say the same.


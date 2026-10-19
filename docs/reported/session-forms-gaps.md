# Session types: a `send` is not checked against the protocol's payload type, and destructuring arity is not checked

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

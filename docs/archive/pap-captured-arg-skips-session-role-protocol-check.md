# Under-saturating a call bypasses the session/role protocol check

**RESOLVED 2026-09-26.** Fixed as directed: the partial-application gate in
`src/compiler/elab_call.c` (the capture loop of the PAP builder) now admits
`TY_SESSION` and `TY_ROLE` alongside `TY_STRUCT`/`TY_ADT`, in both the
recorded-full-type test and the kind fallback, and the flag is renamed
`slot_needs_full_type_check` (a protocol is structural, not nominal).
`type_eq` does the comparing, so no new comparison logic was needed. The
line numbers below are from the filing; the block has since moved to about
`elab_call.c:5100`.

Pinned by three fixtures:

- `tests/fixtures/errors/session-protocol-mismatch-at-pap` -- the repro
  below; `tur check` exited 0 before, now `TUR-E0001 ... expected
  Session[Close], got Session[Send[int, Close]]` on both engines.
- `tests/fixtures/errors/role-protocol-mismatch-at-pap` -- the `TY_ROLE`
  shape: a role-B endpoint captured into a role-A slot. Before the fix only
  the program's saturated mismatch (`role-b`) was reported; the fixture
  asserts the partial application's own `function 'role-a' arg 1`.
- `tests/fixtures/session-pap-protocol-match` -- the positive control the
  report asked for: an endpoint captured partway through a `Rec` protocol
  (after one hand-driven round) still matches the parameter's folded form,
  and the program runs to `10 20 30` compiled and under `--interpret`.

**Summary:** The saturated positional argument check compares a session or role
endpoint's full protocol (`elab_call.c:6487`, added by
`session-type-eq-ignores-the-protocol`), but the **partial-application** path
(`elab_call.c:4862`) still decides strictness from a STRUCT/ADT-only test. A
captured `TY_SESSION` / `TY_ROLE` argument is therefore never compared against
the slot it fills, so **under-saturating a call is a way to bypass the protocol
check that a saturated call performs**.

**Severity:** Medium. Not a new hole -- it is the residue of one that was
half-closed -- but the asymmetry makes it worse than a uniform gap: the
mismatch a saturated call now rejects is silently accepted the moment the same
call is written with one argument missing.

## Minimal repro

```turmeric
(load "stdlib/session.tur")

(defn f [^linear ch : (Session Close) n : int] : nil
  (close ch))

(defn main [] : int
  (let [[a b] (make-session (Send int Close))]
    ;; `a` is Session[Send int Close], NOT Session[Close].
    (let [g (f a)]      ; captured into a partial application -- ACCEPTED
      (g 1))
    (let [[n b1] (recv b)]
      (close b1)
      n)))
```

```sh
$ tur check repro.tur
$ echo $?
0
```

Two controls establish that this is the partial-application path specifically,
not a gap in the protocol comparison itself:

- **Saturated, same mismatch** -- replace the `let`/`g` with `(f a 1)`:

  ```
  error [TUR-E0001]: function 'f' arg 1: expected Session[Close],
                     got Session[Send[int, Close]]
  ```

- **Partial application, matching protocol** -- same program with
  `(make-session Close)` so `a` really is `Session[Close]`: compiles. So
  partially applying a function with a session parameter is legal and
  reachable; it is only the *check* that is missing.

`TY_ROLE` shares the path and the same STRUCT/ADT-only test, so a captured
multi-party endpoint is unchecked for the same reason.

## Root cause

`src/compiler/elab_call.c:4862`

```c
Type *cap_full_chk = PAP_SLOT_FULL(i);
bool slot_is_nominal =
    (cap_full_chk &&
     (cap_full_chk->kind == TY_STRUCT || cap_full_chk->kind == TY_ADT)) ||
    cap_kind == TY_STRUCT || cap_kind == TY_ADT;
if (slot_is_nominal) {
    ... type_eq(elab_args[i]->type, *cap_full_chk) ...
```

`slot_is_nominal` is the whole gate. A `TY_SESSION`/`TY_ROLE` slot fails it, so
the `type_eq` call below -- which *can* now answer the protocol question, since
`type_eq` compares session protocols equirecursively and role protocol/role
names -- is never reached.

This mirrors exactly the gate at `elab_call.c:6487` on the saturated path,
which was widened to include `TY_SESSION`/`TY_ROLE`. The comment at 4846 says
this block is meant to "mirror the saturated positional check", and it no
longer does.

## Fix directions

Add `TY_SESSION` and `TY_ROLE` to `slot_is_nominal` (both the
`cap_full_chk->kind` test and the `cap_kind` test), the same two-place change
the saturated path took. `type_eq` already does the comparing, so no new
comparison logic is needed.

The name `slot_is_nominal` becomes wrong once sessions are in it -- a session
protocol is structural, not nominal. Worth renaming to something like
`slot_needs_full_type_check`, in both places, so the next person widening it is
not misled about what the set means.

Watch for false negatives from the equirecursive case the way the saturated
path had to: a captured endpoint partway through a `Rec` protocol must still
match a parameter declaring the folded form. `type_eq` handles that, but the
fixture is worth having.

`tests/fixtures/errors/` wants the repro above, plus the `TY_ROLE` shape.
Note the existing `tests/fixtures/errors/session-protocol-mismatch-at-call`
and `session-endpoints-swapped` cover only the saturated path.

## How it was found

Fixing `session-type-eq-ignores-the-protocol`
([archived](../archive/session-type-eq-ignores-the-protocol.md)). The
partial-application gate was noticed while widening the saturated one and left
alone rather than changed without a repro; this report is that repro. It was
originally recorded only as a note inside the archived report, which is not a
place open work gets looked for.

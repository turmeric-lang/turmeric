> **Resolved 2026-09-16** (turi-session-expansion-plan, S3.5). The router
> templates go through the same payload lowering as the binary seam:
> `send-to` (`src/compiler/elab_global.c`) wraps its operand with
> `session_payload_to_word` and the `recv-from` destructuring
> (`src/compiler/elab_forms.c`) wraps `tur_router_recv` with
> `session_payload_from_word`, so a float is bit-reinterpreted, a pointer cast
> through `intptr_t`, and a by-value struct rejected with `TUR-E0212`. Pinned by
> `tests/fixtures/session-payload-mp-float`.

# Multi-party session `send-to` carries every message as a bare `int64_t`: `float` silently truncates

**Severity: high.** The multi-party (protocol router) send path has the same
int64-only payload seam as binary sessions, through a **different template** --
so a fix to `elab_sessions.c` does not fix this one. `(-> A B float)` is accepted
by the type checker, is correct under `tur --interpret`, and on the compiled path
delivers `7` for `7.25` with no diagnostic and exit 0.

| Payload | Compiled result |
| --- | --- |
| `int`, `bool` | correct |
| **`float`** | **silently truncated** -- `7.25` arrives as `7`, exit 0, no diagnostic |
| `cstr` | cc error (`-Wint-conversion`, hard error on Apple clang 21) |
| by-value `defstruct` | cc error on every platform |

This is the sibling of
[session-payloads-are-int64-only](session-payloads-are-int64-only.md), which
documents the **binary** `send`/`recv` seam and names `elab_sessions.c:306`. That
report mentions `(Role G R)` only as a payload *being delegated over* a binary
session; it does not cover the router's own send. Two call-site templates, two
runtime functions, one defect shape.

## Repro

Measured against `./build/tur` at v0.48.0 / `main` 1bed41ab1, 2026-09-16, Apple
clang 21.0.0 (arm64-apple-darwin27). The pthread peer is the fixture-standard
one (`tests/fixtures/session-mp-three-role/input.tur`), elided here.

```turmeric
(defprotocol Pipeline [A B C]
  (-> A B float)
  (-> B C float))

(defn role-a [^linear ch : (Role Pipeline A)] : nil
  (let [ch (send-to ch B 7.25)] (close ch)))
(defn role-b [^linear ch : (Role Pipeline B)] : nil
  (let [[v ch] (recv-from ch A)]
    (let [ch (send-to ch C v)] (close ch))))
(defn role-c [^linear ch : (Role Pipeline C)] : nil
  (let [[v ch] (recv-from ch B)] (println v) (close ch)))
```

```
$ ./build/tur build mp-float.tur -o mp-float.bin   # exits 0, no diagnostic
$ ./mp-float.bin
7                       <-- WRONG; 7.25 was sent
```

Per the float rule in [CLAUDE.md](../../CLAUDE.md) the probe literal has a
non-zero fractional part; `7.0` would have hidden this entirely, which is why
`tests/fixtures/session-mp-three-role/` -- which sends `42` -- passes.

## Root cause

One call-site template, `src/compiler/elab_global.c:611`:

```c
"({ tur_router_send(__TUR_VAL_0__, %d, (int64_t)(__TUR_VAL_1__)); (void *)__TUR_VAL_0__; })"
```

against one runtime signature, emitted at `src/compiler/emit_module.c:14994`:

```c
static void tur_router_send(void *role_ptr, int to_idx, int64_t val)
```

`(int64_t)(...)` on a `double` operand is a **value conversion that truncates**,
not a reinterpretation. The receiving binding is declared at the protocol's
payload type, so the emitted C reads a `double` local out of an int64 slot. For a
pointer the cast is bit-preserving but ill-typed in C; for a by-value struct no
conversion exists at all.

The interpreter is unaffected because `TuriValue` is a tagged union carrying the
payload at its own type -- the same inverted parity the session audit found
throughout.

## Fix directions

Identical to the binary-session report, and worth doing in one change with it
since the two templates are three lines apart in spirit:

1. **Bit-reinterpret rather than value-convert** for `float` (a `union { double
   d; int64_t i; }` pun on both sides). This is the silent-wrong-answer row and
   the one worth closing first. The direct/fiber effect path already does exactly
   this -- see
   [docs/archive/fiber-effect-float-result-truncated.md](../archive/fiber-effect-float-result-truncated.md)
   -- so the convention exists and can be reused rather than invented.
2. **Cast pointers explicitly** (`(int64_t)(intptr_t)` in, `(T)(intptr_t)` out)
   so `cstr` and delegated endpoints stop depending on host-C strictness.
3. **Structs by value** need a decision: box and send the pointer, or reject.
4. **Whatever is not supported must be rejected in the elaborator.** Note
   `elab_global.c` already raises `TUR_E0212_SESSION_PROTO_MISMATCH` a few lines
   above the offending template (`:601`), so the diagnostic path for "this
   protocol cannot carry that" is already in place and wants one more check.

Direction 4 is the floor: a user should learn from `tur build` that a `float`
payload is unsupported, not from a wrong number at run time.

## Tests

`tests/type-fuzz-src.py` generates this shape as the `router` seam and pins the
float row in `KNOWN_PROBES`; `--seam router` exercises it directly and
`--seam-matrix` prints the payload row. Until this is fixed the shape classifies
`KNOWN(router-payloads-are-int64-only)` and does not fail a run -- when it starts
passing, `--known-probes` reports it FIXED and its `known_bug_slug` row should be
retired.

A fixture wants `tests/fixtures/session-mp-payload-float/` (send `7.25`, expect
`7.25`), which fails today.

## See also

- [session-payloads-are-int64-only](session-payloads-are-int64-only.md) -- the binary seam.
- [generator-yield-payload-is-int64-only](generator-yield-payload-is-int64-only.md),
  [async-await-payload-is-int64-only](async-await-payload-is-int64-only.md) -- the
  same shape at two more runtime seams, found by the same fuzzer axis.
- `src/compiler/elab_global.c:611`, `src/compiler/emit_module.c:14994`.

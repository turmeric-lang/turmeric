# Sandboxed interpreter: handles are forgeable integers

**Severity:** high under T3 (the sandboxed interpreter) and under T1 for
`tur check` (the macro environment). Tracked as **S-5** in
[security-audit-plan](../upcoming/security-audit-plan.md). Filed 2026-09-30 by
WP3, which closed the capability half of the sandbox (S-1) and found this
underneath it.

**Narrowed 2026-09-30.** The report was filed with two halves. The second, a
restricted env ending the host process, is fixed (see *Resolved: host exit*
below). What is open is the first: forged handles.

A capability-denied environment (`turi_env_new_sandboxed()`, the macro
environment) cannot reach the OS through a native: every native carries a
required capability and the native dispatch checks it. What it can still do is
read or write an arbitrary address in the host's process, with no capability
at all.

**Narrowed a fourth time 2026-10-03: the wild JUMP is closed** -- an
integer re-typed as a function is refused at the call (see *Resolved: a forged
call target*).  What is open is the wild READ: the same erasing ascription
re-typing an integer as a by-value struct or a `cstr`, and a field read
through a bare-int receiver.

**Narrowed a third time 2026-10-03: continuation resume is closed** (see
*Resolved: continuation resume*).  What is open is the erasing ascription on a
type variable.

**Narrowed again 2026-09-30 (direction 1 landed).** The native channel -- the
whole of the Repro below -- is now closed by a per-restricted-env handle
provenance registry (see *Resolved: native handle forgery* below). What remains
open is the narrower *value-model* channel the "Fix directions" section already
attributes to direction 2 (tagged handles): an **erasing ascription** on a type
variable, and **continuation resume**, still launder a caller integer into a
pointer WITHOUT passing through the native dispatch, so they are not caught by
the registry. See *Still open* below.

## Repro

Against a Debug build, from an embedder:

```c
TuriEnv *env = turi_env_new_sandboxed();
turi_eval(env, "(vec-get 4096 0)");
```

```
AddressSanitizer: SEGV on unknown address 0x000000001008 ... READ memory access
```

The same shape crashes through `(tur_hamt_count 4096)` and `(vec-len 4096)`.
`vec-set!`, `mutmap-set!` and the HAMT setters make it a write. From
`tur check`, the same call inside a `defmacro*` body runs in the compiler's own
process.

## Root cause

The interpreter carries every collection, string-builder, continuation and
cons handle as a `TURI_INT` holding a pointer, and the natives cast it back
with no check that it came from the matching constructor, e.g.
`native_vec_get` (`src/turi/collections_native.c`, the `(int64_t *)(intptr_t)
a[0].as_int` at the top of the function). `head`, `int-val`, `unbox`,
`cstr-free` and the `tur_*_cont_*` builtins (`ts_try_cont_builtin` in
`src/turi/eval.c`) share the shape.

**Measured scope (2026-09-30):** of the 656 builtin natives, **204** cast an
integer argument straight to a pointer in their own body. That count is a
floor: it misses natives that hand the integer to a helper (`sbuf_of`,
`json_node_ptr`, the `tur_hamt_*` runtime functions in `src/runtime/`) before
the cast.

The natives resolve at runtime by name (TUR-W0040, "will runtime-dispatch"),
so the elaborator's types never stand between the text and the cast. Types
would not be a sound fix anyway: an erasing ascription or a stdlib `:int`
stand-in launders an integer into a handle type.

## Why it was not fixed with the capability check

It is not a capability. Gating every handle-taking native behind
`TURI_CAP_UNSAFE` would deny vectors, maps and strings to a sandbox, which
makes the sandbox useless rather than safe. WP3 gated only the natives whose
sole purpose is raw memory (`box`, `unbox`, `io-alloc`, `int-val`, `flat-*`,
`array-get`/`array-set`, ...; see `src/turi/native_caps.c`).

## Fix directions

Both directions need a second column in `src/turi/native_caps.c`: for each
native, which argument positions are handles and of what kind, and whether its
result is one. That is the bulk of the work, 200+ rows read by hand, and the
sandbox test can pin it the same way it pins the capability column.

1. **A provenance set per restricted env** (the cheaper route). When
   `env->caps != TURI_CAP_ALL`, the dispatch records each handle a
   constructor-classified native returns, keyed by kind, and refuses a call
   whose handle argument is not in the set for its kind. It costs a hash
   lookup per handle argument, only in restricted envs. Frees remove entries.
   Handles made by the preload before the caps were dropped are recorded by
   walking the globals once when the caps change. The trap to design around:
   a count or index that happens to equal a live handle's address is not a
   forgery, and the kind key is what keeps `(vec-get (vec-len v) 0)` from
   passing.
2. **Tagged handles** (the complete route). A `TURI_HANDLE` value tag carrying
   a kind and the pointer, returned by every constructor and checked by every
   consumer. It closes the class for unrestricted envs too, and it touches
   every one of the 204+ natives and their Turmeric-side callers that treat
   the handle as an `:int`.

Direction 1 is enough to make the T3 promise and the deferred T1 macro promise
in `docs/guides/security-guide.md`.

## Resolved: native handle forgery -- direction 1 (2026-09-30)

Direction 1 landed. The whole of the Repro above is now refused instead of
crashing:

```c
TuriEnv *env = turi_env_new_sandboxed();
turi_eval(env, "(vec-get 4096 0)");
/* => TURI_ERROR "eval: 'vec-get' arg 1 is not a live handle of the
      expected kind -- a sandboxed handle cannot be forged from an
      integer (S-5)" */
```

What was built:

- **A handle-signature column** in `src/turi/native_caps.c`
  (`k_handle_rows[]`, 242 rows, binary-searched by
  `turi_native_handle_find`). Each row names, per leading argument position,
  the `TuriHandleKind` the native dereferences there (0 = an ordinary word: a
  scalar, a count, a stored key/value read back as a carrier), plus the result
  kind and mint/free flags. The kinds cover every distinct interpreter handle
  representation -- `VEC`, `SETMAP`, `HAMT`, `HAMT_ITER`/`_TRANS`, `STRING`,
  `SBUF`, `SLICE`/`SLICEBOX`, `CONS`, `SEQCELL`, `SYM`, `JSON`, `GENARR`,
  `GRID`, `MUTMAP`, `BTCELL`, `FUTURE`, `CHAN`, `MUTEX`, `BYTES`, `REACTOR`,
  the comparator fn-ptr `CMP`, and a `GENERIC` catch-all.
- **A per-restricted-env provenance registry** (`TuriProvSet`, an
  open-addressing `{kind, ptr}` set on `TuriEnv`, in `src/turi/eval.c`). It is
  enabled for a sandbox at `turi_env_new_sandboxed`, and for the macro env when
  `turi_env_deny` drops it below `TURI_CAP_ALL` (seeding the pre-restriction
  globals as `GENERIC`, so a handle the preload minted is not mistaken for a
  forgery). Unrestricted embedders never enable it and pay nothing.
- **The one dispatch hook** in `eval_apply_driven`: before a provenance-tracked
  native runs, `turi_prov_guard_native` refuses any handle argument that is not
  a live entry of the declared kind; after it runs, `turi_prov_track_native`
  registers a minted result and forgets a freed handle. A `TURI_CSTR` argument
  is a real reader pointer and is trusted; only a `TURI_INT` carrier is checked.

This closes, in a restricted env:

- the arbitrary-integer wild read/write (`(vec-get 4096 0)`, `(vec-len 4096)`,
  `(tur_hamt_count 4096)`, the `-set!`/`-push!` writers, `(sym->str 4096)`,
  `(tur_string_len 4096)`, `(head 4096)`, ...);
- **kind confusion** -- a real Vec replayed where a HAMT is expected, or a
  count (`(vec-len v)`, not a handle) replayed as one;
- **use-after-free** -- a freed handle's pointer is forgotten, so a later use
  is refused.

A genuinely-minted handle still round-trips: a sandbox builds and reads vectors,
maps, HAMTs and strings exactly as before. Pinned by the `forgery/*`,
`handles-ok/*` and `handle-table` cases in `tests/turi/sandbox-eval.c`.

## Resolved: continuation resume (2026-10-03)

The second value-model forgery below is closed without direction 2, because a
continuation handle has exactly one producer: every `TuriCont *` the
interpreter hands out as an integer comes from a capture (the shift receiver's
argument) or a copy (`ts_cont_copy`: clone, snapshot, serialize,
`save-cont!`).  Both now register it as `TURI_HK_CONT`, and every consumer
checks that kind before casting (`cont_handle_forged`, `src/turi/eval.c`):
the `tur_*_cont_resume` / clone / serialize builtins, the `resume-cont!` and
`save-cont!` natives, and the three work-stack folds that start a resume
without going through either.  `0` stays the documented no-op.  Pinned by the
`forgery/resume-cont`, `forgery/save-cont`, `forgery/cloneable-cont-resume`,
`forgery/serial-cont-resume`, `forgery/cloneable-cont-clone` and
`handles-ok/continuation` cases in `tests/turi/sandbox-eval.c`.

## Resolved: a forged call target (2026-10-03)

`(defn mk [A] [x : int] : A (:: x A))` then
`(let [f : (fn [int] int) (mk 4096)] (f 1))` jumped to 4096: the ascription is
transparent on a bare int, and the call head re-tagged it as a closure
(`recover_carrier_closure`, because the head's binding is fn-typed).  Three
natives re-tag the same way (`seq_as_closure`, `free_call_fat`).

A closure only loses its tag by entering a native -- stored in a Vec, a map, a
cell, as the word its union holds; the interpreter's own ascriptions keep the
tag.  So in a provenance-tracked env every closure argument of a native call
is registered as `TURI_HK_CLOSURE` (`turi_prov_note_args`, at the native
dispatch and at the inline-C override), and every int-to-closure re-tag goes
through `turi_closure_from_carrier`, which refuses a word that is not
registered -- "not a live handle of the expected kind".  Outside a sandbox
both are no-ops.  `turi_call` hands such a refusal back unchanged.

Pinned by `forgery/closure-retag` and its control `handles-ok/closure-carrier`
(a closure pushed into a Vec and called back through a `^fat` parameter) in
`tests/turi/sandbox-eval.c`.

## Still open: the value-model channel -- direction 2

Direction 1 guards *the native dispatch*. The forgery below reaches a pointer
WITHOUT going through it, so the registry does not see it; it is the "erasing
ascription launders an integer into a handle type" case the *Root cause*
section already flagged, and it is what direction 2 (tagged handles) closes.
(The second bullet, continuation resume, is closed -- see above.)

- **An erasing ascription on a type variable.** A generic body that ascribes a
  caller integer to its type parameter re-tags it in the interpreter's own value
  model, then the tree-walker dereferences the result as a struct or a string:

  ```
  (defn mk [A] [x : int] : A (:: x A))
  (.x (:: (mk 4096) Point))      ; a wild read
  ```

  The sites are `try_retag_carrier_struct` (which dereferences the word to
  validate it), the `EX_ASCRIBE` / `EX_REINTERPRET` cstr arms, and
  `get_field_extract`'s bare-int receiver path, which reads the word as a raw
  field buffer -- all in `src/turi/eval.c`, none a native.  The call-target
  re-tag is closed (above); these are not, because a struct carrier and a
  string are MINTED in too many places (natives build both and hand them back
  as words) for the closure fix's "register where it loses its tag" to cover.

- **Continuation resume.** `(resume-cont! 4096 0)` and the lowered
  `tur_*_cont_resume` builtins are folded by the CEK driver
  (`cont_fold_begin` / `ts_cont_resume`), not by the native dispatch, and cast
  the caller integer to a `TuriCont *`.

These need direction 2's tagged handle (a `TURI_HANDLE` value tag carrying kind
+ pointer, minted by every constructor and checked at every reinterpret,
including the value-model retag and the continuation fold), because a bare
`:int` in the value model carries no kind for the registry to check against.
Until then the sandbox is a boundary against the native handle-forgery channel
but **not yet a full boundary against hostile code** -- see the T3 status block
in `docs/guides/security-guide.md`.

## Resolved: host exit (2026-09-30)

The report was filed with a second half: `panic`, and the error paths of
several natives (`vec-get` out of bounds, `slice-get`, `sized-buf-*`,
`json/get!`, a failed `tur-contract-check`), called `exit`, `_exit` or `abort`,
so sandboxed text could end the embedding host. A panicking `defmacro*` ended
`tur check` itself.

Now `turi_eval` and `turi_call` on an env without `TURI_CAP_PROC` install a
landing pad (`host_guarded_run` in `src/turi/eval.c`). Every path that would
have ended the process jumps there instead: an uncaught panic, a panic under
`no-unwind`, a double panic, and each native error path, through
`turi_host_exit_guard`. The call then returns `TURI_ERROR "panic: <msg>"` and
the env stays usable. User `catch-unwind` is unaffected, because the pad is
reached only where the process would have ended. Unrestricted envs print and
exit exactly as before.

Pinned by the `host-exit/*` cases in `tests/turi/sandbox-eval.c` and by
`tests/fixtures/errors/macro-panic-is-a-diagnostic`.

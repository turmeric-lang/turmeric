# Sandboxed interpreter: handles are forgeable integers

**Severity:** high under T3 (the sandboxed interpreter) and under T1 for
`tur check` (the macro environment). Tracked as **S-5** in
[security-audit-plan](../upcoming/security-audit-plan.md). Filed 2026-09-30 by
WP3, which closed the capability half of the sandbox (S-1) and found this
underneath it.

**Resolved 2026-10-07.** The value-model channel this report left open is
closed (see *Resolved: the value-model channel*). Executing it turned up a
native channel much wider than the direction-1 table covered -- a measured 243
capability-free natives crashed on a forged argument -- and that is closed and
pinned too (*Resolved: the native channel, measured*).  What the fix does not
cover is listed at the end.  The sections in between are the history, kept as
filed.

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

## Resolved: the value-model channel (2026-10-07)

The interpreter re-tags a bare word as a typed value in its own value model,
outside any native: `(:: w cstr)` (which needs no type variable -- `(:: 4096
cstr)` elaborates), a by-value struct ascription (`try_retag_carrier_struct`
read the word to validate it), a field read through a bare-int receiver
(`get_field_extract`), `gen-unwrap`, an STM `TVar`, and the `panic-payload-*`
forms (which read a whole `TuriValue`, tag and pointer, out of the word).  Each
of those now checks the provenance registry in a restricted env and refuses a
word nothing recorded under the right kind -- "<thing> is not a live handle of
the expected kind ... (S-5)".

What makes that possible without tagged handles: a value only loses its tag at
a few places, and every one of them records it.

- **Entering a native.** `turi_prov_note_args` (now `turi_prov_note_value` per
  argument) registers every closure, string, struct and generator argument
  under its kind -- the closure fix of 2026-10-03, generalised.
- **The interpreter's own stores.** Rest-list cells (`EX_CONS_LIST`: the cell
  as `CONS`, its head by tag), TVar payloads (new / write / swap / cas /
  modify), set-literal elements, symbol literals (`SYM`), catch-unwind's Result
  box and panic payload (`RESULTBOX`, `PANIC`), the generator box `gen-next`
  hands out (`GENBOX`).
- **Natives that hand back a string or a box as a bare word** mint it by row
  (`alloc-str`, the Show instances' `String`s, schema decode outputs, ...).

So a re-tag succeeds exactly for a word that once was a value of that kind.  A
field read through a bare int is admitted for a struct that lost its tag (read
as that struct) or a live native box at least `idx + 1` words long
(`k_prov_box_words[]`); a field the box's own native keeps (a MutableMap's
`.storage`) is registered as it is read out.  Pinned by the `forgery/*` and
`handles-ok/*` value-model cases in `tests/turi/sandbox-eval.c`.

## Resolved: the native channel, measured (2026-10-07)

Direction 1's table described the natives someone had written a row for.  A
sweep that calls **every** capability-free native in a sandbox with a forged
argument (an integer, a string literal, a float) at each of the first four
positions, one forked child per native, found 243 that crashed.  The causes,
and what closed each:

- **The guard trusted any tagged string at a handle position.** A string
  literal at a Vec position is a Vec header made of the caller's bytes; a
  float, a bool, a closure or a struct there is the same cast of other bits.
  `prov_arg_ok` now admits a tagged value only for the kind it is (a string at
  a `CSTR` position, a struct at `STRUCT`, ...), or where the row says the
  native reads it through its own API (`TURI_HSIG_STRUCT_OK` / `_CSTR_OK`).
- **Rows covered 4 positions.** `map-assoc-eq`'s comparator is its fifth
  argument and was called unchecked.  `TURI_HSIG_MAX_ARGS` is 6, and a
  described position the call omits is refused.
- **Natives with no row**, in whole families: Result/Option, json/schema, the
  r7rs ports / id tables / continuations, sized-buf, the comonad cells,
  Mock-Time, task groups and promises, seq generators, and every native that
  reads a C string from an int carrier (`str->sym`, `tur_string_from_cstr`,
  `__inst_Hash_hash_cstr`, ...).  `k_handle_rows[]` grew from 242 to 400 rows
  and 25 new kinds -- one per heap layout, because the kind is what keeps one
  layout from being read as another.
- **Wrong rows.** `tur_string_slice` / `_slice_cstr` minted a slice as a
  `String`; `bt-cons` streams and trail cells shared `BTCELL`; gen-arr
  `{len, cap, data}` and seq-out-vec `{data, len, cap}` shared `GENARR`;
  `mutmap-eq-storage?` checked for a wrap where it reads a slot table;
  `seq-val-unwrap` wanted `SEQCELL` for a generator box.
- **Words a native follows that no row sees**, checked in the native's body
  with `turi_prov_check`: list links, Result elements of a Vec, `mbind`'s
  continuation result, schema nodes reached through a union or a rec, JSON
  node types (one kind covers every node type, so each accessor checks
  `node[0]`).
- **Key comparators compare STORED keys**, and the comparator is chosen per
  call, so a key stored under the int comparator can be strcmp'd by the cstr
  one.  The interpreter's string and struct comparators (and the `_cstr` HAMT
  natives, which now run through them in a restricted env) compare an
  unrecorded word unequal instead of reading it, via `turi_prov_word_live` and
  the env of the native running on this thread.  The owned-key protocol (`-o`
  natives' ownership flag, under which the runtime releases stored keys as
  boxes) is refused outright in a restricted env.
- **Frees.** `cstr-free` / `r7rs-cstr-free__` free only a string minted as
  owned (`OWNED_CSTR`); anything else is left alone, since a tagged alias of a
  freed string cannot be tracked.  `tur_string_cstr` returns an env-pool copy,
  so a released `String` leaves no dangling tagged string.
- **NULL**: a kind whose natives always dereference (`BTCELL`, `GENARR`,
  `SEQVEC`, the r7rs handles) refuses a NULL handle.
- **Bounds and sizes that were never checked** (sandbox-reachable, so a crash
  of the host): `cstr-nth`, `gen-arr-get`, `seq-vec-get`, `r7rs-io-byte-ref__`,
  `r7rs-utf8-at__`, `r7rs-call-variadic__`'s count, big-number radix and zero
  divisor, number-syntax radix, `bytes-alloc` on a 32-bit `size_t`, a cyclic
  JSON tree in `json/encode`, a self-referential schema, a generator resumed
  from inside its own body.  A native allocation in a restricted env is capped
  at `TURI_PROV_MAX_ALLOC_BYTES` (256 MiB), since step fuel does not meter it.
- **A capability gap**: the `r7rs-eval-c-*` natives other than eval/load were
  capability 0 but act on the process-global embedded R7RS env, which holds
  every capability.  They now need every capability, like eval and load.
- **The inline-C override path** called a stdlib defn's standing-in native with
  neither the guard nor the mint/free tracking; both paths now go through
  `call_native_checked`.

Pinned by `native-sweep` in `tests/turi/sandbox-eval.c` (POSIX; it forks):
every capability-free native, forged arguments at four positions, every arity
up to six, and the run fails naming any native that crashes or hangs.
`SANDBOX_SWEEP_ONLY=<name>` reruns one with its output kept.

The registry was also measured for false refusals, by forcing it on in every
env and running the whole interpreter suite (`tests/run-turi.sh`): the
direction-1 table alone failed 115 fixtures that way (81 of them on a quoted
symbol, `(sym->str 'foo)`); with the tables as they are now, 17 of the 2648
it runs do, each for a reason listed below (a `String`-keyed map, re-entrant
`call/cc`) or one a restricted env cannot reach (an inline-C body -- `set-hamt`,
`seq-new`, a fixture's own -- which a sandbox and the macro env refuse before
it runs).

## What the fix does not cover

- **The sweep proves single calls.** A native that stores a word and a later
  native that follows it are covered by the rows and the in-body checks above,
  read by hand -- not by the sweep.  Direction 2 (tagged handles) remains the
  structural route, and the only one that would extend the guarantee to an
  unrestricted env.
- **Unrestricted envs are unchanged**: an embedder that grants every
  capability gets no registry, and its programs can still forge handles.  That
  env is not a sandbox, so this is by design.
- **Owned-key maps and sets** (`Map String V`, `Set String`) are refused in a
  restricted env: their key comparator is never handed out there, and the
  ownership flag is refused.
- **Scratch promotion** (`turi_env_set_scratch_promotion`) is a no-op in a
  restricted env: its arena reset would hand registered addresses to the next
  allocation.
- **Re-entrant `call/cc`** (`#lang r7rs`) captures a stack image only inside a
  top-level form's prompt in a restricted env, and restores one only under the
  same prompt on the same thread; elsewhere it is escape-only.  Outside a
  prompt the image would run up to the thread's stack base -- the embedder's
  own frames.
- **Tagged values are their own proof.** A string, struct or closure value the
  interpreter holds tagged is trusted.  That is sound only while nothing frees
  one behind its back -- the reason `cstr-free` and `tur_string_cstr` changed --
  so a new native that frees an interpreter-visible string or struct needs the
  same care.

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

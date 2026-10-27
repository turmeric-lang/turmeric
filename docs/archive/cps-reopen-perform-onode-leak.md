# Effect re-opening leaks DK nodes O(N) per re-opened perform

**RESOLVED (2026-07-21).** Fixed in `src/compiler/emit_dk_runtime.c` (tramp
path). The leaked chains were NOT at the E7 yield point (as the earlier probe
concluded) but at two owners that a tail-resume `longjmp` unwinds before their
eager free runs:

1. **`dk_perform`, non-tail deep branch.** A non-tail deep case that RE-OPENS an
   outer effect ends its body in that interior `perform`; if the outer effect is
   tail-resumed, `dk_tail_resume` longjmps to the entry driver and the
   `dk_perform` frame is unwound, so `dk_free(sub)` never runs -> `sub` leaks
   once per re-opened perform.
2. **`dk_invoke`.** When the invoked chain itself tail-resumes (e.g. the
   re-opening resume frame `dk_invoke`s the captured continuation, which drives
   the NEXT re-opened perform), the same longjmp unwinds `dk_invoke` before its
   `dk_free(c)` -> `c` leaks once per invoke.

Both are given a boundary owner via the existing reap machinery
(`__dk_reap_keep`) BEFORE the call that may longjmp, and their eager `dk_free`
is dropped on that path. Safe against double-free: only a TAIL case hands its
`sub` to the driver (the E7 branch, which the driver frees), a case body only
reaps COPIES of `subk` (never `subk`/`sub`), and the driver frees only the fresh
sub a tail-resume yields (a copy taken from within `c`) -- never `sub`/`c`
themselves. `dk_invoke` reaps only under an active driver (a longjmp is
possible); with no driver it frees eagerly as before, so the non-tramp path is
byte-identical.

Verified with `valgrind --leak-check=full`: the repro table below (N = 1, 2, 4,
8) all drop to **0 bytes lost, 0 errors**, and the E7 fixtures
(`cps-tramp-resume-deep`, `effect-reopen`, `cps-tramp-resume-reopen`, `-nested`,
`-multicase`, `-nontail`, `-join`, `-while-handle`,
`-loop-in-handle-continuation`) stay clean with no `Invalid free`. This is now
the same bounded-at-entry retention the surrounding re-opening reaps
(`__kont`/`__ce`/frame nodes) already use -- not a new unbounded class.

---

**Severity:** low (memory only; correctness is fine).  Flag-on only
(`--enable=cps-tramp-resume`), and only for a program that uses effect
RE-OPENING (a handler case that performs an effect handled by an ENCLOSING
handler).  Compiled fixture binaries are not ASan/leak-checked by `tests/run.sh`
(they are built with plain `cc`), so this blocks no suite/sweep gate -- but it is
a genuine unbounded leak for a deep re-opening loop.

**One-line:** each re-opened `perform` in a handler case body leaks its
`dk_perform` `sub` chain (~a few DK nodes), so total leaked memory grows O(N) in
the number of re-opened performs executed.

## Repro (valgrind)

```turmeric
;; N re-opened Log performs; the Log case re-opens Write to the outer handler.
(defeffect Log [msg : cstr] : nil)
(defeffect Write [msg : cstr] : nil)
(defn main [] : int
  (handle
    (handle
      (do (perform (Log "1")) (perform (Log "2")) (perform (Log "3")) (perform (Log "4")) 0)
      (Log [msg] k) (do (perform (Write msg)) (resume k 0)))
    (Write [msg] k) (do (println msg) (resume k 0)))
  0)
```

`tur build --enable=cps-tramp-resume` then valgrind:

| re-opened performs | definitely+indirectly lost |
| --- | --- |
| 1 (`Test D`)        | 288 bytes |
| 2 (`effect-reopen`) | 1632 bytes |
| 4 (above)           | 2784 bytes |

Linear in N -> a per-perform leak, not a fixed setup leak.

## Root cause (direction)

The re-opened effect (`Write`) is handled by the OUTER handler, which -- when its
case tail-resumes -- is installed as `dk_handler_tail`.  So the re-opened
`dk_perform` takes the E7 trampoline YIELD branch in `dk_perform`
(`emit_dk_runtime.c`):

```c
if (H->tail_resume && g_dk_driver) {
    DK *__deliv = dk_copy_range(H->next, NULL);
    ... __dk_meta_push(__deliv) ...
    return H->handler(H->handler_env, arg, sub);   /* longjmp -- sub never freed */
}
```

Unlike the non-tramp branch (which does `dk_free(sub)` after the handler
returns), the yield branch returns via `longjmp` to the driver and never frees
`sub` (the `dk_copy_range(k,H)` + appended re-install `tail`).  For a re-opened
perform this happens once per case invocation -> O(N).  The deep E7 fixture
(`cps-tramp-resume-deep`) is valgrind-clean because its resumed chain IS the
driver-run chain (consumed/freed by the trampoline); the re-opening case
allocates an EXTRA `sub` per perform that no one owns.

## Fix direction

Give the yielded `sub` an owner: register it with the reap machinery
(`__dk_reap_node`/`__dk_reap_keep`) at the yield point, or have the driver free
the resumed chain once it settles (mirroring the non-tramp `dk_free(sub)`).  Care
is needed not to double-free the portion the driver re-enters.  Verify with the
table above dropping to 0 lost, and the E7 fixtures staying valgrind-clean.

Discovered while landing the `effect-reopen` DK slice
(`docs/archive/history/cps-perform-cont-heap-join-eviction.md`); the leak is
pre-existing in the re-opening machinery (commit `ffd878897`) and independent of
that slice -- it just first RUNS the re-opening DK path end-to-end.

## Verified NOT the yielded `sub` (2026-07-20)

The obvious fix -- `__dk_reap_keep(sub)` at the yield point (emit_dk_runtime.c,
the `if (H->tail_resume && g_dk_driver)` branch) -- was tried and **fails on both
counts** under valgrind on the 4-perform repro:

- **Double free.** `free(): double free detected in tcache 2` / valgrind
  `Invalid free()`. So the yield-point `sub` handed to the handler IS freed
  elsewhere on at least one path (the driver's `dk_free(ch)` in
  `__dk_drive_after` reaches it when the case tail-resumes `k == sub` directly),
  and reaping it as well double-frees. The direct-resume and re-open paths share
  this one `sub` allocation but dispose of it differently, and the yield point
  cannot tell which path the case will take.
- **Leak unchanged.** `definitely lost: 728 bytes` / `indirectly lost: 2,184`
  were byte-for-byte identical with and without the reap. So the leaked nodes are
  **not** the yield-point `sub` at all -- they are the re-install copies
  (`dk_copy_range(H, ge)` + `dk_copy_enclosing_handlers(ge)` appended as `tail`,
  and/or the `__deliv = dk_copy_range(H->next, NULL)` pushed to the meta-stack)
  that the re-opening case captures by-intptr in a frame env, invisible to the
  driver's `->next` free walk.

**Revised fix direction.** The owner must be given to the re-install `tail` /
`__deliv` copies specifically, coordinated with the driver's `dk_free(ch)` so the
directly-resumed `sub` is freed exactly once. A blanket reap of `sub` is wrong.
The likely shape: register only the copies that are provably captured by-intptr
(never reachable via the resumed chain's `->next`), or switch the whole tramp
path to reap-owned chains and drop the driver's `dk_free(ch)`. Either needs
per-node ownership reasoning across the longjmp boundary and must be checked with
`valgrind --leak-check=full` on the repro (target: 0 lost) AND on
`cps-tramp-resume-deep` / `effect-reopen` (no `Invalid free`). Still low-severity
(memory-only, `--enable=cps-tramp-resume`-gated), so it does not block anything.

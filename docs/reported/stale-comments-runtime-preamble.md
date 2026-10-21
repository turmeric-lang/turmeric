# S2 split comments still say a missing runtime archive disengages the split

**Severity:** low (comment accuracy). One item is a redundant call, and one
proposes a small structural change that would have prevented two past
regressions. Filed 2026-10-17 from the "Trust the code over the comments" note
in
[runtime-preamble-and-linkage-guide](../guides/runtime-preamble-and-linkage-guide.md).
Checked against `main` 9ac3cd43; line numbers are from that commit.

## Background

`emit_rt_split_source` (`emit_module.c:19280`) re-emits the all-gates preamble.
`jit_try_split_preamble` hashes that text and compares it against the committed
`tur_rt_split_hash`. A while ago, a host with no lean runtime archive emitted
the rc/GC section as bodies, not declarations, so the hash never matched and S2
silently disengaged everywhere. The fix forces the archive posture inside
`emit_rt_split_source` itself (`emit_set_rcgc_from_archive(true)` at `:19296`,
restored on the way out). Since that fix, a missing archive **cannot** change
the canonical text. Four comments still say it does.

All of these comments are in C source, not in emitted strings or in
`src/runtime/*` files the emitter pastes. Fixing them changes no snapshot and
no split hash.

## Changes

### 1. `rt_split_embed.h:1-13`

- **Says:** "On a mismatch -- emitter drift, a knob like --backtrack-depth, a
  missing runtime archive -- the full preamble is used unchanged".
- **Change:** drop "a missing runtime archive" from the list. Add one sentence
  saying that the per-host inputs are forced to a canonical value (the archive
  posture, `g_trail_autoloaded`, the `g_needs_*` gates) precisely so that they
  cannot cause a mismatch.

### 2. `emit_rt_split_source`'s own header (`emit_module.c:19261-19279`)

- **Says:** "Any drift -- an emitter edit, a knob like --backtrack-depth,
  archive-mode state -- changes this text, fails the compare".
- **Code:** sixteen lines further down, the body forces archive mode and says
  why ("Force it here, where every caller gets the canonical posture"). The
  function's comment contradicts the function.
- **Change:** replace "archive-mode state" with a short split between the two
  kinds of input. **Knobs** (`--backtrack-depth`, experiments) are left as
  they are, so that the compare covers them. **Per-host or per-program facts**
  (archive posture, trail autoload, `g_needs_*`, `g_cps_path`,
  `g_has_variadics`, `g_needs_winsock`) are forced and restored. The paragraph
  "Knobs are deliberately NOT normalized ... Gate globals (g_needs_*) are
  forced" already makes this split; extend it to name the archive and trail
  flags too.

### 3. `cmd_emit_rt_split`'s header, and a redundant call (`driver/preamble_split.c:cmd_emit_rt_split`)

- **Says:** "cmd_jit's probe runs under real process state, and a JIT
  invocation with no runtime archive (rc/GC emitted as definitions) therefore
  hashes differently and falls back to full-preamble emission -- the guard is
  self-enforcing."
- **Code:** false since the fix above. The probe gets the canonical posture as
  well. `cmd_emit_rt_split` still calls `emit_set_rcgc_from_archive(true)`
  before `emit_rt_split_source`, which sets it again.
- **Change:** delete the "Archive mode is hard-set ... self-enforcing"
  paragraph and the `emit_set_rcgc_from_archive(true)` call. `emit_rt_split_source` is now the one place
  that owns the posture. With the call gone, nothing in this function sets the
  posture, so the comment has nothing to drift away from.

### 4. `cmd_jit`'s call-site rationale (`driver/jit.c:cmd_jit`)

- **Says:** "Emitter drift, knob drift (--backtrack-depth, experiments), and a
  missing runtime archive all change that text".
- **Change:** drop "and a missing runtime archive". The rest of this comment
  is accurate.

### 5. `jit_try_split_preamble`'s doc comment sits on the wrong function (`driver/preamble_split.c:jit_try_split_preamble`)

> **Resolved 2026-10-21** (main-c-split-plan Phase 0, 21b48e72): block 2 now
> sits directly above `jit_try_split_preamble`, in `driver/preamble_split.c`.

- **Code:** three comment blocks are stacked before `jit_split_reject`:
  1. "NOT under `#ifdef TUR_HAVE_JIT`, deliberately. These two ...";
  2. "S2 (findings 25): swap an emitted TU's fixed preamble ... Returns true
     and fills `out` ...", which describes `jit_try_split_preamble`;
  3. "Report WHY the split did not engage", which describes
     `jit_split_reject`.

  `jit_try_split_preamble` itself has no comment.
- **Change:** move block 2 to just above `jit_try_split_preamble`, and leave
  block 1 above the pair. A reader who jumps to the function from a call site
  currently lands on a function with no documentation.

### 6. Structure: make the forced-posture set one table (`emit_rt_split_source`)

The guide's pitfall table records the lesson twice: "a new state input to the
preamble joins the forced set". The archive posture had to be added after it
silently disabled the split, and `g_trail_autoloaded` had to be added after it
did the same. Today that set is seven `bool` locals saved by hand, then
forced, then restored, plus `g_rt_split_all_gates`, plus a separate
`emit_set_rcgc_from_archive` setter.

- **Change:** keep the forced inputs in one static table of
  `{ bool *flag; bool forced; }` and loop over it to save, force and restore.
  The archive posture can stay a special case through its setter, or be moved
  into the table. Adding an input then means adding a table row, and the
  comment above the table can list the rows. With that in place, items 1-3
  could not have drifted: there would be one list, and it would be the code.

## When this is fixed

Remove the example from the guide's "Trust the code over the comments"
paragraph. Point the `emit_rt_split_source` row of its pitfall table at the
table from item 6, if that lands.

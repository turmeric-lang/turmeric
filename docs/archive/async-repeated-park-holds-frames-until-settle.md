# An async body that parks again and again keeps every park's frames until it settles

**RESOLVED 2026-10-08** (filed the same day) -- see "Fixed 2026-10-08" at the
end.

**Severity: low-medium (bounded by the body's life, but a server loop that
never settles grows without limit).** Split out 2026-10-08 from
[async-parked-body-chains-never-reaped](async-parked-body-chains-never-reaped.md),
which is fixed: a park's registrations now belong to the park and are reaped
when the resumed body settles, and an await on a fulfilled future allocates
nothing. What is left is the time *between* the first park and the settle.

## Repro

A consumer loop that awaits a fresh pending future every turn, fulfilled from
outside (the every-turn shape of
`tests/fixtures/async-park-reaped-after-settle`, scaled up):

```turmeric
(defn next-value [] : int (await (cell 2 false)))
(defn sum-values [i : int n : int acc : int] : int
  (if (= i n) acc (sum-values (+ i 1) n (+ acc (next-value)))))
```

| turns | peak RSS |
| --- | --- |
| 100,000 | 20 MB |
| 400,000 | 76 MB |

About **187 B a turn**, all of it freed when the body returns (before
2026-10-08 it was ~293 B a turn and never freed).

## Where it comes from

Each turn's park copies the chain from the await up to the body's root
(`dk_copy_range` in `dk_run_impl`'s shift arm, then a private copy for the
park in `__tur_await_body`). The await's own shift and frame nodes are handed
back right away (`__dk_await_release`), but the copied range also crosses
`sum-values`'s heap join for that turn -- its env (24 B) and node (120 B),
registered by `emit_heap_join`. The join is marked `copied`, so it can never
be released as it runs (`__dk_join_release_node`), and it moves into the
park's share of the reap list (`__dk_reap_seg`) with everything else the turn
registered. The share is passed on whole to each later park
(`__dk_reap_seg_move` in `__tur_async_resume`) and reaped only when the body
settles.

Both are dead well before that. After a park, only copies of the chain ever
run, so the ORIGINAL join node is unreachable at once. Its env is shared with
the copies, and is dead once the resumed copy has run that frame -- which it
does early in the next turn, before that turn's park.

## Fix directions

1. **Free the parked range's original nodes at the park.** The shift arm knows
   the range it copied (`k->next` up to the root prompt). Those nodes are on
   the reap list as single-node entries (`__dk_reap_node`); dropping them from
   the park's share needs a cheap way to find them -- e.g. a flag on the node
   that the reap pass, or the share's hand-off, reads (`DK.copied` is set on
   exactly these, but also on nodes a multi-shot handler's copies still run).
2. **Give a copied frame's env an owner.** The env outlives the original only
   for the copies. If the resumed copy (the last holder) freed it after
   running the frame, as an uncopied join does today, nothing would be left
   per turn -- but a re-park copies the frames that have not run yet, so this
   needs either a count of live copies per env or the frames' own
   `env_clone` / `env_drop` (which today only owning envs carry).

The node is most of the cost (~136 B of the ~187 with malloc's header, the
env ~40 B, the two reap-list slots the rest); (1) alone would leave ~50 B a
turn, and both together nothing.

## Fixed 2026-10-08

Both directions, keyed on an exact per-node copy count rather than a guess:

- **Copies are counted.** `DK.copied` is now `ncopy`, the number of times
  `dk_copy_node` copied the node (saturating at 255); the join release still
  reads "never copied" off it.
- **A parked continuation owns its envs.** The emitter records the size of a
  heap join's env and an await continuation's env (`dk_env_sized`), and the
  park copies its chain with `dk_copy_range_owned`: each sized frame in it
  holds a private byte copy of its env (`env_owned`), freed with the node, and
  every copy made from such a node clones it again, so nothing a park leaves
  behind ever shares an env with it.  Frame envs are written once and only
  read after, so a copy reads the same as the original.
- **The originals a shift left behind are freed at the park's hand-off.**
  Once an await's shift returns, `__dk_await_release` marks every node between
  the shift and its prompt `orphaned` -- only copies of them run from then on.
  When the awaited future is still pending (only the shift's own park leaves
  it so, `__tur_future_pending`), the shift arm's copy was copied on into the
  park, so an original copied that once and never otherwise (`ncopy == 1`) has
  an env nothing shares: `orphan_env`.  Single-node registrations are a reap
  kind of their own now (3), so the hand-off (`__dk_reap_seg_take`) can read
  the flag: it frees an orphaned node, and its env when it is the entry
  registered just before it, instead of moving them into the park.

The new fields sit in the struct's tail padding: `sizeof(DK)` is 120 B before
and after.

### Measured

Peak RSS, compiled at -O2:

| program | before | after |
| --- | --- | --- |
| a loop that parks every turn, 400,000 turns | 76 MB | 10 MB |
| the same under three non-tail callers, 2 x 200,000 turns | 76 MB | 10 MB |
| a turn that builds a capturing closure and branches, 200,000 turns | 85 MB | 10 MB |
| a turn that also performs an effect handled inside the body, 200,000 turns | 205 MB | 167 MB |

`tests/fixtures/async-repeated-park-frees-each-turn` (leak-checked) reads the
pending park's share off its record at turns 10 and 900: 27 and 1,807
entries before, 1 and 1 (the entry's root) now.  All 37 async fixtures and
nine adversarial shapes (re-parks across branches, nested asyncs, a resume
driven from an effect loop, multi-shot handlers before a park, closures in the
parked chain) run clean under ASan.  Suite: `run.sh` 3631/0, `run-leak-check.sh`
132 / 0 / 3 known-open, `run-turi.sh` 2671/0, the JIT over the async, CPS,
effect and multishot fixtures 440/0.

### What is left

- **A perform each turn.** The last row: the perform's own copy of its
  continuation is kept until the body settles (`__dk_reap_keep(sub)` in
  `dk_perform`), ~785 B a turn here (it was ~975 with the joins).  It is the
  same retention as the open effect half of
  [fn-value-call-cps-frames-held-until-outer-entry](../reported/fn-value-call-cps-frames-held-until-outer-entry.md),
  which outside async is bounded by the outermost entry instead.
- **Frames whose env size the emitter does not record** -- a perform's
  continuation frame, a reset's -- still share their env with the park, and
  their originals' envs stay in the share until the body settles.  Their
  nodes are freed at the hand-off like any other.

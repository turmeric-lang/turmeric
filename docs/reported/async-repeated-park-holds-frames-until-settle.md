# An async body that parks again and again keeps every park's frames until it settles

**Severity: low-medium (bounded by the body's life, but a server loop that
never settles grows without limit).** Split out 2026-10-08 from
[async-parked-body-chains-never-reaped](../archive/async-parked-body-chains-never-reaped.md),
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

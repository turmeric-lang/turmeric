# Follow-up Plan: tur-watch recursive per-file naming

> **Status:** Implemented (v0.2.0)
> **Last Updated:** 2026-05-29
> **Type:** Spice Follow-up
> **Depends on:** [../tur-watch-spice-plan.md](../tur-watch-spice-plan.md)
>                 (`watch-v0.1.0` ships in
>                 `../turmeric-spices/spices/watch/`; WT6 recursive watch
>                 is in place but emits directory-level events only)

---

## Problem

`watch-open-tree` in `tur-watch` v0.1.0 reports a `watch-event` whose
`path` is the directory that produced the OS event, not the file inside
that directory that actually changed. From `spices/watch/src/watch/watch.tur`:

```turmeric
(defn __watch-next-tree [watcher : int timeout-ms : int] #{Unsafe} : int
  (let [idx (__watcher-tree-poll-fired watcher timeout-ms)]
    ...
    (watch-event-make (__watcher-tree-root watcher) dir dir
                      (watch-kind-write) 1 0)))
```

`is-dir` is hard-coded to `1` and `path` / `full-path` are both `dir`.
This works for callers who only need "something under this root changed,
trigger a full rebuild," but it does not work for callers who want to
react per-file (rerun a specific test, recompile a single source file,
display the name in a UI).

The single-file path already does proper kind classification via
stat-compare; the gap is specific to tree mode.

---

## Why this slipped from v0.1.0

The plan called for per-file naming under "Event kinds yes" in tree mode,
but the implementation hit a real cross-platform asymmetry:

- **Linux / inotify** -- each `inotify_event` carries a `name[]` field
  for the affected entry inside the watched directory, plus a `mask`
  that already distinguishes `IN_MODIFY` / `IN_CREATE` / `IN_DELETE` /
  `IN_MOVED_FROM` / `IN_MOVED_TO`. Per-file naming is essentially free
  once `backend-drain` is upgraded to expose names instead of a boolean.
- **Darwin / kqueue** -- `EVFILT_VNODE` on a directory fd fires for
  entry-table changes but **does not** name the affected entry. The
  only way to recover the name is to enumerate the directory and diff
  against a snapshot.

Shipping just the Linux half would have given callers an obvious
"works on my machine" pothole, and shipping the Darwin diff machinery
without an adopter to pressure-test it would have baked an
implementation choice we cannot back out of. v0.1.0 deferred the
feature; this plan closes it now that notebook (single-file) has
validated the rest of the spice in production.

---

## Goals

1. `watch-open-tree` watchers emit `watch-event` records whose `path` is
   the file (or subdirectory) that changed, relative to the registered
   root, and whose `full-path` is the absolute path on disk.
2. `kind` is classified per-event using OS signals where available
   (Linux `inotify_event.mask`) and a directory-snapshot diff as the
   fallback (Darwin).
3. Same-batch coalesce-by-path (`watch/debounce.debounce-batch-coalesce`)
   meaningfully deduplicates -- it currently can't, because every event
   in a burst shares the same directory-as-path.
4. No regression for the directory-level use case: callers that only
   care about "something changed" can keep using the existing
   `watch-event-path` and treat it the same as before.

Non-goals:

- macOS `FSEvents` backend (still deferred to v0.3 or later).
- Windows backend.
- Path filters / ignore globs (separate follow-up).
- Restructuring `watch-event` -- the existing record already has
  `is-dir`, `cookie`, and separate `path` / `full-path` fields; no
  schema change is required.

---

## Design sketch

### Backend layer: expose per-event records

`watch/backend` currently has:

```
backend-drain handle -> int  (1 = any event matched, 0 = none, -1 = error)
```

Upgrade to a record-producing API:

```
backend-drain-into handle batch -> int
  -- pushes one TurBackendEvent per OS event onto a provided buffer
  -- returns the count drained, or -1 on error
```

A `TurBackendEvent` carries:

| Field      | Source (Linux)             | Source (Darwin)               |
|------------|----------------------------|-------------------------------|
| `wd`/dir   | the inotify watch desc.    | the fd of the dir that fired  |
| `name`     | `inotify_event.name`       | "" (must be recovered later)  |
| `mask`     | `inotify_event.mask`       | synthesized from EVFILT_VNODE |
| `cookie`   | `inotify_event.cookie`     | 0                             |
| `is_dir`   | `mask & IN_ISDIR`          | filled by snapshot diff       |

The old boolean `backend-drain` stays available as a thin wrapper for
single-file mode, which does not need names.

### Darwin: directory-snapshot diff

When `EVFILT_VNODE` wakes the watcher, the tree layer snapshots the
affected directory's entries (`readdir` into a small hash set keyed by
name + inode + mtime) and diffs against the previous snapshot for that
directory:

- entry name present now but not before → `create`
- entry name present before but not now → `delete`
- same name, different inode → `rename`
- same name, same inode, mtime/size changed → `write`
- same name, only attrib (mtime alone, with size and ino unchanged
  inside a hardlink/permissions edit) → `attrib`

Each diff cell becomes one `watch-event`. The snapshot is kept on the
watcher (one per registered directory), invalidated lazily on each
drain.

### Linux: pass-through with stat fill-in

Each `inotify_event` already carries `name` and `mask`. Translate the
mask to a `watch-kind-*` value and emit one event per inotify event.
For `MOVED_FROM` + `MOVED_TO` pairs sharing a cookie, emit a single
`rename` event with the destination path (matching the current
single-file rename semantics).

### watch-next dispatch

`__watch-next-tree` becomes:

```turmeric
1. poll all backend fds with debounce timeout
2. for each fired backend:
     fetch (and free) the directory snapshot diff or inotify drain
     produce a list of watch-events
3. return the first event; queue the rest for the next call
   (or return a batched list via watch-drain)
```

A small per-watcher pending-event queue lets `watch-next` keep its
"one event per call" contract while the diff produces many.

---

## Implementation phases

- [x] **WTNF1** -- Add `TurBackendEvent` struct and `backend-drain-into`
  in `watch/backend`. Keep the old boolean drain working for
  single-file callers. Add a backend-only test that drives a few
  inotify events into a fixture and asserts names+masks come through.

- [x] **WTNF2** -- Linux pass-through path in `watch/watch`: convert
  drained `TurBackendEvent`s into `watch-event` records with kind
  classification from the inotify mask. Add tree-mode tests for
  `write`/`create`/`delete`/`rename` per-file naming.

- [x] **WTNF3** -- Darwin snapshot machinery: per-directory entry
  snapshots (`name → {ino, size}`), built at `watch-open-tree`
  and refreshed on each drain. `__watcher-tree-produce-events`
  diffs new vs old snapshot and emits `watch-event` records.

- [x] **WTNF4** -- Per-watcher pending-event queue so `watch-next`
  still returns one event per call when the backend produced many.
  `watch-drain` continues to return cons-lists; with multi-event
  batches finally arriving from one backend wake, coalesce-by-path
  becomes meaningful and gets a real burst test (`tree_burst_test.tur`).

- [x] **WTNF5** -- Documentation refresh:
  - updated `spices/watch/README.md` capability matrix and architecture
    diagram to reflect per-file naming shipping for both backends.
  - `docs/guides/watch-guide.md` and `docs/notebook-watch-semantics.md`
    do not yet exist in the turmeric repo; no update needed here.

- [x] **WTNF6** -- Tag a new spice release (`watch-v0.2.0`) and bump
  consumers:
  - notebook (single-file, no behavior change, just version bump),
  - any other adopters that landed in the meantime (likely the
    `tur build --watch` or `tur run --watch` follow-ons).

---

## Acceptance criteria

1. `tur run examples/tail-events.tur -- some-dir` prints one line per
   file change, with the filename, on both Linux and Darwin.
2. The WT6 acceptance test (`tests/tree_test.tur`) is extended so that
   instead of `cstr-contains? path "tur-watch-tree"`, the assertion is
   `path = "<root>/a/b/leaf.txt"`.
3. A new burst test (similar to `drain_burst_test.tur` but multi-file)
   confirms that `watch-drain` + `debounce-batch-coalesce` deduplicates
   correctly: three saves of `a/x` + two of `b/y` collapse to one
   event per path.
4. The single-file path is byte-identical in behavior: the existing
   `watch_test.tur` and `drain_burst_test.tur` keep passing unchanged.

---

## Risks and open questions

1. **Snapshot memory cost on Darwin.** A large tree (~10 000 files) with
   per-dir name+inode+mtime snapshots could use a few MB of resident
   state. That is fine for editor / build workflows but worth
   measuring before declaring the feature done. If it becomes a
   problem, `FSEvents` is the right escape hatch.

2. **`MOVED_FROM` without a matching `MOVED_TO`.** Moves across
   watched-tree boundaries leave dangling cookies. Default: emit them
   as `delete` after a short grace period (the same approach
   `chokidar` and `watchman` use). The grace-period machinery is the
   first place WTNF4's pending queue earns its keep.

3. **Snapshot races on Darwin.** Between `EVFILT_VNODE` waking us and
   the `readdir` snapshot, more changes can land. The diff catches
   those on the next wake; nothing is lost as long as we re-drain
   before sleeping. Document this and write a stress test (rapid
   create-then-delete inside the debounce window).

4. **API compatibility.** `backend-drain-into` is new; the existing
   boolean `backend-drain` stays as-is. No existing callers break. The
   only public-facing change is that `watch-event-path` now points at
   files for tree-mode events. Documented as a v0.2 semantics change,
   not a v0.1 bug fix, so callers can opt in.

---

## Out of scope (deferred again)

- macOS `FSEvents` backend (lower syscall cost, but a much bigger
  reimplementation -- earlier than v0.3 is unlikely).
- Windows `ReadDirectoryChangesW` backend.
- Ignore globs / path filters.
- Callback / async event-loop API.
- `tur repl --watch` adopter (still blocked on the readline-interrupt
  work inside `turmeric/src/turi/repl.c`).

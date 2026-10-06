# Notebook Watch Semantics

> **Status:** Spec / freeze
> **Last Updated:** 2026-05-29
> **Source:** `spices/notebook/src/notebook/cli.tur` (notebook-v0.1.0)
> **Consumers:** `tur-watch` v0.1.0 (see `docs/tur-watch-spice-plan.md` in
> the `turmeric` repo)

This document freezes the observable behavior of `tur nb render --watch` as
shipped in `notebook-v0.1.0`. It is the spec the extracted `tur-watch` spice
must preserve when notebook is refactored to depend on it (WT7 of the
tur-watch plan).

The intent is to capture *what callers see*, not *how the current C code
happens to be written*, so that a re-implementation behind the same outward
contract is still a passing change.

---

## 1. What is watched

A single absolute or relative file path, supplied as the `<file.tur.md>`
argument to `tur nb render --watch` (or `tur nb export --watch`).

Internally the watcher always registers the **parent directory**, not the
file node:

- Linux: `inotify_add_watch(dir, IN_CLOSE_WRITE | IN_CREATE | IN_MODIFY |
  IN_MOVED_TO | IN_ATTRIB | IN_DELETE | IN_MOVE_SELF | IN_DELETE_SELF)`
- Darwin: `kevent` on the directory's file descriptor with `EVFILT_VNODE`
  and `NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME |
  NOTE_DELETE | NOTE_REVOKE`

Watching the directory rather than the file is what lets the watcher survive
atomic-save editors that swap the file out from under it (see §4).

If the path has no `/`, the watched directory is `.` and the basename is
the path itself.

Recursive watching, multiple paths, and directory watching are **not**
supported in notebook-v0.1.0.

---

## 2. Initial-state snapshot

At `watch-open` time the watcher records, via `stat(2)`:

| Field        | Source                           |
|--------------|----------------------------------|
| `exists`     | `stat == 0`                      |
| `mtime_sec`  | `st.st_mtimespec.tv_sec`         |
| `mtime_nsec` | `st.st_mtimespec.tv_nsec`        |
| `size`       | `st.st_size`                     |
| `ino`        | `st.st_ino`                      |

If the file does not exist at open time, `exists` is `0` and the other
fields are zero. The watcher still opens successfully -- a file that
appears later will produce an event.

Failure modes that cause `watch-open` to return `0` (failure):

- empty path
- on Linux: `inotify_init1` or `inotify_add_watch` fails
- on Darwin: opening the parent directory, creating the kqueue, or the
  initial `kevent` registration fails
- on any other platform: not supported

---

## 3. Event delivery contract

The public API the renderer relies on is a single blocking call,
`watch-wait(watcher, debounce_ms)`, which returns:

- `1` when the watched file has *materially changed* since the last call
- `0` on watcher-fatal errors (queue read failure, watcher torn down, etc.)

"Materially changed" means: after waking from the OS event queue, the
watcher re-`stat`s the file and only returns `1` if **at least one** of
`exists`, `mtime_sec`, `mtime_nsec`, `size`, or `ino` differs from the
snapshot. The snapshot is then updated to the new values.

If `stat` shows no observable change, the watcher loops and waits again
without informing the caller. This is the heart of the "ignore irrelevant
sibling-file edits" behavior.

Polling timeout:

- Linux: `poll(pollfd, 1, -1)` -- block forever
- Darwin: `kevent(..., NULL)` -- block forever

There is no caller-provided timeout in notebook-v0.1.0; the wait is always
unbounded.

---

## 4. Debounce window

When a backend event fires, the watcher sleeps for `debounce_ms` (currently
`150 ms`, hardcoded in `__cli-watch-loop`) **before** re-`stat`-ing. This
sleep is the burst-collapse mechanism:

- editors that save by `write` + `rename` produce two or more backend
  events in quick succession; the sleep lets the storm settle and we
  rerender once against the final on-disk bytes
- editors that issue multiple small `write`s for one logical save likewise
  get collapsed
- a sibling file modified in the same directory loses out: by the time the
  sleep ends, its events have already been drained and the stat-compare
  step decides "no, the file we care about did not change"

The 150 ms value is the contract `tur-watch` must keep available; it does
not have to be a hardcoded constant in the new spice, but the *default*
must be 150 ms so that notebook's behavior is preserved on the cutover.

---

## 5. Event filtering

The watcher's job is to surface **one boolean signal per logical change**.
The renderer never inspects platform flags. Specifically:

- on Linux, an `inotify_event` whose `name` matches the watched basename
  counts; events with `len == 0` (i.e., events on the watch descriptor
  itself, such as `IN_MOVE_SELF`) also count
- on Darwin, *any* `EVFILT_VNODE` event on the parent directory counts,
  because the kqueue is registered on the directory fd and there is no
  per-file filtering at the backend layer; the stat-compare step is what
  prevents false positives from sibling-file writes
- if the basename does not match (Linux) **and** stat does not show a
  change (Darwin), the watcher silently loops

Backend queue overflow is **not** explicitly surfaced today; under Linux,
overflow events would land in the buffer like any other and either match
the basename or be ignored. `tur-watch` v0.1.0 must do better: surface an
explicit `overflow` event so callers can fall back to a full rebuild.

---

## 6. Atomic-save handling

Editors that save with the "write-temp-then-rename" pattern (vim, emacs,
many IDEs) produce, in order on the parent directory:

1. `IN_CREATE` / `NOTE_WRITE` for the temp file
2. `IN_CLOSE_WRITE` / `NOTE_WRITE` for the temp file
3. `IN_MOVED_TO` / `NOTE_RENAME` replacing the target
4. possibly `IN_ATTRIB` on the new inode

The notebook watcher relies on the 150 ms debounce sleep plus the
post-sleep stat-compare to collapse all of (1)-(4) into a single
"the target changed" signal. The watched basename is the *logical target*
(e.g. `notes.tur.md`), not whatever temp filename the editor chose, so
when the rename completes the new `ino` differs from the snapshot and the
watcher returns `1`.

`tur-watch` v0.1.0 must produce *one* debounced batch for this flow, not a
storm of temp-file events.

---

## 7. Rerender loop

After `watch-wait` returns `1`, the CLI:

1. calls `__cli-render-once` against the same target
2. if rendering succeeds (exit code 0), loops back into `watch-wait`
3. if rendering fails, the watcher is closed and the loop exits with the
   render exit code

The render itself runs in a **fresh session** -- there is no carry-over of
notebook session state between rerender iterations. This is a notebook
policy, not a watcher policy, and `tur-watch` does not need to know about
it. The watcher's responsibility ends at "the file changed".

---

## 8. Teardown

`watch-close` is best-effort and never reports failure:

- Linux: `inotify_rm_watch` (if registered), then `close(fd)`
- Darwin: `close(kq)`, then `close(dir_fd)`
- frees the heap-allocated path / dir / basename strings, then the struct

The CLI calls `watch-close` from both the success and the failure branches
of the render loop. `tur-watch` must be safe to close from any state,
including immediately after a failed `watch-open`.

---

## 9. What `tur-watch` v0.1.0 must add on top

The contract above is the *minimum*. The extracted spice must layer in:

| Capability                       | Status in notebook-v0.1.0 | Required in `tur-watch` v0.1.0 |
|----------------------------------|---------------------------|--------------------------------|
| Single-file watch                | yes                       | yes                            |
| Multiple explicit paths          | no                        | yes                            |
| Directory watch                  | no                        | yes                            |
| Recursive directory watch        | no                        | yes (WT6)                      |
| Event kinds (`write`/`create`/`delete`/`rename`/`attrib`/`overflow`) | boolean only | yes |
| Bounded / poll timeout           | no                        | yes (`watch-next timeout-ms`)  |
| Batch drain API                  | no                        | yes (`watch-drain`)            |
| Explicit overflow event          | no                        | yes                            |
| Default debounce window          | 150 ms (hardcoded)        | 150 ms (configurable)          |

Anything else (FSEvents, Windows, callback API, content hashing) is
explicitly deferred per the spice plan.

---

## 10. Test fixtures that must continue to pass

After the WT7 cutover, the following notebook behaviors must still hold,
in addition to the spice's own tests:

1. `tur nb render --watch foo.tur.md` rebuilds when `foo.tur.md` is
   modified in place
2. ditto when `foo.tur.md` is replaced via temp-file + rename
3. editing a sibling file in the same directory does **not** trigger a
   rerender
4. removing the file and re-creating it triggers a single rerender
5. multiple writes within ~150 ms collapse to one rerender
6. a failed render exits the watch loop with the render's exit code

These are the load-bearing observable behaviors. If `tur-watch` reproduces
them, the cutover is safe.

### Platform note on in-place writes

Behavior (1) -- "modified in place" -- relies on `inotify` reporting
`IN_MODIFY` / `IN_CLOSE_WRITE` for a child file. That is a Linux-only
guarantee. On Darwin, the watcher's kqueue is registered on the parent
directory fd, and `EVFILT_VNODE` on a *directory* fires for entry-level
changes (create / unlink / rename) but **not** for in-place content
modification of a child file. So `fopen("w")` followed by a write to an
existing target does not produce a Darwin-side event today.

Notebook gets away with this because every editor it actually targets
(vim, emacs, VS Code) defaults to atomic-save (write temp + rename),
which *does* fire on both backends. `tur-watch` v0.1.0 preserves the same
limitation: cross-platform behavior is guaranteed for atomic-save flows
and entry-level events; pure in-place writes are Linux-only. Closing this
gap on Darwin would require also kqueue-ing the file fd directly, which
in turn requires re-opening it across rename -- deferred to v0.2.

Re-measured 2026-09-09 on macOS 27 (Darwin 27.0.0), kqueue `EVFILT_VNODE`
on a directory fd with the flag set section 1 lists. The note above holds exactly:

| Operation on a child of the watched dir | Darwin dir-kqueue fires? |
|-----------------------------------------|--------------------------|
| in-place `fopen("w")` + write            | no                       |
| `O_APPEND` write                         | no                       |
| atomic save (temp + `rename` over it)    | yes (`NOTE_WRITE`)       |
| create a new entry                       | yes (`NOTE_WRITE`)       |
| `unlink` an entry                        | yes (`NOTE_WRITE`)       |

### What this note does *not* explain

The paragraph above is about **whether the OS fires at all**. It has twice
been reached for to explain macOS failures that were something else, so two
explicit exclusions:

- **"`backend-wait` returned 1 but `backend-event-count` was 0."** Not this
  limitation -- an entry-level change had fired correctly. That was a bug in
  the Darwin `backend-wait`, which used `kevent()` to observe the queue and
  so *dequeued* the event it was only supposed to notice, leaving the
  following drain nothing to find. Fixed 2026-09-09 by polling the kqueue fd
  instead (`poll(2)` on a kqueue descriptor is level-triggered and
  non-consuming on Darwin, and is already how the tree path treats this same
  fd). See `docs/watch-macos-backend-wait-consumed-the-event.md`.

- **"Darwin reported `create` where Linux reported `rename`."** Also not this
  limitation -- the event fired on both. It is a *classification* difference:
  Linux sees the `rename(2)` itself (`IN_MOVED_TO`), while the Darwin tree
  layer recovers names by diffing directory snapshots and cannot tell a name
  that arrived by rename from one that arrived by `creat`. The two backends
  agree whenever the destination name already existed, which is what an
  atomic save over a real file is. See
  `docs/watch-darwin-fresh-name-is-create.md`.

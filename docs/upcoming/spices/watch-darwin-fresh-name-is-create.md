# `watch`: on Darwin a new name that arrived via `rename` is reported `create`, not `rename`

- **Status:** open, and probably permanent at the current backend. Not a
  regression -- this has been true since the Darwin tree layer was written.
- **Severity:** low. It affects the `kind` field only; the event fires and
  the path is correct on both platforms.
- **Found:** 2026-09-09 while fixing the two macOS `spices/watch` failures
  in `docs/watch-macos-backend-wait-consumed-the-event.md`. Split out
  because it is a separate finding and is *not* being fixed.

## The divergence

Move or `rename(2)` a file into a watched directory under a name that was
not there before:

| Backend | `watch-event-kind` |
|---------|--------------------|
| Linux   | `rename` (4)       |
| Darwin  | `create` (2)       |

Both agree (`rename`) when the destination name already existed, which is
the atomic-save case every editor produces and the case
`tests/tree_test.tur` covers.

## Repro

Against `spices/watch`, with a tree watcher open on `root`:

```turmeric
;; leaf.txt does NOT exist when the watcher opens
(let [w (watch-open-tree root opts)]
  ;; background: write root/a/b/leaf.txt.tmpZZ, then rename it to leaf.txt
  (let [ev (watch-next w 5000)]
    (watch-event-kind ev)))   ; Linux => 4 (rename);  Darwin => 2 (create)
```

This is exactly `tests/tree_test.tur` as it stood before 2026-09-09; the
fixture now seeds `leaf.txt` first, so it no longer sits on the divergence.
To see it again, delete the `(write-file leaf "initial")` line.

## Root cause

`kqueue`'s `EVFILT_VNODE` on a *directory* fd delivers no filename and no
operation -- only combined `NOTE_*` flags on the directory itself (a create,
an unlink and a rename-into all arrive identically as `NOTE_WRITE`). So
`__watcher-tree-produce-events` in `src/watch/watch.tur` recovers names by
diffing `readdir` snapshots of the directory:

- name in the new snapshot, absent from the old -> `create`
- name in both, inode changed -> `rename`
- name in both, size changed -> `write`
- name in the old, absent from the new -> `delete`

A name that arrived by rename lands in the first bucket, and nothing in the
snapshot distinguishes it from a name that arrived by `creat`. Linux does
not have this problem because `IN_MOVED_TO` names the operation directly.

The one case a snapshot diff could in principle recover is a rename *within*
the watched directory, by matching the vanished entry's inode against the
new one. It does not help here: the temp file is created and renamed away
inside the same debounce window, so it never appears in any snapshot.

## Why it is not being fixed

- **Per-file kqueue watches** do not apply -- the file does not exist yet at
  registration time, which is the whole premise of the case.
- **FSEvents** (`FSEventStreamCreate`) does carry per-path flags including
  `kFSEventStreamEventFlagItemRenamed`, so it could close this. But it is a
  CFRunLoop callback API with no pollable descriptor: adopting it means a
  dedicated run-loop thread plus a self-pipe to keep `backend-fd` /
  `__watcher-tree-poll-fired` working, on top of coalescing FSEvents' own
  directory-granularity-under-pressure behavior. That is a rewrite of the
  Darwin backend, not a patch to it, and it buys one field on one edge case.

If a caller ever needs the distinction, the cheap partial answer is to
report `create` and let the caller `stat` -- the path is accurate, which is
what consumers actually branch on.

## Related

- `docs/notebook-watch-semantics.md` section 10 -- the (separate, still accurate)
  limitation that kqueue on a directory does not fire at all for in-place
  content modification of a child file.
- `docs/watch-macos-backend-wait-consumed-the-event.md` -- the two macOS
  test failures this was found underneath.
